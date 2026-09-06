/*
 * random.c -- secure random primitives over the OS CSPRNG:
 * secure-rand-bytes, rand-hex, rand-token.
 *
 * The single entropy edge is prim_os_entropy: getentropy on POSIX,
 * BCryptGenRandom on Windows. There is no PRNG fallback; when the OS
 * source fails the prims throw and never return weak bytes, so a
 * predictable token can never reach a caller. rand-hex and rand-token
 * draw n random bytes and encode them (lowercase hex, unpadded
 * base64url) so the entropy count is explicit in the argument.
 */

#define _POSIX_C_SOURCE 200809L
#if defined(__APPLE__)
#  define _DARWIN_C_SOURCE 1
#endif

#include "prim/internal.h"
#include "mino.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#  define WIN32_LEAN_AND_MEAN
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  include <windows.h>
#  include <bcrypt.h>
#else
/* getentropy: POSIX 2024, but the libc headers that declare it
 * disagree (sys/random.h on glibc, unistd.h on musl) and the
 * feature-test gating varies; one prototype covers every target,
 * matching the tls.c seeder's rationale. */
int getentropy(void *buf, size_t buflen);
#endif

/* getentropy fails past 256 bytes; chunk every request to the ceiling. */
#define OS_ENTROPY_CHUNK 256u

/* Fill buf with len CSPRNG bytes. Returns 0 on success, -1 on OS
 * failure. On failure the buffer is zeroed so no stale stack contents
 * masquerade as entropy. */
int prim_os_entropy(unsigned char *buf, size_t len)
{
    size_t off = 0;
    if (len == 0) return 0;
    if (buf == NULL) return -1;
#ifdef _WIN32
    while (off < len) {
        size_t want = len - off;
        ULONG  n    = (want > 0xFFFFFFFFu) ? 0xFFFFFFFFu : (ULONG)want;
        NTSTATUS rc = BCryptGenRandom(NULL, buf + off, n,
                                      BCRYPT_USE_SYSTEM_PREFERRED_RNG);
        if (!BCRYPT_SUCCESS(rc)) {
            memset(buf, 0, len);
            return -1;
        }
        off += n;
    }
#else
    while (off < len) {
        size_t want = len - off;
        if (want > OS_ENTROPY_CHUNK) want = OS_ENTROPY_CHUNK;
        if (getentropy(buf + off, want) != 0) {
            memset(buf, 0, len);
            return -1;
        }
        off += want;
    }
#endif
    return 0;
}

/* Read the single non-negative int argument shared by all three prims.
 * Returns 0 on success and fills *out; on any arity/type/range error it
 * throws through S and returns -1. */
static int random_count_arg(mino_state *S, const char *who, mino_val *args,
                            long long *out)
{
    mino_val *v;
    long long n;
    char      msg[96];

    if (!mino_is_cons(args) || mino_is_cons(args->as.cons.cdr)) {
        snprintf(msg, sizeof(msg), "%s requires one argument", who);
        prim_throw_classified(S, "eval/arity", "MAR001", msg);
        return -1;
    }
    v = args->as.cons.car;
    if (!as_long(v, &n)) {
        snprintf(msg, sizeof(msg), "%s: count must be an int", who);
        prim_throw_classified(S, "eval/type", "MTY001", msg);
        return -1;
    }
    if (n < 0) {
        snprintf(msg, sizeof(msg), "%s: count must be non-negative", who);
        prim_throw_classified(S, "eval/contract", "MCT001", msg);
        return -1;
    }
    *out = n;
    return 0;
}

/* (secure-rand-bytes n) -- n bytes from the OS CSPRNG as a bytes value. */
static mino_val *prim_secure_rand_bytes(mino_state *S, mino_val *args,
                                        mino_env *env)
{
    long long      n;
    unsigned char *buf;
    mino_val      *result;
    (void)env;

    if (random_count_arg(S, "secure-rand-bytes", args, &n) != 0) return NULL;
    if (n == 0) return mino_bytes(S, NULL, 0);
    if ((uint64_t)n > SIZE_MAX) {
        return prim_throw_classified(S, "eval/bounds", "MBD001",
                                     "secure-rand-bytes: count too large");
    }
    buf = (unsigned char *)malloc((size_t)n);
    if (buf == NULL) {
        return prim_throw_classified(S, "internal", "MIN001",
                                     "secure-rand-bytes: out of memory");
    }
    if (prim_os_entropy(buf, (size_t)n) != 0) {
        free(buf);
        return prim_throw_classified(S, "host", "MHO001",
                                     "secure-rand-bytes: OS entropy source "
                                     "failed");
    }
    result = mino_bytes(S, buf, (size_t)n);
    memset(buf, 0, (size_t)n);
    free(buf);
    return result;
}

static const char k_rand_hex_lower[] = "0123456789abcdef";

/* (rand-hex n) -- n random bytes as a lowercase hex string of length 2n. */
static mino_val *prim_rand_hex(mino_state *S, mino_val *args, mino_env *env)
{
    long long      n;
    unsigned char *buf;
    char          *out;
    mino_val      *result;
    size_t         i;
    (void)env;

    if (random_count_arg(S, "rand-hex", args, &n) != 0) return NULL;
    if (n == 0) return mino_string_n(S, "", 0);
    if ((uint64_t)n > SIZE_MAX / 2) {
        return prim_throw_classified(S, "eval/bounds", "MBD001",
                                     "rand-hex: count too large");
    }
    buf = (unsigned char *)malloc((size_t)n);
    if (buf == NULL) {
        return prim_throw_classified(S, "internal", "MIN001",
                                     "rand-hex: out of memory");
    }
    if (prim_os_entropy(buf, (size_t)n) != 0) {
        free(buf);
        return prim_throw_classified(S, "host", "MHO001",
                                     "rand-hex: OS entropy source failed");
    }
    out = (char *)malloc((size_t)n * 2);
    if (out == NULL) {
        memset(buf, 0, (size_t)n);
        free(buf);
        return prim_throw_classified(S, "internal", "MIN001",
                                     "rand-hex: out of memory");
    }
    for (i = 0; i < (size_t)n; i++) {
        out[2 * i]     = k_rand_hex_lower[buf[i] >> 4];
        out[2 * i + 1] = k_rand_hex_lower[buf[i] & 0x0f];
    }
    result = mino_string_n(S, out, (size_t)n * 2);
    memset(buf, 0, (size_t)n);
    free(buf);
    free(out);
    return result;
}

/* base64url alphabet (RFC 4648 section 5): '+' and '/' become '-' and
 * '_', and rand-token emits no '=' padding, so a token is URL-safe. */
static const char k_b64url_alphabet[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";

/* Unpadded base64url length for n input bytes: 4 chars per full 3-byte
 * group, plus 2 for a 1-byte tail and 3 for a 2-byte tail. */
static size_t b64url_len(size_t n)
{
    size_t full = n / 3;
    size_t rem  = n % 3;
    return full * 4 + (rem == 0 ? 0 : rem + 1);
}

/* (rand-token n) -- n random bytes as an unpadded base64url token. */
static mino_val *prim_rand_token(mino_state *S, mino_val *args, mino_env *env)
{
    long long      n;
    unsigned char *buf;
    char          *out;
    mino_val      *result;
    size_t         len, i, o = 0, rem;
    (void)env;

    if (random_count_arg(S, "rand-token", args, &n) != 0) return NULL;
    if (n == 0) return mino_string_n(S, "", 0);
    if ((uint64_t)n > SIZE_MAX / 4) {
        return prim_throw_classified(S, "eval/bounds", "MBD001",
                                     "rand-token: count too large");
    }
    len = (size_t)n;
    buf = (unsigned char *)malloc(len);
    if (buf == NULL) {
        return prim_throw_classified(S, "internal", "MIN001",
                                     "rand-token: out of memory");
    }
    if (prim_os_entropy(buf, len) != 0) {
        free(buf);
        return prim_throw_classified(S, "host", "MHO001",
                                     "rand-token: OS entropy source failed");
    }
    out = (char *)malloc(b64url_len(len));
    if (out == NULL) {
        memset(buf, 0, len);
        free(buf);
        return prim_throw_classified(S, "internal", "MIN001",
                                     "rand-token: out of memory");
    }
    for (i = 0; i + 3 <= len; i += 3) {
        uint32_t w = ((uint32_t)buf[i] << 16)
                   | ((uint32_t)buf[i + 1] << 8)
                   | (uint32_t)buf[i + 2];
        out[o++] = k_b64url_alphabet[(w >> 18) & 63u];
        out[o++] = k_b64url_alphabet[(w >> 12) & 63u];
        out[o++] = k_b64url_alphabet[(w >> 6) & 63u];
        out[o++] = k_b64url_alphabet[w & 63u];
    }
    rem = len - i;
    if (rem == 1) {
        uint32_t w = (uint32_t)buf[i] << 16;
        out[o++] = k_b64url_alphabet[(w >> 18) & 63u];
        out[o++] = k_b64url_alphabet[(w >> 12) & 63u];
    } else if (rem == 2) {
        uint32_t w = ((uint32_t)buf[i] << 16) | ((uint32_t)buf[i + 1] << 8);
        out[o++] = k_b64url_alphabet[(w >> 18) & 63u];
        out[o++] = k_b64url_alphabet[(w >> 12) & 63u];
        out[o++] = k_b64url_alphabet[(w >> 6) & 63u];
    }
    result = mino_string_n(S, out, o);
    memset(buf, 0, len);
    free(buf);
    free(out);
    return result;
}

const mino_prim_def k_prims_random[] = {
    {"secure-rand-bytes", prim_secure_rand_bytes,
     "Returns n bytes drawn from the OS cryptographic random source as a "
     "bytes value. Throws when the OS source fails rather than falling "
     "back to a weak generator, and rejects a negative count."},
    {"rand-hex", prim_rand_hex,
     "Returns n random bytes from the OS source encoded as a lowercase "
     "hex string of length 2n. Suited to opaque identifiers."},
    {"rand-token", prim_rand_token,
     "Returns n random bytes from the OS source encoded as an unpadded "
     "base64url token (alphabet A-Za-z0-9-_). URL-safe with no padding; "
     "suited to session tokens and nonces."},
};

const size_t k_prims_random_count =
    sizeof(k_prims_random) / sizeof(k_prims_random[0]);

/* random rides its own MINO_CAP_RANDOM bit (in MINO_CAP_DEFAULT). The
 * bit is set here as well as by the capability dispatch loop so a direct
 * mino_install_random sets it too, matching the codec/digest install. */
void mino_install_random(mino_state *S, mino_env *env)
{
    mino_env *core_env = ns_env_ensure(S, "clojure.core");
    (void)env;
    prim_install_table_with_capability(S, core_env, "clojure.core",
                                       k_prims_random, k_prims_random_count,
                                       "random");
    S->caps_installed |= MINO_CAP_RANDOM;
}
