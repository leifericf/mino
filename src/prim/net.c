/*
 * net.c -- TCP socket primitives: net-connect, net-read,
 * net-read-all, net-write, net-close; server side: net-listen,
 * net-accept, net-listener-port.
 *
 * Sockets are MINO_HANDLE values (tag "mino/net-socket") wrapping a
 * malloc'd descriptor record; listeners use their own tag
 * ("mino/net-listener") over the same shape. The handle finalizer
 * closes the fd when the value is collected or the state is torn
 * down, so a dropped handle never leaks; net-close is the explicit,
 * idempotent form and accepts either tag.
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
#  include <netinet/in.h>
#  include <netinet/tcp.h>
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
#define NET_LISTENER_TAG "mino/net-listener"

/* Default per-operation timeouts, milliseconds. */
#define NET_DEFAULT_CONNECT_TIMEOUT_MS 10000LL
#define NET_DEFAULT_READ_TIMEOUT_MS    30000LL
#define NET_DEFAULT_WRITE_TIMEOUT_MS   30000LL
#define NET_DEFAULT_ACCEPT_TIMEOUT_MS  60000LL

#define NET_DEFAULT_LISTEN_BACKLOG 16

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

typedef struct {
    mino_net_fd_t fd;
    int           closed;
} mino_net_listener_t;

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

static void net_listener_finalize(void *ptr, const char *tag)
{
    mino_net_listener_t *l = (mino_net_listener_t *)ptr;
    (void)tag;
    if (l == NULL) return;
    if (!l->closed) net_close_fd(l->fd);
    free(l);
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

/* Same contract as net_sock_arg, for the listener tag. */
static mino_net_listener_t *net_listener_arg(mino_state *S, mino_val *v,
                                             const char *who)
{
    if (v == NULL || mino_type_of(v) != MINO_HANDLE
        || v->as.handle.tag == NULL
        || strcmp(v->as.handle.tag, NET_LISTENER_TAG) != 0
        || v->as.handle.ptr == NULL) {
        char msg[160];
        snprintf(msg, sizeof(msg),
                 "%s: argument must be a net listener", who);
        prim_throw_classified(S, "eval/type", "MTY001", msg);
        return NULL;
    }
    return (mino_net_listener_t *)v->as.handle.ptr;
}

/* Read an optional non-negative ms timeout out of opts under key.
 * Falls back to def when absent. Throws eval/contract on a negative
 * or non-integer value. Returns 0 on success. Shared with tls.c. */
int net_opt_ms(mino_state *S, mino_val *opts, const char *key,
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

/* Ask the socket itself to suppress SIGPIPE where no per-send
 * MSG_NOSIGNAL flag exists (BSD / Apple; Windows has no SIGPIPE). */
static void net_suppress_sigpipe(mino_net_fd_t fd)
{
#if !defined(_WIN32) && !defined(MSG_NOSIGNAL)
#  ifdef SO_NOSIGPIPE
    int one = 1;
    (void)setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof(one));
#  else
    (void)fd;
#  endif
#else
    (void)fd;
#endif
}

/* Loopback-oriented accepted sockets: coalesce nothing, the peer is
 * on the same machine and per-segment latency dominates. Failure is
 * non-fatal (an optimization, not a correctness requirement). */
static void net_set_tcp_nodelay(mino_net_fd_t fd)
{
    int one = 1;
    (void)setsockopt(fd, IPPROTO_TCP, TCP_NODELAY,
                     (const char *)&one, sizeof(one));
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

/* Wait until fd is readable or the timeout passes. Returns 0
 * readable, 1 timeout, -1 error. Same poll-not-select reasoning and
 * yield discipline as net_wait_connected. */
static int net_wait_readable(mino_state *S, mino_net_fd_t fd,
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
            pfd.events  = POLLIN;
            pfd.revents = 0;
            rc = WSAPoll(&pfd, 1, poll_ms);
            mino_resume_lock(S, depth);
#else
            struct pollfd pfd;
            int depth = mino_yield_lock(S);
            pfd.fd      = fd;
            pfd.events  = POLLIN;
            pfd.revents = 0;
            rc = poll(&pfd, 1, poll_ms);
            mino_resume_lock(S, depth);
#endif
        }
        if (rc < 0) {
#ifdef _WIN32
            if (WSAGetLastError() == WSAEINTR) continue;
#else
            if (errno == EINTR) continue;
#endif
            return -1;
        }
        if (rc == 0) return 1;
        return 0;
    }
}

/* (net-connect host port [opts]) -> net-socket handle.
 * opts keys :connect-timeout / :read-timeout / :write-timeout (ms).
 * Cross-TU: tls.c calls it for its host+port convenience arity. */
mino_val *prim_net_connect(mino_state *S, mino_val *args,
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
        /* Precision bounds keep each diagnostic inside msg[] by
         * construction, which gcc's -Wformat-truncation can prove. */
#ifdef _WIN32
        snprintf(msg, sizeof(msg), "net-connect: cannot resolve host %.200s: "
                 "getaddrinfo error %d", host, gai_rc);
#else
        snprintf(msg, sizeof(msg),
                 "net-connect: cannot resolve host %.200s: %.60s",
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
                         "net-connect: connect to %.200s:%lld timed out "
                         "after %lld ms", host, port, connect_ms);
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
        snprintf(msg, sizeof(msg), "net-connect: cannot connect to %.180s:"
                 "%lld: %.60s", host, port,
                 detail[0] ? detail : "no usable addresses");
        return prim_throw_classified(S, "net/connect", "MNE002", msg);
    }

    net_suppress_sigpipe(fd);
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

/* ---- listen / accept ---- */

/* Socket + bind + listen for one candidate address. Returns 0 with
 * *fd_out set, -1 with the OS detail in err. SO_REUSEADDR is set
 * before bind on POSIX only: winsock's REUSEADDR also lets a second
 * socket hijack a port another socket still holds, so listeners stay
 * without it there. */
static int net_try_bind(mino_net_fd_t *fd_out, const struct sockaddr *sa,
                        socklen_t salen, int backlog, char *err,
                        size_t err_cap)
{
    mino_net_fd_t fd = socket(sa->sa_family, SOCK_STREAM, 0);
    if (fd == MINO_NET_INVALID_FD) {
        net_os_error(err, err_cap);
        return -1;
    }
#ifndef _WIN32
    {
        int one = 1;
        (void)setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    }
#endif
    if (bind(fd, sa, salen) != 0) {
        net_os_error(err, err_cap);
        net_close_fd(fd);
        return -1;
    }
    if (listen(fd, backlog) != 0) {
        net_os_error(err, err_cap);
        net_close_fd(fd);
        return -1;
    }
    *fd_out = fd;
    return 0;
}

/* (net-listen host port [opts]) -> listener handle. host is a bind
 * address (IP literal typical); "" or "*" binds the IPv4 wildcard.
 * port 0 lets the kernel choose; net-listener-port reads the choice
 * back. opts key :backlog (positive integer, default 16). */
static mino_val *prim_net_listen(mino_state *S, mino_val *args,
                                 mino_env *env)
{
    mino_val *host_val, *port_val, *opts = NULL, *hv;
    long long port, backlog;
    char host[512];
    char portstr[16];
    char detail[128];
    mino_net_fd_t fd = MINO_NET_INVALID_FD;
    mino_net_listener_t *rec;
    int wildcard;
    int depth;
    (void)env;

    if (!mino_is_cons(args)) {
        return prim_throw_classified(S, "eval/arity", "MAR001",
                                     "net-listen requires host and port");
    }
    host_val = args->as.cons.car;
    args = args->as.cons.cdr;
    if (!mino_is_cons(args)) {
        return prim_throw_classified(S, "eval/arity", "MAR001",
                                     "net-listen requires host and port");
    }
    port_val = args->as.cons.car;
    args = args->as.cons.cdr;
    if (mino_is_cons(args)) {
        opts = args->as.cons.car;
        if (opts != NULL && mino_type_of(opts) == MINO_MAP
            && mino_is_cons(args->as.cons.cdr)) {
            return prim_throw_classified(S, "eval/arity", "MAR001",
                                         "net-listen takes at most 3 "
                                         "arguments");
        }
        if (opts != NULL && mino_type_of(opts) != MINO_MAP
            && mino_type_of(opts) != MINO_NIL) {
            return prim_throw_classified(S, "eval/type", "MTY001",
                                         "net-listen: opts must be a map");
        }
    }
    if (host_val == NULL || mino_type_of(host_val) != MINO_STRING) {
        return prim_throw_classified(S, "eval/type", "MTY001",
                                     "net-listen: host must be a string");
    }
    if (host_val->as.s.len >= sizeof(host)) {
        return prim_throw_classified(S, "eval/contract", "MCT001",
                                     "net-listen: host is too long");
    }
    memcpy(host, host_val->as.s.data, host_val->as.s.len);
    host[host_val->as.s.len] = '\0';
    wildcard = host[0] == '\0' || strcmp(host, "*") == 0;
    if (!as_long(port_val, &port) || port < 0 || port > 65535) {
        return prim_throw_classified(S, "eval/contract", "MCT001",
                                     "net-listen: port must be an integer "
                                     "in 0..65535");
    }
    backlog = NET_DEFAULT_LISTEN_BACKLOG;
    if (opts != NULL && mino_type_of(opts) == MINO_MAP) {
        mino_val *v = map_get_val(opts, mino_keyword(S, "backlog"));
        if (v != NULL && mino_type_of(v) != MINO_NIL) {
            if (!as_long(v, &backlog) || backlog < 1) {
                return prim_throw_classified(S, "eval/contract", "MCT001",
                                             "net-listen: opts key "
                                             ":backlog must be a positive "
                                             "integer");
            }
        }
    }
    if (backlog > SOMAXCONN) backlog = SOMAXCONN;

#ifdef _WIN32
    if (net_winsock_init() != 0) {
        return prim_throw_classified(S, "net", "MNE004",
                                     "net-listen: WSAStartup failed");
    }
#endif
    /* Pre-flight the handle value before any descriptor exists, per
     * the net-connect ownership note. */
    hv = mino_handle_ex(S, NULL, NET_LISTENER_TAG, net_listener_finalize);
    gc_pin(hv);

    if (wildcard) {
        struct sockaddr_in any;
        memset(&any, 0, sizeof(any));
        any.sin_family      = AF_INET;
        any.sin_addr.s_addr = htonl(INADDR_ANY);
        any.sin_port        = htons((unsigned short)port);
        if (net_try_bind(&fd, (struct sockaddr *)&any, sizeof(any),
                         (int)backlog, detail, sizeof(detail)) != 0) {
            fd = MINO_NET_INVALID_FD;
        }
    } else {
        struct addrinfo hints, *res = NULL, *ai;
        int gai_rc;
        snprintf(portstr, sizeof(portstr), "%lld", port);
        memset(&hints, 0, sizeof(hints));
        hints.ai_flags   = AI_PASSIVE;
        hints.ai_family  = AF_UNSPEC;
        hints.ai_socktype = SOCK_STREAM;
        depth = mino_yield_lock(S);
        gai_rc = getaddrinfo(host, portstr, &hints, &res);
        mino_resume_lock(S, depth);
        if (gai_rc != 0) {
            char msg[300];
            gc_unpin(1);
#ifdef _WIN32
            snprintf(msg, sizeof(msg), "net-listen: cannot resolve bind "
                     "address %.190s: getaddrinfo error %d", host, gai_rc);
#else
            snprintf(msg, sizeof(msg), "net-listen: cannot resolve bind "
                     "address %.190s: %.60s", host, gai_strerror(gai_rc));
#endif
            return prim_throw_classified(S, "net/dns", "MNE001", msg);
        }
        detail[0] = '\0';
        for (ai = res; ai != NULL; ai = ai->ai_next) {
            if (net_try_bind(&fd, ai->ai_addr, ai->ai_addrlen,
                             (int)backlog, detail, sizeof(detail)) == 0)
                break;
            fd = MINO_NET_INVALID_FD;
        }
        freeaddrinfo(res);
    }
    if (fd == MINO_NET_INVALID_FD) {
        char msg[300];
        gc_unpin(1);
        snprintf(msg, sizeof(msg), "net-listen: cannot listen on %.120s:"
                 "%lld: %.60s", host, port,
                 detail[0] ? detail : "no usable addresses");
        return prim_throw_classified(S, "net", "MNE004", msg);
    }
    /* Non-blocking listener: accept is gated on a poll with a
     * deadline, and a connection stolen between poll and accept must
     * surface as EWOULDBLOCK so the wait can resume, never block. */
    if (net_set_nonblocking(fd, 1) != 0) {
        char msg[200];
        net_os_error(detail, sizeof(detail));
        net_close_fd(fd);
        gc_unpin(1);
        snprintf(msg, sizeof(msg), "net-listen: cannot set non-blocking "
                 "mode: %.100s", detail);
        return prim_throw_classified(S, "net", "MNE004", msg);
    }

    rec = (mino_net_listener_t *)malloc(sizeof(*rec));
    if (rec == NULL) {
        net_close_fd(fd);
        gc_unpin(1);
        return prim_throw_classified(S, "internal", "MIN001",
                                     "net-listen: out of memory");
    }
    rec->fd     = fd;
    rec->closed = 0;
    hv->as.handle.ptr = rec;
    gc_unpin(1);
    return hv;
}

/* (net-accept listener [opts]) -> net-socket handle for one accepted
 * connection. opts keys :accept-timeout bounding the wait for a
 * connection (default 60000) and :read-timeout / :write-timeout
 * preset on the accepted socket (defaults 30000 / 30000, matching
 * net-connect). */
static mino_val *prim_net_accept(mino_state *S, mino_val *args,
                                 mino_env *env)
{
    mino_val *l_val, *opts = NULL, *hv;
    mino_net_listener_t *listener;
    long long accept_ms, read_ms, write_ms, deadline;
    mino_net_fd_t cfd;
    mino_net_sock_t *sock;
    char detail[128];
    (void)env;

    if (!mino_is_cons(args)) {
        return prim_throw_classified(S, "eval/arity", "MAR001",
                                     "net-accept requires a listener");
    }
    l_val = args->as.cons.car;
    args = args->as.cons.cdr;
    if (mino_is_cons(args)) {
        opts = args->as.cons.car;
        if (mino_is_cons(args->as.cons.cdr)) {
            return prim_throw_classified(S, "eval/arity", "MAR001",
                                         "net-accept takes at most 2 "
                                         "arguments");
        }
        if (opts != NULL && mino_type_of(opts) != MINO_MAP
            && mino_type_of(opts) != MINO_NIL) {
            return prim_throw_classified(S, "eval/type", "MTY001",
                                         "net-accept: opts must be a map");
        }
    }
    listener = net_listener_arg(S, l_val, "net-accept");
    if (listener == NULL) return NULL;
    if (listener->closed) {
        return prim_throw_classified(S, "net", "MNE004",
                                     "net-accept: listener is closed");
    }
    if (net_opt_ms(S, opts, "accept-timeout",
                   NET_DEFAULT_ACCEPT_TIMEOUT_MS, &accept_ms) != 0)
        return NULL;
    if (net_opt_ms(S, opts, "read-timeout",
                   NET_DEFAULT_READ_TIMEOUT_MS, &read_ms) != 0)
        return NULL;
    if (net_opt_ms(S, opts, "write-timeout",
                   NET_DEFAULT_WRITE_TIMEOUT_MS, &write_ms) != 0)
        return NULL;

    /* Pre-flight the socket handle value (net-connect ownership
     * note); pin it and the listener across the yield windows of the
     * wait loop, since both are re-read after each resume. */
    hv = mino_handle_ex(S, NULL, NET_SOCK_TAG, net_sock_finalize);
    gc_pin(hv);
    gc_pin(l_val);

    if (accept_ms > NET_MAX_TIMEOUT_MS) accept_ms = NET_MAX_TIMEOUT_MS;
    deadline = mino_monotonic_ns() + accept_ms * 1000000LL;
    for (;;) {
        long long remaining_ms = (deadline - mino_monotonic_ns())
                                 / 1000000LL;
        int rc;
        if (remaining_ms < 0) remaining_ms = 0;
        rc = net_wait_readable(S, listener->fd, remaining_ms);
        if (rc == 1) {
            char msg[160];
            gc_unpin(2);
            snprintf(msg, sizeof(msg),
                     "net-accept: accept timed out after %lld ms",
                     accept_ms);
            return prim_throw_classified(S, "net/timeout", "MNE003", msg);
        }
        if (rc < 0) {
            char msg[200];
            net_os_error(detail, sizeof(detail));
            gc_unpin(2);
            snprintf(msg, sizeof(msg), "net-accept: accept failed: %.100s",
                     detail);
            return prim_throw_classified(S, "net/connect", "MNE002", msg);
        }
        cfd = accept(listener->fd, NULL, NULL);
        if (cfd == MINO_NET_INVALID_FD) {
#ifdef _WIN32
            int e = WSAGetLastError();
            if (e == WSAEINTR || e == WSAEWOULDBLOCK) continue;
#else
            /* EWOULDBLOCK: the readable event was consumed by a
             * racing acceptor; ECONNABORTED / EPROTO: the peer died
             * mid-handshake, the next pending connection may still be
             * there. All three resume the wait. */
            if (errno == EINTR || errno == EAGAIN
                || errno == EWOULDBLOCK || errno == ECONNABORTED
                || errno == EPROTO)
                continue;
#endif
            {
                char msg[200];
                net_os_error(detail, sizeof(detail));
                gc_unpin(2);
                snprintf(msg, sizeof(msg),
                         "net-accept: accept failed: %.100s", detail);
                return prim_throw_classified(S, "net/connect", "MNE002",
                                             msg);
            }
        }
        break;
    }
    /* Accepted sockets inherit the listener's non-blocking mode on
     * Windows; POSIX never inherits. Force blocking either way. */
    if (net_set_nonblocking(cfd, 0) != 0) {
        char msg[200];
        net_os_error(detail, sizeof(detail));
        net_close_fd(cfd);
        gc_unpin(2);
        snprintf(msg, sizeof(msg), "net-accept: cannot reset blocking "
                 "mode on accepted socket: %.100s", detail);
        return prim_throw_classified(S, "net/connect", "MNE002", msg);
    }
    net_suppress_sigpipe(cfd);
    net_set_tcp_nodelay(cfd);
    net_apply_io_timeouts(cfd, read_ms, write_ms);

    sock = (mino_net_sock_t *)malloc(sizeof(*sock));
    if (sock == NULL) {
        net_close_fd(cfd);
        gc_unpin(2);
        return prim_throw_classified(S, "internal", "MIN001",
                                     "net-accept: out of memory");
    }
    sock->fd              = cfd;
    sock->read_timeout_ms  = read_ms;
    sock->write_timeout_ms = write_ms;
    sock->closed          = 0;
    hv->as.handle.ptr = sock;
    gc_unpin(2);
    return hv;
}

/* (net-listener-port listener) -> bound port; how a caller learns the
 * kernel-chosen port after net-listen with port 0. */
static mino_val *prim_net_listener_port(mino_state *S, mino_val *args,
                                        mino_env *env)
{
    mino_val *l_val;
    mino_net_listener_t *listener;
    struct sockaddr_storage ss;
    unsigned short port;
    (void)env;

    if (!mino_is_cons(args) || mino_is_cons(args->as.cons.cdr)) {
        return prim_throw_classified(S, "eval/arity", "MAR001",
                                     "net-listener-port requires one "
                                     "argument");
    }
    l_val = args->as.cons.car;
    listener = net_listener_arg(S, l_val, "net-listener-port");
    if (listener == NULL) return NULL;
    if (listener->closed) {
        return prim_throw_classified(S, "net", "MNE004",
                                     "net-listener-port: listener is "
                                     "closed");
    }
    memset(&ss, 0, sizeof(ss));
    {
#ifdef _WIN32
        int ss_len = (int)sizeof(ss);
        if (getsockname(listener->fd, (struct sockaddr *)&ss, &ss_len)
            != 0) {
#else
        socklen_t ss_len = sizeof(ss);
        if (getsockname(listener->fd, (struct sockaddr *)&ss, &ss_len)
            != 0) {
#endif
            char detail[128];
            char msg[200];
            net_os_error(detail, sizeof(detail));
            snprintf(msg, sizeof(msg),
                     "net-listener-port: getsockname failed: %.60s",
                     detail);
            return prim_throw_classified(S, "net", "MNE004", msg);
        }
    }
    if (ss.ss_family == AF_INET6) {
        struct sockaddr_in6 *a6 = (struct sockaddr_in6 *)&ss;
        port = ntohs(a6->sin6_port);
    } else {
        struct sockaddr_in *a4 = (struct sockaddr_in *)&ss;
        port = ntohs(a4->sin_port);
    }
    return mino_int(S, (long long)port);
}

/* ---- read ---- */

/* Single recv of up to n bytes into buf. Returns:
 *   1  got 1..*got bytes
 *   0  clean EOF before any byte
 *  -1  error (kind/code/msg filled) */
static int net_recv_fd(mino_state *S, mino_net_fd_t fd, long long read_ms,
                       unsigned char *buf, size_t n, size_t *got,
                       const char **kind, const char **code, char *msg,
                       size_t msg_cap)
{
    for (;;) {
        int rc;
        int depth = mino_yield_lock(S);
#ifdef _WIN32
        rc = recv(fd, (char *)buf, (int)n, 0);
        mino_resume_lock(S, depth);
        if (rc == SOCKET_ERROR) {
            int e = WSAGetLastError();
            if (e == WSAEINTR) continue;
            if (e == WSAETIMEDOUT) {
                snprintf(msg, msg_cap,
                         "read timed out after %lld ms", read_ms);
                *kind = "net/timeout";
                *code = "MNE003";
                return -1;
            }
            snprintf(msg, msg_cap, "read failed: winsock error %d", e);
            *kind = "net";
            *code = "MNE004";
            return -1;
        }
#else
        ssize_t r;
        rc = 0;
        r = recv(fd, buf, n, 0);
        mino_resume_lock(S, depth);
        if (r < 0) {
            if (errno == EINTR) continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                /* Blocking fd + SO_RCVTIMEO: EAGAIN marks expiry. */
                snprintf(msg, msg_cap,
                         "read timed out after %lld ms", read_ms);
                *kind = "net/timeout";
                *code = "MNE003";
                return -1;
            }
            snprintf(msg, msg_cap, "read failed: %s", strerror(errno));
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

/* Thin socket-record wrapper used by the net prims. */
static int net_recv_once(mino_state *S, mino_net_sock_t *sock,
                         unsigned char *buf, size_t n, size_t *got,
                         const char **kind, const char **code, char *msg,
                         size_t msg_cap)
{
    return net_recv_fd(S, sock->fd, sock->read_timeout_ms, buf, n, got,
                       kind, code, msg, msg_cap);
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

/* Send the whole buffer, looping over partial sends. Returns 0 on
 * success, -1 on error (kind/code/msg filled). */
static int net_send_all_fd(mino_state *S, mino_net_fd_t fd,
                           const unsigned char *data, size_t len,
                           long long write_ms, const char **kind,
                           const char **code, char *msg, size_t msg_cap)
{
    size_t sent = 0;
    while (sent < len) {
        int rc;
        int depth = mino_yield_lock(S);
#ifdef _WIN32
        rc = send(fd, (const char *)data + sent, (int)(len - sent), 0);
        mino_resume_lock(S, depth);
        if (rc == SOCKET_ERROR) {
            int e = WSAGetLastError();
            if (e == WSAEINTR) continue;
            if (e == WSAETIMEDOUT) {
                snprintf(msg, msg_cap, "write timed out after %lld ms",
                         write_ms);
                *kind = "net/timeout";
                *code = "MNE003";
                return -1;
            }
            snprintf(msg, msg_cap, "write failed: winsock error %d", e);
            *kind = "net";
            *code = "MNE004";
            return -1;
        }
#else
        ssize_t r;
        rc = 0;
        r = send(fd, data + sent, len - sent, NET_SEND_FLAGS);
        mino_resume_lock(S, depth);
        if (r < 0) {
            if (errno == EINTR) continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                snprintf(msg, msg_cap, "write timed out after %lld ms",
                         write_ms);
                *kind = "net/timeout";
                *code = "MNE003";
                return -1;
            }
            snprintf(msg, msg_cap, "write failed: %s", strerror(errno));
            *kind = "net";
            *code = "MNE004";
            return -1;
        }
        rc = (int)r;
#endif
        sent += (size_t)rc;
    }
    return 0;
}

/* (net-write sock data) -> byte count written. data is a string
 * (UTF-8 bytes) or bytes. Blocks until every byte is written, the
 * write timeout expires, or the connection fails. */
static mino_val *prim_net_write(mino_state *S, mino_val *args, mino_env *env)
{
    mino_val *sock_val, *data_val;
    mino_net_sock_t *sock;
    const unsigned char *data;
    size_t len;
    const char *kind = "net", *code = "MNE004";
    char msg[200];
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

    /* data points into a GC string or bytes while the send loop
     * yields the state lock; pin the value so a concurrent
     * collection cannot sweep the payload mid-send. */
    gc_pin(data_val);
    if (net_send_all_fd(S, sock->fd, data, len, sock->write_timeout_ms,
                        &kind, &code, msg, sizeof(msg)) != 0) {
        gc_unpin(1);
        return prim_throw_classified(S, kind, code, msg);
    }
    gc_unpin(1);
    return mino_int(S, (long long)len);
}

/* ---- close ---- */

/* (net-close sock-or-listener) -> nil. Idempotent for both net
 * handle tags; the record is marked so the handle finalizer never
 * double-closes the fd. */
static mino_val *prim_net_close(mino_state *S, mino_val *args, mino_env *env)
{
    mino_val *v;
    (void)env;

    if (!mino_is_cons(args) || mino_is_cons(args->as.cons.cdr)) {
        return prim_throw_classified(S, "eval/arity", "MAR001",
                                     "net-close requires one argument");
    }
    v = args->as.cons.car;
    if (v != NULL && mino_type_of(v) == MINO_HANDLE
        && v->as.handle.tag != NULL && v->as.handle.ptr != NULL) {
        if (strcmp(v->as.handle.tag, NET_SOCK_TAG) == 0) {
            mino_net_sock_t *sock = (mino_net_sock_t *)v->as.handle.ptr;
            if (!sock->closed) {
                net_close_fd(sock->fd);
                sock->closed = 1;
            }
            return mino_nil(S);
        }
        if (strcmp(v->as.handle.tag, NET_LISTENER_TAG) == 0) {
            mino_net_listener_t *l;
            l = (mino_net_listener_t *)v->as.handle.ptr;
            if (!l->closed) {
                net_close_fd(l->fd);
                l->closed = 1;
            }
            return mino_nil(S);
        }
    }
    return prim_throw_classified(S, "eval/type", "MTY001",
                                 "net-close: argument must be a net "
                                 "socket or listener");
}

/* ---- fd bridge for the TLS layer ---- */

/* prim/tls.c drives a connected descriptor through these: the TLS
 * engine pumps raw records against the socket with the same timeout
 * and error classification as the net prims. Descriptors cross the
 * boundary widened to uintptr_t (int on POSIX, SOCKET on Windows). */

int mino_net_adopt(mino_state *S, mino_val *v, uintptr_t *fd_out,
                   long long *read_ms_out, long long *write_ms_out)
{
    mino_net_sock_t *sock = net_sock_arg(S, v, "tls-connect");
    if (sock == NULL) return -1;
    if (sock->closed) {
        prim_throw_classified(S, "tls", "MTL004",
                              "tls-connect: underlying socket is "
                              "closed");
        return -1;
    }
    *fd_out         = (uintptr_t)sock->fd;
    *read_ms_out    = sock->read_timeout_ms;
    *write_ms_out   = sock->write_timeout_ms;
    /* Ownership of the descriptor passes to the caller; the net
     * handle is marked closed so its finalizer will not close it. */
    sock->closed = 1;
    return 0;
}

void mino_net_apply_timeouts_raw(uintptr_t fd, long long read_ms,
                                 long long write_ms)
{
    net_apply_io_timeouts((mino_net_fd_t)fd, read_ms, write_ms);
}

void mino_net_close_raw(uintptr_t fd)
{
    net_close_fd((mino_net_fd_t)fd);
}

int mino_net_recv_raw(mino_state *S, uintptr_t fd, unsigned char *buf,
                      size_t n, size_t *got, long long read_ms,
                      const char **kind, const char **code, char *msg,
                      size_t msg_cap)
{
    return net_recv_fd(S, (mino_net_fd_t)fd, read_ms, buf, n, got,
                       kind, code, msg, msg_cap);
}

int mino_net_send_raw(mino_state *S, uintptr_t fd, const unsigned char *buf,
                      size_t n, long long write_ms, const char **kind,
                      const char **code, char *msg, size_t msg_cap)
{
    return net_send_all_fd(S, (mino_net_fd_t)fd, buf, n, write_ms,
                           kind, code, msg, msg_cap);
}

/* ---- handle bridge for the keep-alive pool (prim/pool.c) ---- */

const char *mino_net_sock_tag(void)
{
    return NET_SOCK_TAG;
}

/* Borrow the descriptor of a live net-socket handle for a zero-timeout
 * liveness poll. 1 with *fd_out set for an open socket, 0 for any
 * other value (wrong tag, closed, NULL). */
int mino_net_handle_fd(mino_val *v, uintptr_t *fd_out)
{
    mino_net_sock_t *s;
    if (v == NULL || mino_type_of(v) != MINO_HANDLE
        || v->as.handle.tag == NULL
        || strcmp(v->as.handle.tag, NET_SOCK_TAG) != 0
        || v->as.handle.ptr == NULL) {
        return 0;
    }
    s = (mino_net_sock_t *)v->as.handle.ptr;
    if (s->closed) return 0;
    *fd_out = (uintptr_t)s->fd;
    return 1;
}

/* Idempotent close of a net-socket handle: closes the descriptor and
 * marks the record so the handle finalizer never closes it again. */
void mino_net_handle_close(mino_val *v)
{
    mino_net_sock_t *s;
    if (v == NULL || mino_type_of(v) != MINO_HANDLE
        || v->as.handle.tag == NULL
        || strcmp(v->as.handle.tag, NET_SOCK_TAG) != 0
        || v->as.handle.ptr == NULL) {
        return;
    }
    s = (mino_net_sock_t *)v->as.handle.ptr;
    if (!s->closed) {
        net_close_fd(s->fd);
        s->closed = 1;
    }
}

/* ---- install ---- */

static const mino_prim_def k_prims_net[] = {    {"net-connect",  prim_net_connect,
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
    {"net-listen",  prim_net_listen,
     "Binds a TCP listener and returns a listener handle. host is a "
     "bind address (IP literal typical); \"\" or \"*\" binds the IPv4 "
     "wildcard. port 0 asks the kernel to choose (learn it with "
     "net-listener-port). Opts map key :backlog (positive integer, "
     "default 16). SO_REUSEADDR is set before bind on POSIX only; "
     "winsock's REUSEADDR would let another socket hijack the port. "
     "Throws :net/dns when the bind address cannot resolve, :net "
     "otherwise."},
    {"net-accept",  prim_net_accept,
     "Waits for one inbound connection on a listener and returns it "
     "as a socket handle (the type net-connect returns; net-read / "
     "net-write / net-close work on it). Opts map keys :accept-timeout "
     "(non-negative ms bounding the wait, default 60000) plus "
     ":read-timeout / :write-timeout preset on the accepted socket "
     "(defaults 30000 / 30000, matching net-connect). Accepted "
     "sockets set TCP_NODELAY. Throws :net/timeout when the accept "
     "deadline passes, :net/connect when accept itself fails."},
    {"net-listener-port", prim_net_listener_port,
     "Returns the port a listener is bound to; how a caller learns "
     "the kernel-chosen port after net-listen with port 0. Throws "
     ":net on a closed listener."},
    {"net-close",    prim_net_close,
     "Closes a socket or listener. Returns nil. Idempotent; dropped "
     "handles are also closed by the garbage collector."},
};

static const size_t k_prims_net_count =
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
