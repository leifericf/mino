/*
 * prim/bits.c -- Erlang-inspired bit-syntax surface over MINO_BYTES.
 *
 *   (bits [v :size N :type T :endian E :signed? B] ...)
 *   (bits-get bs :offset O :size N :type T :endian E :signed? B)
 *   (subbits bs start end)
 *
 * Segments specifiers:
 *
 *   :size N      -- bit count. Default depends on :type. For :int/:uint
 *                   default 8. For :float, must be 32 or 64. For :bytes,
 *                   default = (* 8 (bytes-len v)).
 *   :type T      -- :int, :uint, :float, or :bytes. Default :int.
 *   :endian E    -- :big or :little. Default :big. Native-endian opt-in
 *                   isn't shipped; embedders that need it can pin with
 *                   :big or :little explicitly.
 *   :signed? B   -- true to sign-extend on read of :int. Default false
 *                   so :int is read as raw bit-pattern -> long long. The
 *                   :uint synonym is provided for clarity.
 *
 * The packing layout uses MSB-first within each byte (Erlang's default
 * for the big-endian path). `:little` endian is supported only when the
 * size is a multiple of 8, matching Erlang's restriction. Bit-aligned
 * values keep their MINO_BYTES bit_tail in [1..7]; the resulting value
 * satisfies `(bitstring? ...)` but not `(bytes? ...)`.
 */

#include "runtime/internal.h"
#include "prim/internal.h"
#include "collections/internal.h"

#include <stdlib.h>
#include <string.h>
#include <math.h>

/* ------------------------------------------------------------------------- */
/* Bit-level helpers                                                         */
/* ------------------------------------------------------------------------- */

static void set_bit(unsigned char *buf, size_t bit_off, unsigned b)
{
    size_t by = bit_off / 8;
    unsigned bi = 7u - (unsigned)(bit_off % 8);
    if (b & 1u) buf[by] |=  (unsigned char)(1u << bi);
    else        buf[by] &= (unsigned char)~(1u << bi);
}

static unsigned get_bit(const unsigned char *buf, size_t bit_off)
{
    size_t by = bit_off / 8;
    unsigned bi = 7u - (unsigned)(bit_off % 8);
    return (unsigned)((buf[by] >> bi) & 1u);
}

/* Write n bits (0..64) from `val` into `buf` starting at `bit_off`. The
 * most-significant bit of val (bit n-1) goes first; the LSB (bit 0)
 * goes last. */
static void write_bits_be(unsigned char *buf, size_t bit_off,
                          unsigned n, uint64_t val)
{
    unsigned i;
    for (i = 0; i < n; i++) {
        unsigned b = (unsigned)((val >> (n - 1u - i)) & 1u);
        set_bit(buf, bit_off + i, b);
    }
}

/* Write a multi-byte value in little-endian: bytes lowest-order first.
 * Requires n to be a multiple of 8 (caller validates). */
static void write_bits_le(unsigned char *buf, size_t bit_off,
                          unsigned n, uint64_t val)
{
    unsigned bytes = n / 8u;
    unsigned i;
    for (i = 0; i < bytes; i++) {
        unsigned char b = (unsigned char)((val >> (i * 8u)) & 0xffu);
        write_bits_be(buf, bit_off + i * 8u, 8u, b);
    }
}

static uint64_t read_bits_be(const unsigned char *buf, size_t bit_off,
                              unsigned n)
{
    uint64_t v = 0u;
    unsigned i;
    for (i = 0; i < n; i++) {
        v = (v << 1) | (uint64_t)get_bit(buf, bit_off + i);
    }
    return v;
}

static uint64_t read_bits_le(const unsigned char *buf, size_t bit_off,
                              unsigned n)
{
    uint64_t v = 0u;
    unsigned bytes = n / 8u;
    unsigned i;
    for (i = 0; i < bytes; i++) {
        uint64_t b = (uint64_t)(unsigned char)read_bits_be(buf,
            bit_off + i * 8u, 8u);
        v |= b << (i * 8u);
    }
    return v;
}

/* Sign-extend a `n`-bit signed value to a full long long. */
static long long sign_extend(uint64_t v, unsigned n)
{
    if (n == 0u || n >= 64u) return (long long)v;
    {
        uint64_t mask = (uint64_t)1u << (n - 1u);
        if (v & mask) {
            uint64_t ext = (~(uint64_t)0u) << n;
            return (long long)(v | ext);
        }
        return (long long)v;
    }
}

/* ------------------------------------------------------------------------- */
/* Segment option parsing                                                    */
/* ------------------------------------------------------------------------- */

typedef enum {
    BITS_TYPE_INT   = 0,
    BITS_TYPE_UINT  = 1,
    BITS_TYPE_FLOAT = 2,
    BITS_TYPE_BYTES = 3
} bits_type_t;

typedef enum {
    BITS_BIG    = 0,
    BITS_LITTLE = 1
} bits_endian_t;

typedef struct {
    bits_type_t   type;
    bits_endian_t endian;
    int           signed_;   /* 1 if :signed? true */
    int           size_set;
    long long     size;       /* in bits */
} seg_opts_t;

static int kw_match(const mino_val *k, const char *name)
{
    if (k == NULL || mino_type_of(k) != MINO_KEYWORD) return 0;
    return strcmp(k->as.s.data, name) == 0;
}

/* Parse the option keyword/value pairs after the leading value of a
 * segment vector. `start` is the index in the vector after the value;
 * `vec_len` is the vector's length. Returns 0 on success, -1 on a
 * structural error (already throws via prim_throw_classified). */
static int parse_seg_opts(mino_state *S, const mino_val *vec,
                          size_t start, size_t vec_len, seg_opts_t *out,
                          const char *opname)
{
    size_t i;
    out->type     = BITS_TYPE_INT;
    out->endian   = BITS_BIG;
    out->signed_  = 0;
    out->size_set = 0;
    out->size     = 0;
    if ((vec_len - start) % 2u != 0u) {
        char buf[96];
        snprintf(buf, sizeof(buf),
            "%s: segment options must come in keyword/value pairs",
            opname);
        prim_throw_classified(S, "eval/type", "MTY001", buf);
        return -1;
    }
    for (i = start; i + 1 < vec_len; i += 2) {
        mino_val *k = vec_nth(vec, i);
        mino_val *v = vec_nth(vec, i + 1);
        if (kw_match(k, "size")) {
            if (v == NULL || !mino_val_int_p(v)
                || mino_val_int_get(v) < 0) {
                char buf[96];
                snprintf(buf, sizeof(buf),
                    "%s: :size must be a non-negative integer", opname);
                prim_throw_classified(S, "eval/type", "MTY001", buf);
                return -1;
            }
            out->size     = mino_val_int_get(v);
            out->size_set = 1;
        } else if (kw_match(k, "type")) {
            if (kw_match(v, "int"))        out->type = BITS_TYPE_INT;
            else if (kw_match(v, "uint"))  out->type = BITS_TYPE_UINT;
            else if (kw_match(v, "float")) out->type = BITS_TYPE_FLOAT;
            else if (kw_match(v, "bytes")) out->type = BITS_TYPE_BYTES;
            else {
                char buf[120];
                snprintf(buf, sizeof(buf),
                    "%s: :type must be :int, :uint, :float, or :bytes",
                    opname);
                prim_throw_classified(S, "eval/type", "MTY001", buf);
                return -1;
            }
        } else if (kw_match(k, "endian")) {
            if (kw_match(v, "big"))         out->endian = BITS_BIG;
            else if (kw_match(v, "little")) out->endian = BITS_LITTLE;
            else {
                char buf[120];
                snprintf(buf, sizeof(buf),
                    "%s: :endian must be :big or :little", opname);
                prim_throw_classified(S, "eval/type", "MTY001", buf);
                return -1;
            }
        } else if (kw_match(k, "signed?")) {
            out->signed_ = mino_is_truthy_inline(v) ? 1 : 0;
        } else {
            char buf[120];
            snprintf(buf, sizeof(buf),
                "%s: unknown segment option %s", opname,
                (k != NULL && mino_type_of(k) == MINO_KEYWORD) ? k->as.s.data : "<non-keyword>");
            prim_throw_classified(S, "eval/type", "MTY001", buf);
            return -1;
        }
    }
    return 0;
}

/* ------------------------------------------------------------------------- */
/* prim_bits                                                                 */
/* ------------------------------------------------------------------------- */

mino_val *prim_bits(mino_state *S, mino_val *args, mino_env *env)
{
    /* Two passes: first compute total bit length, then write. */
    size_t total_bits = 0;
    size_t total_bytes;
    unsigned char *buf;
    size_t pos = 0;
    mino_val *result;
    mino_val *p;
    (void)env;
    /* Pass 1: total bit length. */
    for (p = args; p != NULL && mino_is_cons(p); p = p->as.cons.cdr) {
        mino_val *seg = p->as.cons.car;
        size_t vec_len;
        mino_val *value;
        seg_opts_t opts;
        if (seg == NULL || mino_type_of(seg) != MINO_VECTOR
            || seg->as.vec.len == 0) {
            return prim_throw_classified(S, "eval/type", "MTY001",
                "bits: each segment must be a [value & opts] vector");
        }
        vec_len = seg->as.vec.len;
        value = vec_nth(seg, 0);
        if (parse_seg_opts(S, seg, 1, vec_len, &opts, "bits") != 0) {
            return NULL;
        }
        if (!opts.size_set) {
            if (opts.type == BITS_TYPE_FLOAT) {
                opts.size = 64;
            } else if (opts.type == BITS_TYPE_BYTES) {
                if (value == NULL || mino_type_of(value) != MINO_BYTES) {
                    return prim_throw_classified(S, "eval/type", "MTY001",
                        "bits: :type :bytes requires a bytes-typed value");
                }
                opts.size = (long long)mino_bytes_bit_len(value);
            } else {
                opts.size = 8;
            }
        }
        if (opts.size < 0 || opts.size > 64) {
            if (opts.type != BITS_TYPE_BYTES) {
                return prim_throw_classified(S, "eval/type", "MTY001",
                    "bits: :size must be in 0..64 for :int/:uint/:float");
            }
        }
        if (opts.type == BITS_TYPE_FLOAT
            && opts.size != 32 && opts.size != 64) {
            return prim_throw_classified(S, "eval/type", "MTY001",
                "bits: :type :float requires :size of 32 or 64");
        }
        if (opts.endian == BITS_LITTLE && (opts.size % 8) != 0) {
            return prim_throw_classified(S, "eval/type", "MTY001",
                "bits: :endian :little requires :size to be a multiple of 8");
        }
        if (opts.type == BITS_TYPE_BYTES) {
            if (value == NULL || mino_type_of(value) != MINO_BYTES) {
                return prim_throw_classified(S, "eval/type", "MTY001",
                    "bits: :type :bytes requires a bytes-typed value");
            }
            if ((size_t)opts.size > mino_bytes_bit_len(value)) {
                return prim_throw_classified(S, "eval/bounds", "MBD001",
                    "bits: :type :bytes :size exceeds source bit length");
            }
        }
        total_bits += (size_t)opts.size;
    }
    total_bytes = (total_bits + 7u) / 8u;
    buf = (unsigned char *)calloc(1, total_bytes > 0 ? total_bytes : 1);
    if (buf == NULL && total_bytes > 0) {
        return prim_throw_classified(S, "internal", "MIN001",
            "bits: out of memory");
    }
    /* Pass 2: write each segment. */
    for (p = args; p != NULL && mino_is_cons(p); p = p->as.cons.cdr) {
        mino_val *seg = p->as.cons.car;
        size_t vec_len = seg->as.vec.len;
        mino_val *value = vec_nth(seg, 0);
        seg_opts_t opts;
        (void)parse_seg_opts(S, seg, 1, vec_len, &opts, "bits");
        if (!opts.size_set) {
            if (opts.type == BITS_TYPE_FLOAT) {
                opts.size = 64;
            } else if (opts.type == BITS_TYPE_BYTES) {
                opts.size = (long long)mino_bytes_bit_len(value);
            } else {
                opts.size = 8;
            }
        }
        if (opts.type == BITS_TYPE_INT || opts.type == BITS_TYPE_UINT) {
            long long vv;
            if (value == NULL || !mino_val_int_p(value)) {
                free(buf);
                return prim_throw_classified(S, "eval/type", "MTY001",
                    "bits: :type :int/:uint requires an integer value");
            }
            vv = mino_val_int_get(value);
            /* Range check for unsigned. For signed, allow the natural
             * two's-complement range. */
            if (opts.size < 64) {
                if (opts.type == BITS_TYPE_UINT) {
                    long long max_u = (long long)((1ULL << opts.size) - 1u);
                    if (vv < 0 || vv > max_u) {
                        free(buf);
                        return prim_throw_classified(S, "eval/bounds", "MBD001",
                            "bits: integer value out of unsigned range for :size");
                    }
                } else {
                    long long max_s = (long long)((1ULL << (opts.size - 1)) - 1u);
                    long long min_s = -max_s - 1;
                    if (vv < min_s || vv > max_s) {
                        if (vv < 0 || vv > (long long)((1ULL << opts.size) - 1u)) {
                            free(buf);
                            return prim_throw_classified(S, "eval/bounds", "MBD001",
                                "bits: integer value out of signed/unsigned range for :size");
                        }
                    }
                }
            }
            {
                uint64_t u = (uint64_t)vv;
                if (opts.size < 64) {
                    u &= ((uint64_t)1u << opts.size) - 1u;
                }
                if (opts.endian == BITS_LITTLE) {
                    write_bits_le(buf, pos, (unsigned)opts.size, u);
                } else {
                    write_bits_be(buf, pos, (unsigned)opts.size, u);
                }
            }
        } else if (opts.type == BITS_TYPE_FLOAT) {
            double d = 0.0;
            uint64_t u = 0;
            if (value != NULL && mino_val_int_p(value)) {
                d = (double)mino_val_int_get(value);
            } else if (value != NULL && (mino_type_of(value) == MINO_FLOAT
                                          || mino_type_of(value) == MINO_FLOAT32)) {
                d = value->as.f;
            } else {
                free(buf);
                return prim_throw_classified(S, "eval/type", "MTY001",
                    "bits: :type :float requires a number");
            }
            if (opts.size == 32) {
                float f = (float)d;
                uint32_t u32;
                memcpy(&u32, &f, 4);
                u = (uint64_t)u32;
            } else {
                memcpy(&u, &d, 8);
            }
            if (opts.endian == BITS_LITTLE) {
                write_bits_le(buf, pos, (unsigned)opts.size, u);
            } else {
                write_bits_be(buf, pos, (unsigned)opts.size, u);
            }
        } else { /* BITS_TYPE_BYTES */
            size_t bits_to_copy = (size_t)opts.size;
            size_t i;
            const unsigned char *src = mino_bytes_data(value);
            for (i = 0; i < bits_to_copy; i++) {
                set_bit(buf, pos + i, get_bit(src, i));
            }
        }
        pos += (size_t)opts.size;
    }
    result = mino_bytes(S, buf, total_bytes);
    free(buf);
    if (result != NULL) {
        unsigned tail = (unsigned)(total_bits % 8u);
        result->as.bytes.bit_tail = (uint8_t)tail;
        result->as.bytes.byte_len = total_bytes;
    }
    return result;
}

/* ------------------------------------------------------------------------- */
/* prim_bits_get                                                             */
/* ------------------------------------------------------------------------- */

/* (bits-get bs :offset O :size N :type T :endian E :signed? B) */
mino_val *prim_bits_get(mino_state *S, mino_val *args, mino_env *env)
{
    mino_val *bs;
    size_t arglen = list_length(S, args);
    long long offset = 0;
    bits_type_t type = BITS_TYPE_INT;
    bits_endian_t endian = BITS_BIG;
    int signed_ = 0;
    int size_set = 0;
    long long size = 0;
    mino_val *p;
    size_t total_bits;
    (void)env;
    if (arglen < 1u || ((arglen - 1u) % 2u) != 0u) {
        return prim_throw_classified(S, "eval/arity", "MAR001",
            "bits-get: (bits-get bs & opts), opts in keyword/value pairs");
    }
    bs = args->as.cons.car;
    if (bs == NULL || mino_type_of(bs) != MINO_BYTES) {
        return prim_throw_classified(S, "eval/type", "MTY001",
            "bits-get: first argument must be a bytes value");
    }
    total_bits = mino_bytes_bit_len(bs);
    for (p = args->as.cons.cdr; p != NULL && mino_is_cons(p);
         p = p->as.cons.cdr) {
        mino_val *k = p->as.cons.car;
        if (!mino_is_cons(p->as.cons.cdr)) break;
        p = p->as.cons.cdr;
        {
            mino_val *v = p->as.cons.car;
            if (kw_match(k, "offset")) {
                if (v == NULL || !mino_val_int_p(v) || mino_val_int_get(v) < 0) {
                    return prim_throw_classified(S, "eval/type", "MTY001",
                        "bits-get: :offset must be a non-negative integer");
                }
                offset = mino_val_int_get(v);
            } else if (kw_match(k, "size")) {
                if (v == NULL || !mino_val_int_p(v) || mino_val_int_get(v) < 0) {
                    return prim_throw_classified(S, "eval/type", "MTY001",
                        "bits-get: :size must be a non-negative integer");
                }
                size = mino_val_int_get(v);
                size_set = 1;
            } else if (kw_match(k, "type")) {
                if      (kw_match(v, "int"))   type = BITS_TYPE_INT;
                else if (kw_match(v, "uint"))  type = BITS_TYPE_UINT;
                else if (kw_match(v, "float")) type = BITS_TYPE_FLOAT;
                else if (kw_match(v, "bytes")) type = BITS_TYPE_BYTES;
                else return prim_throw_classified(S, "eval/type", "MTY001",
                    "bits-get: :type must be :int, :uint, :float, or :bytes");
            } else if (kw_match(k, "endian")) {
                if      (kw_match(v, "big"))    endian = BITS_BIG;
                else if (kw_match(v, "little")) endian = BITS_LITTLE;
                else return prim_throw_classified(S, "eval/type", "MTY001",
                    "bits-get: :endian must be :big or :little");
            } else if (kw_match(k, "signed?")) {
                signed_ = mino_is_truthy_inline(v) ? 1 : 0;
            } else {
                return prim_throw_classified(S, "eval/type", "MTY001",
                    "bits-get: unknown option");
            }
        }
    }
    if (!size_set) size = 8;
    if (size < 0) {
        return prim_throw_classified(S, "eval/bounds", "MBD001",
            "bits-get: :size must be non-negative");
    }
    if ((size_t)(offset + size) > total_bits) {
        return prim_throw_classified(S, "eval/bounds", "MBD001",
            "bits-get: :offset + :size exceeds bit length");
    }
    if (endian == BITS_LITTLE && (size % 8) != 0) {
        return prim_throw_classified(S, "eval/type", "MTY001",
            "bits-get: :endian :little requires :size to be a multiple of 8");
    }
    if (type == BITS_TYPE_FLOAT && size != 32 && size != 64) {
        return prim_throw_classified(S, "eval/type", "MTY001",
            "bits-get: :type :float requires :size 32 or 64");
    }
    if (type == BITS_TYPE_BYTES) {
        /* Slice. Produce a fresh MINO_BYTES holding the bit range. */
        size_t n_bits = (size_t)size;
        size_t n_bytes = (n_bits + 7u) / 8u;
        size_t i;
        unsigned char *out;
        mino_val *result;
        out = (unsigned char *)calloc(1, n_bytes > 0 ? n_bytes : 1);
        if (out == NULL && n_bytes > 0) {
            return prim_throw_classified(S, "internal", "MIN001",
                "bits-get: out of memory");
        }
        for (i = 0; i < n_bits; i++) {
            set_bit(out, i, get_bit(mino_bytes_data(bs),
                                    (size_t)offset + i));
        }
        result = mino_bytes(S, out, n_bytes);
        free(out);
        if (result != NULL) {
            result->as.bytes.bit_tail = (uint8_t)(n_bits % 8u);
        }
        return result;
    }
    {
        uint64_t u;
        if (endian == BITS_LITTLE) {
            u = read_bits_le(mino_bytes_data(bs),
                              (size_t)offset, (unsigned)size);
        } else {
            u = read_bits_be(mino_bytes_data(bs),
                              (size_t)offset, (unsigned)size);
        }
        if (type == BITS_TYPE_FLOAT) {
            if (size == 32) {
                float f;
                uint32_t u32 = (uint32_t)u;
                memcpy(&f, &u32, 4);
                return mino_float(S, (double)f);
            }
            {
                double d;
                memcpy(&d, &u, 8);
                return mino_float(S, d);
            }
        }
        if (type == BITS_TYPE_INT && signed_) {
            return mino_int(S, sign_extend(u, (unsigned)size));
        }
        return mino_int(S, (long long)u);
    }
}

/* ------------------------------------------------------------------------- */
/* prim_subbits                                                              */
/* ------------------------------------------------------------------------- */

mino_val *prim_subbits(mino_state *S, mino_val *args, mino_env *env)
{
    mino_val *bs;
    long long start;
    long long end;
    size_t total_bits;
    size_t n_bits;
    size_t n_bytes;
    size_t i;
    unsigned char *out;
    mino_val *result;
    (void)env;
    if (!mino_is_cons(args) || !mino_is_cons(args->as.cons.cdr)
        || !mino_is_cons(args->as.cons.cdr->as.cons.cdr)
        || mino_is_cons(args->as.cons.cdr->as.cons.cdr->as.cons.cdr)) {
        return prim_throw_classified(S, "eval/arity", "MAR001",
            "subbits requires three arguments (bs, start, end)");
    }
    bs = args->as.cons.car;
    if (bs == NULL || mino_type_of(bs) != MINO_BYTES) {
        return prim_throw_classified(S, "eval/type", "MTY001",
            "subbits: first argument must be a bytes value");
    }
    {
        mino_val *sv = args->as.cons.cdr->as.cons.car;
        mino_val *ev = args->as.cons.cdr->as.cons.cdr->as.cons.car;
        if (sv == NULL || !mino_val_int_p(sv)
            || ev == NULL || !mino_val_int_p(ev)) {
            return prim_throw_classified(S, "eval/type", "MTY001",
                "subbits: start and end must be integers");
        }
        start = mino_val_int_get(sv);
        end   = mino_val_int_get(ev);
    }
    total_bits = mino_bytes_bit_len(bs);
    if (start < 0 || end < start || (size_t)end > total_bits) {
        return prim_throw_classified(S, "eval/bounds", "MBD001",
            "subbits: range out of bit length");
    }
    n_bits = (size_t)(end - start);
    n_bytes = (n_bits + 7u) / 8u;
    out = (unsigned char *)calloc(1, n_bytes > 0 ? n_bytes : 1);
    if (out == NULL && n_bytes > 0) {
        return prim_throw_classified(S, "internal", "MIN001",
            "subbits: out of memory");
    }
    for (i = 0; i < n_bits; i++) {
        set_bit(out, i, get_bit(mino_bytes_data(bs),
                                (size_t)start + i));
    }
    result = mino_bytes(S, out, n_bytes);
    free(out);
    if (result != NULL) {
        result->as.bytes.bit_tail = (uint8_t)(n_bits % 8u);
    }
    return result;
}

/* ------------------------------------------------------------------------- */
/* Registration table                                                        */
/* ------------------------------------------------------------------------- */

const mino_prim_def k_prims_bits[] = {
    {"bits",      prim_bits,
     "Pack a sequence of [value & options] segments into an immutable "
     "MINO_BYTES value. Options: :size (bits), :type (:int/:uint/:float/"
     ":bytes), :endian (:big/:little), :signed? (true/false). Bit-"
     "aligned totals leave a 1..7 bit_tail; the result satisfies "
     "bitstring? but not necessarily bytes?."},
    {"bits-get", prim_bits_get,
     "Read a bit field out of a bytes value. Required: :offset and :size. "
     "Optional: :type (:int/:uint/:float/:bytes), :endian, :signed?. "
     "For :type :bytes returns a MINO_BYTES slice; for :float returns "
     "a double; otherwise returns an integer."},
    {"subbits",   prim_subbits,
     "Zero-copy-semantics slice of a bytes value over a half-open bit "
     "range [start..end). Result satisfies bitstring? and is byte-"
     "aligned when (- end start) is a multiple of 8."},
};

const size_t k_prims_bits_count =
    sizeof(k_prims_bits) / sizeof(k_prims_bits[0]);
