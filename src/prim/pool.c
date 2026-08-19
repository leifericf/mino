/*
 * pool.c -- keep-alive connection pool primitives: pool-checkout,
 * pool-return, pool-close-all.
 *
 * One pool per endpoint (scheme, host, port), held per mino state in
 * S->net_pools. Entries are idle net or TLS socket handles rooted
 * through mino_ref (the host-retained root list), stamped with a
 * monotonic last-used time. The pool never opens connections: checkout
 * answers a live idle handle or nil, and the caller connects on nil.
 * Expiry (the :keepalive ms age) and liveness (a zero-timeout POLLIN
 * poll: a readable idle socket means the peer closed or desynced) are
 * checked as entries come out, so a dead or stale entry is closed and
 * dropped and the next one is tried.
 *
 * Locking follows the host_threads discipline: one mutex per endpoint
 * pool guarding queue operations only, never blocking IO. Prims on one
 * state already serialize through the state lock; the pool mutexes
 * keep the queue operations correct independently of that.
 *
 * The registry is freed by mino_net_pool_state_free, wired into state
 * teardown, so pooled sockets die with their state even when the
 * script never returns them.
 *
 * Trust model mirrors net.c: argument shapes are validated, endpoints
 * are not policed, and the same MINO_CAP_NET bit gates the prims.
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
#else
#  include <poll.h>
#endif

#include <string.h>
#include <stdlib.h>

#define POOL_DEFAULT_KEEPALIVE_MS 120000LL
#define POOL_HOST_CAP             256

#if defined(_WIN32) && defined(_MSC_VER)
typedef CRITICAL_SECTION pool_mu_t;
static void pool_mu_init(pool_mu_t *mu)   { InitializeCriticalSection(mu); }
static void pool_mu_lock(pool_mu_t *mu)   { EnterCriticalSection(mu); }
static void pool_mu_unlock(pool_mu_t *mu) { LeaveCriticalSection(mu); }
static void pool_mu_destroy(pool_mu_t *mu){ DeleteCriticalSection(mu); }
#else
typedef pthread_mutex_t pool_mu_t;
static void pool_mu_init(pool_mu_t *mu)   { pthread_mutex_init(mu, NULL); }
static void pool_mu_lock(pool_mu_t *mu)   { pthread_mutex_lock(mu); }
static void pool_mu_unlock(pool_mu_t *mu) { pthread_mutex_unlock(mu); }
static void pool_mu_destroy(pool_mu_t *mu){ pthread_mutex_destroy(mu); }
#endif

typedef struct pool_entry {
    mino_ref          *ref;   /* rooted handle value */
    long long         last_used_ms;
    struct pool_entry *next;
} pool_entry_t;

typedef struct pool_endpoint {
    struct pool_endpoint *next;
    pool_entry_t         *idle;   /* LIFO: most recently returned first */
    char                 host[POOL_HOST_CAP];
    size_t               host_len;
    int                  port;
    int                  is_https;
    pool_mu_t            mu;
} pool_endpoint_t;

struct mino_net_pools {
    pool_endpoint_t *endpoints;
    pool_mu_t       mu;
};

/* Zero-timeout liveness poll. A readable (or failed/invalid) idle
 * socket is not reusable: readable on an idle connection means EOF is
 * pending or the peer desynced. Returns 1 dead, 0 live. */
static int pool_socket_dead(uintptr_t fd_raw)
{
#ifdef _WIN32
    WSAPOLLFD pfd;
    int rc;
    pfd.fd      = (SOCKET)fd_raw;
    pfd.events  = POLLIN;
    pfd.revents = 0;
    rc = WSAPoll(&pfd, 1, 0);
    if (rc == SOCKET_ERROR) return 1;
    return pfd.revents != 0;
#else
    struct pollfd pfd;
    int rc;
    pfd.fd      = (int)fd_raw;
    pfd.events  = POLLIN;
    pfd.revents = 0;
    rc = poll(&pfd, 1, 0);
    if (rc < 0) return 1;
    return pfd.revents != 0;
#endif
}

/* ---- handle kind dispatch over the net and tls tags ---- */

typedef struct {
    int  (*fd_of)(mino_val *v, uintptr_t *fd_out);
    void (*close)(mino_val *v);
} pool_handle_ops_t;

static const pool_handle_ops_t pool_ops_net = {
    mino_net_handle_fd, mino_net_handle_close
};
static const pool_handle_ops_t pool_ops_tls = {
    mino_tls_handle_fd, mino_tls_handle_close
};

static const pool_handle_ops_t *pool_ops_for(const mino_val *v)
{
    const char *tag;
    if (v == NULL || mino_type_of(v) != MINO_HANDLE
        || v->as.handle.tag == NULL) {
        return NULL;
    }
    tag = v->as.handle.tag;
    if (strcmp(tag, mino_net_sock_tag()) == 0) return &pool_ops_net;
    if (strcmp(tag, mino_tls_sock_tag()) == 0) return &pool_ops_tls;
    return NULL;
}

static long long pool_now_ms(void)
{
    return mino_monotonic_ns() / 1000000LL;
}

/* ---- registry and endpoint lookup ---- */

/* Return the state's registry, allocating it on first use. Prims run
 * under the state lock, so the lazy allocation has no creator race. */
static struct mino_net_pools *pool_registry(mino_state *S)
{
    struct mino_net_pools *reg;
    if (S->net_pools != NULL) return S->net_pools;
    reg = (struct mino_net_pools *)calloc(1, sizeof(*reg));
    if (reg == NULL) return NULL;
    pool_mu_init(&reg->mu);
    S->net_pools = reg;
    return reg;
}

static int pool_endpoint_matches(const pool_endpoint_t *ep,
                                 const char *host, size_t host_len,
                                 int port, int is_https)
{
    return ep->port == port && ep->is_https == is_https
        && ep->host_len == host_len
        && memcmp(ep->host, host, host_len) == 0;
}

/* Find an endpoint pool; create selects find-or-create. NULL when the
 * registry is missing (lookup) or allocation fails (create). */
static pool_endpoint_t *pool_endpoint(mino_state *S, const char *host,
                                      size_t host_len, int port,
                                      int is_https, int create)
{
    struct mino_net_pools *reg;
    pool_endpoint_t *ep;

    if (!create) {
        reg = S->net_pools;
        if (reg == NULL) return NULL;
        for (ep = reg->endpoints; ep != NULL; ep = ep->next) {
            if (pool_endpoint_matches(ep, host, host_len, port,
                                      is_https))
                return ep;
        }
        return NULL;
    }
    reg = pool_registry(S);
    if (reg == NULL) return NULL;
    pool_mu_lock(&reg->mu);
    for (ep = reg->endpoints; ep != NULL; ep = ep->next) {
        if (pool_endpoint_matches(ep, host, host_len, port, is_https)) {
            pool_mu_unlock(&reg->mu);
            return ep;
        }
    }
    ep = (pool_endpoint_t *)calloc(1, sizeof(*ep));
    if (ep == NULL) {
        pool_mu_unlock(&reg->mu);
        return NULL;
    }
    memcpy(ep->host, host, host_len);
    ep->host_len    = host_len;
    ep->port        = port;
    ep->is_https    = is_https;
    pool_mu_init(&ep->mu);
    ep->next       = reg->endpoints;
    reg->endpoints = ep;
    pool_mu_unlock(&reg->mu);
    return ep;
}

/* Pop every entry off one endpoint, closing and releasing each handle.
 * The endpoint struct itself stays; mino_net_pool_state_free and
 * pool-close-all free it. */
static void pool_drain_endpoint(mino_state *S, pool_endpoint_t *ep)
{
    pool_entry_t *e;
    pool_mu_lock(&ep->mu);
    e = ep->idle;
    ep->idle = NULL;
    pool_mu_unlock(&ep->mu);
    while (e != NULL) {
        pool_entry_t *next = e->next;
        mino_val *v = mino_deref(e->ref);
        if (v != NULL) {
            const pool_handle_ops_t *ops = pool_ops_for(v);
            if (ops != NULL) ops->close(v);
        }
        mino_unref(S, e->ref);
        free(e);
        e = next;
    }
}

void mino_net_pool_state_free(mino_state *S)
{
    struct mino_net_pools *reg;
    pool_endpoint_t *ep;

    if (S == NULL || S->net_pools == NULL) return;
    reg = S->net_pools;
    ep = reg->endpoints;
    while (ep != NULL) {
        pool_endpoint_t *next = ep->next;
        pool_drain_endpoint(S, ep);
        pool_mu_destroy(&ep->mu);
        free(ep);
        ep = next;
    }
    pool_mu_destroy(&reg->mu);
    free(reg);
    S->net_pools = NULL;
}

/* ---- argument helpers ---- */

/* Name text out of a string or plain keyword (namespace, if any, stays
 * part of the name and fails the match). */
static int pool_name_text(const mino_val *v, const char **out,
                          size_t *len_out)
{
    if (v == NULL) return 0;
    if (mino_type_of(v) == MINO_STRING) {
        *out = v->as.s.data;
        *len_out = v->as.s.len;
        return 1;
    }
    if (mino_type_of(v) == MINO_KEYWORD || mino_type_of(v) == MINO_SYMBOL) {
        size_t skip = v->as.s.ns_len > 0 ? v->as.s.ns_len + 1 : 0;
        if (skip >= v->as.s.len) return 0;
        *out = v->as.s.data + skip;
        *len_out = v->as.s.len - skip;
        return 1;
    }
    return 0;
}

/* Endpoint map {:scheme :host :port} into the pool key. host arrives
 * lowercased into host_out. Returns 0 on success, -1 with a
 * classified throw. */
static int pool_endpoint_arg(mino_state *S, mino_val *m,
                             char *host_out, size_t *host_len_out,
                             int *port_out, int *is_https_out)
{
    mino_val *v;
    const char *scheme;
    size_t scheme_len, i;
    long long port;

    if (m == NULL || mino_type_of(m) != MINO_MAP) {
        prim_throw_classified(S, "eval/type", "MTY001",
                              "pool: endpoint must be a map with "
                              ":scheme :host :port");
        return -1;
    }
    v = map_get_val(m, mino_keyword(S, "scheme"));
    if (!pool_name_text(v, &scheme, &scheme_len)
        || !((scheme_len == 4 && memcmp(scheme, "http", 4) == 0)
             || (scheme_len == 5 && memcmp(scheme, "https", 5) == 0))) {
        prim_throw_classified(S, "eval/contract", "MCT001",
                              "pool: :scheme must be :http or :https");
        return -1;
    }
    *is_https_out = scheme_len == 5;
    v = map_get_val(m, mino_keyword(S, "host"));
    if (v == NULL || mino_type_of(v) != MINO_STRING
        || v->as.s.len == 0 || v->as.s.len >= POOL_HOST_CAP) {
        prim_throw_classified(S, "eval/contract", "MCT001",
                              "pool: :host must be a non-empty string "
                              "under 256 bytes");
        return -1;
    }
    memcpy(host_out, v->as.s.data, v->as.s.len);
    *host_len_out = v->as.s.len;
    for (i = 0; i < *host_len_out; i++) {
        char c = host_out[i];
        host_out[i] = (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c;
    }
    v = map_get_val(m, mino_keyword(S, "port"));
    if (!as_long(v, &port) || port < 1 || port > 65535) {
        prim_throw_classified(S, "eval/contract", "MCT001",
                              "pool: :port must be an integer in "
                              "1..65535");
        return -1;
    }
    *port_out = (int)port;
    return 0;
}

/* Optional trailing opts map (nil accepted). Returns 0 on success. */
static int pool_opts_arg(mino_state *S, mino_val *opts, const char *who)
{
    if (opts != NULL && mino_type_of(opts) != MINO_MAP
        && mino_type_of(opts) != MINO_NIL) {
        char msg[96];
        snprintf(msg, sizeof(msg), "%s: opts must be a map", who);
        prim_throw_classified(S, "eval/type", "MTY001", msg);
        return -1;
    }
    return 0;
}

/* :keepalive ms out of opts. Any integer is legal; 0 or negative
 * disables reuse. Returns 0 on success. */
static int pool_opt_keepalive(mino_state *S, mino_val *opts,
                              long long *out)
{
    mino_val *v;
    *out = POOL_DEFAULT_KEEPALIVE_MS;
    if (opts == NULL || mino_type_of(opts) != MINO_MAP) return 0;
    v = map_get_val(opts, mino_keyword(S, "keepalive"));
    if (v == NULL || mino_type_of(v) == MINO_NIL) return 0;
    if (!as_long(v, out)) {
        prim_throw_classified(S, "eval/contract", "MCT001",
                              "pool: opts key :keepalive must be an "
                              "integer");
        return -1;
    }
    return 0;
}

/* ---- prims ---- */

/* (pool-checkout endpoint [opts]) -> socket or TLS handle, or nil when
 * nothing live and unexpired is pooled. The caller connects on nil;
 * the pool never opens connections. */
static mino_val *prim_pool_checkout(mino_state *S, mino_val *args,
                                    mino_env *env)
{
    mino_val *m, *opts = NULL;
    char host[POOL_HOST_CAP];
    size_t host_len;
    int port, is_https;
    long long keepalive;
    pool_endpoint_t *ep;
    long long now;
    (void)env;

    if (!mino_is_cons(args)) {
        return prim_throw_classified(S, "eval/arity", "MAR001",
                                     "pool-checkout requires an endpoint "
                                     "map");
    }
    m = args->as.cons.car;
    args = args->as.cons.cdr;
    if (mino_is_cons(args)) {
        opts = args->as.cons.car;
        if (mino_is_cons(args->as.cons.cdr)) {
            return prim_throw_classified(S, "eval/arity", "MAR001",
                                         "pool-checkout takes at most 2 "
                                         "arguments");
        }
    }
    if (pool_opts_arg(S, opts, "pool-checkout") != 0) return NULL;
    if (pool_endpoint_arg(S, m, host, &host_len, &port, &is_https) != 0)
        return NULL;
    if (pool_opt_keepalive(S, opts, &keepalive) != 0) return NULL;
    if (keepalive <= 0) return mino_nil(S);
    ep = pool_endpoint(S, host, host_len, port, is_https, 0);
    if (ep == NULL) return mino_nil(S);

    now = pool_now_ms();
    for (;;) {
        pool_entry_t *e;
        mino_val *v;
        uintptr_t fd;
        const pool_handle_ops_t *ops;

        pool_mu_lock(&ep->mu);
        e = ep->idle;
        if (e != NULL) ep->idle = e->next;
        pool_mu_unlock(&ep->mu);
        if (e == NULL) return mino_nil(S);
        v = mino_deref(e->ref);
        ops = v != NULL ? pool_ops_for(v) : NULL;
        if (v == NULL || ops == NULL
            || now - e->last_used_ms >= keepalive
            || ops->fd_of(v, &fd) == 0
            || pool_socket_dead(fd)) {
            /* Expired, closed, or peer-gone: close, drop, try next. */
            if (v != NULL && ops != NULL) ops->close(v);
            mino_unref(S, e->ref);
            free(e);
            continue;
        }
        mino_unref(S, e->ref);
        free(e);
        return v;
    }
}

/* (pool-return endpoint handle [opts]) -> nil. Gives an idle socket or
 * TLS handle back to its endpoint pool for reuse. :keepalive 0 or
 * negative closes the handle instead of pooling it. */
static mino_val *prim_pool_return(mino_state *S, mino_val *args,
                                  mino_env *env)
{
    mino_val *m, *handle, *opts = NULL;
    char host[POOL_HOST_CAP];
    size_t host_len;
    int port, is_https;
    long long keepalive;
    const pool_handle_ops_t *ops;
    uintptr_t fd;
    pool_endpoint_t *ep;
    pool_entry_t *e;
    (void)env;

    if (!mino_is_cons(args) || !mino_is_cons(args->as.cons.cdr)) {
        return prim_throw_classified(S, "eval/arity", "MAR001",
                                     "pool-return requires an endpoint "
                                     "map and a handle");
    }
    m = args->as.cons.car;
    handle = args->as.cons.cdr->as.cons.car;
    args = args->as.cons.cdr->as.cons.cdr;
    if (mino_is_cons(args)) {
        opts = args->as.cons.car;
        if (mino_is_cons(args->as.cons.cdr)) {
            return prim_throw_classified(S, "eval/arity", "MAR001",
                                         "pool-return takes at most 3 "
                                         "arguments");
        }
    }
    if (pool_opts_arg(S, opts, "pool-return") != 0) return NULL;
    if (pool_endpoint_arg(S, m, host, &host_len, &port, &is_https) != 0)
        return NULL;
    ops = pool_ops_for(handle);
    if (ops == NULL) {
        return prim_throw_classified(S, "eval/type", "MTY001",
                                     "pool-return: handle must be a net "
                                     "or TLS socket");
    }
    if (pool_opt_keepalive(S, opts, &keepalive) != 0) return NULL;
    if (keepalive <= 0 || ops->fd_of(handle, &fd) == 0) {
        /* Reuse disabled, or the handle is already closed: keep the
         * pool free of entries that can never be handed out. */
        if (keepalive <= 0) ops->close(handle);
        return mino_nil(S);
    }
    ep = pool_endpoint(S, host, host_len, port, is_https, 1);
    e = (pool_entry_t *)malloc(sizeof(*e));
    if (ep == NULL || e == NULL) {
        free(e);
        ops->close(handle);
        return prim_throw_classified(S, "internal", "MIN001",
                                     "pool-return: out of memory");
    }
    e->ref = mino_ref_new(S, handle);
    if (e->ref == NULL) {
        free(e);
        ops->close(handle);
        return prim_throw_classified(S, "internal", "MIN001",
                                     "pool-return: out of memory");
    }
    e->last_used_ms = pool_now_ms();
    pool_mu_lock(&ep->mu);
    e->next  = ep->idle;
    ep->idle = e;
    pool_mu_unlock(&ep->mu);
    return mino_nil(S);
}

/* (pool-close-all) -> nil. Closes every pooled socket in every
 * endpoint and drops the registry; idempotent. */
static mino_val *prim_pool_close_all(mino_state *S, mino_val *args,
                                     mino_env *env)
{
    struct mino_net_pools *reg;
    pool_endpoint_t *ep;
    (void)env;

    if (mino_is_cons(args)) {
        return prim_throw_classified(S, "eval/arity", "MAR001",
                                     "pool-close-all takes no arguments");
    }
    reg = S->net_pools;
    if (reg == NULL) return mino_nil(S);
    ep = reg->endpoints;
    while (ep != NULL) {
        pool_endpoint_t *next = ep->next;
        pool_drain_endpoint(S, ep);
        pool_mu_destroy(&ep->mu);
        free(ep);
        ep = next;
    }
    pool_mu_destroy(&reg->mu);
    free(reg);
    S->net_pools = NULL;
    return mino_nil(S);
}

/* ---- install ---- */

static const mino_prim_def k_prims_pool[] = {
    {"pool-checkout", prim_pool_checkout,
     "Returns a live idle keep-alive socket or TLS handle for the "
     "endpoint map {:scheme :host :port}, or nil when nothing is "
     "pooled (the caller connects on nil; the pool never connects). "
     "Opts key :keepalive (ms, default 120000) bounds entry age at "
     "checkout; 0 or negative disables reuse. Stale entries are closed "
     "and dropped; liveness is a zero-timeout poll, so a peer-closed "
     "socket is never handed out."},
    {"pool-return", prim_pool_return,
     "Gives an idle socket or TLS handle back to its endpoint pool for "
     "reuse. Returns nil. Opts key :keepalive (ms, default 120000) "
     "stamps the entry; 0 or negative closes the handle instead of "
     "pooling it. Returning an already-closed handle is a no-op."},
    {"pool-close-all", prim_pool_close_all,
     "Closes every pooled socket in every endpoint and empties the "
     "pools. Returns nil. Idempotent. State teardown runs it "
     "automatically, so pooled sockets never outlive their state."},
};

static const size_t k_prims_pool_count =
    sizeof(k_prims_pool) / sizeof(k_prims_pool[0]);

void mino_install_pool(mino_state *S, mino_env *env)
{
    mino_env *core_env = ns_env_ensure(S, "clojure.core");
    (void)env;
    prim_install_table_with_capability(S, core_env, "clojure.core",
                                       k_prims_pool, k_prims_pool_count,
                                       "net");
    S->caps_installed |= MINO_CAP_NET;
}
