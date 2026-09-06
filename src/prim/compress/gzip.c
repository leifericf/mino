/*
 * gzip.c -- gzip and raw deflate decompression primitives.
 *
 * gzip-decompress parses the RFC 1952 container (header flags, the
 * deflate body, the CRC32 + ISIZE trailer) and inflates the body
 * through the vendored miniz tinfl core; deflate-decompress inflates
 * a raw RFC 1951 stream. Both are pure functions over buffers with no
 * state; input is untrusted, so every loop is bounded by the input
 * length or the output cap, and output never exceeds the cap the
 * caller set (decompression-bomb guard).
 *
 * One classified error family: :codec/truncated (input ends
 * mid-structure), :codec/magic (not a gzip container), :codec/crc
 * (CRC32 or ISIZE mismatch), :codec/corrupt (malformed stream,
 * reserved header bits, bytes past the single member),
 * :codec/limit (output passed :max-bytes). The gzip header CRC16 is
 * consumed, never verified; payload integrity is the CRC32 gate.
 * deflate-decompress decodes raw deflate only; a zlib-wrapped
 * stream (RFC 1950) is corrupt data here, by decision.
 */

#include "prim/internal.h"
#include "mino.h"
/* Trim define: without it miniz.h declares static zlib-name wrappers
 * (crc32, compress, ...) that this TU never calls. */
#define MINIZ_NO_ZLIB_COMPATIBLE_NAMES 1
#include "miniz.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define GZ_DEFAULT_MAX (64u * 1024u * 1024u)

/* gz_status and gz_out are declared in prim/internal.h: the raw
 * inflate core below is shared with compress.c (zlib-decompress),
 * so its signature's types are part of that seam. */

/* Inflate one raw deflate stream from in into a growing heap buffer
 * bounded by max_out. On GZ_OK, *consumed holds the input bytes the
 * stream occupied (the coroutine pushes back lookahead bytes at end
 * of stream, so this lands on the first byte past the stream).
 * Shared cross-TU as mino_inflate_raw; see prim/internal.h. */
gz_status mino_inflate_raw(const unsigned char *in, size_t in_len,
                           size_t max_out, gz_out *out, size_t *consumed)
{
    tinfl_decompressor decomp;
    unsigned char     *buf = NULL;
    size_t             cap = 0, in_ofs = 0;

    out->data = NULL;
    out->len  = 0;
    *consumed = 0;
    tinfl_init(&decomp);
    for (;;) {
        size_t         src_left = in_len - in_ofs;
        size_t         dst_left = cap - out->len;
        tinfl_status   st;
        unsigned char *grown;

        st = tinfl_decompress(&decomp, in + in_ofs, &src_left,
                              buf, buf ? buf + out->len : NULL, &dst_left,
                              TINFL_FLAG_USING_NON_WRAPPING_OUTPUT_BUF);
        in_ofs  += src_left;
        out->len += dst_left;
        if (st == TINFL_STATUS_DONE) {
            out->data = buf;
            *consumed = in_ofs;
            return GZ_OK;
        }
        if (st == TINFL_STATUS_NEEDS_MORE_INPUT
            || st == TINFL_STATUS_FAILED_CANNOT_MAKE_PROGRESS) {
            free(buf);
            return GZ_TRUNCATED;
        }
        if (st < 0) {
            free(buf);
            return GZ_CORRUPT;
        }
        /* HAS_MORE_OUTPUT: the buffer is full; grow it, or fire the
         * cap when there is no headroom left. */
        if (out->len == max_out) {
            free(buf);
            return GZ_LIMIT;
        }
        if (cap == 0) {
            cap = (max_out < 256) ? max_out : 256;
        } else if (cap > max_out - cap) {
            cap = max_out;
        } else {
            cap = cap * 2;
        }
        grown = (unsigned char *)realloc(buf, cap);
        if (grown == NULL) {
            free(buf);
            return GZ_OOM;
        }
        buf = grown;
    }
}

/* Walk a gzip header to the first byte of the deflate body. */
static gz_status gz_parse_header(const unsigned char *in, size_t in_len,
                                 size_t *body_ofs)
{
    size_t p = 10;

    if (in_len < 10) return GZ_TRUNCATED;
    if (in[0] != 0x1f || in[1] != 0x8b || in[2] != 8) return GZ_MAGIC;
    if ((in[3] & 0xe0) != 0) return GZ_CORRUPT; /* reserved flag bits */
    if (in[3] & 0x04) {                         /* FEXTRA */
        size_t xlen;
        if (in_len - p < 2) return GZ_TRUNCATED;
        xlen = (size_t)in[p] | ((size_t)in[p + 1] << 8);
        p += 2;
        if (in_len - p < xlen) return GZ_TRUNCATED;
        p += xlen;
    }
    if (in[3] & 0x08) {                         /* FNAME */
        while (p < in_len && in[p] != 0) p++;
        if (p == in_len) return GZ_TRUNCATED;
        p++;
    }
    if (in[3] & 0x10) {                         /* FCOMMENT */
        while (p < in_len && in[p] != 0) p++;
        if (p == in_len) return GZ_TRUNCATED;
        p++;
    }
    if (in[3] & 0x02) {                         /* FHCRC: skip, unverified */
        if (in_len - p < 2) return GZ_TRUNCATED;
        p += 2;
    }
    *body_ofs = p;
    return GZ_OK;
}

static uint32_t gz_le32(const unsigned char *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8)
         | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

/* Validate (data, opts) shared by both prims. Returns 0 with data and
 * max_out filled, or a thrown error via NULL. */
static int gz_args(mino_state *S, mino_val *args, const char *who,
                   const unsigned char **data, size_t *len,
                   size_t *max_out)
{
    mino_val *v, *opts;
    long long mb;

    if (!mino_is_cons(args)) {
        prim_throw_classified(S, "eval/arity", "MAR001", who);
        return -1;
    }
    v = args->as.cons.car;
    args = args->as.cons.cdr;
    if (mino_is_cons(args)) {
        opts = args->as.cons.car;
        if (mino_is_cons(args->as.cons.cdr)) {
            prim_throw_classified(S, "eval/arity", "MAR001", who);
            return -1;
        }
    } else {
        opts = NULL;
    }
    if (v == NULL || !mino_is_bytes(v)) {
        char msg[96];
        snprintf(msg, sizeof(msg),
                 "%s: input must be a bytes value", who);
        prim_throw_classified(S, "eval/type", "MTY001", msg);
        return -1;
    }
    if (opts != NULL && mino_type_of(opts) != MINO_MAP
        && mino_type_of(opts) != MINO_NIL) {
        char msg[96];
        snprintf(msg, sizeof(msg), "%s: opts must be a map", who);
        prim_throw_classified(S, "eval/type", "MTY001", msg);
        return -1;
    }
    *max_out = GZ_DEFAULT_MAX;
    if (opts != NULL && mino_type_of(opts) == MINO_MAP) {
        mino_val *mv = map_get_val(opts, mino_keyword(S, "max-bytes"));
        if (mv != NULL && mino_type_of(mv) != MINO_NIL) {
            if (!as_long(mv, &mb) || mb < 0
                || (unsigned long long)mb > SIZE_MAX / 2) {
                char msg[112];
                snprintf(msg, sizeof(msg),
                         "%s: :max-bytes must be a non-negative integer",
                         who);
                prim_throw_classified(S, "eval/contract", "MCT001", msg);
                return -1;
            }
            *max_out = (size_t)mb;
        }
    }
    *data = mino_bytes_data(v);
    *len  = mino_bytes_len(v);
    /* Empty bytes values carry a NULL data pointer; normalize to a
     * valid pointer so the buffer arithmetic below stays defined. */
    if (*data == NULL) *data = (const unsigned char *)"";
    return 0;
}

/* Translate a core status into the classified throw. */
static mino_val *gz_throw(mino_state *S, gz_status st, size_t max_out,
                          const char *who)
{
    char msg[128];

    switch (st) {
    case GZ_TRUNCATED:
        snprintf(msg, sizeof(msg), "%s: input is truncated", who);
        return prim_throw_classified(S, "codec/truncated", "MGC001", msg);
    case GZ_MAGIC:
        snprintf(msg, sizeof(msg),
                 "%s: not a gzip container (bad magic or method)", who);
        return prim_throw_classified(S, "codec/magic", "MGC002", msg);
    case GZ_CRC:
        snprintf(msg, sizeof(msg),
                 "%s: trailer CRC32 or ISIZE mismatch", who);
        return prim_throw_classified(S, "codec/crc", "MGC003", msg);
    case GZ_CORRUPT:
        snprintf(msg, sizeof(msg),
                 "%s: corrupt or unsupported stream", who);
        return prim_throw_classified(S, "codec/corrupt", "MGC004", msg);
    case GZ_LIMIT:
        snprintf(msg, sizeof(msg),
                 "%s: output exceeds the %lu byte cap",
                 who, (unsigned long)max_out);
        return prim_throw_classified(S, "codec/limit", "MGC005", msg);
    default:
        snprintf(msg, sizeof(msg), "%s: out of memory", who);
        return prim_throw_classified(S, "internal", "MIN001", msg);
    }
}

/* (gzip-decompress data opts?) -- one gzip member, strictly: the CRC32
 * and ISIZE trailer is verified and trailing bytes are an error.
 * Cross-TU: http.c calls it to decode Content-Encoding gzip bodies. */
mino_val *prim_gzip_decompress(mino_state *S, mino_val *args,
                               mino_env *env)
{
    const unsigned char *data;
    size_t               len, max_out, body_ofs, consumed, end;
    gz_out               out;
    gz_status            st;
    mino_val            *result;
    uint32_t             crc, isize;
    (void)env;

    if (gz_args(S, args, "gzip-decompress", &data, &len, &max_out) != 0)
        return NULL;
    st = gz_parse_header(data, len, &body_ofs);
    if (st == GZ_OK)
        st = mino_inflate_raw(data + body_ofs, len - body_ofs, max_out,
                              &out, &consumed);
    if (st != GZ_OK) return gz_throw(S, st, max_out, "gzip-decompress");
    end = body_ofs + consumed;
    if (len - end < 8) {
        free(out.data);
        return gz_throw(S, GZ_TRUNCATED, max_out, "gzip-decompress");
    }
    crc   = gz_le32(data + end);
    isize = gz_le32(data + end + 4);
    if ((uint32_t)mz_crc32(MZ_CRC32_INIT, out.data, out.len) != crc
        || (uint32_t)out.len != isize) {
        free(out.data);
        return gz_throw(S, GZ_CRC, max_out, "gzip-decompress");
    }
    if (len - end != 8) {
        free(out.data);
        return gz_throw(S, GZ_CORRUPT, max_out, "gzip-decompress");
    }
    result = mino_bytes(S, out.data, out.len);
    free(out.data);
    return result;
}

/* (deflate-decompress data opts?) -- one raw deflate stream, strictly:
 * the stream must consume the whole input.
 * Cross-TU: http.c calls it to decode Content-Encoding deflate. */
mino_val *prim_deflate_decompress(mino_state *S, mino_val *args,
                                  mino_env *env)
{
    const unsigned char *data;
    size_t               len, max_out, consumed;
    gz_out               out;
    gz_status            st;
    mino_val            *result;
    (void)env;

    if (gz_args(S, args, "deflate-decompress", &data, &len, &max_out) != 0)
        return NULL;
    st = mino_inflate_raw(data, len, max_out, &out, &consumed);
    if (st != GZ_OK) return gz_throw(S, st, max_out, "deflate-decompress");
    if (consumed != len) {
        free(out.data);
        return gz_throw(S, GZ_CORRUPT, max_out, "deflate-decompress");
    }
    result = mino_bytes(S, out.data, out.len);
    free(out.data);
    return result;
}

const mino_prim_def k_prims_gzip[] = {
    {"gzip-decompress", prim_gzip_decompress,
     "Decodes a single-member gzip container from a bytes value and "
     "returns the decompressed bytes. The CRC32 and ISIZE trailer is "
     "verified; truncated, corrupt, or CRC-mismatched input throws a "
     ":codec/* classified error. Output past :max-bytes throws "
     ":codec/limit (default cap 64 MiB); pass {:max-bytes n} to move "
     "the cap. Strings are rejected: input must be bytes."},
    {"deflate-decompress", prim_deflate_decompress,
     "Decodes one raw deflate stream (RFC 1951, no container) from a "
     "bytes value and returns the decompressed bytes. The stream must "
     "occupy the whole input. A zlib-wrapped stream (RFC 1950, the "
     "0x78 0x9c header) is NOT handled and throws a :codec/* "
     "classified error. Same :max-bytes cap behavior as "
     "gzip-decompress; input must be bytes."},
};

const size_t k_prims_gzip_count =
    sizeof(k_prims_gzip) / sizeof(k_prims_gzip[0]);
