/*
 * compress.c -- compression write-side and zlib wrapper primitives.
 *
 * gzip-compress, deflate-compress, and zlib-compress write through
 * the vendored miniz tdefl core (whole-buffer in and out, one
 * output allocation at the vendor's own worst-case bound, no
 * growth loop); zlib-decompress is the strict RFC 1950 sibling of
 * the raw deflate reader, inflating through the shared
 * mino_inflate_raw core (promoted out of gzip.c) and verifying the
 * big-endian Adler-32 trailer with the vendored mz_adler32.
 *
 * The gzip member is framed by hand (RFC 1952 10-byte header, FNAME
 * only when :name is supplied and stripped to its basename, CRC32 +
 * ISIZE trailer via mz_crc32); the zlib wrapper and its Adler
 * trailer come from tdefl's TDEFL_WRITE_ZLIB_HEADER path. Output is
 * deterministic by contract: same input and options give
 * byte-identical bytes.
 *
 * Errors share the gzip read side's one family: :codec/truncated,
 * :codec/magic (bad zlib header), :codec/crc (Adler-32 mismatch),
 * :codec/corrupt (trailing bytes), :codec/limit (:max-bytes),
 * :codec/unsupported (FDICT preset dictionaries), codes MGC001-005
 * and MGC007. Nothing escapes :internal except OOM. Input is
 * untrusted on the decompress path: the header checks run before
 * any allocation, the inflate loop is bounded by the input length
 * and the output cap, and the Adler is verified before return.
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

#define CMP_DEFAULT_MAX (64u * 1024u * 1024u)
#define CMP_GZ_MAX_MTIME 0xFFFFFFFFull

/* ---- shared argument handling ---- */

/* Parse the (data opts?) shape shared by all four prims. data must
 * be bytes (stream prims are bytes-strict, the gzip.c symmetry);
 * opts must be a map or nil. Returns 0 with data/len/opts filled,
 * or a thrown error via NULL. */
static int cmp_args(mino_state *S, mino_val *args, const char *who,
                    const unsigned char **data, size_t *len,
                    mino_val **opts)
{
    mino_val *v;
    char      msg[96];

    if (!mino_is_cons(args)) {
        prim_throw_classified(S, "eval/arity", "MAR001", who);
        return -1;
    }
    v = args->as.cons.car;
    args = args->as.cons.cdr;
    if (mino_is_cons(args)) {
        *opts = args->as.cons.car;
        if (mino_is_cons(args->as.cons.cdr)) {
            prim_throw_classified(S, "eval/arity", "MAR001", who);
            return -1;
        }
    } else {
        *opts = NULL;
    }
    if (v == NULL || !mino_is_bytes(v)) {
        snprintf(msg, sizeof(msg), "%s: input must be a bytes value", who);
        prim_throw_classified(S, "eval/type", "MTY001", msg);
        return -1;
    }
    if (*opts != NULL && mino_type_of(*opts) != MINO_MAP
        && mino_type_of(*opts) != MINO_NIL) {
        snprintf(msg, sizeof(msg), "%s: opts must be a map", who);
        prim_throw_classified(S, "eval/type", "MTY001", msg);
        return -1;
    }
    *data = mino_bytes_data(v);
    *len  = mino_bytes_len(v);
    /* Empty bytes values carry a NULL data pointer; normalize so the
     * buffer arithmetic below stays defined. */
    if (*data == NULL) *data = (const unsigned char *)"";
    return 0;
}

/* Read :level from opts: integers 0-9 only, checked before any
 * allocation (:level nil means absent, the max-bytes precedent).
 * Returns 0 with level filled, or a thrown error via -1. */
static int cmp_level_opt(mino_state *S, mino_val *opts, const char *who,
                         int *level)
{
    mino_val *lv;
    long long n;
    char      msg[112];

    *level = 6;
    if (opts == NULL || mino_type_of(opts) != MINO_MAP) return 0;
    lv = map_get_val(opts, mino_keyword(S, "level"));
    if (lv == NULL || mino_type_of(lv) == MINO_NIL) return 0;
    if (!as_long(lv, &n) || n < 0 || n > 9) {
        snprintf(msg, sizeof(msg),
                 "%s: :level must be an integer 0-9", who);
        prim_throw_classified(S, "eval/contract", "MCT001", msg);
        return -1;
    }
    *level = (int)n;
    return 0;
}

/* Read :max-bytes from opts (non-negative integer; default 64 MiB).
 * Returns 0 with the cap filled, or a thrown error via -1. */
static int cmp_max_bytes_opt(mino_state *S, mino_val *opts, const char *who,
                             size_t *max_out)
{
    mino_val *mv;
    long long mb;
    char      msg[112];

    *max_out = CMP_DEFAULT_MAX;
    if (opts == NULL || mino_type_of(opts) != MINO_MAP) return 0;
    mv = map_get_val(opts, mino_keyword(S, "max-bytes"));
    if (mv != NULL && mino_type_of(mv) != MINO_NIL) {
        if (!as_long(mv, &mb) || mb < 0
            || (unsigned long long)mb > SIZE_MAX / 2) {
            snprintf(msg, sizeof(msg),
                     "%s: :max-bytes must be a non-negative integer", who);
            prim_throw_classified(S, "eval/contract", "MCT001", msg);
            return -1;
        }
        *max_out = (size_t)mb;
    }
    return 0;
}

/* ---- the compression core ---- */

/* The vendor's own worst-case output bound (upstream mz_deflateBound
 * formula, miniz.c): compression output never exceeds this, so one
 * allocation covers the result with no growth loop. The 128-byte
 * slack absorbs the gzip or zlib framing. */
static size_t cmp_deflate_bound(size_t n)
{
    return MZ_MAX(128 + n + n / 10, 128 + n + (n / 31744 + 1) * 5);
}

/* Compress data at level into a freshly malloc'd buffer (raw
 * deflate, or the RFC 1950 wrapper when zlib is set; tdefl writes
 * the zlib header and Adler-32 trailer itself). Returns NULL with
 * *out_len 0 only on allocator or invariant failure. */
static unsigned char *cmp_tdefl(const unsigned char *data, size_t len,
                                int level, int zlib, size_t *out_len)
{
    mz_uint      flags = tdefl_create_comp_flags_from_zip_params(
        level, zlib ? 15 : -15, MZ_DEFAULT_STRATEGY);
    size_t       cap = cmp_deflate_bound(len) + (zlib ? 6 : 0);
    unsigned char *buf = (unsigned char *)malloc(cap);

    *out_len = 0;
    if (buf == NULL) return NULL;
    *out_len = tdefl_compress_mem_to_mem(buf, cap, data, len, (int)flags);
    if (*out_len == 0) {
        free(buf);
        return NULL;
    }
    return buf;
}

/* XFL per RFC 1952: 2 at maximum compression (level 9), 4 at
 * fastest (level 1), 0 otherwise. Advisory, but pinned by test. */
static unsigned char cmp_gzip_xfl(int level)
{
    if (level == 9) return 2;
    if (level == 1) return 4;
    return 0;
}

/* (gzip-compress data opts?) -- one RFC 1952 member over a bytes
 * value. Opts: {:level 6 :mtime 0 :name nil :os 255}. :name is
 * stripped to its basename and stored as FNAME; :mtime is the
 * little-endian MTIME word (0 default); :os is the header OS byte
 * (255, unknown, default). Header, body, and CRC32 + ISIZE trailer
 * share the one bound-sized allocation. */
static mino_val *prim_gzip_compress(mino_state *S, mino_val *args,
                                    mino_env *env)
{
    const unsigned char *data;
    size_t               len, cap, hdr_len, name_len = 0, body_len;
    mino_val            *opts;
    int                  level;
    long long            mtime = 0, os = 255;
    const char          *name_base = NULL;
    unsigned char       *buf, *p;
    uint32_t             crc;
    mino_val            *result;
    char                 msg[128];
    (void)env;

    if (cmp_args(S, args, "gzip-compress", &data, &len, &opts) != 0)
        return NULL;
    if (cmp_level_opt(S, opts, "gzip-compress", &level) != 0) return NULL;
    if (opts != NULL && mino_type_of(opts) == MINO_MAP) {
        mino_val *mv = map_get_val(opts, mino_keyword(S, "mtime"));
        if (mv != NULL && mino_type_of(mv) != MINO_NIL
            && (!as_long(mv, &mtime) || mtime < 0
                || (unsigned long long)mtime > CMP_GZ_MAX_MTIME)) {
            prim_throw_classified(S, "eval/contract", "MCT001",
                                  "gzip-compress: :mtime must be an "
                                  "integer 0..4294967295");
            return NULL;
        }
        mv = map_get_val(opts, mino_keyword(S, "os"));
        if (mv != NULL && mino_type_of(mv) != MINO_NIL
            && (!as_long(mv, &os) || os < 0 || os > 255)) {
            prim_throw_classified(S, "eval/contract", "MCT001",
                                  "gzip-compress: :os must be an "
                                  "integer 0-255");
            return NULL;
        }
        mv = map_get_val(opts, mino_keyword(S, "name"));
        if (mv != NULL && mino_type_of(mv) != MINO_NIL) {
            const char *slash;
            if (mino_type_of(mv) != MINO_STRING) {
                prim_throw_classified(S, "eval/contract", "MCT001",
                                      "gzip-compress: :name must be a "
                                      "string");
                return NULL;
            }
            /* Strip to the basename (RFC 1952 / gzip(1) intent): the
             * stored name never carries directory components. */
            name_base = mv->as.s.data;
            slash = strrchr(name_base, '/');
            if (slash != NULL) name_base = slash + 1;
            name_len = mv->as.s.len - (size_t)(name_base - mv->as.s.data);
            if (name_len == 0) {
                prim_throw_classified(S, "eval/contract", "MCT001",
                                      "gzip-compress: :name has an empty "
                                      "basename");
                return NULL;
            }
        }
    }

    hdr_len = 10 + (name_len != 0 ? name_len + 1 : 0);
    cap = cmp_deflate_bound(len) + hdr_len + 8;
    buf = (unsigned char *)malloc(cap);
    if (buf == NULL) {
        snprintf(msg, sizeof(msg), "gzip-compress: out of memory");
        return prim_throw_classified(S, "internal", "MIN001", msg);
    }
    buf[0] = 0x1f;
    buf[1] = 0x8b;
    buf[2] = 8;                                   /* CM: deflate */
    buf[3] = (name_len != 0) ? 0x08 : 0x00;       /* FLG: FNAME only */
    buf[4] = (unsigned char)(mtime & 0xFF);
    buf[5] = (unsigned char)((mtime >> 8) & 0xFF);
    buf[6] = (unsigned char)((mtime >> 16) & 0xFF);
    buf[7] = (unsigned char)((mtime >> 24) & 0xFF);
    buf[8] = cmp_gzip_xfl(level);
    buf[9] = (unsigned char)os;
    if (name_len != 0) {
        memcpy(buf + 10, name_base, name_len);
        buf[10 + name_len] = 0;
    }

    body_len = tdefl_compress_mem_to_mem(
        buf + hdr_len, cap - hdr_len - 8, data, len,
        (int)tdefl_create_comp_flags_from_zip_params(
            level, -15, MZ_DEFAULT_STRATEGY));
    if (body_len == 0) {
        /* The bound is proven; landing here is an invariant break. */
        free(buf);
        snprintf(msg, sizeof(msg),
                 "gzip-compress: compressor failed at the proven bound");
        return prim_throw_classified(S, "internal", "MIN001", msg);
    }
    crc = (uint32_t)mz_crc32(MZ_CRC32_INIT, data, len);
    p = buf + hdr_len + body_len;
    p[0] = (unsigned char)(crc & 0xFF);
    p[1] = (unsigned char)((crc >> 8) & 0xFF);
    p[2] = (unsigned char)((crc >> 16) & 0xFF);
    p[3] = (unsigned char)((crc >> 24) & 0xFF);
    p[4] = (unsigned char)(len & 0xFF);
    p[5] = (unsigned char)((len >> 8) & 0xFF);
    p[6] = (unsigned char)((len >> 16) & 0xFF);
    p[7] = (unsigned char)((len >> 24) & 0xFF);
    result = mino_bytes(S, buf, hdr_len + body_len + 8);
    free(buf);
    return result;
}

/* (deflate-compress data opts?) -- one raw RFC 1951 stream.
 * Opts: {:level 6}. */
static mino_val *prim_deflate_compress(mino_state *S, mino_val *args,
                                       mino_env *env)
{
    const unsigned char *data;
    size_t               len, out_len;
    mino_val            *opts, *result;
    int                  level;
    unsigned char       *buf;
    char                 msg[128];
    (void)env;

    if (cmp_args(S, args, "deflate-compress", &data, &len, &opts) != 0)
        return NULL;
    if (cmp_level_opt(S, opts, "deflate-compress", &level) != 0) return NULL;
    buf = cmp_tdefl(data, len, level, 0, &out_len);
    if (buf == NULL) {
        snprintf(msg, sizeof(msg), "deflate-compress: out of memory");
        return prim_throw_classified(S, "internal", "MIN001", msg);
    }
    result = mino_bytes(S, buf, out_len);
    free(buf);
    return result;
}

/* (zlib-compress data opts?) -- one RFC 1950 stream (CMF/FLG header
 * and big-endian Adler-32 trailer from tdefl). Opts: {:level 6}. */
static mino_val *prim_zlib_compress(mino_state *S, mino_val *args,
                                    mino_env *env)
{
    const unsigned char *data;
    size_t               len, out_len;
    mino_val            *opts, *result;
    int                  level;
    unsigned char       *buf;
    char                 msg[128];
    (void)env;

    if (cmp_args(S, args, "zlib-compress", &data, &len, &opts) != 0)
        return NULL;
    if (cmp_level_opt(S, opts, "zlib-compress", &level) != 0) return NULL;
    buf = cmp_tdefl(data, len, level, 1, &out_len);
    if (buf == NULL) {
        snprintf(msg, sizeof(msg), "zlib-compress: out of memory");
        return prim_throw_classified(S, "internal", "MIN001", msg);
    }
    result = mino_bytes(S, buf, out_len);
    free(buf);
    return result;
}

/* (zlib-decompress data opts?) -- one strict RFC 1950 stream: CM 8,
 * CINFO at most 7, FCHECK divisible by 31, no FDICT, the deflate
 * body consuming every byte before the big-endian Adler-32 trailer,
 * which is verified against the output. Opts: {:max-bytes 64 MiB}. */
static mino_val *prim_zlib_decompress(mino_state *S, mino_val *args,
                                      mino_env *env)
{
    const unsigned char *data;
    size_t               len, max_out, consumed, body_len;
    mino_val            *opts, *result;
    gz_out               out;
    gz_status            st;
    uint32_t             adler;
    char                 msg[128];
    (void)env;

    if (cmp_args(S, args, "zlib-decompress", &data, &len, &opts) != 0)
        return NULL;
    if (cmp_max_bytes_opt(S, opts, "zlib-decompress", &max_out) != 0)
        return NULL;

    if (len < 2) {
        return prim_throw_classified(S, "codec/truncated", "MGC001",
                                     "zlib-decompress: input is truncated");
    }
    if ((data[0] & 0x0F) != 8 || (data[0] >> 4) > 7
        || ((data[0] << 8) | data[1]) % 31 != 0) {
        return prim_throw_classified(S, "codec/magic", "MGC002",
                                     "zlib-decompress: not a zlib stream "
                                     "(bad CMF/FLG header)");
    }
    if ((data[1] & 0x20) != 0) {
        return prim_throw_classified(S, "codec/unsupported", "MGC007",
                                     "zlib-decompress: FDICT preset "
                                     "dictionaries are not supported");
    }
    if (len < 6) {
        return prim_throw_classified(S, "codec/truncated", "MGC001",
                                     "zlib-decompress: input is truncated");
    }

    body_len = len - 6;
    adler = ((uint32_t)data[len - 4] << 24) | ((uint32_t)data[len - 3] << 16)
          | ((uint32_t)data[len - 2] << 8) | (uint32_t)data[len - 1];
    st = mino_inflate_raw(data + 2, body_len, max_out, &out, &consumed);
    if (st == GZ_TRUNCATED) {
        snprintf(msg, sizeof(msg), "zlib-decompress: input is truncated");
        return prim_throw_classified(S, "codec/truncated", "MGC001", msg);
    }
    if (st == GZ_CORRUPT) {
        snprintf(msg, sizeof(msg), "zlib-decompress: corrupt stream");
        return prim_throw_classified(S, "codec/corrupt", "MGC004", msg);
    }
    if (st == GZ_LIMIT) {
        snprintf(msg, sizeof(msg),
                 "zlib-decompress: output exceeds the %lu byte cap",
                 (unsigned long)max_out);
        return prim_throw_classified(S, "codec/limit", "MGC005", msg);
    }
    if (st != GZ_OK) {
        snprintf(msg, sizeof(msg), "zlib-decompress: out of memory");
        return prim_throw_classified(S, "internal", "MIN001", msg);
    }
    if (consumed != body_len) {
        free(out.data);
        snprintf(msg, sizeof(msg),
                 "zlib-decompress: trailing bytes after the stream");
        return prim_throw_classified(S, "codec/corrupt", "MGC004", msg);
    }
    if ((uint32_t)mz_adler32(MZ_ADLER32_INIT, out.data, out.len) != adler) {
        free(out.data);
        snprintf(msg, sizeof(msg),
                 "zlib-decompress: Adler-32 trailer mismatch");
        return prim_throw_classified(S, "codec/crc", "MGC003", msg);
    }
    result = mino_bytes(S, out.data, out.len);
    free(out.data);
    return result;
}

const mino_prim_def k_prims_compress[] = {
    {"gzip-compress", prim_gzip_compress,
     "Compresses a bytes value into one RFC 1952 gzip member and "
     "returns the bytes. Opts: {:level 6 :mtime 0 :name nil :os 255}. "
     ":level takes integers 0-9 (XFL is 2 at level 9, 4 at level 1); "
     ":mtime fills the header MTIME word; :name is stripped to its "
     "basename and stored as FNAME; :os is the header OS byte. "
     "Output is deterministic: same input and opts give identical "
     "bytes. Strings are rejected: input must be bytes."},
    {"deflate-compress", prim_deflate_compress,
     "Compresses a bytes value into one raw deflate stream (RFC "
     "1951, no container) and returns the bytes. Opts: {:level 6} "
     "with integers 0-9 only. Deterministic output; input must be "
     "bytes."},
    {"zlib-compress", prim_zlib_compress,
     "Compresses a bytes value into one RFC 1950 zlib stream (CMF/FLG "
     "header plus big-endian Adler-32 trailer) and returns the bytes. "
     "Opts: {:level 6} with integers 0-9 only. Deterministic output; "
     "input must be bytes."},
    {"zlib-decompress", prim_zlib_decompress,
     "Decodes one RFC 1950 zlib stream from a bytes value and returns "
     "the decompressed bytes. Strict: CM 8, CINFO at most 7, FCHECK "
     "divisible by 31, no FDICT preset dictionaries (:codec/"
     "unsupported), the body consuming every byte before the "
     "big-endian Adler-32 trailer, which is verified. Output past "
     ":max-bytes throws :codec/limit (default cap 64 MiB); input must "
     "be bytes."},
};

const size_t k_prims_compress_count =
    sizeof(k_prims_compress) / sizeof(k_prims_compress[0]);
