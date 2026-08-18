/*
 * url.c -- URL text primitives: percent-encoding (RFC 3986).
 *
 * Pure data in, data out. The codecs are plain functions over buffers
 * with no state; the prims wrap them and map malformed input to
 * classified :eval/contract errors. Input is untrusted bytes from the
 * script side, so every walk is bounded by the input length and every
 * buffer is sized from it before the walk starts.
 */

#include "prim/internal.h"
#include "mino.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

static int url_unreserved(unsigned char c)
{
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z')
        || (c >= '0' && c <= '9')
        || c == '-' || c == '.' || c == '_' || c == '~';
}

static int url_hex_digit(unsigned char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static const char k_url_hex_upper[] = "0123456789ABCDEF";

/* Extract a byte view from a string or bytes argument. Returns 0 on
 * any other type. */
static int url_text_arg(const mino_val *v, const unsigned char **data,
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

/* (percent-encode v) -- RFC 3986 percent-encoding. */
static mino_val *prim_percent_encode(mino_state *S, mino_val *args,
                                     mino_env *env)
{
    mino_val           *v;
    const unsigned char *src;
    unsigned char      *out;
    mino_val           *result;
    size_t             len, i, o = 0;
    int                is_string;
    (void)env;

    if (!mino_is_cons(args) || mino_is_cons(args->as.cons.cdr)) {
        return prim_throw_classified(S, "eval/arity", "MAR001",
                                     "percent-encode requires one argument");
    }
    v = args->as.cons.car;
    if (!url_text_arg(v, &src, &len, &is_string)) {
        return prim_throw_classified(S, "eval/type", "MTY001",
                                     "percent-encode: argument must be a "
                                     "string or bytes value");
    }
    if (len > (SIZE_MAX - 1) / 3) {
        return prim_throw_classified(S, "eval/bounds", "MBD001",
                                     "percent-encode: input too large");
    }
    out = (unsigned char *)malloc(len * 3 + 1);
    if (out == NULL) {
        return prim_throw_classified(S, "internal", "MIN001",
                                     "percent-encode: out of memory");
    }
    for (i = 0; i < len; i++) {
        unsigned char c = src[i];
        if (url_unreserved(c)) {
            out[o++] = c;
        } else {
            out[o++] = '%';
            out[o++] = (unsigned char)k_url_hex_upper[c >> 4];
            out[o++] = (unsigned char)k_url_hex_upper[c & 0x0f];
        }
    }
    result = is_string
        ? mino_string_n(S, (const char *)out, o)
        : mino_bytes(S, out, o);
    free(out);
    return result;
}

/* Strict UTF-8 validation (RFC 3629): rejects overlong forms,
 * surrogate halves, codepoints above U+10FFFF, and truncated
 * sequences. On failure writes the offending byte offset. */
static int url_utf8_valid(const unsigned char *s, size_t n, size_t *bad_off)
{
    size_t i = 0;
    while (i < n) {
        unsigned char c   = s[i];
        unsigned char lo  = 0x80, hi = 0xBF;
        size_t         need;
        size_t         k;
        if (c < 0x80) { i++; continue; }
        if (c >= 0xC2 && c <= 0xDF) {
            need = 1;
        } else if (c == 0xE0) {
            need = 2; lo = 0xA0;
        } else if ((c >= 0xE1 && c <= 0xEC) || c == 0xED || c == 0xEE
                   || c == 0xEF) {
            need = 2;
            if (c == 0xED) hi = 0x9F;
        } else if (c == 0xF0) {
            need = 3; lo = 0x90;
        } else if (c >= 0xF1 && c <= 0xF3) {
            need = 3;
        } else if (c == 0xF4) {
            need = 3; hi = 0x8F;
        } else {
            *bad_off = i;
            return 0;
        }
        if (i + need >= n) {
            *bad_off = i;
            return 0;
        }
        if (s[i + 1] < lo || s[i + 1] > hi) {
            *bad_off = i + 1;
            return 0;
        }
        for (k = 2; k <= need; k++) {
            if (s[i + k] < 0x80 || s[i + k] > 0xBF) {
                *bad_off = i + k;
                return 0;
            }
        }
        i += need + 1;
    }
    return 1;
}

/* (percent-decode v) -- inverse of percent-encode. */
static mino_val *prim_percent_decode(mino_state *S, mino_val *args,
                                     mino_env *env)
{
    mino_val           *v;
    const unsigned char *src;
    unsigned char      *out;
    mino_val           *result;
    size_t             len, i, o = 0;
    int                is_string;
    char               msg[96];
    (void)env;

    if (!mino_is_cons(args) || mino_is_cons(args->as.cons.cdr)) {
        return prim_throw_classified(S, "eval/arity", "MAR001",
                                     "percent-decode requires one argument");
    }
    v = args->as.cons.car;
    if (!url_text_arg(v, &src, &len, &is_string)) {
        return prim_throw_classified(S, "eval/type", "MTY001",
                                     "percent-decode: argument must be a "
                                     "string or bytes value");
    }
    /* Pass 1: validate every escape, count the output length. */
    for (i = 0; i < len; ) {
        if (src[i] == '%') {
            if (i + 2 >= len || url_hex_digit(src[i + 1]) < 0
                || url_hex_digit(src[i + 2]) < 0) {
                snprintf(msg, sizeof(msg),
                         "percent-decode: malformed percent-escape at "
                         "byte %lu", (unsigned long)i);
                return prim_throw_classified(S, "eval/contract", "MCT001",
                                             msg);
            }
            o++;
            i += 3;
        } else {
            o++;
            i++;
        }
    }
    out = (unsigned char *)malloc(o > 0 ? o : 1);
    if (out == NULL) {
        return prim_throw_classified(S, "internal", "MIN001",
                                     "percent-decode: out of memory");
    }
    /* Pass 2: fill. Escapes were validated above; no re-check needed. */
    for (i = 0, o = 0; i < len; ) {
        if (src[i] == '%') {
            out[o++] = (unsigned char)(url_hex_digit(src[i + 1]) * 16
                                       + url_hex_digit(src[i + 2]));
            i += 3;
        } else {
            out[o++] = src[i++];
        }
    }
    if (is_string) {
        size_t bad_off = 0;
        if (!url_utf8_valid(out, o, &bad_off)) {
            snprintf(msg, sizeof(msg),
                     "percent-decode: decoded bytes are not valid UTF-8 "
                     "at byte %lu", (unsigned long)bad_off);
            free(out);
            return prim_throw_classified(S, "eval/contract", "MCT001", msg);
        }
        result = mino_string_n(S, (const char *)out, o);
    } else {
        result = mino_bytes(S, out, o);
    }
    free(out);
    return result;
}

const mino_prim_def k_prims_url[] = {
    {"percent-encode", prim_percent_encode,
     "Percent-encodes a string or bytes value per RFC 3986: unreserved "
     "characters (letters, digits, - . _ ~) stay literal and every other "
     "byte becomes %XX with uppercase hex. Space encodes as %20, never "
     "plus. Returns the same kind it was given."},
    {"percent-decode", prim_percent_decode,
     "Decodes %XX escapes in a string or bytes value, returning the same "
     "kind. Hex digits are case-insensitive. Malformed escapes throw; a "
     "string result must be valid UTF-8 or the decode throws."},
};

const size_t k_prims_url_count =
    sizeof(k_prims_url) / sizeof(k_prims_url[0]);
