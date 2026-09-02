/*
 * udp.c -- UDP datagram primitives and DNS lookup: udp-socket,
 * udp-socket-port, udp-send, udp-recv, udp-close, plus dns-lookup.
 *
 * A udp socket is a MINO_HANDLE value (tag "mino/udp-socket") wrapping
 * a malloc'd descriptor record. The handle finalizer closes the fd
 * when the value is collected or the state is torn down, so a dropped
 * handle never leaks; udp-close is the explicit, idempotent form.
 *
 * Blocking recv yields the state lock for its duration (poll then
 * recvfrom) so worker threads keep progressing, mirroring net.c.
 * getaddrinfo in udp-send and dns-lookup resolves on the calling
 * thread across a yield window, like net-connect.
 *
 * Trust model mirrors net.c: the script author is the trust boundary.
 * Argument shapes are validated; destinations are not policed. An
 * embedder that wants no datagram or resolver reach refuses to install
 * MINO_CAP_UDP (it is not forced on an embed that declines it, though
 * it rides MINO_CAP_DEFAULT for the standalone binary).
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
#  include <arpa/inet.h>
#endif

#include <string.h>
#include <stdlib.h>
#include <limits.h>

#ifdef _WIN32
typedef SOCKET mino_udp_fd_t;
#define MINO_UDP_INVALID_FD INVALID_SOCKET
#else
typedef int mino_udp_fd_t;
#define MINO_UDP_INVALID_FD (-1)
#endif

#define UDP_SOCK_TAG "mino/udp-socket"

/* Default per-socket recv timeout, milliseconds; overridable via the
 * :read-timeout socket opt and the per-call :read-timeout opt. */
#define UDP_DEFAULT_READ_TIMEOUT_MS 30000LL

/* Largest datagram we ever pull in one recv: the practical IPv4 UDP
 * payload ceiling. A per-call :max-bytes shrinks the buffer to expose
 * truncation deterministically in a test. */
#define UDP_MAX_DATAGRAM 65535

/* Ceiling before a caller ms timeout scales to nanoseconds; matches
 * net.c so the ms->ns multiply cannot overflow a signed long long. */
#define UDP_MAX_TIMEOUT_MS 2147483647LL

typedef struct {
    mino_udp_fd_t fd;
    long long     read_timeout_ms;
    int           closed;
} mino_udp_sock_t;

/* ---- platform shims (mirror net.c) ---- */

#ifdef _WIN32
static BOOL CALLBACK udp_winsock_start(PINIT_ONCE once, PVOID param,
                                       PVOID *ctx)
{
    WSADATA data;
    (void)once; (void)param; (void)ctx;
    return WSAStartup(MAKEWORD(2, 2), &data) == 0 ? TRUE : FALSE;
}

static int udp_winsock_init(void)
{
    static INIT_ONCE once = INIT_ONCE_STATIC_INIT;
    if (!InitOnceExecuteOnce(&once, udp_winsock_start, NULL, NULL))
        return -1;
    return 0;
}
#endif

static void udp_close_fd(mino_udp_fd_t fd)
{
#ifdef _WIN32
    closesocket(fd);
#else
    close(fd);
#endif
}

/* OS detail string for diagnostics. buf must be >= 96 bytes. */
static void udp_os_error(char *buf, size_t cap)
{
#ifdef _WIN32
    snprintf(buf, cap, "winsock error %d", (int)WSAGetLastError());
#else
    snprintf(buf, cap, "%s", strerror(errno));
#endif
}

static void udp_sock_finalize(void *ptr, const char *tag)
{
    mino_udp_sock_t *s = (mino_udp_sock_t *)ptr;
    (void)tag;
    if (s == NULL) return;
    if (!s->closed) udp_close_fd(s->fd);
    free(s);
}

/* Extract the socket record from a udp-socket handle. Throws eval/type
 * for a non-socket value; returns NULL only when the throw unwound to
 * a host diagnostic rather than a catch frame. */
static mino_udp_sock_t *udp_sock_arg(mino_state *S, mino_val *v,
                                     const char *who)
{
    if (v == NULL || mino_type_of(v) != MINO_HANDLE
        || v->as.handle.tag == NULL
        || strcmp(v->as.handle.tag, UDP_SOCK_TAG) != 0
        || v->as.handle.ptr == NULL) {
        char msg[160];
        snprintf(msg, sizeof(msg), "%s: argument must be a udp socket",
                 who);
        prim_throw_classified(S, "eval/type", "MTY001", msg);
        return NULL;
    }
    return (mino_udp_sock_t *)v->as.handle.ptr;
}

/* Copy a MINO_STRING host argument into buf (NUL-terminated). Returns 0
 * on success, -1 after firing a throw. */
static int udp_host_arg(mino_state *S, mino_val *v, const char *who,
                        char *buf, size_t cap)
{
    char msg[200];
    if (v == NULL || mino_type_of(v) != MINO_STRING) {
        snprintf(msg, sizeof(msg), "%s: host must be a string", who);
        prim_throw_classified(S, "eval/type", "MTY001", msg);
        return -1;
    }
    if (v->as.s.len >= cap) {
        snprintf(msg, sizeof(msg), "%s: host is too long", who);
        prim_throw_classified(S, "eval/contract", "MCT001", msg);
        return -1;
    }
    memcpy(buf, v->as.s.data, v->as.s.len);
    buf[v->as.s.len] = '\0';
    return 0;
}

/* Read a port (0..65535) from a value. Returns 0 ok, -1 after a throw. */
static int udp_port_arg(mino_state *S, mino_val *v, const char *who,
                        long long *out)
{
    char msg[200];
    if (!as_long(v, out) || *out < 0 || *out > 65535) {
        snprintf(msg, sizeof(msg),
                 "%s: port must be an integer in 0..65535", who);
        prim_throw_classified(S, "eval/contract", "MCT001", msg);
        return -1;
    }
    return 0;
}

/* Apply the recv timeout to the socket fd via SO_RCVTIMEO. */
static void udp_apply_read_timeout(mino_udp_fd_t fd, long long read_ms)
{
    if (read_ms <= 0) return;
#ifdef _WIN32
    {
        DWORD ms = (DWORD)read_ms;
        (void)setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO,
                         (const char *)&ms, sizeof(ms));
    }
#else
    {
        struct timeval tv;
        tv.tv_sec  = (time_t)(read_ms / 1000);
        tv.tv_usec = (long)((read_ms % 1000) * 1000);
        (void)setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    }
#endif
}

/* Format a sockaddr's numeric IP into buf. Returns 0 ok, -1 unknown
 * family. Numeric only (NI_NUMERICHOST) so no reverse lookup ever
 * fires from a recv path. */
static int udp_addr_ip(const struct sockaddr *sa, socklen_t salen,
                       char *buf, size_t cap)
{
    if (getnameinfo(sa, salen, buf, (unsigned)cap, NULL, 0,
                    NI_NUMERICHOST) != 0) {
        return -1;
    }
    return 0;
}

/* Read the numeric port out of a sockaddr (v4 or v6). */
static unsigned short udp_addr_port(const struct sockaddr_storage *ss)
{
    if (ss->ss_family == AF_INET6) {
        const struct sockaddr_in6 *a6 = (const struct sockaddr_in6 *)ss;
        return ntohs(a6->sin6_port);
    }
    {
        const struct sockaddr_in *a4 = (const struct sockaddr_in *)ss;
        return ntohs(a4->sin_port);
    }
}

/* ---- udp-socket ---- */

/* (udp-socket [opts]) -> udp-socket handle. opts keys :host (bind
 * address, default 127.0.0.1), :port (0 or omitted => kernel-chosen
 * ephemeral), :read-timeout (ms). */
static mino_val *prim_udp_socket(mino_state *S, mino_val *args,
                                 mino_env *env)
{
    mino_val *opts = NULL, *hv, *hostv;
    char host[512];
    long long port = 0, read_ms = UDP_DEFAULT_READ_TIMEOUT_MS;
    struct addrinfo hints, *res = NULL, *ai;
    char portstr[16];
    mino_udp_fd_t fd = MINO_UDP_INVALID_FD;
    mino_udp_sock_t *sock;
    char detail[128];
    int gai_rc, depth;
    (void)env;

    strcpy(host, "127.0.0.1");
    if (mino_is_cons(args)) {
        opts = args->as.cons.car;
        if (mino_is_cons(args->as.cons.cdr)) {
            return prim_throw_classified(S, "eval/arity", "MAR001",
                                         "udp-socket takes at most 1 "
                                         "argument");
        }
        if (opts != NULL && mino_type_of(opts) != MINO_MAP
            && mino_type_of(opts) != MINO_NIL) {
            return prim_throw_classified(S, "eval/type", "MTY001",
                                         "udp-socket: opts must be a map");
        }
    }
    if (opts != NULL && mino_type_of(opts) == MINO_MAP) {
        hostv = map_get_val(opts, mino_keyword(S, "host"));
        if (hostv != NULL && mino_type_of(hostv) != MINO_NIL) {
            if (udp_host_arg(S, hostv, "udp-socket", host, sizeof(host))
                != 0)
                return NULL;
        }
        {
            mino_val *pv = map_get_val(opts, mino_keyword(S, "port"));
            if (pv != NULL && mino_type_of(pv) != MINO_NIL
                && udp_port_arg(S, pv, "udp-socket", &port) != 0)
                return NULL;
        }
    }
    if (net_opt_ms(S, opts, "read-timeout",
                   UDP_DEFAULT_READ_TIMEOUT_MS, &read_ms) != 0)
        return NULL;

#ifdef _WIN32
    if (udp_winsock_init() != 0) {
        return prim_throw_classified(S, "net", "MNE004",
                                     "udp-socket: WSAStartup failed");
    }
#endif
    /* Pre-flight the handle value before any fd exists: its allocation
     * can throw for OOM, and from here every failure path holds an fd
     * or a malloc'd record a late throw would strand. */
    hv = mino_handle_ex(S, NULL, UDP_SOCK_TAG, udp_sock_finalize);
    gc_pin(hv);

    snprintf(portstr, sizeof(portstr), "%lld", port);
    memset(&hints, 0, sizeof(hints));
    hints.ai_family   = AF_UNSPEC;
    hints.ai_socktype = SOCK_DGRAM;
    hints.ai_flags    = AI_PASSIVE | AI_NUMERICHOST;
    depth = mino_yield_lock(S);
    gai_rc = getaddrinfo(host, portstr, &hints, &res);
    mino_resume_lock(S, depth);
    if (gai_rc != 0) {
        char msg[300];
        gc_unpin(1);
#ifdef _WIN32
        snprintf(msg, sizeof(msg), "udp-socket: cannot resolve bind "
                 "address %.190s: getaddrinfo error %d", host, gai_rc);
#else
        snprintf(msg, sizeof(msg), "udp-socket: cannot resolve bind "
                 "address %.190s: %.60s", host, gai_strerror(gai_rc));
#endif
        return prim_throw_classified(S, "net/dns", "MNE001", msg);
    }

    detail[0] = '\0';
    for (ai = res; ai != NULL; ai = ai->ai_next) {
        mino_udp_fd_t try_fd = socket(ai->ai_family, ai->ai_socktype,
                                      ai->ai_protocol);
        if (try_fd == MINO_UDP_INVALID_FD) {
            udp_os_error(detail, sizeof(detail));
            continue;
        }
        if (bind(try_fd, ai->ai_addr, ai->ai_addrlen) != 0) {
            udp_os_error(detail, sizeof(detail));
            udp_close_fd(try_fd);
            continue;
        }
        fd = try_fd;
        break;
    }
    freeaddrinfo(res);
    if (fd == MINO_UDP_INVALID_FD) {
        char msg[300];
        gc_unpin(1);
        snprintf(msg, sizeof(msg), "udp-socket: cannot bind %.140s:%lld: "
                 "%.60s", host, port,
                 detail[0] ? detail : "no usable addresses");
        return prim_throw_classified(S, "net", "MNE004", msg);
    }

    udp_apply_read_timeout(fd, read_ms);

    sock = (mino_udp_sock_t *)malloc(sizeof(*sock));
    if (sock == NULL) {
        udp_close_fd(fd);
        gc_unpin(1);
        return prim_throw_classified(S, "internal", "MIN001",
                                     "udp-socket: out of memory");
    }
    sock->fd              = fd;
    sock->read_timeout_ms = read_ms;
    sock->closed          = 0;
    hv->as.handle.ptr = sock;
    gc_unpin(1);
    return hv;
}

/* (udp-socket-port sock) -> the bound port; how a caller learns the
 * kernel-chosen port after an ephemeral bind. */
static mino_val *prim_udp_socket_port(mino_state *S, mino_val *args,
                                      mino_env *env)
{
    mino_val *sock_val;
    mino_udp_sock_t *sock;
    struct sockaddr_storage ss;
    (void)env;

    if (!mino_is_cons(args) || mino_is_cons(args->as.cons.cdr)) {
        return prim_throw_classified(S, "eval/arity", "MAR001",
                                     "udp-socket-port requires one "
                                     "argument");
    }
    sock_val = args->as.cons.car;
    sock = udp_sock_arg(S, sock_val, "udp-socket-port");
    if (sock == NULL) return NULL;
    if (sock->closed) {
        return prim_throw_classified(S, "net", "MNE004",
                                     "udp-socket-port: socket is closed");
    }
    memset(&ss, 0, sizeof(ss));
    {
#ifdef _WIN32
        int ss_len = (int)sizeof(ss);
#else
        socklen_t ss_len = sizeof(ss);
#endif
        if (getsockname(sock->fd, (struct sockaddr *)&ss, &ss_len) != 0) {
            char detail[128], msg[200];
            udp_os_error(detail, sizeof(detail));
            snprintf(msg, sizeof(msg),
                     "udp-socket-port: getsockname failed: %.60s",
                     detail);
            return prim_throw_classified(S, "net", "MNE004", msg);
        }
    }
    return mino_int(S, (long long)udp_addr_port(&ss));
}

/* ---- udp-send ---- */

/* (udp-send sock host port data) -> byte count sent. data is a string
 * (UTF-8 bytes) or bytes. */
static mino_val *prim_udp_send(mino_state *S, mino_val *args, mino_env *env)
{
    mino_val *sock_val, *host_val, *port_val, *data_val;
    mino_udp_sock_t *sock;
    char host[512], portstr[16];
    long long port;
    const unsigned char *data;
    size_t len;
    struct addrinfo hints, *res = NULL, *ai;
    int gai_rc, depth, sent_ok = 0;
    char detail[128];
    (void)env;

    if (!mino_is_cons(args) || !mino_is_cons(args->as.cons.cdr)
        || !mino_is_cons(args->as.cons.cdr->as.cons.cdr)
        || !mino_is_cons(args->as.cons.cdr->as.cons.cdr->as.cons.cdr)
        || mino_is_cons(args->as.cons.cdr->as.cons.cdr
                        ->as.cons.cdr->as.cons.cdr)) {
        return prim_throw_classified(S, "eval/arity", "MAR001",
                                     "udp-send requires a socket, host, "
                                     "port, and data");
    }
    sock_val = args->as.cons.car;
    host_val = args->as.cons.cdr->as.cons.car;
    port_val = args->as.cons.cdr->as.cons.cdr->as.cons.car;
    data_val = args->as.cons.cdr->as.cons.cdr->as.cons.cdr->as.cons.car;

    sock = udp_sock_arg(S, sock_val, "udp-send");
    if (sock == NULL) return NULL;
    if (sock->closed) {
        return prim_throw_classified(S, "net", "MNE004",
                                     "udp-send: socket is closed");
    }
    if (udp_host_arg(S, host_val, "udp-send", host, sizeof(host)) != 0)
        return NULL;
    if (udp_port_arg(S, port_val, "udp-send", &port) != 0)
        return NULL;
    if (port < 1) {
        return prim_throw_classified(S, "eval/contract", "MCT001",
                                     "udp-send: port must be in 1..65535");
    }
    if (data_val != NULL && mino_type_of(data_val) == MINO_STRING) {
        data = (const unsigned char *)data_val->as.s.data;
        len  = data_val->as.s.len;
    } else if (data_val != NULL && mino_type_of(data_val) == MINO_BYTES) {
        data = mino_bytes_data(data_val);
        len  = mino_bytes_len(data_val);
    } else {
        return prim_throw_classified(S, "eval/type", "MTY001",
                                     "udp-send: data must be a string or "
                                     "bytes");
    }

#ifdef _WIN32
    if ((unsigned long long)len > (unsigned long long)INT_MAX) {
        return prim_throw_classified(S, "eval/contract", "MCT001",
                                     "udp-send: datagram too large");
    }
#endif
    snprintf(portstr, sizeof(portstr), "%lld", port);
    memset(&hints, 0, sizeof(hints));
    hints.ai_family   = AF_UNSPEC;
    hints.ai_socktype = SOCK_DGRAM;
    /* Pin the data value across the resolver + sendto yield windows so
     * a concurrent collection cannot sweep the payload mid-send. */
    gc_pin(data_val);
    depth = mino_yield_lock(S);
    gai_rc = getaddrinfo(host, portstr, &hints, &res);
    mino_resume_lock(S, depth);
    if (gai_rc != 0) {
        char msg[300];
        gc_unpin(1);
#ifdef _WIN32
        snprintf(msg, sizeof(msg), "udp-send: cannot resolve host %.190s: "
                 "getaddrinfo error %d", host, gai_rc);
#else
        snprintf(msg, sizeof(msg), "udp-send: cannot resolve host %.190s: "
                 "%.60s", host, gai_strerror(gai_rc));
#endif
        return prim_throw_classified(S, "net/dns", "MNE001", msg);
    }

    detail[0] = '\0';
    for (ai = res; ai != NULL; ai = ai->ai_next) {
        long long r;
        int d = mino_yield_lock(S);
#ifdef _WIN32
        r = sendto(sock->fd, (const char *)data, (int)len, 0,
                   ai->ai_addr, (int)ai->ai_addrlen);
        mino_resume_lock(S, d);
        if (r == SOCKET_ERROR) { udp_os_error(detail, sizeof(detail));
                                 continue; }
#else
        r = sendto(sock->fd, data, len, 0, ai->ai_addr, ai->ai_addrlen);
        mino_resume_lock(S, d);
        if (r < 0) { udp_os_error(detail, sizeof(detail)); continue; }
#endif
        sent_ok = 1;
        len = (size_t)r;
        break;
    }
    freeaddrinfo(res);
    gc_unpin(1);
    if (!sent_ok) {
        char msg[300];
        snprintf(msg, sizeof(msg), "udp-send: cannot send to %.140s:%lld: "
                 "%.60s", host, port,
                 detail[0] ? detail : "no usable addresses");
        return prim_throw_classified(S, "net", "MNE004", msg);
    }
    return mino_int(S, (long long)len);
}

/* ---- udp-recv ---- */

/* Wait until fd is readable or the timeout passes. Returns 0 readable,
 * 1 timeout, -1 error. Same poll-not-select reasoning and yield
 * discipline as net.c. */
static int udp_wait_readable(mino_state *S, mino_udp_fd_t fd,
                             long long timeout_ms)
{
    long long deadline;
    if (timeout_ms > UDP_MAX_TIMEOUT_MS) timeout_ms = UDP_MAX_TIMEOUT_MS;
    deadline = mino_monotonic_ns() + timeout_ms * 1000000LL;
    for (;;) {
        long long now = mino_monotonic_ns();
        long long remaining_ms;
        int poll_ms, rc;
        if (now >= deadline) return 1;
        remaining_ms = (deadline - now) / 1000000LL;
        poll_ms = remaining_ms > (long long)INT_MAX
                    ? INT_MAX : (int)remaining_ms;
        {
#ifdef _WIN32
            WSAPOLLFD pfd;
            int depth = mino_yield_lock(S);
            pfd.fd = fd; pfd.events = POLLIN; pfd.revents = 0;
            rc = WSAPoll(&pfd, 1, poll_ms);
            mino_resume_lock(S, depth);
#else
            struct pollfd pfd;
            int depth = mino_yield_lock(S);
            pfd.fd = fd; pfd.events = POLLIN; pfd.revents = 0;
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

/* (udp-recv sock [opts]) -> {:data bytes :address ip :port n
 * :truncated? bool}. opts key :read-timeout (ms) overrides the socket
 * default; :max-bytes shrinks the recv buffer (default 65535) to force
 * the truncation policy in a test. A datagram longer than the buffer
 * fills it, sets :truncated? true, and drops the excess (the kernel
 * discards the tail of the datagram), never queuing it for the next
 * read. */
static mino_val *prim_udp_recv(mino_state *S, mino_val *args, mino_env *env)
{
    mino_val *sock_val, *opts = NULL, *result;
    mino_udp_sock_t *sock;
    long long read_ms, max_bytes = UDP_MAX_DATAGRAM;
    unsigned char *buf;
    size_t cap;
    struct sockaddr_storage ss;
    char ip[128];
    int rc, truncated = 0;
    long long got = 0;
    (void)env;

    if (!mino_is_cons(args)) {
        return prim_throw_classified(S, "eval/arity", "MAR001",
                                     "udp-recv requires a socket");
    }
    sock_val = args->as.cons.car;
    args = args->as.cons.cdr;
    if (mino_is_cons(args)) {
        opts = args->as.cons.car;
        if (mino_is_cons(args->as.cons.cdr)) {
            return prim_throw_classified(S, "eval/arity", "MAR001",
                                         "udp-recv takes at most 2 "
                                         "arguments");
        }
        if (opts != NULL && mino_type_of(opts) != MINO_MAP
            && mino_type_of(opts) != MINO_NIL) {
            return prim_throw_classified(S, "eval/type", "MTY001",
                                         "udp-recv: opts must be a map");
        }
    }
    sock = udp_sock_arg(S, sock_val, "udp-recv");
    if (sock == NULL) return NULL;
    if (sock->closed) {
        return prim_throw_classified(S, "net", "MNE004",
                                     "udp-recv: socket is closed");
    }
    read_ms = sock->read_timeout_ms;
    if (net_opt_ms(S, opts, "read-timeout", read_ms, &read_ms) != 0)
        return NULL;
    if (net_opt_ms(S, opts, "max-bytes", UDP_MAX_DATAGRAM, &max_bytes)
        != 0)
        return NULL;
    if (max_bytes < 1) max_bytes = 1;
    if (max_bytes > UDP_MAX_DATAGRAM) max_bytes = UDP_MAX_DATAGRAM;
    cap = (size_t)max_bytes;

    buf = (unsigned char *)malloc(cap);
    if (buf == NULL) {
        return prim_throw_classified(S, "internal", "MIN001",
                                     "udp-recv: out of memory");
    }
    /* Pin the socket handle across the poll + recvfrom yield windows,
     * so a concurrent collection cannot finalize it mid-park. */
    gc_pin(sock_val);
    rc = udp_wait_readable(S, sock->fd, read_ms);
    if (rc != 0) {
        char msg[160];
        free(buf);
        gc_unpin(1);
        if (rc == 1) {
            snprintf(msg, sizeof(msg),
                     "udp-recv: timed out after %lld ms", read_ms);
            return prim_throw_classified(S, "net/timeout", "MNE003", msg);
        }
        udp_os_error(msg, sizeof(msg));
        return prim_throw_classified(S, "net", "MNE004", msg);
    }
    memset(&ss, 0, sizeof(ss));
    {
#ifdef _WIN32
        int ss_len = (int)sizeof(ss);
        int r;
        int depth = mino_yield_lock(S);
        r = recvfrom(sock->fd, (char *)buf, (int)cap, 0,
                     (struct sockaddr *)&ss, &ss_len);
        mino_resume_lock(S, depth);
        if (r == SOCKET_ERROR) {
            int e = WSAGetLastError();
            char msg[160];
            free(buf);
            gc_unpin(1);
            /* WSAEMSGSIZE: the datagram was larger than the buffer.
             * winsock discards the excess and reports the error rather
             * than a short count; the buffer holds cap bytes. */
            if (e == WSAEMSGSIZE) {
                got = (long long)cap;
                truncated = 1;
            } else if (e == WSAETIMEDOUT) {
                snprintf(msg, sizeof(msg),
                         "udp-recv: timed out after %lld ms", read_ms);
                return prim_throw_classified(S, "net/timeout", "MNE003",
                                             msg);
            } else {
                snprintf(msg, sizeof(msg),
                         "udp-recv: recv failed: winsock error %d", e);
                return prim_throw_classified(S, "net", "MNE004", msg);
            }
        } else {
            got = (long long)r;
        }
#else
        ssize_t r;
        struct msghdr mh;
        struct iovec iov;
        int depth;
        /* recvmsg reports truncation portably: MSG_TRUNC in the
         * returned msg_flags means the datagram was longer than the
         * buffer, on both Linux and BSD/macOS (where MSG_TRUNC is a
         * returned flag, not an input flag recvfrom honors). The return
         * value is always the bytes copied into the buffer (<= cap), so
         * the buffer holds exactly what arrived and the excess is
         * dropped with the datagram. */
        memset(&mh, 0, sizeof(mh));
        iov.iov_base       = buf;
        iov.iov_len        = cap;
        mh.msg_name        = &ss;
        mh.msg_namelen     = sizeof(ss);
        mh.msg_iov         = &iov;
        mh.msg_iovlen      = 1;
        depth = mino_yield_lock(S);
        r = recvmsg(sock->fd, &mh, 0);
        mino_resume_lock(S, depth);
        if (r < 0) {
            char msg[160];
            free(buf);
            gc_unpin(1);
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                snprintf(msg, sizeof(msg),
                         "udp-recv: timed out after %lld ms", read_ms);
                return prim_throw_classified(S, "net/timeout", "MNE003",
                                             msg);
            }
            snprintf(msg, sizeof(msg), "udp-recv: recv failed: %s",
                     strerror(errno));
            return prim_throw_classified(S, "net", "MNE004", msg);
        }
        got = (long long)r;
        if (mh.msg_flags & MSG_TRUNC) truncated = 1;
#endif
    }
    if (udp_addr_ip((struct sockaddr *)&ss,
#ifdef _WIN32
                    (socklen_t)sizeof(ss),
#else
                    sizeof(ss),
#endif
                    ip, sizeof(ip)) != 0) {
        ip[0] = '\0';
    }
    {
        mino_val *keys[4], *vals[4];
        mino_val *bytes = mino_bytes(S, buf, (size_t)got);
        free(buf);
        buf = NULL;
        gc_pin(bytes);
        keys[0] = mino_keyword(S, "data");
        vals[0] = bytes;
        keys[1] = mino_keyword(S, "address");
        vals[1] = mino_string(S, ip);
        keys[2] = mino_keyword(S, "port");
        vals[2] = mino_int(S, (long long)udp_addr_port(&ss));
        keys[3] = mino_keyword(S, "truncated?");
        vals[3] = truncated ? mino_true(S) : mino_false(S);
        result = mino_map(S, keys, vals, 4);
        gc_unpin(1);
    }
    gc_unpin(1);
    return result;
}

/* ---- udp-close ---- */

/* (udp-close sock) -> nil. Idempotent; the record is marked so the
 * handle finalizer never double-closes the fd. */
static mino_val *prim_udp_close(mino_state *S, mino_val *args, mino_env *env)
{
    mino_val *v;
    (void)env;

    if (!mino_is_cons(args) || mino_is_cons(args->as.cons.cdr)) {
        return prim_throw_classified(S, "eval/arity", "MAR001",
                                     "udp-close requires one argument");
    }
    v = args->as.cons.car;
    if (v != NULL && mino_type_of(v) == MINO_HANDLE
        && v->as.handle.tag != NULL
        && strcmp(v->as.handle.tag, UDP_SOCK_TAG) == 0
        && v->as.handle.ptr != NULL) {
        mino_udp_sock_t *s = (mino_udp_sock_t *)v->as.handle.ptr;
        if (!s->closed) {
            udp_close_fd(s->fd);
            s->closed = 1;
        }
        return mino_nil(S);
    }
    return prim_throw_classified(S, "eval/type", "MTY001",
                                 "udp-close: argument must be a udp "
                                 "socket");
}

/* ---- dns-lookup ---- */

/* (dns-lookup host [opts]) -> vector of {:address ip :family kw}
 * address maps. opts key :family narrows to :inet or :inet6. Throws
 * :net/dns on a host that does not resolve. */
static mino_val *prim_dns_lookup(mino_state *S, mino_val *args,
                                 mino_env *env)
{
    mino_val *host_val, *opts = NULL, *result;
    char host[512];
    struct addrinfo hints, *res = NULL, *ai;
    int gai_rc, depth, family = AF_UNSPEC;
    size_t n = 0, i;
    mino_val **items = NULL;
    (void)env;

    if (!mino_is_cons(args)) {
        return prim_throw_classified(S, "eval/arity", "MAR001",
                                     "dns-lookup requires a host");
    }
    host_val = args->as.cons.car;
    args = args->as.cons.cdr;
    if (mino_is_cons(args)) {
        opts = args->as.cons.car;
        if (mino_is_cons(args->as.cons.cdr)) {
            return prim_throw_classified(S, "eval/arity", "MAR001",
                                         "dns-lookup takes at most 2 "
                                         "arguments");
        }
        if (opts != NULL && mino_type_of(opts) != MINO_MAP
            && mino_type_of(opts) != MINO_NIL) {
            return prim_throw_classified(S, "eval/type", "MTY001",
                                         "dns-lookup: opts must be a map");
        }
    }
    if (udp_host_arg(S, host_val, "dns-lookup", host, sizeof(host)) != 0)
        return NULL;
    if (opts != NULL && mino_type_of(opts) == MINO_MAP) {
        mino_val *fv = map_get_val(opts, mino_keyword(S, "family"));
        if (fv != NULL && mino_type_of(fv) == MINO_KEYWORD) {
            if (strcmp(fv->as.s.data, "inet") == 0) family = AF_INET;
            else if (strcmp(fv->as.s.data, "inet6") == 0)
                family = AF_INET6;
        }
    }

#ifdef _WIN32
    if (udp_winsock_init() != 0) {
        return prim_throw_classified(S, "net", "MNE004",
                                     "dns-lookup: WSAStartup failed");
    }
#endif
    memset(&hints, 0, sizeof(hints));
    hints.ai_family   = family;
    hints.ai_socktype = SOCK_DGRAM;
    depth = mino_yield_lock(S);
    gai_rc = getaddrinfo(host, NULL, &hints, &res);
    mino_resume_lock(S, depth);
    if (gai_rc != 0) {
        char msg[300];
#ifdef _WIN32
        snprintf(msg, sizeof(msg), "dns-lookup: cannot resolve %.190s: "
                 "getaddrinfo error %d", host, gai_rc);
#else
        snprintf(msg, sizeof(msg), "dns-lookup: cannot resolve %.190s: "
                 "%.60s", host, gai_strerror(gai_rc));
#endif
        return prim_throw_classified(S, "net/dns", "MNE001", msg);
    }

    for (ai = res; ai != NULL; ai = ai->ai_next) {
        if (ai->ai_family == AF_INET || ai->ai_family == AF_INET6) n++;
    }
    if (n == 0) {
        freeaddrinfo(res);
        return prim_throw_classified(S, "net/dns", "MNE001",
                                     "dns-lookup: no addresses");
    }
    items = (mino_val **)malloc(n * sizeof(*items));
    if (items == NULL) {
        freeaddrinfo(res);
        return prim_throw_classified(S, "internal", "MIN001",
                                     "dns-lookup: out of memory");
    }
    /* Each address map is pinned as it is built so a collection
     * triggered by a later map allocation cannot sweep an earlier one;
     * unpinned in one batch once the enclosing vector roots them all. */
    i = 0;
    for (ai = res; ai != NULL; ai = ai->ai_next) {
        char ip[128];
        mino_val *keys[2], *vals[2], *m;
        const char *fam;
        if (ai->ai_family != AF_INET && ai->ai_family != AF_INET6)
            continue;
        if (udp_addr_ip(ai->ai_addr, (socklen_t)ai->ai_addrlen,
                        ip, sizeof(ip)) != 0) {
            ip[0] = '\0';
        }
        fam = ai->ai_family == AF_INET6 ? "inet6" : "inet";
        keys[0] = mino_keyword(S, "address");
        vals[0] = mino_string(S, ip);
        keys[1] = mino_keyword(S, "family");
        vals[1] = mino_keyword(S, fam);
        m = mino_map(S, keys, vals, 2);
        gc_pin(m);
        items[i++] = m;
    }
    freeaddrinfo(res);
    result = mino_vector(S, items, i);
    gc_unpin((int)i);
    free(items);
    return result;
}

/* ---- install ---- */

const mino_prim_def k_prims_udp[] = {
    {"udp-socket",      prim_udp_socket,
     "Binds a UDP datagram socket and returns a socket handle. Opts map "
     "keys :host (bind address, default 127.0.0.1), :port (0 or omitted "
     "asks the kernel for an ephemeral port; read it with "
     "udp-socket-port), :read-timeout (non-negative ms, default 30000). "
     "The bind address is numeric only."},
    {"udp-socket-port", prim_udp_socket_port,
     "Returns the port a udp socket is bound to; how a caller learns "
     "the kernel-chosen port after an ephemeral bind. Throws :net on a "
     "closed socket."},
    {"udp-send",        prim_udp_send,
     "Sends a datagram to host:port from a udp socket. data is a string "
     "(UTF-8 bytes) or bytes. Returns the number of bytes sent. Throws "
     ":net/dns when the host cannot resolve, :net on send failure."},
    {"udp-recv",        prim_udp_recv,
     "Receives one datagram on a udp socket. Returns a map {:data bytes "
     ":address sender-ip :port sender-port :truncated? bool}. Opts map "
     "keys :read-timeout (ms, overrides the socket default) and "
     ":max-bytes (recv buffer cap, default 65535). A datagram larger "
     "than the buffer fills it, sets :truncated? true, and drops the "
     "excess. Throws :net/timeout when the deadline passes."},
    {"udp-close",       prim_udp_close,
     "Closes a udp socket. Returns nil. Idempotent; dropped handles are "
     "also closed by the garbage collector."},
    {"dns-lookup",      prim_dns_lookup,
     "Resolves a host to a vector of address maps {:address ip-string "
     ":family :inet|:inet6}. Opts map key :family narrows to :inet or "
     ":inet6. Throws :net/dns when the host does not resolve."},
};

const size_t k_prims_udp_count =
    sizeof(k_prims_udp) / sizeof(k_prims_udp[0]);

/* udp rides its own MINO_CAP_UDP bit (udp + dns, in MINO_CAP_DEFAULT).
 * The bit is set here as well as by the capability dispatch loop so a
 * direct mino_install_udp sets it too, matching the net/random install. */
void mino_install_udp(mino_state *S, mino_env *env)
{
    mino_env *core_env = ns_env_ensure(S, "clojure.core");
    (void)env;
    prim_install_table_with_capability(S, core_env, "clojure.core",
                                       k_prims_udp, k_prims_udp_count,
                                       "udp");
    S->caps_installed |= MINO_CAP_UDP;
}
