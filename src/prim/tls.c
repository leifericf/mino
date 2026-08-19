/*
 * tls.c -- TLS client primitives over the vendored BearSSL engine:
 * tls-connect, tls-read, tls-read-all, tls-write, tls-close.
 *
 * A TLS socket is a MINO_HANDLE (tag "mino/tls-socket") wrapping a
 * malloc'd record holding the BearSSL client context, its X.509
 * validator, the record I/O buffer, and the adopted OS descriptor.
 * The handle finalizer closes the descriptor when the value is
 * collected; tls-close is the explicit, idempotent form.
 *
 * Verification is on by default: full client profile, chain validated
 * against the vendored Mozilla root snapshot (see
 * src/vendor/bearssl/roots.c), host name matched against SAN/CN, SNI
 * always sent from the host argument. {:insecure? true} keeps the
 * full handshake but replaces the chain validator with one that only
 * decodes the end-entity certificate for its public key; it is for
 * local fixtures, never production peers.
 *
 * Entropy: this file replaces the vendored sysrng unit as the
 * br_prng_seeder_system provider (getentropy on POSIX,
 * BCryptGenRandom on Windows). The engine pulls a fresh seed per
 * connection; no RNG state is shared across sockets.
 *
 * Trust model mirrors net.c: argument shapes are validated,
 * destinations are not policed, and the peer's bytes are untrusted
 * input. Every engine failure maps to a classified :tls error; the
 * underlying socket keeps the net prims' :net/timeout and :net
 * classification through the fd bridge in net.c.
 */

#define _POSIX_C_SOURCE 200809L
#if defined(__APPLE__)
#  define _DARWIN_C_SOURCE 1
#endif

#include "prim/internal.h"
#include "mino.h"
#include "bearssl_x509.h"
#include "bearssl_ssl.h"
#include "roots.h"

#include <stdint.h>
#include <string.h>
#include <stdlib.h>

#ifdef _WIN32
#  define WIN32_LEAN_AND_MEAN
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  include <winsock2.h>
#  include <windows.h>
#  include <bcrypt.h>
#else
/* getentropy: POSIX 2024, but the libc headers that declare it
 * disagree (sys/random.h on glibc, unistd.h on musl) and the
 * feature-test gating varies; one prototype covers every target. */
int getentropy(void *buf, size_t buflen);
/* pthread_once serializes the anchor-table build across states. */
#  include <pthread.h>
#endif

#define TLS_SOCK_TAG "mino/tls-socket"

/* Same defaults as the net prims, milliseconds. */
#define TLS_DEFAULT_READ_TIMEOUT_MS  30000LL
#define TLS_DEFAULT_WRITE_TIMEOUT_MS 30000LL

#define TLS_READ_ALL_DEFAULT_MAX_BYTES (16LL * 1024LL * 1024LL)

/* Trust-anchor decode storage. Sized for the vendored snapshot (121
 * anchors); an anchor whose DN or key does not fit is dropped from
 * the table, which fails closed (verification then rejects the
 * chains that needed it). */
#define TLS_MAX_ANCHORS   256
#define TLS_ANCHOR_DN_CAP 512

typedef struct {
    const br_x509_class    *vtable;
    br_x509_decoder_context dec;
    int                    ee_active; /* decoding the first cert */
    int                    ee_done;
    int                    ee_err;
} mino_tls_skip_ctx;

typedef struct {
    br_ssl_client_context   cc;
    br_x509_minimal_context xc;
    mino_tls_skip_ctx       skip;
    unsigned char          *iobuf;
    uintptr_t               fd;
    long long               read_timeout_ms;
    long long               write_timeout_ms;
    int                     closed;
} mino_tls_sock_t;

/* ---- OS entropy (replaces the vendored sysrng unit) ---- */

static int tls_seed_os(const br_prng_class **ctx)
{
    unsigned char seed[32];
#ifdef _WIN32
    NTSTATUS rc = BCryptGenRandom(NULL, seed, (ULONG)sizeof seed,
                                  BCRYPT_USE_SYSTEM_PREFERRED_RNG);
    if (!BCRYPT_SUCCESS(rc)) return 0;
#else
    if (getentropy(seed, sizeof seed) != 0) return 0;
#endif
    (*ctx)->update(ctx, seed, sizeof seed);
    memset(seed, 0, sizeof seed);
    return 1;
}

/* see bearssl_rand.h */
br_prng_seeder br_prng_seeder_system(const char **name)
{
    if (name != NULL) *name = "mino-host";
    return &tls_seed_os;
}

/* ---- trust anchors ---- */

static br_x509_trust_anchor tls_anchors[TLS_MAX_ANCHORS];
static unsigned char tls_anchor_dn[TLS_MAX_ANCHORS][TLS_ANCHOR_DN_CAP];
static unsigned char tls_anchor_pkey[TLS_MAX_ANCHORS][BR_X509_BUFSIZE_KEY];
static size_t tls_anchor_count;

typedef struct {
    unsigned char *dst;
    size_t         cap;
    size_t         len;
    int            overflow;
} tls_dn_capture;

static void tls_capture_dn(void *ctx_, const void *buf, size_t len)
{
    tls_dn_capture *c = (tls_dn_capture *)ctx_;
    if (c->len + len > c->cap) {
        c->overflow = 1;
        return;
    }
    memcpy(c->dst + c->len, buf, len);
    c->len += len;
}

/* Rebase a key-material pointer that lives inside the decoder's
 * pkey_data buffer onto the static copy. The two buffers are
 * distinct objects, so pointer subtraction between them is
 * undefined; the offset is integer arithmetic and the result is
 * formed inside the destination array. */
static unsigned char *tls_rebase_key(const unsigned char *p,
                                     const unsigned char *old_base,
                                     unsigned char *new_base)
{
    return new_base + ((uintptr_t)p - (uintptr_t)old_base);
}

/* Decode the vendored DER snapshot into the anchor table the X.509
 * minimal validator consumes. Pure function of const data into
 * static storage, run once at install; any anchor that fails to
 * decode is skipped so verification fails closed for the chains that
 * would have needed it. */
static void tls_build_anchors(void)
{
    size_t i;
    size_t count = mino_ca_anchor_count;

    if (count > TLS_MAX_ANCHORS) count = TLS_MAX_ANCHORS;
    tls_anchor_count = 0;
    for (i = 0; i < count; i++) {
        br_x509_decoder_context dec;
        tls_dn_capture cap;
        br_x509_pkey *pk;
        br_x509_trust_anchor *ta;

        cap.dst = tls_anchor_dn[i];
        cap.cap = TLS_ANCHOR_DN_CAP;
        cap.len = 0;
        cap.overflow = 0;
        br_x509_decoder_init(&dec, tls_capture_dn, &cap);
        br_x509_decoder_push(&dec, mino_ca_anchors[i].der,
                             mino_ca_anchors[i].len);
        pk = br_x509_decoder_get_pkey(&dec);
        if (pk == NULL || cap.overflow
            || br_x509_decoder_last_error(&dec) != 0) {
            continue;
        }
        /* The decoder's key material lives inside its own context;
         * copy the backing bytes and rebase the pointers onto the
         * static copy. */
        memcpy(tls_anchor_pkey[i], dec.pkey_data, BR_X509_BUFSIZE_KEY);
        ta = &tls_anchors[tls_anchor_count];
        ta->dn.data = tls_anchor_dn[i];
        ta->dn.len  = cap.len;
        ta->flags   = BR_X509_TA_CA;
        ta->pkey    = *pk;
        if (ta->pkey.key_type == BR_KEYTYPE_RSA) {
            ta->pkey.key.rsa.n = tls_rebase_key(ta->pkey.key.rsa.n,
                                                dec.pkey_data,
                                                tls_anchor_pkey[i]);
            ta->pkey.key.rsa.e = tls_rebase_key(ta->pkey.key.rsa.e,
                                                dec.pkey_data,
                                                tls_anchor_pkey[i]);
        } else {
            ta->pkey.key.ec.q = tls_rebase_key(ta->pkey.key.ec.q,
                                               dec.pkey_data,
                                               tls_anchor_pkey[i]);
        }
        tls_anchor_count++;
    }
}

/* ---- verification-skipping validator ---- */

/* Accepts any chain, decoding only the end-entity certificate so the
 * handshake still has a public key to work with. Still fails when
 * that certificate is undecodable: {:insecure? true} skips trust
 * decisions, not the protocol. */
static void tls_skip_start_chain(const br_x509_class **ctx,
                                 const char *server_name)
{
    mino_tls_skip_ctx *c = (mino_tls_skip_ctx *)ctx;
    (void)server_name;
    c->ee_active = 0;
    c->ee_done   = 0;
    c->ee_err    = 0;
}

static void tls_skip_start_cert(const br_x509_class **ctx, uint32_t len)
{
    mino_tls_skip_ctx *c = (mino_tls_skip_ctx *)ctx;
    (void)len;
    if (!c->ee_done) {
        br_x509_decoder_init(&c->dec, NULL, NULL);
        c->ee_active = 1;
    }
}

static void tls_skip_append(const br_x509_class **ctx,
                            const unsigned char *buf, size_t len)
{
    mino_tls_skip_ctx *c = (mino_tls_skip_ctx *)ctx;
    if (c->ee_active) br_x509_decoder_push(&c->dec, buf, len);
}

static void tls_skip_end_cert(const br_x509_class **ctx)
{
    mino_tls_skip_ctx *c = (mino_tls_skip_ctx *)ctx;
    if (c->ee_active) {
        c->ee_active = 0;
        c->ee_done   = 1;
        c->ee_err    = br_x509_decoder_last_error(&c->dec);
    }
}

static unsigned tls_skip_end_chain(const br_x509_class **ctx)
{
    mino_tls_skip_ctx *c = (mino_tls_skip_ctx *)ctx;
    return (unsigned)c->ee_err;
}

static const br_x509_pkey *tls_skip_get_pkey(const br_x509_class *const *ctx,
                                             unsigned *usages)
{
    mino_tls_skip_ctx *c = (mino_tls_skip_ctx *)(void *)ctx;
    if (usages != NULL) *usages = BR_KEYTYPE_KEYX | BR_KEYTYPE_SIGN;
    return br_x509_decoder_get_pkey(&c->dec);
}

static const br_x509_class tls_skip_vtable = {
    sizeof(mino_tls_skip_ctx),
    tls_skip_start_chain,
    tls_skip_start_cert,
    tls_skip_append,
    tls_skip_end_cert,
    tls_skip_end_chain,
    tls_skip_get_pkey,
};

/* ---- engine error mapping ---- */

static const char *tls_engine_err_desc(int err)
{
    switch (err) {
    case BR_ERR_BAD_PARAM:           return "bad parameter";
    case BR_ERR_BAD_STATE:           return "invalid engine state";
    case BR_ERR_UNSUPPORTED_VERSION: return "unsupported TLS version";
    case BR_ERR_BAD_VERSION:         return "invalid TLS version";
    case BR_ERR_BAD_LENGTH:          return "invalid record length";
    case BR_ERR_TOO_LARGE:           return "record too large";
    case BR_ERR_BAD_MAC:             return "record authentication failed";
    case BR_ERR_NO_RANDOM:           return "no entropy source available";
    case BR_ERR_UNKNOWN_TYPE:        return "unknown record type";
    case BR_ERR_UNEXPECTED:          return "unexpected record";
    case BR_ERR_BAD_CCS:             return "invalid change-cipher-spec";
    case BR_ERR_BAD_ALERT:           return "invalid alert";
    case BR_ERR_BAD_HANDSHAKE:       return "malformed handshake data";
    case BR_ERR_OVERSIZED_ID:        return "oversized session id";
    case BR_ERR_BAD_CIPHER_SUITE:    return "unsupported cipher suite";
    case BR_ERR_BAD_COMPRESSION:     return "unsupported compression";
    case BR_ERR_BAD_FRAGLEN:         return "invalid fragment length";
    case BR_ERR_BAD_SECRENEG:        return "bad renegotiation";
    case BR_ERR_EXTRA_EXTENSION:     return "unexpected extension";
    case BR_ERR_BAD_SNI:             return "invalid server name";
    case BR_ERR_BAD_HELLO_DONE:      return "malformed server hello done";
    case BR_ERR_LIMIT_EXCEEDED:      return "limit exceeded";
    case BR_ERR_BAD_FINISHED:        return "bad finished message";
    case BR_ERR_RESUME_MISMATCH:     return "session resumption mismatch";
    case BR_ERR_INVALID_ALGORITHM:   return "invalid algorithm";
    case BR_ERR_BAD_SIGNATURE:       return "invalid signature";
    case BR_ERR_WRONG_KEY_USAGE:     return "wrong key usage";
    case BR_ERR_X509_EXPIRED:
    case BR_ERR_X509_BAD_SERVER_NAME:
    case BR_ERR_X509_NOT_TRUSTED:
    case BR_ERR_X509_BAD_SIGNATURE:
    case BR_ERR_X509_INVALID_VALUE:
    case BR_ERR_X509_TRUNCATED:
    case BR_ERR_X509_EMPTY_CHAIN:
    case BR_ERR_X509_UNSUPPORTED:
    case BR_ERR_X509_TIME_UNKNOWN:
    case BR_ERR_X509_WEAK_PUBLIC_KEY:
    default:
        if (err >= BR_ERR_X509_OK && err <= BR_ERR_X509_NOT_TRUSTED)
            return "server certificate rejected";
        if (err >= BR_ERR_RECV_FATAL_ALERT)
            return "fatal alert from server";
        if (err >= BR_ERR_SEND_FATAL_ALERT)
            return "fatal alert sent";
        return "connection failed";
    }
}

/* The certificate failures callers must be able to act on get exact
 * text; everything else carries the generic description plus the
 * numeric code for diagnostics. */
static void tls_engine_error_text(const char *who, const char *op,
                                  const br_ssl_engine_context *eng,
                                  char *msg, size_t msg_cap)
{
    int err = br_ssl_engine_last_error(eng);
    switch (err) {
    case BR_ERR_X509_EXPIRED:
        snprintf(msg, msg_cap, "%s: %s failed: server certificate is "
                 "expired or not yet valid", who, op);
        break;
    case BR_ERR_X509_BAD_SERVER_NAME:
        snprintf(msg, msg_cap, "%s: %s failed: server certificate does "
                 "not match the requested server name", who, op);
        break;
    case BR_ERR_X509_NOT_TRUSTED:
        snprintf(msg, msg_cap, "%s: %s failed: server certificate chain "
                 "is not trusted (no matching root CA)", who, op);
        break;
    default:
        snprintf(msg, msg_cap, "%s: %s failed: %s (code %d)",
                 who, op, tls_engine_err_desc(err), err);
        break;
    }
}

/* ---- record pump ---- */

/* Drain every byte the engine wants sent. 0 ok, -1 socket failure
 * (kind/code/msg filled). */
static int tls_drain_sendrec(mino_state *S, mino_tls_sock_t *ts,
                             const char **kind, const char **code,
                             char *msg, size_t msg_cap)
{
    for (;;) {
        size_t len;
        unsigned char *buf = br_ssl_engine_sendrec_buf(&ts->cc.eng, &len);
        if (buf == NULL || len == 0) return 0;
        if (mino_net_send_raw(S, ts->fd, buf, len, ts->write_timeout_ms,
                              kind, code, msg, msg_cap) != 0)
            return -1;
        br_ssl_engine_sendrec_ack(&ts->cc.eng, len);
    }
}

/* Feed the engine from the socket until a record completes. 0 ok,
 * -1 socket failure or truncated stream. */
static int tls_fill_recvrec(mino_state *S, mino_tls_sock_t *ts,
                            const char **kind, const char **code,
                            char *msg, size_t msg_cap)
{
    size_t len;
    size_t got = 0;
    unsigned char *buf = br_ssl_engine_recvrec_buf(&ts->cc.eng, &len);
    if (buf == NULL || len == 0) return 0;
    /* 1 = bytes received, 0 = clean EOF, -1 = classified failure. */
    if (mino_net_recv_raw(S, ts->fd, buf, len, &got, ts->read_timeout_ms,
                          kind, code, msg, msg_cap) < 0)
        return -1;
    if (got == 0) {
        *kind = "tls";
        *code = "MTL003";
        snprintf(msg, msg_cap,
                 "tls: connection closed by peer mid-record");
        return -1;
    }
    br_ssl_engine_recvrec_ack(&ts->cc.eng, got);
    return 0;
}

/* Run the engine against the socket until the handshake completes or
 * fails. 0 ok, -1 with kind/code/msg filled. */
static int tls_handshake(mino_state *S, mino_tls_sock_t *ts,
                         const char **kind, const char **code,
                         char *msg, size_t msg_cap)
{
    for (;;) {
        unsigned st = br_ssl_engine_current_state(&ts->cc.eng);
        if (st & BR_SSL_CLOSED) {
            tls_engine_error_text("tls-connect", "handshake",
                                  &ts->cc.eng, msg, msg_cap);
            *kind = "tls";
            *code = "MTL002";
            return -1;
        }
        /* SENDAPP/RECVAPP mean the engine takes application data
         * now, even while it also wants records (RECVREC) from the
         * peer: the handshake is complete in that state. */
        if (st & (BR_SSL_SENDAPP | BR_SSL_RECVAPP)) return 0;
        if (st & BR_SSL_SENDREC) {
            if (tls_drain_sendrec(S, ts, kind, code, msg, msg_cap) != 0)
                return -1;
            continue;
        }
        if (st & BR_SSL_RECVREC) {
            if (tls_fill_recvrec(S, ts, kind, code, msg, msg_cap) != 0)
                return -1;
            continue;
        }
        tls_engine_error_text("tls-connect", "handshake",
                              &ts->cc.eng, msg, msg_cap);
        *kind = "tls";
        *code = "MTL003";
        return -1;
    }
}

/* Decrypt up to n bytes into buf. Returns 1 got bytes, 0 clean close,
 * -1 failure (kind/code/msg filled). */
static int tls_read_engine(mino_state *S, mino_tls_sock_t *ts,
                           unsigned char *buf, size_t n, size_t *got,
                           const char **kind, const char **code,
                           char *msg, size_t msg_cap)
{
    for (;;) {
        unsigned st = br_ssl_engine_current_state(&ts->cc.eng);
        if (st & BR_SSL_CLOSED) {
            int err = br_ssl_engine_last_error(&ts->cc.eng);
            if (err == 0) return 0;
            tls_engine_error_text("tls-read", "read", &ts->cc.eng, msg,
                                  msg_cap);
            *kind = "tls";
            *code = "MTL003";
            return -1;
        }
        if (st & BR_SSL_RECVAPP) {
            size_t len;
            unsigned char *app = br_ssl_engine_recvapp_buf(&ts->cc.eng,
                                                           &len);
            size_t take;
            if (app == NULL || len == 0) {
                *kind = "tls";
                *code = "MTL003";
                snprintf(msg, msg_cap,
                         "tls-read: empty application buffer");
                return -1;
            }
            take = n < len ? n : len;
            memcpy(buf, app, take);
            br_ssl_engine_recvapp_ack(&ts->cc.eng, take);
            *got = take;
            return 1;
        }
        if (st & BR_SSL_SENDREC) {
            if (tls_drain_sendrec(S, ts, kind, code, msg, msg_cap) != 0)
                return -1;
            continue;
        }
        if (st & BR_SSL_RECVREC) {
            if (tls_fill_recvrec(S, ts, kind, code, msg, msg_cap) != 0)
                return -1;
            continue;
        }
        *kind = "tls";
        *code = "MTL003";
        snprintf(msg, msg_cap, "tls-read: engine has no work pending");
        return -1;
    }
}

/* ---- socket argument helpers ---- */

static mino_tls_sock_t *tls_sock_arg(mino_state *S, mino_val *v,
                                     const char *who)
{
    if (v == NULL || mino_type_of(v) != MINO_HANDLE
        || v->as.handle.tag == NULL
        || strcmp(v->as.handle.tag, TLS_SOCK_TAG) != 0
        || v->as.handle.ptr == NULL) {
        char msg[160];
        snprintf(msg, sizeof(msg), "%s: argument must be a TLS socket",
                 who);
        prim_throw_classified(S, "eval/type", "MTY001", msg);
        return NULL;
    }
    return (mino_tls_sock_t *)v->as.handle.ptr;
}

static void tls_sock_finalize(void *ptr, const char *tag)
{
    mino_tls_sock_t *ts = (mino_tls_sock_t *)ptr;
    (void)tag;
    if (ts == NULL) return;
    /* No engine teardown exists (BearSSL contexts are plain memory);
     * skipping close_notify on collection is acceptable, the
     * descriptor release is what must not leak. */
    if (!ts->closed) mino_net_close_raw(ts->fd);
    free(ts->iobuf);
    free(ts);
}

/* :insecure? opt: absent/nil -> 0; boolean -> its value; anything
 * else throws. Returns 0 ok. */
static int tls_opt_insecure(mino_state *S, mino_val *opts, int *out)
{
    mino_val *v;
    *out = 0;
    if (opts == NULL || mino_type_of(opts) != MINO_MAP) return 0;
    v = map_get_val(opts, mino_keyword(S, "insecure?"));
    if (v == NULL || mino_type_of(v) == MINO_NIL) return 0;
    if (mino_type_of(v) != MINO_BOOL) {
        prim_throw_classified(S, "eval/contract", "MCT001",
                              "tls-connect: opts key :insecure? must be "
                              "a boolean");
        return -1;
    }
    *out = mino_val_bool_get(v);
    return 0;
}

/* ---- connect ---- */

/* (tls-connect sock host opts?) / (tls-connect host port opts?) */
static mino_val *prim_tls_connect(mino_state *S, mino_val *args,
                                  mino_env *env)
{
    mino_val *a1, *a2, *opts = NULL;
    mino_val *hv;
    mino_tls_sock_t *ts = NULL;
    unsigned char *iobuf = NULL;
    char host[256];
    long long read_ms, write_ms;
    long long port;
    int insecure;
    int have_port_arity;
    const char *kind = "tls", *code = "MTL003";
    char msg[240];
    (void)env;

    if (!mino_is_cons(args) || !mino_is_cons(args->as.cons.cdr)) {
        return prim_throw_classified(S, "eval/arity", "MAR001",
                                     "tls-connect requires a socket and a "
                                     "host, or a host and a port");
    }
    a1 = args->as.cons.car;
    a2 = args->as.cons.cdr->as.cons.car;
    args = args->as.cons.cdr->as.cons.cdr;
    if (mino_is_cons(args)) {
        opts = args->as.cons.car;
        if (mino_is_cons(args->as.cons.cdr)) {
            return prim_throw_classified(S, "eval/arity", "MAR001",
                                         "tls-connect takes at most 3 "
                                         "arguments");
        }
        if (opts != NULL && mino_type_of(opts) != MINO_MAP
            && mino_type_of(opts) != MINO_NIL) {
            return prim_throw_classified(S, "eval/type", "MTY001",
                                         "tls-connect: opts must be a map");
        }
    }

    if (a1 != NULL && mino_type_of(a1) == MINO_HANDLE) {
        /* Socket-arity: host is the second argument. */
        have_port_arity = 0;
        if (a2 == NULL || mino_type_of(a2) != MINO_STRING) {
            return prim_throw_classified(S, "eval/type", "MTY001",
                                         "tls-connect: second argument "
                                         "must be the host name string");
        }
    } else if (a1 != NULL && mino_type_of(a1) == MINO_STRING) {
        /* Host+port arity: the host is the first argument; the port
         * is validated here so the arity error surfaces before any
         * connection attempt. */
        have_port_arity = 1;
        if (!as_long(a2, &port) || port < 1 || port > 65535) {
            return prim_throw_classified(S, "eval/contract", "MCT001",
                                         "tls-connect: port must be an "
                                         "integer in 1..65535");
        }
    } else {
        return prim_throw_classified(S, "eval/type", "MTY001",
                                     "tls-connect: first argument must "
                                     "be a net socket or a host string");
    }
    {
        const mino_val *host_val = have_port_arity ? a1 : a2;
        if (host_val->as.s.len == 0
            || host_val->as.s.len >= sizeof host) {
            return prim_throw_classified(S, "eval/contract", "MCT001",
                                         "tls-connect: host name is empty "
                                         "or too long");
        }
        memcpy(host, host_val->as.s.data, host_val->as.s.len);
        host[host_val->as.s.len] = '\0';
    }
    if (tls_opt_insecure(S, opts, &insecure) != 0) return NULL;
    if (net_opt_ms(S, opts, "read-timeout", TLS_DEFAULT_READ_TIMEOUT_MS,
                   &read_ms) != 0)
        return NULL;
    if (net_opt_ms(S, opts, "write-timeout", TLS_DEFAULT_WRITE_TIMEOUT_MS,
                   &write_ms) != 0)
        return NULL;

    /* Pre-flight the handle value before any TLS resource exists:
     * its allocation can throw for OOM, and after this point every
     * failure path holds an adopted descriptor, an iobuf, or the
     * socket record that such a throw at the end would strand. The
     * value stays pinned and is filled once the handshake succeeds. */
    hv = mino_handle_ex(S, NULL, TLS_SOCK_TAG, tls_sock_finalize);
    gc_pin(hv);

    ts = (mino_tls_sock_t *)calloc(1, sizeof *ts);
    if (ts == NULL) {
        gc_unpin(1);
        return prim_throw_classified(S, "internal", "MIN001",
                                     "tls-connect: out of memory");
    }

    if (have_port_arity) {
        /* Build the TCP connection through the net prim (DNS, non-
         * blocking connect, winsock init all live there), then adopt
         * its descriptor. prim_net_connect allocates GC values; the
         * malloc'd ts is not GC-tracked, so no pin is needed. */
        mino_val *port_args = mino_cons(S, a1,
                                        mino_cons(S, a2,
                                                  mino_cons(S, opts,
                                                            mino_nil(S))));
        mino_val *sock_val = prim_net_connect(S, port_args, env);
        if (sock_val == NULL) {
            free(ts);
            gc_unpin(1);
            return NULL;
        }
        if (mino_net_adopt(S, sock_val, &ts->fd, &ts->read_timeout_ms,
                           &ts->write_timeout_ms) != 0) {
            free(ts);
            gc_unpin(1);
            return NULL;
        }
    } else {
        if (mino_net_adopt(S, a1, &ts->fd, &ts->read_timeout_ms,
                           &ts->write_timeout_ms) != 0) {
            free(ts);
            gc_unpin(1);
            return NULL;
        }
    }

    ts->read_timeout_ms  = read_ms;
    ts->write_timeout_ms = write_ms;
    mino_net_apply_timeouts_raw(ts->fd, read_ms, write_ms);

    iobuf = (unsigned char *)malloc(BR_SSL_BUFSIZE_BIDI);
    if (iobuf == NULL) {
        mino_net_close_raw(ts->fd);
        free(ts);
        gc_unpin(1);
        return prim_throw_classified(S, "internal", "MIN001",
                                     "tls-connect: out of memory");
    }
    ts->iobuf = iobuf;

    br_ssl_client_init_full(&ts->cc, &ts->xc, tls_anchors,
                            tls_anchor_count);
    if (insecure) {
        ts->skip.vtable = &tls_skip_vtable;
        br_ssl_engine_set_x509(&ts->cc.eng, &ts->skip.vtable);
    }
    br_ssl_engine_set_buffer(&ts->cc.eng, iobuf, BR_SSL_BUFSIZE_BIDI, 1);
    if (!br_ssl_client_reset(&ts->cc, host, 0)) {
        int err = br_ssl_engine_last_error(&ts->cc.eng);
        mino_net_close_raw(ts->fd);
        free(iobuf);
        free(ts);
        gc_unpin(1);
        snprintf(msg, sizeof(msg), "tls-connect: cannot start handshake "
                 "with %s: %s (code %d)", host,
                 tls_engine_err_desc(err), err);
        return prim_throw_classified(S, "tls", "MTL001", msg);
    }
    if (tls_handshake(S, ts, &kind, &code, msg, sizeof msg) != 0) {
        mino_net_close_raw(ts->fd);
        free(iobuf);
        free(ts);
        gc_unpin(1);
        return prim_throw_classified(S, kind, code, msg);
    }
    hv->as.handle.ptr = ts;
    gc_unpin(1);
    return hv;
}

/* ---- read ---- */

/* (tls-read sock n) -> up to n decrypted bytes as soon as any arrive;
 * nil on clean close before the first byte. */
static mino_val *prim_tls_read(mino_state *S, mino_val *args, mino_env *env)
{
    mino_val *sock_val, *n_val;
    mino_tls_sock_t *ts;
    long long n;
    unsigned char *buf = NULL;
    size_t got = 0;
    int rc;
    const char *kind = "tls", *code = "MTL003";
    char msg[240];
    (void)env;

    if (!mino_is_cons(args) || !mino_is_cons(args->as.cons.cdr)
        || mino_is_cons(args->as.cons.cdr->as.cons.cdr)) {
        return prim_throw_classified(S, "eval/arity", "MAR001",
                                     "tls-read requires a TLS socket and "
                                     "a byte count");
    }
    sock_val = args->as.cons.car;
    n_val    = args->as.cons.cdr->as.cons.car;
    ts = tls_sock_arg(S, sock_val, "tls-read");
    if (ts == NULL) return NULL;
    if (ts->closed) {
        return prim_throw_classified(S, "tls", "MTL004",
                                     "tls-read: socket is closed");
    }
    if (!as_long(n_val, &n) || n < 0) {
        return prim_throw_classified(S, "eval/contract", "MCT001",
                                     "tls-read: n must be a non-negative "
                                     "integer");
    }
    if (n == 0) return mino_bytes(S, NULL, 0);
    if ((unsigned long long)n > SIZE_MAX) {
        return prim_throw_classified(S, "eval/contract", "MCT001",
                                     "tls-read: n is too large");
    }
    buf = (unsigned char *)malloc((size_t)n);
    if (buf == NULL) {
        return prim_throw_classified(S, "internal", "MIN001",
                                     "tls-read: out of memory");
    }
    /* The engine pump yields inside; pin the handle so a concurrent
     * collection cannot finalize the TLS record mid-read. */
    gc_pin(sock_val);
    rc = tls_read_engine(S, ts, buf, (size_t)n, &got, &kind, &code, msg,
                         sizeof msg);
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

/* (tls-read-all sock [cap]) -> decrypted bytes until clean close.
 * cap is a byte count or a map with :max-bytes (default 16 MiB);
 * exceeding it throws :net/overflow. */
static mino_val *prim_tls_read_all(mino_state *S, mino_val *args,
                                   mino_env *env)
{
    mino_val *sock_val, *cap_val = NULL;
    mino_tls_sock_t *ts;
    long long max_bytes = TLS_READ_ALL_DEFAULT_MAX_BYTES;
    unsigned char *buf = NULL;
    unsigned char chunk[16384];
    size_t len = 0, cap = 0;
    mino_val *result;
    const char *kind = "tls", *code = "MTL003";
    char msg[240];
    (void)env;

    if (!mino_is_cons(args)) {
        return prim_throw_classified(S, "eval/arity", "MAR001",
                                     "tls-read-all requires a TLS socket");
    }
    sock_val = args->as.cons.car;
    args = args->as.cons.cdr;
    if (mino_is_cons(args)) {
        cap_val = args->as.cons.car;
        if (mino_is_cons(args->as.cons.cdr)) {
            return prim_throw_classified(S, "eval/arity", "MAR001",
                                         "tls-read-all takes at most 2 "
                                         "arguments");
        }
        if (cap_val != NULL && mino_type_of(cap_val) == MINO_MAP) {
            if (net_opt_ms(S, cap_val, "max-bytes",
                           TLS_READ_ALL_DEFAULT_MAX_BYTES,
                           &max_bytes) != 0)
                return NULL;
        } else if (cap_val != NULL && mino_type_of(cap_val) != MINO_NIL) {
            if (!as_long(cap_val, &max_bytes) || max_bytes < 0) {
                return prim_throw_classified(S, "eval/contract", "MCT001",
                                             "tls-read-all: cap must be a "
                                             "non-negative integer");
            }
        }
    }
    ts = tls_sock_arg(S, sock_val, "tls-read-all");
    if (ts == NULL) return NULL;
    if (ts->closed) {
        return prim_throw_classified(S, "tls", "MTL004",
                                     "tls-read-all: socket is closed");
    }

    /* Pinned across the loop: the pump re-reads the TLS record after
     * every yield window, so the handle must stay rooted against a
     * concurrent collection. */
    gc_pin(sock_val);
    for (;;) {
        size_t got = 0;
        int rc = tls_read_engine(S, ts, chunk, sizeof chunk, &got,
                                 &kind, &code, msg, sizeof msg);
        if (rc < 0) {
            free(buf);
            gc_unpin(1);
            return prim_throw_classified(S, kind, code, msg);
        }
        if (rc == 0) break;
        if ((long long)(len + got) > max_bytes) {
            char m[160];
            free(buf);
            gc_unpin(1);
            snprintf(m, sizeof(m),
                     "tls-read-all: exceeded cap of %lld bytes",
                     max_bytes);
            return prim_throw_classified(S, "net/overflow", "MNE005", m);
        }
        if (len + got > cap) {
            size_t new_cap = cap == 0 ? sizeof chunk : cap * 2;
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
                                             "tls-read-all: out of memory");
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

/* (tls-write sock data) -> byte count written. data is a string
 * (UTF-8 bytes) or bytes. */
static mino_val *prim_tls_write(mino_state *S, mino_val *args, mino_env *env)
{
    mino_val *sock_val, *data_val;
    mino_tls_sock_t *ts;
    const unsigned char *data;
    size_t len, sent = 0;
    const char *kind = "tls", *code = "MTL003";
    char msg[240];
    (void)env;

    if (!mino_is_cons(args) || !mino_is_cons(args->as.cons.cdr)
        || mino_is_cons(args->as.cons.cdr->as.cons.cdr)) {
        return prim_throw_classified(S, "eval/arity", "MAR001",
                                     "tls-write requires a TLS socket and "
                                     "data");
    }
    sock_val = args->as.cons.car;
    data_val = args->as.cons.cdr->as.cons.car;
    ts = tls_sock_arg(S, sock_val, "tls-write");
    if (ts == NULL) return NULL;
    if (ts->closed) {
        return prim_throw_classified(S, "tls", "MTL004",
                                     "tls-write: socket is closed");
    }
    if (data_val != NULL && mino_type_of(data_val) == MINO_STRING) {
        data = (const unsigned char *)data_val->as.s.data;
        len  = data_val->as.s.len;
    } else if (data_val != NULL && mino_type_of(data_val) == MINO_BYTES) {
        data = mino_bytes_data(data_val);
        len  = mino_bytes_len(data_val);
    } else {
        return prim_throw_classified(S, "eval/type", "MTY001",
                                     "tls-write: data must be a string or "
                                     "bytes");
    }

    /* Both the TLS record and the payload interior pointer are
     * re-read across yield windows inside the pump; pin the handle
     * and the data value so a concurrent collection cannot finalize
     * either mid-write. */
    gc_pin(sock_val);
    gc_pin(data_val);
    while (sent < len) {
        unsigned st = br_ssl_engine_current_state(&ts->cc.eng);
        if (st & BR_SSL_CLOSED) {
            tls_engine_error_text("tls-write", "write", &ts->cc.eng, msg,
                                  sizeof msg);
            gc_unpin(2);
            return prim_throw_classified(S, "tls", "MTL003", msg);
        }
        if (st & BR_SSL_SENDREC) {
            if (tls_drain_sendrec(S, ts, &kind, &code, msg,
                                  sizeof msg) != 0) {
                gc_unpin(2);
                return prim_throw_classified(S, kind, code, msg);
            }
            continue;
        }
        if (st & BR_SSL_SENDAPP) {
            size_t space;
            unsigned char *app = br_ssl_engine_sendapp_buf(&ts->cc.eng,
                                                           &space);
            size_t take;
            if (app == NULL || space == 0) {
                gc_unpin(2);
                return prim_throw_classified(S, "tls", "MTL003",
                                             "tls-write: no application "
                                             "buffer space");
            }
            take = len - sent < space ? len - sent : space;
            memcpy(app, data + sent, take);
            br_ssl_engine_sendapp_ack(&ts->cc.eng, take);
            sent += take;
            continue;
        }
        /* Only peer input pending: the engine cannot take more app
         * data until the record machinery advances, so feed it. This
         * blocks up to the read timeout rather than spinning; the
         * usual content here is a peer close_notify or alert. */
        if (tls_fill_recvrec(S, ts, &kind, &code, msg, sizeof msg) != 0) {
            gc_unpin(2);
            return prim_throw_classified(S, kind, code, msg);
        }
    }
    br_ssl_engine_flush(&ts->cc.eng, 0);
    if (tls_drain_sendrec(S, ts, &kind, &code, msg, sizeof msg) != 0) {
        gc_unpin(2);
        return prim_throw_classified(S, kind, code, msg);
    }
    gc_unpin(2);
    return mino_int(S, (long long)sent);
}

/* ---- close ---- */

/* (tls-close sock) -> nil. Sends close_notify when the connection is
 * still live (best effort: a silent peer times out rather than
 * failing the close), then releases the descriptor. Idempotent. */
static mino_val *prim_tls_close(mino_state *S, mino_val *args, mino_env *env)
{
    mino_val *sock_val;
    mino_tls_sock_t *ts;
    const char *kind = "tls", *code = "MTL003";
    char msg[240];
    (void)env;

    if (!mino_is_cons(args) || mino_is_cons(args->as.cons.cdr)) {
        return prim_throw_classified(S, "eval/arity", "MAR001",
                                     "tls-close requires one argument");
    }
    sock_val = args->as.cons.car;
    ts = tls_sock_arg(S, sock_val, "tls-close");
    if (ts == NULL) return NULL;
    if (ts->closed) return mino_nil(S);

    /* The close_notify pump yields inside; pin the handle so a
     * concurrent collection cannot finalize the record mid-close. */
    gc_pin(sock_val);
    br_ssl_engine_close(&ts->cc.eng);
    for (;;) {
        unsigned st = br_ssl_engine_current_state(&ts->cc.eng);
        if (st & BR_SSL_CLOSED) break;
        if (st & BR_SSL_SENDREC) {
            if (tls_drain_sendrec(S, ts, &kind, &code, msg,
                                  sizeof msg) != 0)
                break;
            continue;
        }
        if (st & BR_SSL_RECVREC) {
            if (tls_fill_recvrec(S, ts, &kind, &code, msg,
                                 sizeof msg) != 0)
                break;
            continue;
        }
        break;
    }
    gc_unpin(1);
    mino_net_close_raw(ts->fd);
    ts->closed = 1;
    return mino_nil(S);
}

/* ---- handle bridge for the keep-alive pool (prim/pool.c) ---- */

const char *mino_tls_sock_tag(void)
{
    return TLS_SOCK_TAG;
}

/* Borrow the descriptor of a live TLS-socket handle for a zero-timeout
 * liveness poll. 1 with *fd_out set for an open session, 0 for any
 * other value (wrong tag, closed, NULL). */
int mino_tls_handle_fd(mino_val *v, uintptr_t *fd_out)
{
    mino_tls_sock_t *ts;
    if (v == NULL || mino_type_of(v) != MINO_HANDLE
        || v->as.handle.tag == NULL
        || strcmp(v->as.handle.tag, TLS_SOCK_TAG) != 0
        || v->as.handle.ptr == NULL) {
        return 0;
    }
    ts = (mino_tls_sock_t *)v->as.handle.ptr;
    if (ts->closed) return 0;
    *fd_out = ts->fd;
    return 1;
}

/* Release a TLS-socket handle's descriptor without close_notify (the
 * pool drops a peer it no longer trusts mid-conversation; skipping the
 * shutdown pump keeps the release non-blocking, mirroring the
 * finalizer). Idempotent. */
void mino_tls_handle_close(mino_val *v)
{
    mino_tls_sock_t *ts;
    if (v == NULL || mino_type_of(v) != MINO_HANDLE
        || v->as.handle.tag == NULL
        || strcmp(v->as.handle.tag, TLS_SOCK_TAG) != 0
        || v->as.handle.ptr == NULL) {
        return;
    }
    ts = (mino_tls_sock_t *)v->as.handle.ptr;
    if (!ts->closed) {
        mino_net_close_raw(ts->fd);
        ts->closed = 1;
    }
}

/* ---- install ---- */

static const mino_prim_def k_prims_tls[] = {
    {"tls-connect",  prim_tls_connect,
     "Starts a TLS client session over a connected net socket "
     "((tls-connect sock host opts?)) or a fresh TCP connection "
     "((tls-connect host port opts?)). SNI is always sent from host. "
     "Verification is on by default: chain against the vendored "
     "Mozilla roots and SAN/CN host match; opts key :insecure? true "
     "skips verification for local fixtures. Other opts keys: "
     ":read-timeout :write-timeout (and :connect-timeout in the "
     "host+port form). Throws :tls on handshake or verification "
     "failure."},
    {"tls-read",     prim_tls_read,
     "Reads up to n decrypted bytes from a TLS socket as soon as any "
     "arrive. Returns bytes (a short read is normal) or nil on clean "
     "close before the first byte. Throws :net/timeout on read "
     "timeout, :tls on protocol failure."},
    {"tls-read-all", prim_tls_read_all,
     "Reads decrypted bytes from a TLS socket until the peer closes "
     "cleanly. Returns bytes. Optional cap argument is a byte count "
     "or a map with :max-bytes (default 16777216); exceeding it "
     "throws :net/overflow."},
    {"tls-write",    prim_tls_write,
     "Writes a string (UTF-8 bytes) or bytes to a TLS socket. Returns "
     "the number of bytes written. Throws :net/timeout on write "
     "timeout, :tls on protocol failure."},
    {"tls-close",    prim_tls_close,
     "Closes a TLS socket, sending close_notify first. Returns nil. "
     "Idempotent; dropped sockets are also closed by the garbage "
     "collector."},
};

const size_t k_prims_tls_count =
    sizeof(k_prims_tls) / sizeof(k_prims_tls[0]);

#ifndef _WIN32
static pthread_once_t tls_anchors_once = PTHREAD_ONCE_INIT;
#endif

#ifdef _WIN32
static BOOL CALLBACK tls_build_anchors_once(PINIT_ONCE once, PVOID param,
                                            PVOID *ctx)
{
    (void)once; (void)param; (void)ctx;
    tls_build_anchors();
    return TRUE;
}
#endif

void mino_install_tls(mino_state *S, mino_env *env)
{
    mino_env *core_env = ns_env_ensure(S, "clojure.core");
    (void)env;
    /* The anchor tables are file-static and installs can race from
     * independent states on different host threads (each state has
     * its own lock; nothing cross-state guards these). The build is
     * a pure function of const vendored data, so process-wide
     * once-init both serializes it and makes it idempotent. */
#ifdef _WIN32
    {
        static INIT_ONCE once = INIT_ONCE_STATIC_INIT;
        InitOnceExecuteOnce(&once, tls_build_anchors_once, NULL, NULL);
    }
#else
    pthread_once(&tls_anchors_once, tls_build_anchors);
#endif
    prim_install_table_with_capability(S, core_env, "clojure.core",
                                       k_prims_tls, k_prims_tls_count,
                                       "net");
    S->caps_installed |= MINO_CAP_NET;
}
