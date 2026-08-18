/*
 * net.c -- TCP socket primitives: net-connect, net-read,
 * net-read-all, net-write, net-close.
 *
 * Sockets are MINO_HANDLE values (tag "mino/net-socket") wrapping a
 * malloc'd descriptor record. The handle finalizer closes the fd when
 * the value is collected or the state is torn down, so a dropped
 * socket never leaks; net-close is the explicit, idempotent form.
 *
 * Winsock is initialised lazily, once per process, on the first
 * socket call: a WSAStartup failure there can be reported as a
 * script-visible error, which a void install hook cannot do.
 * WSACleanup is intentionally never called; the init is held for the
 * process lifetime and the OS reclaims sockets at exit. POSIX needs
 * no equivalent.
 *
 * Blocking calls (resolver lookup, connect poll, recv, send) yield
 * the state lock for their duration so worker threads keep making
 * progress, mirroring thread-sleep / future-deref.
 *
 * Trust model.
 *
 * Like fs.c, the socket primitives take whatever host / port /
 * payload the script author hands them. The embedder is inside the
 * trust boundary; the script author *is* the trust boundary. Argument
 * shapes are validated; destinations are not policed. An embedder
 * that wants to forbid network access refuses to install
 * MINO_CAP_NET (it is not in MINO_CAP_DEFAULT).
 */

#define _POSIX_C_SOURCE 200809L
#if defined(__APPLE__)
#  define _DARWIN_C_SOURCE 1
#endif

#include "prim/internal.h"
#include "mino.h"

#ifdef _WIN32
#  define WIN32_LEAN_AND_MEAN
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  include <winsock2.h>
#  include <ws2tcpip.h>
#  include <windows.h>
#else
#  include <sys/socket.h>
#  include <sys/types.h>
#  include <poll.h>
#  include <sys/time.h>
#  include <netdb.h>
#  include <unistd.h>
#  include <errno.h>
#  include <fcntl.h>
#endif

#include <string.h>
#include <stdlib.h>
#include <limits.h>

#ifdef _WIN32
typedef SOCKET mino_net_fd_t;
#define MINO_NET_INVALID_FD INVALID_SOCKET
#else
typedef int mino_net_fd_t;
#define MINO_NET_INVALID_FD (-1)
#endif

#define NET_SOCK_TAG "mino/net-socket"

/* Default per-operation timeouts, milliseconds. */
#define NET_DEFAULT_CONNECT_TIMEOUT_MS 10000LL
#define NET_DEFAULT_READ_TIMEOUT_MS    30000LL
#define NET_DEFAULT_WRITE_TIMEOUT_MS   30000LL

/* Ceiling for caller-supplied timeouts before they scale to
 * nanoseconds; above this the ms->ns multiply would overflow a
 * signed long long. About 24 days, far past any sane deadline. */
#define NET_MAX_TIMEOUT_MS 2147483647LL

/* net-read-all accumulates until EOF or this cap (overridable via
 * :max-bytes). Bounds a hostile or broken peer that never closes. */
#define NET_READ_ALL_DEFAULT_MAX_BYTES (16LL * 1024LL * 1024LL)

#define NET_READ_CHUNK 65536

typedef struct {
    mino_net_fd_t fd;
    long long     read_timeout_ms;
    long long     write_timeout_ms;
    int           closed;
} mino_net_sock_t;

/* ---- platform shims ---- */

#ifdef _WIN32
/* Process-lifetime winsock init; see the file-top comment. InitOnce
 * closes the check-then-act window between host threads driving
 * independent states: every caller either observes a completed
 * WSAStartup or runs it exactly once. A failed startup leaves the
 * once unclaimed, so the failure stays reportable to each caller. */
static BOOL CALLBACK net_winsock_start(PINIT_ONCE once, PVOID param,
                                       PVOID *ctx)
{
    WSADATA data;
    (void)once; (void)param; (void)ctx;
    return WSAStartup(MAKEWORD(2, 2), &data) == 0 ? TRUE : FALSE;
}

static int net_winsock_init(void)
{
    static INIT_ONCE once = INIT_ONCE_STATIC_INIT;
    if (!InitOnceExecuteOnce(&once, net_winsock_start, NULL, NULL))
        return -1;
    return 0;
}
#endif

static void net_close_fd(mino_net_fd_t fd)
{
#ifdef _WIN32
    closesocket(fd);
#else
    close(fd);
#endif
}

/* OS detail string for diagnostics. buf must be >= 96 bytes. */
static void net_os_error(char *buf, size_t cap)
{
#ifdef _WIN32
    snprintf(buf, cap, "winsock error %d", (int)WSAGetLastError());
#else
    snprintf(buf, cap, "%s", strerror(errno));
#endif
}

static void net_sock_finalize(void *ptr, const char *tag)
{
    mino_net_sock_t *s = (mino_net_sock_t *)ptr;
    (void)tag;
    if (s == NULL) return;
    if (!s->closed) net_close_fd(s->fd);
    free(s);
}

/* ---- argument helpers ---- */

/* Extract the socket record from a net-socket handle. Throws eval/type
 * for non-socket values; returns NULL only when the throw unwound to
 * a host diagnostic rather than a catch frame. */
static mino_net_sock_t *net_sock_arg(mino_state *S, mino_val *v,
                                     const char *who)
{
    if (v == NULL || mino_type_of(v) != MINO_HANDLE
        || v->as.handle.tag == NULL
        || strcmp(v->as.handle.tag, NET_SOCK_TAG) != 0
        || v->as.handle.ptr == NULL) {
        char msg[160];
        snprintf(msg, sizeof(msg), "%s: argument must be a net socket",
                 who);
        prim_throw_classified(S, "eval/type", "MTY001", msg);
        return NULL;
    }
    return (mino_net_sock_t *)v->as.handle.ptr;
}

/* Read an optional non-negative ms timeout out of opts under key.
 * Falls back to def when absent. Throws eval/contract on a negative
 * or non-integer value. Returns 0 on success. */
static int net_opt_ms(mino_state *S, mino_val *opts, const char *key,
                      long long def, long long *out)
{
    mino_val *v;
    long long    ms;
    *out = def;
    if (opts == NULL || mino_type_of(opts) != MINO_MAP) return 0;
    v = map_get_val(opts, mino_keyword(S, key));
    if (v == NULL || mino_type_of(v) == MINO_NIL) return 0;
    if (!as_long(v, &ms) || ms < 0) {
        char msg[160];
        snprintf(msg, sizeof(msg),
                 "net: opts key :%s must be a non-negative integer", key);
        prim_throw_classified(S, "eval/contract", "MCT001", msg);
        return -1;
    }
    *out = ms;
    return 0;
}

/* Apply read/write timeouts to the connected fd. */
static void net_apply_io_timeouts(mino_net_fd_t fd, long long read_ms,
                                  long long write_ms)
{
    if (read_ms > 0) {
#ifdef _WIN32
        DWORD ms = (DWORD)read_ms;
        (void)setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO,
                         (const char *)&ms, sizeof(ms));
#else
        struct timeval tv;
        tv.tv_sec  = (time_t)(read_ms / 1000);
        tv.tv_usec = (long)((read_ms % 1000) * 1000);
        (void)setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
#endif
    }
    if (write_ms > 0) {
#ifdef _WIN32
        DWORD ms = (DWORD)write_ms;
        (void)setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO,
                         (const char *)&ms, sizeof(ms));
#else
        struct timeval tv;
        tv.tv_sec  = (time_t)(write_ms / 1000);
        tv.tv_usec = (long)((write_ms % 1000) * 1000);
        (void)setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
#endif
    }
}

/* ---- connect ---- */

/* Set / clear non-blocking mode. Returns 0 on success. */
static int net_set_nonblocking(mino_net_fd_t fd, int on)
{
#ifdef _WIN32
    u_long mode = on ? 1u : 0u;
    return ioctlsocket(fd, FIONBIO, &mode) == 0 ? 0 : -1;
#else
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0) return -1;
    return fcntl(fd, F_SETFL, on ? (flags | O_NONBLOCK)
                                 : (flags & ~O_NONBLOCK)) == 0 ? 0 : -1;
#endif
}

/* Wait for the non-blocking connect to complete or time out.
 * Returns 0 connected, -1 failed (detail in err, may be empty on
 * timeout), 1 on timeout.
 *
 * poll(2) (WSAPoll on Windows) rather than select(2): select's
 * fd_set is indexed by descriptor number, so an fd at or above
 * FD_SETSIZE writes past the fixed bitmap on the stack. poll has no
 * descriptor limit. POLLERR and POLLHUP are reported whether or not
 * they were requested, so asking for POLLOUT alone still wakes on
 * all three completion outcomes. */
static int net_wait_connected(mino_state *S, mino_net_fd_t fd,
                              long long timeout_ms)
{
    long long deadline;
    if (timeout_ms > NET_MAX_TIMEOUT_MS) timeout_ms = NET_MAX_TIMEOUT_MS;
    deadline = mino_monotonic_ns() + timeout_ms * 1000000LL;
    for (;;) {
        long long remaining_ms;
        long long now = mino_monotonic_ns();
        int poll_ms;
        int rc;
        if (now >= deadline) return 1;
        remaining_ms = (deadline - now) / 1000000LL;
        poll_ms = remaining_ms > (long long)INT_MAX
                    ? INT_MAX : (int)remaining_ms;
        {
#ifdef _WIN32
            WSAPOLLFD pfd;
            int depth = mino_yield_lock(S);
            pfd.fd      = fd;
            pfd.events  = POLLOUT;
            pfd.revents = 0;
            rc = WSAPoll(&pfd, 1, poll_ms);
            mino_resume_lock(S, depth);
#else
            struct pollfd pfd;
            int depth = mino_yield_lock(S);
            pfd.fd      = fd;
            pfd.events  = POLLOUT;
            pfd.revents = 0;
            rc = poll(&pfd, 1, poll_ms);
            mino_resume_lock(S, depth);
#endif
        }
        if (rc < 0) {
#ifdef _WIN32
            int e = WSAGetLastError();
            if (e == WSAEINTR) continue;
#else
            if (errno == EINTR) continue;
#endif
            return -1;
        }
        if (rc == 0) return 1;
        {
            int so_err = 0;
#ifdef _WIN32
            int len = sizeof(so_err);
            if (getsockopt(fd, SOL_SOCKET, SO_ERROR,
                           (char *)&so_err, &len) != 0) return -1;
#else
            socklen_t len = sizeof(so_err);
            if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &so_err, &len) != 0)
                return -1;
#endif
            if (so_err != 0) {
#ifdef _WIN32
                WSASetLastError(so_err);
#else
                errno = so_err;
#endif
                return -1;
            }
            return 0;
        }
    }
}

/* (net-connect host port [opts]) -> net-socket handle.
 * opts keys :connect-timeout / :read-timeout / :write-timeout (ms). */
static mino_val *prim_net_connect(mino_state *S, mino_val *args,
                                  mino_env *env)
{
    mino_val *host_val, *port_val, *opts = NULL;
    mino_val *hv;
    long long port, connect_ms, read_ms, write_ms;
    char host[512];
    char portstr[16];
    char detail[128];
    struct addrinfo hints, *res = NULL, *ai;
    mino_net_fd_t fd = MINO_NET_INVALID_FD;
    mino_net_sock_t *sock;
    int depth;
    int gai_rc;
    (void)env;

    if (!mino_is_cons(args)) {
        return prim_throw_classified(S, "eval/arity", "MAR001",
                                     "net-connect requires host and port");
    }
    host_val = args->as.cons.car;
    args = args->as.cons.cdr;
    if (!mino_is_cons(args)) {
        return prim_throw_classified(S, "eval/arity", "MAR001",
                                     "net-connect requires host and port");
    }
    port_val = args->as.cons.car;
    args = args->as.cons.cdr;
    if (mino_is_cons(args)) {
        opts = args->as.cons.car;
        if (opts != NULL && mino_type_of(opts) == MINO_MAP
            && mino_is_cons(args->as.cons.cdr)) {
            return prim_throw_classified(S, "eval/arity", "MAR001",
                                         "net-connect takes at most 3 "
                                         "arguments");
        }
        if (opts != NULL && mino_type_of(opts) != MINO_MAP
            && mino_type_of(opts) != MINO_NIL) {
            return prim_throw_classified(S, "eval/type", "MTY001",
                                         "net-connect: opts must be a map");
        }
    }
    if (host_val == NULL || mino_type_of(host_val) != MINO_STRING) {
        return prim_throw_classified(S, "eval/type", "MTY001",
                                     "net-connect: host must be a string");
    }
    if (host_val->as.s.len >= sizeof(host)) {
        return prim_throw_classified(S, "eval/contract", "MCT001",
                                     "net-connect: host is too long");
    }
    memcpy(host, host_val->as.s.data, host_val->as.s.len);
    host[host_val->as.s.len] = '\0';
    if (!as_long(port_val, &port) || port < 1 || port > 65535) {
        return prim_throw_classified(S, "eval/contract", "MCT001",
                                     "net-connect: port must be an integer "
                                     "in 1..65535");
    }
    if (net_opt_ms(S, opts, "connect-timeout",
                   NET_DEFAULT_CONNECT_TIMEOUT_MS, &connect_ms) != 0)
        return NULL;
    if (net_opt_ms(S, opts, "read-timeout",
                   NET_DEFAULT_READ_TIMEOUT_MS, &read_ms) != 0)
        return NULL;
    if (net_opt_ms(S, opts, "write-timeout",
                   NET_DEFAULT_WRITE_TIMEOUT_MS, &write_ms) != 0)
        return NULL;

#ifdef _WIN32
    if (net_winsock_init() != 0) {
        return prim_throw_classified(S, "net", "MNE004",
                                     "net-connect: WSAStartup failed");
    }
#endif
    snprintf(portstr, sizeof(portstr), "%lld", port);
    memset(&hints, 0, sizeof(hints));
    hints.ai_family   = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    /* Pre-flight the handle value before any descriptor exists: its
     * allocation can throw for OOM, and from here on every failure
     * path holds an fd or a malloc'd record that such a throw at the
     * end would strand. The value stays pinned and is filled only
     * once the socket record is complete. */
    hv = mino_handle_ex(S, NULL, NET_SOCK_TAG, net_sock_finalize);
    gc_pin(hv);
    /* No deadline on the lookup itself: :connect-timeout bounds the
     * TCP connect, DNS resolves on the calling thread to its own
     * completion. */
    depth = mino_yield_lock(S);
    gai_rc = getaddrinfo(host, portstr, &hints, &res);
    mino_resume_lock(S, depth);
    if (gai_rc != 0) {
        char msg[300];
        gc_unpin(1);
#ifdef _WIN32
        snprintf(msg, sizeof(msg), "net-connect: cannot resolve host %s: "
                 "getaddrinfo error %d", host, gai_rc);
#else
        snprintf(msg, sizeof(msg), "net-connect: cannot resolve host %s: %s",
                 host, gai_strerror(gai_rc));
#endif
        return prim_throw_classified(S, "net/dns", "MNE001", msg);
    }

    detail[0] = '\0';
    for (ai = res; ai != NULL; ai = ai->ai_next) {
        mino_net_fd_t try_fd;
        int rc;
        try_fd = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
        if (try_fd == MINO_NET_INVALID_FD) {
            net_os_error(detail, sizeof(detail));
            continue;
        }
        if (net_set_nonblocking(try_fd, 1) != 0) {
            net_os_error(detail, sizeof(detail));
            net_close_fd(try_fd);
            continue;
        }
        rc = connect(try_fd, ai->ai_addr, ai->ai_addrlen);
        if (rc != 0) {
#ifdef _WIN32
            if (WSAGetLastError() != WSAEWOULDBLOCK) {
                net_os_error(detail, sizeof(detail));
                net_close_fd(try_fd);
                continue;
            }
#else
            if (errno != EINPROGRESS) {
                net_os_error(detail, sizeof(detail));
                net_close_fd(try_fd);
                continue;
            }
#endif
            rc = net_wait_connected(S, try_fd, connect_ms);
            if (rc == 1) {
                char msg[300];
                net_close_fd(try_fd);
                freeaddrinfo(res);
                gc_unpin(1);
                snprintf(msg, sizeof(msg),
                         "net-connect: connect to %s:%lld timed out after "
                         "%lld ms", host, port, connect_ms);
                return prim_throw_classified(S, "net/connect", "MNE002",
                                             msg);
            }
            if (rc < 0) {
                net_os_error(detail, sizeof(detail));
                net_close_fd(try_fd);
                continue;
            }
        }
        if (net_set_nonblocking(try_fd, 0) != 0) {
            net_os_error(detail, sizeof(detail));
            net_close_fd(try_fd);
            continue;
        }
        fd = try_fd;
        break;
    }
    freeaddrinfo(res);
    if (fd == MINO_NET_INVALID_FD) {
        char msg[300];
        gc_unpin(1);
        snprintf(msg, sizeof(msg), "net-connect: cannot connect to %s:%lld: "
                 "%s", host, port,
                 detail[0] ? detail : "no usable addresses");
        return prim_throw_classified(S, "net/connect", "MNE002", msg);
    }

#if !defined(_WIN32) && !defined(MSG_NOSIGNAL)
    /* No per-send MSG_NOSIGNAL flag on this platform; ask the socket
     * itself to suppress SIGPIPE (BSD / Apple). */
#  ifdef SO_NOSIGPIPE
    {
        int one = 1;
        (void)setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof(one));
    }
#  endif
#endif
    net_apply_io_timeouts(fd, read_ms, write_ms);

    sock = (mino_net_sock_t *)malloc(sizeof(*sock));
    if (sock == NULL) {
        net_close_fd(fd);
        gc_unpin(1);
        return prim_throw_classified(S, "internal", "MIN001",
                                     "net-connect: out of memory");
    }
    sock->fd              = fd;
    sock->read_timeout_ms  = read_ms;
    sock->write_timeout_ms = write_ms;
    sock->closed          = 0;
    hv->as.handle.ptr = sock;
    gc_unpin(1);
    return hv;
}

/* ---- read ---- */

/* Single recv of up to n bytes into a malloc'd buffer. Returns:
 *   1  got 1..*got bytes
 *   0  clean EOF before any byte
 *  -1  error (kind/code/msg filled) */
static int net_recv_once(mino_state *S, mino_net_sock_t *sock,
                         unsigned char *buf, size_t n, size_t *got,
                         const char **kind, const char **code, char *msg,
                         size_t msg_cap)
{
    for (;;) {
        int rc;
        int depth = mino_yield_lock(S);
#ifdef _WIN32
        rc = recv(sock->fd, (char *)buf, (int)n, 0);
        mino_resume_lock(S, depth);
        if (rc == SOCKET_ERROR) {
            int e = WSAGetLastError();
            if (e == WSAEINTR) continue;
            if (e == WSAETIMEDOUT) {
                snprintf(msg, msg_cap,
                         "net-read: timed out after %lld ms",
                         sock->read_timeout_ms);
                *kind = "net/timeout";
                *code = "MNE003";
                return -1;
            }
            snprintf(msg, msg_cap, "net-read: read failed: winsock error %d",
                     e);
            *kind = "net";
            *code = "MNE004";
            return -1;
        }
#else
        ssize_t r;
        rc = 0;
        r = recv(sock->fd, buf, n, 0);
        mino_resume_lock(S, depth);
        if (r < 0) {
            if (errno == EINTR) continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                /* Blocking fd + SO_RCVTIMEO: EAGAIN marks expiry. */
                snprintf(msg, msg_cap,
                         "net-read: timed out after %lld ms",
                         sock->read_timeout_ms);
                *kind = "net/timeout";
                *code = "MNE003";
                return -1;
            }
            snprintf(msg, msg_cap, "net-read: read failed: %s",
                     strerror(errno));
            *kind = "net";
            *code = "MNE004";
            return -1;
        }
        rc = (int)r;
#endif
        if (rc == 0) {
            *got = 0;
            return 0;
        }
        *got = (size_t)rc;
        return 1;
    }
}

/* (net-read sock n) -> up to n bytes as soon as any arrive; nil on
 * clean EOF before the first byte. A short read is normal. */
static mino_val *prim_net_read(mino_state *S, mino_val *args, mino_env *env)
{
    mino_val *sock_val, *n_val;
    mino_net_sock_t *sock;
    long long n;
    unsigned char *buf;
    size_t got = 0;
    int rc;
    const char *kind = "net", *code = "MNE004";
    char msg[160];
    (void)env;

    if (!mino_is_cons(args) || !mino_is_cons(args->as.cons.cdr)
        || mino_is_cons(args->as.cons.cdr->as.cons.cdr)) {
        return prim_throw_classified(S, "eval/arity", "MAR001",
                                     "net-read requires a socket and a byte "
                                     "count");
    }
    sock_val = args->as.cons.car;
    n_val    = args->as.cons.cdr->as.cons.car;
    sock = net_sock_arg(S, sock_val, "net-read");
    if (sock == NULL) return NULL;
    if (sock->closed) {
        return prim_throw_classified(S, "net", "MNE004",
                                     "net-read: socket is closed");
    }
    if (!as_long(n_val, &n) || n < 0) {
        return prim_throw_classified(S, "eval/contract", "MCT001",
                                     "net-read: n must be a non-negative "
                                     "integer");
    }
    if (n == 0) return mino_bytes(S, NULL, 0);
    if ((unsigned long long)n > SIZE_MAX) {
        return prim_throw_classified(S, "eval/contract", "MCT001",
                                     "net-read: n is too large");
    }
    buf = (unsigned char *)malloc((size_t)n);
    if (buf == NULL) {
        return prim_throw_classified(S, "internal", "MIN001",
                                     "net-read: out of memory");
    }
    /* The socket record is re-read after the recv yield windows; pin
     * the handle value so a concurrent collection cannot finalize it
     * mid-park. */
    gc_pin(sock_val);
    rc = net_recv_once(S, sock, buf, (size_t)n, &got, &kind, &code, msg,
                       sizeof(msg));
    gc_unpin(1);
    if (rc < 0) {
        free(buf);
        return prim_throw_classified(S, kind, code, msg);
    }
    if (rc == 0) {
        free(buf);
        return mino_nil(S);
    }
    {
        mino_val *bytes = mino_bytes(S, buf, got);
        free(buf);
        return bytes;
    }
}

/* (net-read-all sock [opts]) -> bytes until EOF. opts key :max-bytes
 * caps accumulation (default 16 MiB); a stream longer than the cap
 * throws :net/overflow rather than buffering a peer that never
 * closes; a stream exactly at the cap drains to EOF and succeeds. */
static mino_val *prim_net_read_all(mino_state *S, mino_val *args,
                                   mino_env *env)
{
    mino_val *sock_val, *opts = NULL;
    mino_net_sock_t *sock;
    long long max_bytes = NET_READ_ALL_DEFAULT_MAX_BYTES;
    unsigned char *buf = NULL;
    size_t len = 0, cap = 0;
    mino_val *result;
    (void)env;

    if (!mino_is_cons(args)) {
        return prim_throw_classified(S, "eval/arity", "MAR001",
                                     "net-read-all requires a socket");
    }
    sock_val = args->as.cons.car;
    args = args->as.cons.cdr;
    if (mino_is_cons(args)) {
        opts = args->as.cons.car;
        if (mino_is_cons(args->as.cons.cdr)) {
            return prim_throw_classified(S, "eval/arity", "MAR001",
                                         "net-read-all takes at most 2 "
                                         "arguments");
        }
        if (opts != NULL && mino_type_of(opts) != MINO_MAP
            && mino_type_of(opts) != MINO_NIL) {
            return prim_throw_classified(S, "eval/type", "MTY001",
                                         "net-read-all: opts must be a map");
        }
    }
    sock = net_sock_arg(S, sock_val, "net-read-all");
    if (sock == NULL) return NULL;
    if (sock->closed) {
        return prim_throw_classified(S, "net", "MNE004",
                                     "net-read-all: socket is closed");
    }
    if (net_opt_ms(S, opts, "max-bytes", NET_READ_ALL_DEFAULT_MAX_BYTES,
                   &max_bytes) != 0)
        return NULL;
    if (max_bytes < 0) max_bytes = 0;

    /* Pinned across the loop: each iteration re-reads the socket
     * record after recv yield windows, so the handle must stay
     * rooted against a concurrent collection. */
    gc_pin(sock_val);
    for (;;) {
        unsigned char chunk[NET_READ_CHUNK];
        size_t got = 0;
        int rc;
        const char *kind = "net", *code = "MNE004";
        char msg[160];
        rc = net_recv_once(S, sock, chunk, sizeof chunk, &got, &kind,
                           &code, msg, sizeof(msg));
        if (rc < 0) {
            free(buf);
            gc_unpin(1);
            return prim_throw_classified(S, kind, code, msg);
        }
        if (rc == 0) break;
        /* Strict-after-read (mirrors tls-read-all): only data past
         * the cap throws, so a stream exactly max-bytes long returns
         * rather than overflowing at EOF. */
        if ((long long)(len + got) > max_bytes) {
            char m[160];
            free(buf);
            gc_unpin(1);
            snprintf(m, sizeof(m),
                     "net-read-all: exceeded :max-bytes cap of %lld bytes",
                     max_bytes);
            return prim_throw_classified(S, "net/overflow", "MNE005", m);
        }
        if (len + got > cap) {
            size_t new_cap = cap == 0 ? NET_READ_CHUNK : cap * 2;
            unsigned char *nb;
            while (new_cap < len + got) {
                if (new_cap > SIZE_MAX / 2) { new_cap = len + got; break; }
                new_cap *= 2;
            }
            nb = (unsigned char *)realloc(buf, new_cap);
            if (nb == NULL) {
                free(buf);
                gc_unpin(1);
                return prim_throw_classified(S, "internal", "MIN001",
                                             "net-read-all: out of memory");
            }
            buf = nb;
            cap = new_cap;
        }
        memcpy(buf + len, chunk, got);
        len += got;
    }
    gc_unpin(1);
    result = mino_bytes(S, buf, len);
    free(buf);
    return result;
}

/* ---- write ---- */

#if defined(MSG_NOSIGNAL)
#  define NET_SEND_FLAGS MSG_NOSIGNAL
#else
#  define NET_SEND_FLAGS 0
#endif

/* (net-write sock data) -> byte count written. data is a string
 * (UTF-8 bytes) or bytes. Blocks until every byte is written, the
 * write timeout expires, or the connection fails. */
static mino_val *prim_net_write(mino_state *S, mino_val *args, mino_env *env)
{
    mino_val *sock_val, *data_val;
    mino_net_sock_t *sock;
    const unsigned char *data;
    size_t len, sent = 0;
    (void)env;

    if (!mino_is_cons(args) || !mino_is_cons(args->as.cons.cdr)
        || mino_is_cons(args->as.cons.cdr->as.cons.cdr)) {
        return prim_throw_classified(S, "eval/arity", "MAR001",
                                     "net-write requires a socket and data");
    }
    sock_val = args->as.cons.car;
    data_val = args->as.cons.cdr->as.cons.car;
    sock = net_sock_arg(S, sock_val, "net-write");
    if (sock == NULL) return NULL;
    if (sock->closed) {
        return prim_throw_classified(S, "net", "MNE004",
                                     "net-write: socket is closed");
    }
    if (data_val != NULL && mino_type_of(data_val) == MINO_STRING) {
        data = (const unsigned char *)data_val->as.s.data;
        len  = data_val->as.s.len;
    } else if (data_val != NULL && mino_type_of(data_val) == MINO_BYTES) {
        data = mino_bytes_data(data_val);
        len  = mino_bytes_len(data_val);
    } else {
        return prim_throw_classified(S, "eval/type", "MTY001",
                                     "net-write: data must be a string or "
                                     "bytes");
    }

    while (sent < len) {
        int rc;
        int depth = mino_yield_lock(S);
#ifdef _WIN32
        rc = send(sock->fd, (const char *)data + sent, (int)(len - sent),
                  0);
        mino_resume_lock(S, depth);
        if (rc == SOCKET_ERROR) {
            int e = WSAGetLastError();
            if (e == WSAEINTR) continue;
            if (e == WSAETIMEDOUT) {
                char msg[160];
                snprintf(msg, sizeof(msg),
                         "net-write: timed out after %lld ms",
                         sock->write_timeout_ms);
                return prim_throw_classified(S, "net/timeout", "MNE003",
                                             msg);
            }
            {
                char msg[200];
                snprintf(msg, sizeof(msg),
                         "net-write: write failed: winsock error %d", e);
                return prim_throw_classified(S, "net", "MNE004", msg);
            }
        }
#else
        ssize_t r;
        rc = 0;
        r = send(sock->fd, data + sent, len - sent, NET_SEND_FLAGS);
        mino_resume_lock(S, depth);
        if (r < 0) {
            if (errno == EINTR) continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                char msg[160];
                snprintf(msg, sizeof(msg),
                         "net-write: timed out after %lld ms",
                         sock->write_timeout_ms);
                return prim_throw_classified(S, "net/timeout", "MNE003",
                                             msg);
            }
            {
                char msg[200];
                snprintf(msg, sizeof(msg), "net-write: write failed: %s",
                         strerror(errno));
                return prim_throw_classified(S, "net", "MNE004", msg);
            }
        }
        rc = (int)r;
#endif
        sent += (size_t)rc;
    }
    return mino_int(S, (long long)sent);
}

/* ---- close ---- */

/* (net-close sock) -> nil. Idempotent; a closed socket is marked so
 * the handle finalizer never double-closes the fd. */
static mino_val *prim_net_close(mino_state *S, mino_val *args, mino_env *env)
{
    mino_val *sock_val;
    mino_net_sock_t *sock;
    (void)env;

    if (!mino_is_cons(args) || mino_is_cons(args->as.cons.cdr)) {
        return prim_throw_classified(S, "eval/arity", "MAR001",
                                     "net-close requires one argument");
    }
    sock_val = args->as.cons.car;
    sock = net_sock_arg(S, sock_val, "net-close");
    if (sock == NULL) return NULL;
    if (!sock->closed) {
        net_close_fd(sock->fd);
        sock->closed = 1;
    }
    return mino_nil(S);
}

/* ---- install ---- */

static const mino_prim_def k_prims_net[] = {
    {"net-connect",  prim_net_connect,
     "Connects to host:port over TCP. Returns a socket handle. Opts "
      "map keys :connect-timeout :read-timeout :write-timeout "
      "(non-negative ms; 0 disables the timeout; defaults 10000 / "
      "30000 / 30000). :connect-timeout bounds the TCP connect only; "
      "DNS resolution has no deadline. Throws :net/dns / :net/connect "
      "on failure."},
    {"net-read",     prim_net_read,
     "Reads up to n bytes from a socket as soon as any arrive. Returns "
     "bytes (a short read is normal) or nil on clean EOF before the "
     "first byte. Throws :net/timeout on read timeout."},
    {"net-read-all", prim_net_read_all,
     "Reads from a socket until EOF. Returns bytes. Optional opts map "
     "key :max-bytes caps accumulation (default 16777216); exceeding "
     "it throws :net/overflow."},
    {"net-write",    prim_net_write,
     "Writes a string (UTF-8 bytes) or bytes to a socket. Returns the "
     "number of bytes written. Throws :net/timeout on write timeout."},
    {"net-close",    prim_net_close,
     "Closes a socket. Returns nil. Idempotent; dropped sockets are "
     "also closed by the garbage collector."},
};

const size_t k_prims_net_count =
    sizeof(k_prims_net) / sizeof(k_prims_net[0]);

void mino_install_net(mino_state *S, mino_env *env)
{
    mino_env *core_env = ns_env_ensure(S, "clojure.core");
    (void)env;
    prim_install_table_with_capability(S, core_env, "clojure.core",
                                       k_prims_net, k_prims_net_count,
                                       "net");
    S->caps_installed |= MINO_CAP_NET;
}
