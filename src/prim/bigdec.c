/*
 * bigdec.c -- MINO_BIGDEC implementation: arbitrary-precision decimal
 * arithmetic. Uses MINO_BIGINT cells (in bignum.c) for the unscaled
 * payload; sharing helpers come in via bignum_shared.h.
 *
 * Internal to the prim subsystem; embedders should only use mino.h.
 */

#include "prim/internal.h"
#include "prim/bignum_shared.h"
#include "imath.h"

#include <ctype.h>
#include <inttypes.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>


/* ------------------------------------------------------------------------- */
/* MINO_BIGDEC                                                                */
/* ------------------------------------------------------------------------- */

mino_val *mino_bigdec_make(mino_state *S, mino_val *unscaled, int scale)
{
    mino_val *v;
    if (unscaled == NULL || mino_type_of(unscaled) != MINO_BIGINT) {
        return prim_throw_classified(S, "internal", "MIN001",
                                     "bigdec: unscaled must be a bigint");
    }
    v = alloc_val(S, MINO_BIGDEC);
    v->as.bigdec.unscaled = unscaled;
    v->as.bigdec.scale    = scale;
    return v;
}

/* Parse a base-10 decimal numeric string into a bigdec. Recognises:
 *   optional sign, integer part, '.' fractional part, 'e'/'E' exponent.
 * Returns NULL on parse failure. */
mino_val *mino_bigdec_from_string(mino_state *S, const char *s)
{
    const char *p, *frac_start;
    char       *digits;
    size_t      digit_count, dlen;
    int         sign = 1, frac_len = 0, has_dot = 0, exp = 0;
    mino_val *unscaled;
    if (s == NULL || *s == '\0') return NULL;
    p = s;
    if (*p == '+') p++;
    else if (*p == '-') { sign = -1; p++; }
    /* Count digits and detect dot / exponent. Build a digits-only buffer
     * (sign attached afterwards) so mp_int_read_string sees a valid
     * integer literal even if the source had a '.'. */
    digits = (char *)malloc(strlen(p) + 2);
    if (digits == NULL) return NULL;
    digit_count = 0;
    if (sign < 0) digits[digit_count++] = '-';
    frac_start = NULL;
    while (*p) {
        if (*p >= '0' && *p <= '9') {
            digits[digit_count++] = *p++;
            if (has_dot) frac_len++;
        } else if (*p == '.' && !has_dot) {
            has_dot = 1;
            frac_start = p;
            p++;
        } else if ((*p == 'e' || *p == 'E') && (p != s)) {
            char *end;
            long  e = strtol(p + 1, &end, 10);
            if (end == p + 1) { free(digits); return NULL; }
            exp = (int)e;
            p = end;
            break;
        } else {
            free(digits); return NULL;
        }
    }
    if (*p != '\0') { free(digits); return NULL; }
    if (digit_count == (size_t)(sign < 0 ? 1 : 0)) {
        /* No digits at all. */
        free(digits); return NULL;
    }
    digits[digit_count] = '\0';
    dlen = digit_count;
    unscaled = mino_bigint_from_string_n(S, digits, dlen);
    free(digits);
    (void)frac_start;
    if (unscaled == NULL) return NULL;
    /* The decimal scale is (frac_len - exp). Negative scale means
     * trailing zeros — scale up by multiplying unscaled by 10^|scale|. */
    {
        int scale = frac_len - exp;
        if (scale < 0) {
            mpz_t pw;
            if (mp_int_init(&pw) != MP_OK) {
                return prim_throw_classified(S, "eval/out-of-memory",
                                             "MOM001", "out of memory");
            }
            if (mp_int_set_value(&pw, 10) != MP_OK ||
                mp_int_expt(&pw, -scale, &pw) != MP_OK) {
                mp_int_clear(&pw);
                return prim_throw_classified(S, "eval/out-of-memory",
                                             "MOM001", "out of memory");
            }
            mp_int_mul((mp_int)unscaled->as.bigint.mpz, &pw,
                       (mp_int)unscaled->as.bigint.mpz);
            mp_int_clear(&pw);
            scale = 0;
        }
        return mino_bigdec_make(S, unscaled, scale);
    }
}

int mino_to_bigdec_str(const mino_val *v, char *buf, size_t n,
                       size_t *out_written)
{
    char *digits;
    int   scale, neg, len, int_part_len;
    size_t w = 0;
    if (v == NULL || mino_type_of(v) != MINO_BIGDEC) return 0;
    if (buf == NULL || n == 0) return -1;
    digits = mino_bigint_to_cstr(v->as.bigdec.unscaled);
    if (digits == NULL) return 0;
    scale = v->as.bigdec.scale;
    neg   = (digits[0] == '-');
    len   = (int)strlen(digits);
    if (scale == 0) {
        if ((size_t)len >= n) { free(digits); return -1; }
        memcpy(buf, digits, (size_t)len);
        buf[len] = '\0';
        w = (size_t)len;
    } else {
        int_part_len = len - (neg ? 1 : 0) - scale;
        if (int_part_len > 0) {
            size_t total = (size_t)len + 1; /* one extra for the '.' */
            if (total >= n) { free(digits); return -1; }
            {
                int i, j = 0;
                for (i = 0; i < (neg ? 1 : 0) + int_part_len; i++) buf[j++] = digits[i];
                buf[j++] = '.';
                for (; i < len; i++) buf[j++] = digits[i];
                buf[j] = '\0';
                w = (size_t)j;
            }
        } else {
            int pad = -int_part_len;
            size_t total = (neg ? 1u : 0u) + 2u + (size_t)pad
                         + (size_t)len - (neg ? 1u : 0u);
            int j = 0;
            if (total >= n) { free(digits); return -1; }
            if (neg) buf[j++] = '-';
            buf[j++] = '0';
            buf[j++] = '.';
            { int p; for (p = 0; p < pad; p++) buf[j++] = '0'; }
            {
                int i;
                for (i = (neg ? 1 : 0); i < len; i++) buf[j++] = digits[i];
            }
            buf[j] = '\0';
            w = (size_t)j;
        }
    }
    free(digits);
    if (out_written != NULL) *out_written = w;
    return 1;
}

/* Print a bigdec as "123.45M" — Clojure's bigdec literal form. */
void mino_bigdec_print(mino_state *S, const mino_val *v, FILE *out)
{
    char *digits;
    int   scale;
    int   neg;
    int   len;
    if (v == NULL || mino_type_of(v) != MINO_BIGDEC) {
        fputs("0M", out); return;
    }
    digits = mino_bigint_to_cstr(v->as.bigdec.unscaled);
    if (digits == NULL) { fputs("0M", out); return; }
    scale = v->as.bigdec.scale;
    neg   = (digits[0] == '-');
    len   = (int)strlen(digits);
    if (scale == 0) {
        fputs(digits, out);
        fputc('M', out);
    } else {
        int int_part_len = len - (neg ? 1 : 0) - scale;
        if (int_part_len > 0) {
            /* Insert '.' before the last `scale` digits. */
            int i;
            for (i = 0; i < (neg ? 1 : 0) + int_part_len; i++) fputc(digits[i], out);
            fputc('.', out);
            for (; i < len; i++) fputc(digits[i], out);
            fputc('M', out);
        } else {
            /* Need leading "0." and zero-padding. */
            int pad;
            if (neg) fputc('-', out);
            fputs("0.", out);
            for (pad = 0; pad < -int_part_len; pad++) fputc('0', out);
            fputs(digits + (neg ? 1 : 0), out);
            fputc('M', out);
        }
    }
    free(digits);
    (void)S;
}

/* Equality for bigdecs is numerical: `(= 1.0M 1.00M)` is true.
 * Mirrors Clojure's `=` (which routes BigDecimal through
 * `Numbers.equiv` -> `compareTo == 0`) rather than Java's
 * `BigDecimal.equals` (which is scale-strict). To compare, scale up
 * the lower-scaled side to match the higher and check unscaled
 * equality. The hash strips trailing zeros so the equal-implies-
 * equal-hash invariant holds across scales. */
int mino_bigdec_equals(const mino_val *a, const mino_val *b)
{
    int   sa, sb, smax;
    mpz_t au, bu, pw;
    int   eq;
    if (a == NULL || b == NULL) return 0;
    if (mino_type_of(a) != MINO_BIGDEC || mino_type_of(b) != MINO_BIGDEC) return 0;
    sa = a->as.bigdec.scale;
    sb = b->as.bigdec.scale;
    if (sa == sb) {
        return mino_bigint_equals(a->as.bigdec.unscaled, b->as.bigdec.unscaled);
    }
    smax = sa > sb ? sa : sb;
    if (mp_int_init(&au) != MP_OK) return 0;
    if (mp_int_init(&bu) != MP_OK) { mp_int_clear(&au); return 0; }
    if (mp_int_init(&pw) != MP_OK) { mp_int_clear(&au); mp_int_clear(&bu); return 0; }
    mp_int_copy((mp_int)a->as.bigdec.unscaled->as.bigint.mpz, &au);
    mp_int_copy((mp_int)b->as.bigdec.unscaled->as.bigint.mpz, &bu);
    if (smax - sa > 0) {
        mp_int_set_value(&pw, 10);
        if (mp_int_expt(&pw, smax - sa, &pw) != MP_OK) goto fail;
        if (mp_int_mul(&au, &pw, &au) != MP_OK) goto fail;
    }
    if (smax - sb > 0) {
        mp_int_set_value(&pw, 10);
        if (mp_int_expt(&pw, smax - sb, &pw) != MP_OK) goto fail;
        if (mp_int_mul(&bu, &pw, &bu) != MP_OK) goto fail;
    }
    eq = (mp_int_compare(&au, &bu) == 0);
    mp_int_clear(&au); mp_int_clear(&bu); mp_int_clear(&pw);
    return eq;
fail:
    mp_int_clear(&au); mp_int_clear(&bu); mp_int_clear(&pw);
    return 0;
}

uint32_t mino_bigdec_hash(const mino_val *v)
{
    /* Hash the value in trailing-zero-stripped form so 1.0M and 1.00M
     * (numerically equal) hash the same. Walk a copy of the unscaled
     * bigint, dividing by 10 while it has a trailing zero and scale >
     * 0; the resulting (unscaled, scale) pair is the canonical rep. */
    mpz_t       acc;
    mpz_t       ten;
    mpz_t       q;
    mpz_t       r;
    int         scale;
    uint32_t    h;
    if (v == NULL || mino_type_of(v) != MINO_BIGDEC) return 0;
    scale = v->as.bigdec.scale;
    if (mp_int_init(&acc) != MP_OK) return 0;
    if (mp_int_copy((mp_int)v->as.bigdec.unscaled->as.bigint.mpz, &acc)
        != MP_OK) {
        mp_int_clear(&acc);
        return 0;
    }
    if (mp_int_init(&ten) != MP_OK) { mp_int_clear(&acc); return 0; }
    if (mp_int_init(&q)   != MP_OK) { mp_int_clear(&acc); mp_int_clear(&ten); return 0; }
    if (mp_int_init(&r)   != MP_OK) { mp_int_clear(&acc); mp_int_clear(&ten); mp_int_clear(&q); return 0; }
    mp_int_set_value(&ten, 10);
    while (scale > 0 && mp_int_compare_zero(&acc) != 0) {
        if (mp_int_div(&acc, &ten, &q, &r) != MP_OK) break;
        if (mp_int_compare_zero(&r) != 0) break;
        mp_int_copy(&q, &acc);
        scale--;
    }
    if (mp_int_compare_zero(&acc) == 0) scale = 0;
    {
        /* Wrap the canonical mpz in a temporary mino_val so we can
         * reuse mino_bigint_hash without copying again. */
        mino_val tmp;
        memset(&tmp, 0, sizeof(tmp));
        tmp.type = MINO_BIGINT;
        tmp.as.bigint.mpz = &acc;
        h = mino_bigint_hash(&tmp);
    }
    mp_int_clear(&acc);
    mp_int_clear(&ten);
    mp_int_clear(&q);
    mp_int_clear(&r);
    h ^= ((uint32_t)scale) * 0x27d4eb2du;
    return h ^ 0x165667b1u;
}

double mino_bigdec_to_double(const mino_val *v)
{
    double d;
    int    scale;
    if (v == NULL || mino_type_of(v) != MINO_BIGDEC) return 0.0;
    d = mino_bigint_to_double(v->as.bigdec.unscaled);
    scale = v->as.bigdec.scale;
    while (scale > 0)  { d /= 10.0; scale--; }
    while (scale < 0)  { d *= 10.0; scale++; }
    return d;
}

/* (bigdec x) — coerce. */
mino_val *prim_bigdec(mino_state *S, mino_val *args, mino_env *env)
{
    mino_val *x;
    (void)env;
    if (!mino_is_cons(args) || mino_is_cons(args->as.cons.cdr)) {
        return prim_throw_classified(S, "eval/arity", "MAR001",
                                     "bigdec requires one argument");
    }
    x = args->as.cons.car;
    if (x == NULL)
        return prim_throw_classified(S, "eval/type", "MTY001",
                                     "bigdec: nil");
    if (mino_type_of(x) == MINO_BIGDEC) return x;
    if (mino_val_int_p(x)) {
        mino_val *u = mino_bigint_from_ll(S, mino_val_int_get(x));
        if (u == NULL) return NULL;
        return mino_bigdec_make(S, u, 0);
    }
    if (mino_type_of(x) == MINO_BIGINT) {
        mino_val *u = to_bigint(S, x);
        if (u == NULL) return NULL;
        return mino_bigdec_make(S, u, 0);
    }
    if (mino_type_of(x) == MINO_FLOAT) {
        char buf[64];
        snprintf(buf, sizeof(buf), "%.17g", x->as.f);
        return mino_bigdec_from_string(S, buf);
    }
    if (mino_type_of(x) == MINO_STRING) {
        mino_val *r = mino_bigdec_from_string(S, x->as.s.data);
        if (r == NULL)
            return prim_throw_classified(S, "eval/type", "MTY001",
                                         "bigdec: invalid numeric string");
        return r;
    }
    if (mino_type_of(x) == MINO_RATIO) {
        /* Convert via float then string. Lossy; the user can pre-call
         * with-precision once it's wired. */
        char buf[64];
        snprintf(buf, sizeof(buf), "%.17g", mino_ratio_to_double(x));
        return mino_bigdec_from_string(S, buf);
    }
    return prim_throw_classified(S, "eval/type", "MTY001",
                                 "bigdec: unsupported argument type");
}

/* (decimal? x) — true for bigdec only. */
mino_val *prim_decimal_p(mino_state *S, mino_val *args, mino_env *env)
{
    (void)env;
    if (!mino_is_cons(args) || mino_is_cons(args->as.cons.cdr)) {
        return prim_throw_classified(S, "eval/arity", "MAR001",
                                     "decimal? requires one argument");
    }
    {
        mino_val *x = args->as.cons.car;
        return (x != NULL && mino_type_of(x) == MINO_BIGDEC)
            ? mino_true(S) : mino_false(S);
    }
}

/* ------------------------------------------------------------------------- */
/* Tier promotion helpers + bigdec arithmetic                                 */
/* ------------------------------------------------------------------------- */

/* Promote int / bigint / ratio to bigdec at scale 0. Lossless. */
mino_val *mino_to_bigdec(mino_state *S, const mino_val *v)
{
    if (v == NULL) return NULL;
    if (mino_type_of(v) == MINO_BIGDEC) return (mino_val *)v;
    if (mino_val_int_p(v)) {
        mino_val *u = mino_bigint_from_ll(S, mino_val_int_get(v));
        if (u == NULL) return NULL;
        return mino_bigdec_make(S, u, 0);
    }
    if (mino_type_of(v) == MINO_BIGINT) {
        mino_val *u = to_bigint(S, v);
        if (u == NULL) return NULL;
        return mino_bigdec_make(S, u, 0);
    }
    /* Ratio promotion to bigdec is non-exact in general (e.g. 1/3) so the
     * tower-dispatch caller drops to float when ratio meets bigdec. */
    return NULL;
}

/* Scale an unscaled bigint up by 10^delta in place; delta must be >= 0. */
static int bigint_mul_pow10(mino_val *bi, int delta)
{
    if (delta <= 0) return 1;
    {
        mpz_t pw;
        if (mp_int_init(&pw) != MP_OK) return 0;
        if (mp_int_set_value(&pw, 10) != MP_OK ||
            mp_int_expt(&pw, delta, &pw) != MP_OK) {
            mp_int_clear(&pw);
            return 0;
        }
        mp_int_mul((mp_int)bi->as.bigint.mpz, &pw,
                   (mp_int)bi->as.bigint.mpz);
        mp_int_clear(&pw);
    }
    return 1;
}

/* Bigdec add / sub: align scales then add unscaled bigints. */
mino_val *mino_bigdec_add(mino_state *S, const mino_val *a,
                            const mino_val *b)
{
    int        sa, sb, smax;
    mino_val *au, *bu, *result;
    if (a == NULL || b == NULL || mino_type_of(a) != MINO_BIGDEC || mino_type_of(b) != MINO_BIGDEC) {
        return prim_throw_classified(S, "eval/type", "MTY001",
                                     "bigdec add: bigdec operands required");
    }
    sa = a->as.bigdec.scale;
    sb = b->as.bigdec.scale;
    smax = sa > sb ? sa : sb;
    au = to_bigint(S, a->as.bigdec.unscaled);
    if (au == NULL) return NULL;
    bu = to_bigint(S, b->as.bigdec.unscaled);
    if (bu == NULL) return NULL;
    if (!bigint_mul_pow10(au, smax - sa) || !bigint_mul_pow10(bu, smax - sb)) {
        return prim_throw_classified(S, "eval/out-of-memory", "MOM001",
                                     "out of memory in bigdec add");
    }
    result = mino_bigint_add(S, au, bu);
    if (result == NULL) return NULL;
    return mino_bigdec_make(S, result, smax);
}

mino_val *mino_bigdec_sub(mino_state *S, const mino_val *a,
                            const mino_val *b)
{
    int        sa, sb, smax;
    mino_val *au, *bu, *result;
    if (a == NULL || b == NULL || mino_type_of(a) != MINO_BIGDEC || mino_type_of(b) != MINO_BIGDEC) {
        return prim_throw_classified(S, "eval/type", "MTY001",
                                     "bigdec sub: bigdec operands required");
    }
    sa = a->as.bigdec.scale;
    sb = b->as.bigdec.scale;
    smax = sa > sb ? sa : sb;
    au = to_bigint(S, a->as.bigdec.unscaled);
    if (au == NULL) return NULL;
    bu = to_bigint(S, b->as.bigdec.unscaled);
    if (bu == NULL) return NULL;
    if (!bigint_mul_pow10(au, smax - sa) || !bigint_mul_pow10(bu, smax - sb)) {
        return prim_throw_classified(S, "eval/out-of-memory", "MOM001",
                                     "out of memory in bigdec sub");
    }
    result = mino_bigint_sub(S, au, bu);
    if (result == NULL) return NULL;
    return mino_bigdec_make(S, result, smax);
}

mino_val *mino_bigdec_mul(mino_state *S, const mino_val *a,
                            const mino_val *b)
{
    mino_val *result;
    if (a == NULL || b == NULL || mino_type_of(a) != MINO_BIGDEC || mino_type_of(b) != MINO_BIGDEC) {
        return prim_throw_classified(S, "eval/type", "MTY001",
                                     "bigdec mul: bigdec operands required");
    }
    result = mino_bigint_mul(S, a->as.bigdec.unscaled, b->as.bigdec.unscaled);
    if (result == NULL) return NULL;
    return mino_bigdec_make(S, result, a->as.bigdec.scale + b->as.bigdec.scale);
}

mino_val *mino_bigdec_neg(mino_state *S, const mino_val *a)
{
    mino_val *u;
    if (a == NULL || mino_type_of(a) != MINO_BIGDEC) {
        return prim_throw_classified(S, "eval/type", "MTY001",
                                     "bigdec neg: bigdec operand required");
    }
    u = mino_bigint_neg(S, a->as.bigdec.unscaled);
    if (u == NULL) return NULL;
    return mino_bigdec_make(S, u, a->as.bigdec.scale);
}

/* Bigdec quot / rem / mod operate on aligned-scale unscaled bigints.
 * The integer quotient (or remainder) lives at scale `smax`, so it is
 * re-encoded as a bigdec by multiplying by 10^smax (for quot, where
 * the value is the integer portion of the result) or by passing the
 * unscaled remainder through directly (for rem / mod). */
static int bigdec_align(mino_state *S, const mino_val *a,
                        const mino_val *b, mino_val **au_out,
                        mino_val **bu_out, int *smax_out)
{
    int sa = a->as.bigdec.scale;
    int sb = b->as.bigdec.scale;
    int smax = sa > sb ? sa : sb;
    mino_val *au = to_bigint(S, a->as.bigdec.unscaled);
    mino_val *bu;
    if (au == NULL) return 0;
    bu = to_bigint(S, b->as.bigdec.unscaled);
    if (bu == NULL) return 0;
    if (!bigint_mul_pow10(au, smax - sa) || !bigint_mul_pow10(bu, smax - sb)) {
        prim_throw_classified(S, "eval/out-of-memory", "MOM001",
                              "out of memory aligning bigdec scales");
        return 0;
    }
    *au_out = au;
    *bu_out = bu;
    *smax_out = smax;
    return 1;
}

mino_val *mino_bigdec_quot(mino_state *S, const mino_val *a,
                             const mino_val *b)
{
    mino_val *au, *bu, *q;
    int smax;
    if (a == NULL || b == NULL || mino_type_of(a) != MINO_BIGDEC || mino_type_of(b) != MINO_BIGDEC) {
        return prim_throw_classified(S, "eval/type", "MTY001",
                                     "bigdec quot: bigdec operands required");
    }
    if (!bigdec_align(S, a, b, &au, &bu, &smax)) return NULL;
    q = mino_bigint_quot(S, au, bu);
    if (q == NULL) return NULL;
    /* Re-encode q at scale smax: unscaled = q * 10^smax. */
    if (!bigint_mul_pow10(q, smax)) {
        return prim_throw_classified(S, "eval/out-of-memory", "MOM001",
                                     "out of memory in bigdec quot");
    }
    return mino_bigdec_make(S, q, smax);
}

mino_val *mino_bigdec_rem(mino_state *S, const mino_val *a,
                            const mino_val *b)
{
    mino_val *au, *bu, *r;
    int smax;
    if (a == NULL || b == NULL || mino_type_of(a) != MINO_BIGDEC || mino_type_of(b) != MINO_BIGDEC) {
        return prim_throw_classified(S, "eval/type", "MTY001",
                                     "bigdec rem: bigdec operands required");
    }
    if (!bigdec_align(S, a, b, &au, &bu, &smax)) return NULL;
    r = mino_bigint_rem(S, au, bu);
    if (r == NULL) return NULL;
    return mino_bigdec_make(S, r, smax);
}

/* Count significant decimal digits of |q|. mp_int_string_len returns
 * an over-estimate (it's the buffer size, not the exact digit count),
 * so we render to string and count actual digits. Zero counts as 1
 * digit (matches JVM BigInteger.toString().length()). */
static int bigdec_sig_digits(mp_int z)
{
    mp_result buf_len;
    char     *buf;
    int       digits;
    if (mp_int_compare_zero(z) == 0) return 1;
    buf_len = mp_int_string_len(z, 10);
    if (buf_len < 2) return 1;
    buf = (char *)malloc((size_t)buf_len);
    if (buf == NULL) return 1; /* OOM: caller will catch downstream */
    if (mp_int_to_string(z, 10, buf, (int)buf_len) != MP_OK) {
        free(buf);
        return 1;
    }
    /* strlen excludes the NUL; '-' counts as a non-digit. */
    digits = (int)strlen(buf);
    if (buf[0] == '-') digits--;
    free(buf);
    if (digits < 1) return 1;
    return digits;
}

/* Resolve *math-context* into precision + rounding mode. Returns 1
 * when a context is active (and *precision_out / *rmode_out are set);
 * returns 0 when no math-context is in scope (caller stays exact).
 * *rmode_out is one of:
 *   1  :half-up   -- implemented
 *  -1  any other  -- caller throws :mino/unsupported. */
static int resolve_math_context(mino_state *S, int *precision_out,
                                int *rmode_out)
{
    mino_val *mc = NULL;
    mino_val *p_key, *rm_key, *p_v, *rm_v;
    long long ll;
    if (mino_current_ctx(S)->dyn_stack != NULL) {
        mc = dyn_lookup(S, "*math-context*");
        if (mc == NULL) mc = dyn_lookup(S, "clojure.core/*math-context*");
    }
    if (mc == NULL) {
        mino_val *var = var_find(S, "clojure.core", "*math-context*");
        if (var != NULL && mino_type_of(var) == MINO_VAR
            && var->as.var.bound) {
            mc = var->as.var.root;
        }
    }
    if (mc == NULL || mino_type_of(mc) == MINO_NIL
        || mino_type_of(mc) != MINO_MAP) {
        return 0;
    }
    p_key  = mino_keyword(S, "precision");
    rm_key = mino_keyword(S, "rounding-mode");
    p_v    = mino_map_lookup(mc, p_key);
    rm_v   = mino_map_lookup(mc, rm_key);
    if (p_v == NULL || !mino_val_int_p(p_v)) return 0;
    ll = mino_val_int_get(p_v);
    if (ll <= 0 || ll > 0x7fffffff) return 0;
    *precision_out = (int)ll;
    /* Default rounding mode is :half-up. */
    *rmode_out = 1;
    if (rm_v != NULL && mino_type_of(rm_v) == MINO_KEYWORD) {
        if (rm_v->as.s.len == 7
            && memcmp(rm_v->as.s.data, "half-up", 7) == 0) {
            *rmode_out = 1;
        } else {
            *rmode_out = -1;  /* Signal unsupported */
        }
    }
    return 1;
}

/* mino_bigdec_div -- exact bigdec division. Mirrors Java's
 * BigDecimal.divide(BigDecimal): preferred scale is sa - sb, but the
 * quotient is computed at whatever scale makes it exact. If the
 * division has a non-terminating decimal expansion AND no
 * *math-context* is in scope, throw -- matching Java's
 * ArithmeticException. With *math-context* set, compute up to the
 * configured precision and round per the configured mode (only
 * :half-up is implemented; other modes throw :mino/unsupported with
 * the mode name). The trailing-zero canonicalisation on mino's
 * BigDecimal `=` (numerical equality) means callers can compare
 * results across scales without worrying about whether the algorithm
 * picked sa-sb vs sa-sb+k. */
mino_val *mino_bigdec_div(mino_state *S, const mino_val *a,
                            const mino_val *b)
{
    int   sa, sb;
    mpz_t num, den, q, r, pw;
    int   extra;
    int   mc_active;
    int   mc_precision = 0;
    int   mc_rmode     = 0;
    int   max_extra;
    mino_val *result = NULL;
    if (a == NULL || b == NULL || mino_type_of(a) != MINO_BIGDEC
        || mino_type_of(b) != MINO_BIGDEC) {
        return prim_throw_classified(S, "eval/type", "MTY001",
                                     "bigdec div: bigdec operands required");
    }
    if (mp_int_compare_zero(
            (mp_int)b->as.bigdec.unscaled->as.bigint.mpz) == 0) {
        return prim_throw_classified(S, "eval/type", "MTY001",
                                     "division by zero");
    }
    mc_active = resolve_math_context(S, &mc_precision, &mc_rmode);
    if (mc_active && mc_rmode == -1) {
        return prim_throw_classified(S, "host", "MHO002",
            "with-precision: only :half-up rounding mode is implemented");
    }
    /* When *math-context* is set, use its precision as the loop cap;
     * otherwise stay at the historical 1024-digit upper bound. */
    max_extra = mc_active ? (mc_precision + 8) : 1024;
    sa = a->as.bigdec.scale;
    sb = b->as.bigdec.scale;
    if (mp_int_init(&num) != MP_OK) goto oom_pre;
    if (mp_int_init(&den) != MP_OK) { mp_int_clear(&num); goto oom_pre; }
    if (mp_int_init(&q)   != MP_OK) { mp_int_clear(&num); mp_int_clear(&den); goto oom_pre; }
    if (mp_int_init(&r)   != MP_OK) { mp_int_clear(&num); mp_int_clear(&den); mp_int_clear(&q); goto oom_pre; }
    if (mp_int_init(&pw)  != MP_OK) { mp_int_clear(&num); mp_int_clear(&den); mp_int_clear(&q); mp_int_clear(&r); goto oom_pre; }
    mp_int_copy((mp_int)a->as.bigdec.unscaled->as.bigint.mpz, &num);
    mp_int_copy((mp_int)b->as.bigdec.unscaled->as.bigint.mpz, &den);
    /* Try increasing precision: numerator * 10^extra divided by denominator
     * until exact. The result lives at scale (sa - sb + extra). */
    for (extra = 0; extra <= max_extra; extra++) {
        if (mp_int_div(&num, &den, &q, &r) != MP_OK) goto oom;
        if (mp_int_compare_zero(&r) == 0) {
            mino_val *q_wrapped;
            mpz_t      *qz_heap = bigint_alloc_zeroed();
            if (qz_heap == NULL) goto oom;
            mp_int_copy(&q, qz_heap);
            q_wrapped = bigint_wrap(S, qz_heap);
            if (q_wrapped == NULL) goto oom;
            result = mino_bigdec_make(S, q_wrapped, sa - sb + extra);
            break;
        }
        /* When a math-context is active and we have at least
         * mc_precision+1 sig digits in q, we have one digit beyond the
         * target precision; round HALF-UP using that excess digit and
         * stop. Stopping at exactly mc_precision would truncate without
         * looking at the next digit and diverge from JVM's
         * BigDecimal.divide(BigDecimal, MathContext). */
        if (mc_active && bigdec_sig_digits(&q) >= mc_precision + 1) {
            int excess = bigdec_sig_digits(&q) - mc_precision;
            mino_val *q_wrapped;
            mpz_t      *qz_heap;
            if (excess > 0) {
                mpz_t divisor, q_rounded, r_rounded;
                int   half_up_carry;
                int   q_neg;
                if (mp_int_init(&divisor)    != MP_OK) goto oom;
                if (mp_int_init(&q_rounded)  != MP_OK) { mp_int_clear(&divisor); goto oom; }
                if (mp_int_init(&r_rounded)  != MP_OK) { mp_int_clear(&divisor); mp_int_clear(&q_rounded); goto oom; }
                /* divisor = 10^excess */
                mp_int_set_value(&divisor, 1);
                {
                    int k;
                    mp_int_set_value(&pw, 10);
                    for (k = 0; k < excess; k++) {
                        if (mp_int_mul(&divisor, &pw, &divisor) != MP_OK) {
                            mp_int_clear(&divisor);
                            mp_int_clear(&q_rounded);
                            mp_int_clear(&r_rounded);
                            goto oom;
                        }
                    }
                }
                /* q_rounded = |q| / divisor; remainder used for half-up. */
                {
                    mpz_t qabs;
                    if (mp_int_init(&qabs) != MP_OK) {
                        mp_int_clear(&divisor);
                        mp_int_clear(&q_rounded);
                        mp_int_clear(&r_rounded);
                        goto oom;
                    }
                    mp_int_abs(&q, &qabs);
                    q_neg = mp_int_compare_zero(&q) < 0;
                    if (mp_int_div(&qabs, &divisor, &q_rounded,
                                   &r_rounded) != MP_OK) {
                        mp_int_clear(&qabs);
                        mp_int_clear(&divisor);
                        mp_int_clear(&q_rounded);
                        mp_int_clear(&r_rounded);
                        goto oom;
                    }
                    mp_int_clear(&qabs);
                }
                /* HALF-UP: if 2*r >= divisor, round magnitude up. */
                {
                    mpz_t twice_r;
                    int   cmp;
                    if (mp_int_init(&twice_r) != MP_OK) {
                        mp_int_clear(&divisor);
                        mp_int_clear(&q_rounded);
                        mp_int_clear(&r_rounded);
                        goto oom;
                    }
                    mp_int_copy(&r_rounded, &twice_r);
                    mp_int_set_value(&pw, 2);
                    mp_int_mul(&twice_r, &pw, &twice_r);
                    cmp = mp_int_compare(&twice_r, &divisor);
                    half_up_carry = (cmp >= 0);
                    mp_int_clear(&twice_r);
                }
                if (half_up_carry) {
                    mp_int_set_value(&pw, 1);
                    mp_int_add(&q_rounded, &pw, &q_rounded);
                }
                /* Re-apply sign. */
                if (q_neg) {
                    mp_int_neg(&q_rounded, &q_rounded);
                }
                qz_heap = bigint_alloc_zeroed();
                if (qz_heap == NULL) {
                    mp_int_clear(&divisor);
                    mp_int_clear(&q_rounded);
                    mp_int_clear(&r_rounded);
                    goto oom;
                }
                mp_int_copy(&q_rounded, qz_heap);
                mp_int_clear(&divisor);
                mp_int_clear(&q_rounded);
                mp_int_clear(&r_rounded);
                q_wrapped = bigint_wrap(S, qz_heap);
                if (q_wrapped == NULL) goto oom;
                result = mino_bigdec_make(S, q_wrapped,
                                          sa - sb + extra - excess);
            } else {
                qz_heap = bigint_alloc_zeroed();
                if (qz_heap == NULL) goto oom;
                mp_int_copy(&q, qz_heap);
                q_wrapped = bigint_wrap(S, qz_heap);
                if (q_wrapped == NULL) goto oom;
                result = mino_bigdec_make(S, q_wrapped, sa - sb + extra);
            }
            break;
        }
        /* Increase precision: num *= 10 */
        mp_int_set_value(&pw, 10);
        if (mp_int_mul(&num, &pw, &num) != MP_OK) goto oom;
    }
    mp_int_clear(&num);
    mp_int_clear(&den);
    mp_int_clear(&q);
    mp_int_clear(&r);
    mp_int_clear(&pw);
    if (result == NULL) {
        return prim_throw_classified(S, "eval/type", "MTY001",
            "non-terminating decimal expansion in bigdec division");
    }
    return result;
oom:
    mp_int_clear(&num);
    mp_int_clear(&den);
    mp_int_clear(&q);
    mp_int_clear(&r);
    mp_int_clear(&pw);
oom_pre:
    return prim_throw_classified(S, "eval/out-of-memory", "MOM001",
                                 "out of memory in bigdec div");
}

mino_val *mino_bigdec_mod(mino_state *S, const mino_val *a,
                            const mino_val *b)
{
    mino_val *au, *bu, *m;
    int smax;
    if (a == NULL || b == NULL || mino_type_of(a) != MINO_BIGDEC || mino_type_of(b) != MINO_BIGDEC) {
        return prim_throw_classified(S, "eval/type", "MTY001",
                                     "bigdec mod: bigdec operands required");
    }
    if (!bigdec_align(S, a, b, &au, &bu, &smax)) return NULL;
    m = mino_bigint_mod(S, au, bu);
    if (m == NULL) return NULL;
    return mino_bigdec_make(S, m, smax);
}

/* Compare two bigdecs. Returns -1, 0, or 1. */
int mino_bigdec_cmp(const mino_val *a, const mino_val *b)
{
    int sa = a->as.bigdec.scale;
    int sb = b->as.bigdec.scale;
    if (sa == sb) {
        return mino_bigint_cmp(a->as.bigdec.unscaled, b->as.bigdec.unscaled);
    }
    /* Multiply the lower-scale unscaled by 10^(diff), then compare. */
    {
        mpz_t lhs, rhs, pw;
        int   r;
        if (mp_int_init(&lhs) != MP_OK) return 0;
        if (mp_int_init(&rhs) != MP_OK) { mp_int_clear(&lhs); return 0; }
        if (mp_int_init(&pw) != MP_OK) {
            mp_int_clear(&lhs); mp_int_clear(&rhs); return 0;
        }
        mp_int_copy((mp_int)a->as.bigdec.unscaled->as.bigint.mpz, &lhs);
        mp_int_copy((mp_int)b->as.bigdec.unscaled->as.bigint.mpz, &rhs);
        if (sa < sb) {
            mp_int_set_value(&pw, 10);
            mp_int_expt(&pw, sb - sa, &pw);
            mp_int_mul(&lhs, &pw, &lhs);
        } else {
            mp_int_set_value(&pw, 10);
            mp_int_expt(&pw, sa - sb, &pw);
            mp_int_mul(&rhs, &pw, &rhs);
        }
        r = mp_int_compare(&lhs, &rhs);
        mp_int_clear(&lhs); mp_int_clear(&rhs); mp_int_clear(&pw);
        return r;
    }
}
