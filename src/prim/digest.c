/*
 * digest.c -- message digest primitives: md5, sha1, sha256, HMAC-SHA256
 * over the vendored BearSSL implementations, and crc32 over the miniz
 * mz_crc32 the gzip layer verifies trailers with.
 *
 * Digests take a string or bytes value (a string contributes its UTF-8
 * bytes, like the codec prims) and return a bytes value; crc32 returns
 * the unsigned 32-bit CRC as an integer. Every walk is a streaming
 * update bounded by the input length into a fixed stack buffer, so no
 * allocation is sized from untrusted data on any path.
 */

#include "prim/internal.h"
#include "mino.h"

/* bearssl's public headers probe BR_DOXYGEN_IGNORE with #if; the macro
 * exists only inside the library's own build, so -Wundef flags it.
 * Scoped silence, the same pragma treatment tls.c gives its headers. */
#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wundef"
#endif
#include "bearssl_hmac.h"
#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic pop
#endif
/* Trim define: without it miniz.h declares static zlib-name wrappers
 * that trip -Wunused-function in every TU but miniz_inflate.c. */
#define MINIZ_NO_ZLIB_COMPATIBLE_NAMES 1
#include "miniz.h"

#include <stdint.h>

/* Extract a byte view from a string or bytes argument. Returns 0 on
 * any other type. */
static int digest_text_arg(const mino_val *v, const unsigned char **data,
                           size_t *len)
{
    if (v == NULL) return 0;
    if (mino_type_of(v) == MINO_STRING) {
        *data = (const unsigned char *)v->as.s.data;
        *len  = v->as.s.len;
        return 1;
    }
    if (mino_is_bytes(v)) {
        *data = mino_bytes_data(v);
        *len  = mino_bytes_len(v);
        return 1;
    }
    return 0;
}

/* Check exactly one argument, answer its byte view. Returns 0 with the
 * classified throw already raised on a bad shape. */
static int digest_one_arg(mino_state *S, mino_val *args, const char *who,
                          const unsigned char **data, size_t *len)
{
    mino_val *v;
    char      msg[96];
    if (!mino_is_cons(args) || mino_is_cons(args->as.cons.cdr)) {
        snprintf(msg, sizeof(msg), "%s requires one argument", who);
        prim_throw_classified(S, "eval/arity", "MAR001", msg);
        return 0;
    }
    v = args->as.cons.car;
    if (!digest_text_arg(v, data, len)) {
        snprintf(msg, sizeof(msg),
                 "%s: argument must be a string or bytes value", who);
        prim_throw_classified(S, "eval/type", "MTY001", msg);
        return 0;
    }
    return 1;
}

static mino_val *digest_sha256(mino_state *S, const unsigned char *data,
                               size_t len)
{
    br_sha256_context cc;
    unsigned char     out[br_sha256_SIZE];
    br_sha256_init(&cc);
    br_sha256_update(&cc, data, len);
    br_sha256_out(&cc, out);
    return mino_bytes(S, out, sizeof out);
}

static mino_val *digest_sha1(mino_state *S, const unsigned char *data,
                             size_t len)
{
    br_sha1_context cc;
    unsigned char   out[br_sha1_SIZE];
    br_sha1_init(&cc);
    br_sha1_update(&cc, data, len);
    br_sha1_out(&cc, out);
    return mino_bytes(S, out, sizeof out);
}

static mino_val *digest_md5(mino_state *S, const unsigned char *data,
                            size_t len)
{
    br_md5_context cc;
    unsigned char  out[br_md5_SIZE];
    br_md5_init(&cc);
    br_md5_update(&cc, data, len);
    br_md5_out(&cc, out);
    return mino_bytes(S, out, sizeof out);
}

/* (sha256 data) / (sha1 data) / (md5 data) -- string or bytes in, the
 * raw digest as a bytes value out. */
static mino_val *prim_sha256(mino_state *S, mino_val *args, mino_env *env)
{
    const unsigned char *data;
    size_t               len;
    (void)env;
    if (!digest_one_arg(S, args, "sha256", &data, &len)) return NULL;
    return digest_sha256(S, data, len);
}

static mino_val *prim_sha1(mino_state *S, mino_val *args, mino_env *env)
{
    const unsigned char *data;
    size_t               len;
    (void)env;
    if (!digest_one_arg(S, args, "sha1", &data, &len)) return NULL;
    return digest_sha1(S, data, len);
}

static mino_val *prim_md5(mino_state *S, mino_val *args, mino_env *env)
{
    const unsigned char *data;
    size_t               len;
    (void)env;
    if (!digest_one_arg(S, args, "md5", &data, &len)) return NULL;
    return digest_md5(S, data, len);
}

/* (hmac-sha256 key data) -- HMAC over SHA-256 (RFC 2104), key and data
 * each string or bytes, the full 32-byte tag as a bytes value. Keys of
 * any length are valid: long keys are hashed down, short keys are
 * zero-padded, per the RFC. */
static mino_val *prim_hmac_sha256(mino_state *S, mino_val *args,
                                  mino_env *env)
{
    const unsigned char  *key, *data;
    size_t                key_len, len;
    br_hmac_key_context   kc;
    br_hmac_context       hc;
    unsigned char         out[br_sha256_SIZE];
    (void)env;
    if (!mino_is_cons(args) || !mino_is_cons(args->as.cons.cdr)
        || mino_is_cons(args->as.cons.cdr->as.cons.cdr)) {
        return prim_throw_classified(S, "eval/arity", "MAR001",
                                     "hmac-sha256 requires two arguments");
    }
    if (!digest_text_arg(args->as.cons.car, &key, &key_len)) {
        return prim_throw_classified(S, "eval/type", "MTY001",
                                     "hmac-sha256: key must be a string or "
                                     "bytes value");
    }
    if (!digest_text_arg(args->as.cons.cdr->as.cons.car, &data, &len)) {
        return prim_throw_classified(S, "eval/type", "MTY001",
                                     "hmac-sha256: data must be a string or "
                                     "bytes value");
    }
    br_hmac_key_init(&kc, &br_sha256_vtable, key, key_len);
    br_hmac_init(&hc, &kc, 0);
    br_hmac_update(&hc, data, len);
    br_hmac_out(&hc, out);
    return mino_bytes(S, out, sizeof out);
}

/* (crc32 data) -- the gzip-spec CRC-32 (same polynomial and byte order
 * as zlib), returned as an unsigned integer in 0..2^32-1. */
static mino_val *prim_crc32(mino_state *S, mino_val *args, mino_env *env)
{
    const unsigned char *data;
    size_t               len;
    (void)env;
    if (!digest_one_arg(S, args, "crc32", &data, &len)) return NULL;
    return mino_int(S, (int64_t)(uint32_t)mz_crc32(MZ_CRC32_INIT, data,
                                                   len));
}

const mino_prim_def k_prims_digest[] = {
    {"sha256", prim_sha256,
     "Computes the SHA-256 digest of a string or bytes value (a string "
     "contributes its UTF-8 bytes) and returns the 32-byte digest as a "
     "bytes value. Pair with hex-encode for display."},
    {"sha1", prim_sha1,
     "Computes the SHA-1 digest of a string or bytes value and returns "
     "the 20-byte digest as a bytes value. SHA-1 is collision-broken; "
     "prefer sha256 unless a protocol requires SHA-1."},
    {"md5", prim_md5,
     "Computes the MD5 digest of a string or bytes value and returns "
     "the 16-byte digest as a bytes value. MD5 is collision-broken; "
     "use only for non-security checksums."},
    {"hmac-sha256", prim_hmac_sha256,
     "Computes the HMAC-SHA256 tag (RFC 2104) of data under key, each a "
     "string or bytes value, and returns the full 32-byte tag as a "
     "bytes value."},
    {"crc32", prim_crc32,
     "Computes the gzip-spec CRC-32 of a string or bytes value and "
     "returns it as an unsigned integer, 0 to 2^32-1."},
};

const size_t k_prims_digest_count =
    sizeof(k_prims_digest) / sizeof(k_prims_digest[0]);
