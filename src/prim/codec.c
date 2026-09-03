/*
 * codec.c -- binary codec primitives: base64 (RFC 4648) and hex.
 *
 * Encoders take a string or bytes value and return a string; decoders
 * take a string or bytes value and return a bytes value, so both
 * decoders agree on one output kind regardless of input kind. The
 * codecs are plain functions over buffers with no state; input is
 * untrusted, so every walk is bounded by the input length and every
 * buffer is sized from it before the walk starts.
 */

#include "prim/internal.h"
#include "mino.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/* Extract a byte view from a string or bytes argument. Returns 0 on
 * any other type. */
static int codec_text_arg(const mino_val *v, const unsigned char **data,
                          size_t *len, int *is_string)
{
    if (v == NULL) return 0;
    if (mino_type_of(v) == MINO_STRING) {
        *data      = (const unsigned char *)v->as.s.data;
        *len       = v->as.s.len;
        *is_string = 1;
        return 1;
    }
    if (mino_is_bytes(v)) {
        *data      = mino_bytes_data(v);
        *len       = mino_bytes_len(v);
        *is_string = 0;
        return 1;
    }
    return 0;
}

/* ---- base64 ---- */

static const char k_b64_alphabet[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static int codec_b64_digit(unsigned char c)
{
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
}

/* (base64-encode v) -- RFC 4648 with padding. */
static mino_val *prim_base64_encode(mino_state *S, mino_val *args,
                                    mino_env *env)
{
    mino_val            *v, *result;
    const unsigned char *src;
    unsigned char       *out;
    size_t              len, out_len, i, o = 0, rem;
    int                 is_string;
    (void)env;

    if (!mino_is_cons(args) || mino_is_cons(args->as.cons.cdr)) {
        return prim_throw_classified(S, "eval/arity", "MAR001",
                                     "base64-encode requires one argument");
    }
    v = args->as.cons.car;
    if (!codec_text_arg(v, &src, &len, &is_string)) {
        return prim_throw_classified(S, "eval/type", "MTY001",
                                     "base64-encode: argument must be a "
                                     "string or bytes value");
    }
    if (len > (SIZE_MAX / 4 - 1) * 3) {
        return prim_throw_classified(S, "eval/bounds", "MBD001",
                                     "base64-encode: input too large");
    }
    out_len = ((len + 2) / 3) * 4;
    out     = (unsigned char *)malloc(out_len + 1);
    if (out == NULL) {
        return prim_throw_classified(S, "internal", "MIN001",
                                     "base64-encode: out of memory");
    }
    for (i = 0; i + 3 <= len; i += 3) {
        uint32_t n = ((uint32_t)src[i] << 16)
                   | ((uint32_t)src[i + 1] << 8)
                   | (uint32_t)src[i + 2];
        out[o++] = (unsigned char)k_b64_alphabet[(n >> 18) & 63u];
        out[o++] = (unsigned char)k_b64_alphabet[(n >> 12) & 63u];
        out[o++] = (unsigned char)k_b64_alphabet[(n >> 6) & 63u];
        out[o++] = (unsigned char)k_b64_alphabet[n & 63u];
    }
    rem = len - i;
    if (rem == 1) {
        uint32_t n = (uint32_t)src[i] << 16;
        out[o++] = (unsigned char)k_b64_alphabet[(n >> 18) & 63u];
        out[o++] = (unsigned char)k_b64_alphabet[(n >> 12) & 63u];
        out[o++] = '=';
        out[o++] = '=';
    } else if (rem == 2) {
        uint32_t n = ((uint32_t)src[i] << 16) | ((uint32_t)src[i + 1] << 8);
        out[o++] = (unsigned char)k_b64_alphabet[(n >> 18) & 63u];
        out[o++] = (unsigned char)k_b64_alphabet[(n >> 12) & 63u];
        out[o++] = (unsigned char)k_b64_alphabet[(n >> 6) & 63u];
        out[o++] = '=';
    }
    result = mino_string_n(S, (const char *)out, o);
    free(out);
    return result;
}

/* (base64-decode v) -- strict RFC 4648: length must be a multiple of
 * 4, padding only as trailing '=', and leftover bits in the final
 * quantum must be zero. Whitespace and newlines are not accepted. */
static mino_val *prim_base64_decode(mino_state *S, mino_val *args,
                                    mino_env *env)
{
    mino_val            *v, *result;
    const unsigned char *src;
    unsigned char       *out;
    size_t              len, blk, o = 0;
    int                 is_string;
    char                msg[112];
    (void)env;

    if (!mino_is_cons(args) || mino_is_cons(args->as.cons.cdr)) {
        return prim_throw_classified(S, "eval/arity", "MAR001",
                                     "base64-decode requires one argument");
    }
    v = args->as.cons.car;
    if (!codec_text_arg(v, &src, &len, &is_string)) {
        return prim_throw_classified(S, "eval/type", "MTY001",
                                     "base64-decode: argument must be a "
                                     "string or bytes value");
    }
    if (len == 0) return mino_bytes(S, NULL, 0);
    if (len % 4 != 0) {
        snprintf(msg, sizeof(msg),
                 "base64-decode: input length %lu is not a multiple of 4",
                 (unsigned long)len);
        return prim_throw_classified(S, "eval/contract", "MCT001", msg);
    }
    out = (unsigned char *)malloc(3 * (len / 4));
    if (out == NULL) {
        return prim_throw_classified(S, "internal", "MIN001",
                                     "base64-decode: out of memory");
    }
    for (blk = 0; blk < len; blk += 4) {
        int pad = 0;
        int d[4];
        int j;
        int last = (blk + 4 == len);
        if (last && src[blk + 3] == '=') {
            pad++;
            if (src[blk + 2] == '=') pad++;
        }
        for (j = 0; j < 4 - pad; j++) {
            d[j] = codec_b64_digit(src[blk + j]);
            if (d[j] < 0) {
                snprintf(msg, sizeof(msg),
                         "base64-decode: invalid character 0x%02x at "
                         "byte %lu", (unsigned)src[blk + j],
                         (unsigned long)(blk + j));
                free(out);
                return prim_throw_classified(S, "eval/contract", "MCT001",
                                             msg);
            }
        }
        if (pad > 0) {
            /* Canonical form: the final data character's unused low
             * bits must be zero (4 bits for "==", 2 bits for "="). */
            int leftover = (pad == 2) ? (d[1] & 0x0f) : (d[2] & 0x03);
            if (leftover != 0) {
                snprintf(msg, sizeof(msg),
                         "base64-decode: non-canonical block at byte %lu "
                         "(leftover bits set)", (unsigned long)blk);
                free(out);
                return prim_throw_classified(S, "eval/contract", "MCT001",
                                             msg);
            }
        }
        if (pad == 0) {
            uint32_t n = ((uint32_t)d[0] << 18) | ((uint32_t)d[1] << 12)
                       | ((uint32_t)d[2] << 6) | (uint32_t)d[3];
            out[o++] = (unsigned char)(n >> 16);
            out[o++] = (unsigned char)(n >> 8);
            out[o++] = (unsigned char)n;
        } else if (pad == 1) {
            uint32_t n = ((uint32_t)d[0] << 18) | ((uint32_t)d[1] << 12)
                       | ((uint32_t)d[2] << 6);
            out[o++] = (unsigned char)(n >> 16);
            out[o++] = (unsigned char)(n >> 8);
        } else {
            uint32_t n = ((uint32_t)d[0] << 18) | ((uint32_t)d[1] << 12);
            out[o++] = (unsigned char)(n >> 16);
        }
    }
    result = mino_bytes(S, out, o);
    free(out);
    return result;
}

/* ---- hex ---- */

static const char k_hex_lower[] = "0123456789abcdef";

static int codec_hex_digit(unsigned char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

/* (hex-encode v) -- lowercase hex, two digits per byte. */
static mino_val *prim_hex_encode(mino_state *S, mino_val *args,
                                 mino_env *env)
{
    mino_val            *v, *result;
    const unsigned char *src;
    unsigned char       *out;
    size_t              len, i;
    int                 is_string;
    (void)env;

    if (!mino_is_cons(args) || mino_is_cons(args->as.cons.cdr)) {
        return prim_throw_classified(S, "eval/arity", "MAR001",
                                     "hex-encode requires one argument");
    }
    v = args->as.cons.car;
    if (!codec_text_arg(v, &src, &len, &is_string)) {
        return prim_throw_classified(S, "eval/type", "MTY001",
                                     "hex-encode: argument must be a "
                                     "string or bytes value");
    }
    if (len > SIZE_MAX / 2) {
        return prim_throw_classified(S, "eval/bounds", "MBD001",
                                     "hex-encode: input too large");
    }
    out = (unsigned char *)malloc(2 * len + 1);
    if (out == NULL) {
        return prim_throw_classified(S, "internal", "MIN001",
                                     "hex-encode: out of memory");
    }
    for (i = 0; i < len; i++) {
        out[2 * i]     = (unsigned char)k_hex_lower[src[i] >> 4];
        out[2 * i + 1] = (unsigned char)k_hex_lower[src[i] & 0x0f];
    }
    result = mino_string_n(S, (const char *)out, 2 * len);
    free(out);
    return result;
}

/* (hex-decode v) -- case-insensitive; always returns a bytes value. */
static mino_val *prim_hex_decode(mino_state *S, mino_val *args,
                                 mino_env *env)
{
    mino_val            *v, *result;
    const unsigned char *src;
    unsigned char       *out;
    size_t              len, i;
    int                 is_string;
    char                msg[96];
    (void)env;

    if (!mino_is_cons(args) || mino_is_cons(args->as.cons.cdr)) {
        return prim_throw_classified(S, "eval/arity", "MAR001",
                                     "hex-decode requires one argument");
    }
    v = args->as.cons.car;
    if (!codec_text_arg(v, &src, &len, &is_string)) {
        return prim_throw_classified(S, "eval/type", "MTY001",
                                     "hex-decode: argument must be a "
                                     "string or bytes value");
    }
    if (len % 2 != 0) {
        snprintf(msg, sizeof(msg),
                 "hex-decode: input length %lu is odd", (unsigned long)len);
        return prim_throw_classified(S, "eval/contract", "MCT001", msg);
    }
    out = (unsigned char *)malloc(len / 2 > 0 ? len / 2 : 1);
    if (out == NULL) {
        return prim_throw_classified(S, "internal", "MIN001",
                                     "hex-decode: out of memory");
    }
    for (i = 0; i < len; i += 2) {
        int hi = codec_hex_digit(src[i]);
        int lo = codec_hex_digit(src[i + 1]);
        if (hi < 0 || lo < 0) {
            unsigned char bad = (hi < 0) ? src[i] : src[i + 1];
            size_t        off = (hi < 0) ? i : i + 1;
            snprintf(msg, sizeof(msg),
                     "hex-decode: invalid character 0x%02x at byte %lu",
                     (unsigned)bad, (unsigned long)off);
            free(out);
            return prim_throw_classified(S, "eval/contract", "MCT001", msg);
        }
        out[i / 2] = (unsigned char)(hi * 16 + lo);
    }
    result = mino_bytes(S, out, len / 2);
    free(out);
    return result;
}

/* ---- utf-8 ---- */

/* Validate a UTF-8 byte range in place. On success returns 1; on
 * malformed input returns 0 and sets *bad_off to the offending byte
 * offset. STRICT: rejects overlong forms, UTF-16 surrogates, code
 * points past U+10FFFF, bad continuations, and truncated tails. This
 * matches the mino.http decode-utf8 contract exactly. */
static int codec_utf8_validate(const unsigned char *src, size_t len,
                               size_t *bad_off)
{
    size_t i = 0;
    while (i < len) {
        unsigned char c0 = src[i];
        size_t        width;
        unsigned char lo, hi; /* accepted range of the first continuation */
        if (c0 < 0x80) {
            i++;
            continue;
        }
        if (c0 >= 0xC2 && c0 <= 0xDF) {
            width = 2; lo = 0x80; hi = 0xBF;
        } else if (c0 == 0xE0) {
            width = 3; lo = 0xA0; hi = 0xBF;
        } else if (c0 >= 0xE1 && c0 <= 0xEC) {
            width = 3; lo = 0x80; hi = 0xBF;
        } else if (c0 == 0xED) {
            width = 3; lo = 0x80; hi = 0x9F;
        } else if (c0 >= 0xEE && c0 <= 0xEF) {
            width = 3; lo = 0x80; hi = 0xBF;
        } else if (c0 == 0xF0) {
            width = 4; lo = 0x90; hi = 0xBF;
        } else if (c0 >= 0xF1 && c0 <= 0xF3) {
            width = 4; lo = 0x80; hi = 0xBF;
        } else if (c0 == 0xF4) {
            width = 4; lo = 0x80; hi = 0x8F;
        } else {
            *bad_off = i; /* 0xC0/0xC1, 0xF5-0xFF, and lone continuations */
            return 0;
        }
        if (i + width > len) {
            *bad_off = i; /* truncated multibyte tail */
            return 0;
        }
        {
            unsigned char c1 = src[i + 1];
            size_t        j;
            if (c1 < lo || c1 > hi) {
                *bad_off = i + 1;
                return 0;
            }
            for (j = 2; j < width; j++) {
                unsigned char cj = src[i + j];
                if (cj < 0x80 || cj > 0xBF) {
                    *bad_off = i + j;
                    return 0;
                }
            }
        }
        i += width;
    }
    return 1;
}

/* (bytes->string v) / (bytes->string v max-bytes) -- decode a string or
 * bytes value as strict UTF-8. With max-bytes, an input longer than the
 * cap is rejected before any decode work. */
static mino_val *prim_bytes_to_string(mino_state *S, mino_val *args,
                                      mino_env *env)
{
    mino_val            *v, *cap_arg;
    const unsigned char *src;
    size_t              len, bad_off = 0;
    int                 is_string, have_cap = 0;
    long long           cap = 0;
    char                msg[96];
    (void)env;

    if (!mino_is_cons(args)) {
        return prim_throw_classified(S, "eval/arity", "MAR001",
                                     "bytes->string requires one or two "
                                     "arguments");
    }
    v = args->as.cons.car;
    if (mino_is_cons(args->as.cons.cdr)) {
        mino_val *rest = args->as.cons.cdr;
        cap_arg  = rest->as.cons.car;
        have_cap = 1;
        if (mino_is_cons(rest->as.cons.cdr)) {
            return prim_throw_classified(S, "eval/arity", "MAR001",
                                         "bytes->string requires one or two "
                                         "arguments");
        }
    }
    if (!codec_text_arg(v, &src, &len, &is_string)) {
        return prim_throw_classified(S, "eval/type", "MTY001",
                                     "bytes->string: argument must be a "
                                     "string or bytes value");
    }
    if (have_cap) {
        if (!as_long(cap_arg, &cap) || cap < 0) {
            return prim_throw_classified(S, "eval/contract", "MCT001",
                                         "bytes->string: max-bytes must be a "
                                         "non-negative integer");
        }
        if (len > (unsigned long long)cap) {
            snprintf(msg, sizeof(msg),
                     "bytes->string: input length %lu exceeds max-bytes %lld",
                     (unsigned long)len, cap);
            return prim_throw_classified(S, "eval/bounds", "MBD001", msg);
        }
    }
    if (!codec_utf8_validate(src, len, &bad_off)) {
        snprintf(msg, sizeof(msg),
                 "bytes->string: invalid UTF-8 at byte %lu",
                 (unsigned long)bad_off);
        return prim_throw_classified(S, "eval/contract", "MCT001", msg);
    }
    /* Validated bytes are already a well-formed UTF-8 string, so a
     * straight copy preserves byte identity with one allocation. */
    return mino_string_n(S, (const char *)src, len);
}

const mino_prim_def k_prims_codec[] = {
    {"base64-encode", prim_base64_encode,
     "Encodes a string or bytes value as base64 (RFC 4648) with "
     "padding. A string input encodes its UTF-8 bytes. Returns a "
     "string."},
    {"base64-decode", prim_base64_decode,
     "Decodes base64 (RFC 4648) from a string or bytes value. Strict: "
     "the length must be a multiple of 4, whitespace and newlines are "
     "rejected, and non-canonical trailing bits are rejected. Returns "
     "a bytes value."},
    {"hex-encode", prim_hex_encode,
     "Encodes a string or bytes value as lowercase hex, two digits per "
     "byte. A string input encodes its UTF-8 bytes. Returns a string."},
    {"hex-decode", prim_hex_decode,
     "Decodes hex from a string or bytes value; hex digits are "
     "case-insensitive and odd-length or non-hex input throws. Always "
     "returns a bytes value; use bytes->string to coerce bytes to a "
     "string as UTF-8."},
    {"bytes->string", prim_bytes_to_string,
     "Decodes a string or bytes value as strict UTF-8 into a string. "
     "Overlong forms, UTF-16 surrogates, code points past U+10FFFF, "
     "bad continuations, and truncated tails throw. With a second "
     "max-bytes argument (a non-negative integer), an input longer "
     "than the cap is rejected before any decode work."},
};

const size_t k_prims_codec_count =
    sizeof(k_prims_codec) / sizeof(k_prims_codec[0]);
