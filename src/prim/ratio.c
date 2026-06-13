/*
 * ratio.c -- MINO_RATIO implementation: canonical-form fractions
 * with bigint numerator and denominator.
 *
 * Ratios always live in canonical form (gcd-reduced, positive
 * denominator); the constructors enforce that and downgrade to int
 * or bigint when the denominator collapses to one. Bigint helpers
 * come in via bignum_shared.h.
 *
 * Internal to the prim subsystem; embedders should only use mino.h.
 */

#include "prim/internal.h"
#include "prim/bignum_shared.h"
#include "imath.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------------------- */
/* MINO_RATIO and MINO_BIGDEC                                                */
/*                                                                           */
/* Both types are built on top of MINO_BIGINT cells; they hold pointers to   */
/* GC-allocated bigints and rely on the regular tracer to keep the children  */
/* alive. Ratios always live in canonical form (gcd-reduced, positive        */
/* denominator); the constructors enforce that and downgrade to int /        */
/* bigint when the denominator collapses to one.                             */
/* ------------------------------------------------------------------------- */

/* Coerce a value to a fresh GC-owned MINO_BIGINT. Accepts MINO_INT and
 * MINO_BIGINT; returns NULL with a thrown error on any other type. */
mino_val *to_bigint(mino_state *S, const mino_val *v)
{
    if (v == NULL) return NULL;
    if (mino_type_of(v) == MINO_BIGINT) {
        mp_int z = bigint_alloc_zeroed();
        if (z == NULL) {
            return prim_throw_classified(S, "eval/out-of-memory", "MOM001",
                                         "out of memory");
        }
        if (mp_int_copy((mp_int)v->as.bigint.mpz, z) != MP_OK) {
            mp_int_clear(z); free(z);
            return prim_throw_classified(S, "eval/out-of-memory", "MOM001",
                                         "out of memory");
        }
        return bigint_wrap(S, z);
    }
    if (mino_val_int_p(v)) {
        return mino_bigint_from_ll(S, mino_val_int_get(v));
    }
    return NULL;
}

/* Try to narrow a bigint cell back to a long long. Returns 1 on success
 * with *out set; 0 if the bigint doesn't fit. */
static int bigint_fits_ll(const mino_val *v, long long *out)
{
    return mino_as_ll(v, out);
}

mino_val *mino_ratio_make_unchecked(mino_state *S, mino_val *num,
                                      mino_val *denom)
{
    mino_val *v;
    if (num == NULL || denom == NULL ||
        mino_type_of(num) != MINO_BIGINT || mino_type_of(denom) != MINO_BIGINT) {
        return prim_throw_classified(S, "internal", "MIN001",
                                     "ratio components must be bigints");
    }
    v = alloc_val(S, MINO_RATIO);
    v->as.ratio.num   = num;
    v->as.ratio.denom = denom;
    return v;
}

/* Build a canonical ratio from two arbitrary numeric (int or bigint)
 * values. Returns int / bigint when the result is integer, otherwise a
 * MINO_RATIO. Throws on division-by-zero. */
mino_val *mino_ratio_make(mino_state *S, mino_val *num,
                            mino_val *denom)
{
    mino_val *bnum, *bdenom;
    mp_int      g;
    mpz_t       g_buf;
    bnum   = to_bigint(S, num);
    if (bnum == NULL) {
        return prim_throw_classified(S, "eval/type", "MTY001",
                                     "ratio numerator must be an integer");
    }
    gc_pin(bnum);
    bdenom = to_bigint(S, denom);
    gc_unpin(1);
    if (bdenom == NULL) {
        return prim_throw_classified(S, "eval/type", "MTY001",
                                     "ratio denominator must be an integer");
    }
    if (mp_int_compare_zero((mp_int)bdenom->as.bigint.mpz) == 0) {
        return prim_throw_classified(S, "eval/type", "MTY001",
                                     "ratio: division by zero");
    }
    /* Normalise sign: denom always positive. */
    if (mp_int_compare_zero((mp_int)bdenom->as.bigint.mpz) < 0) {
        mp_int_neg((mp_int)bnum->as.bigint.mpz,   (mp_int)bnum->as.bigint.mpz);
        mp_int_neg((mp_int)bdenom->as.bigint.mpz, (mp_int)bdenom->as.bigint.mpz);
    }
    /* Reduce by gcd. mp_int_gcd produces the unsigned gcd. */
    if (mp_int_init(&g_buf) != MP_OK) {
        return prim_throw_classified(S, "eval/out-of-memory", "MOM001",
                                     "out of memory");
    }
    g = &g_buf;
    if (mp_int_gcd((mp_int)bnum->as.bigint.mpz,
                   (mp_int)bdenom->as.bigint.mpz, g) != MP_OK) {
        mp_int_clear(g);
        return prim_throw_classified(S, "eval/out-of-memory", "MOM001",
                                     "out of memory in ratio reduce");
    }
    /* If gcd > 1, divide both num and denom by g. */
    if (mp_int_compare_value(g, 1) > 0) {
        mpz_t   r_buf;
        if (mp_int_init(&r_buf) != MP_OK) {
            mp_int_clear(g);
            return prim_throw_classified(S, "eval/out-of-memory", "MOM001",
                                         "out of memory");
        }
        mp_int_div((mp_int)bnum->as.bigint.mpz, g,
                   (mp_int)bnum->as.bigint.mpz, &r_buf);
        mp_int_div((mp_int)bdenom->as.bigint.mpz, g,
                   (mp_int)bdenom->as.bigint.mpz, &r_buf);
        mp_int_clear(&r_buf);
    }
    mp_int_clear(g);
    /* If denominator collapsed to 1, result is integer-valued. Narrow
     * to MINO_INT when it fits, else keep as MINO_BIGINT. */
    if (mp_int_compare_value((mp_int)bdenom->as.bigint.mpz, 1) == 0) {
        long long ll;
        if (bigint_fits_ll(bnum, &ll)) return mino_int(S, ll);
        return bnum;
    }
    return mino_ratio_make_unchecked(S, bnum, bdenom);
}

mino_val *mino_ratio_from_ll(mino_state *S, long long num, long long denom)
{
    mino_val *bn = mino_bigint_from_ll(S, num);
    mino_val *bd;
    if (bn == NULL) return NULL;
    gc_pin(bn);
    bd = mino_bigint_from_ll(S, denom);
    gc_unpin(1);
    if (bd == NULL) return NULL;
    return mino_ratio_make(S, bn, bd);
}

void mino_ratio_print(mino_state *S, const mino_val *v, FILE *out)
{
    if (v == NULL || mino_type_of(v) != MINO_RATIO) {
        fputs("0/1", out);
        return;
    }
    /* Print numerator as a plain integer (no N suffix), then "/", then
     * the denominator. The form `1/2` re-reads as the same ratio. */
    {
        char *n = mino_bigint_to_cstr(v->as.ratio.num);
        char *d = mino_bigint_to_cstr(v->as.ratio.denom);
        if (n == NULL || d == NULL) {
            free(n); free(d);
            fputs("0/1", out);
            return;
        }
        fputs(n, out);
        fputc('/', out);
        fputs(d, out);
        free(n); free(d);
    }
    (void)S;
}

int mino_ratio_equals(const mino_val *a, const mino_val *b)
{
    if (a == NULL || b == NULL) return 0;
    if (mino_type_of(a) != MINO_RATIO || mino_type_of(b) != MINO_RATIO) return 0;
    return mino_bigint_equals(a->as.ratio.num,   b->as.ratio.num)
        && mino_bigint_equals(a->as.ratio.denom, b->as.ratio.denom);
}

/* Compare two ratios. Sign of (a.num * b.denom - b.num * a.denom).
 * Both denominators are positive so cross-multiplication is safe. */
int mino_ratio_cmp(const mino_val *a, const mino_val *b)
{
    mpz_t lhs, rhs;
    int   r;
    if (a == NULL || b == NULL) return 0;
    if (mino_type_of(a) != MINO_RATIO || mino_type_of(b) != MINO_RATIO) return 0;
    if (mp_int_init(&lhs) != MP_OK) return 0;
    if (mp_int_init(&rhs) != MP_OK) { mp_int_clear(&lhs); return 0; }
    mp_int_mul((mp_int)a->as.ratio.num->as.bigint.mpz,
               (mp_int)b->as.ratio.denom->as.bigint.mpz, &lhs);
    mp_int_mul((mp_int)b->as.ratio.num->as.bigint.mpz,
               (mp_int)a->as.ratio.denom->as.bigint.mpz, &rhs);
    r = mp_int_compare(&lhs, &rhs);
    mp_int_clear(&lhs);
    mp_int_clear(&rhs);
    return r;
}

uint32_t mino_ratio_hash(const mino_val *v)
{
    uint32_t h;
    if (v == NULL || mino_type_of(v) != MINO_RATIO) return 0;
    h = mino_bigint_hash(v->as.ratio.num);
    h ^= mino_bigint_hash(v->as.ratio.denom) * 0x85ebca77u;
    /* Tag with a ratio-specific salt so a ratio doesn't collide with a
     * plain bigint of the same numerator. */
    return h ^ 0x5bd1e995u;
}

double mino_ratio_to_double(const mino_val *v)
{
    double n, d;
    if (v == NULL || mino_type_of(v) != MINO_RATIO) return 0.0;
    n = mino_bigint_to_double(v->as.ratio.num);
    d = mino_bigint_to_double(v->as.ratio.denom);
    if (d == 0.0) return 0.0;
    return n / d;
}

/* (numerator r) — accepts ratio (returns its numerator) or integer
 * (returns the integer; integers act as r/1). */
mino_val *prim_numerator(mino_state *S, mino_val *args, mino_env *env)
{
    mino_val *x;
    (void)env;
    if (!mino_is_cons(args) || mino_is_cons(args->as.cons.cdr)) {
        return prim_throw_classified(S, "eval/arity", "MAR001",
                                     "numerator requires one argument");
    }
    x = args->as.cons.car;
    if (x == NULL)
        return prim_throw_classified(S, "eval/type", "MTY001",
                                     "numerator: nil");
    if (mino_type_of(x) == MINO_RATIO) {
        long long ll;
        if (mino_as_ll(x->as.ratio.num, &ll)) return mino_int(S, ll);
        return x->as.ratio.num;
    }
    /* Per Clojure, numerator/denominator are defined only for Ratio.
     * Plain integers throw rather than silently returning the value. */
    return prim_throw_classified(S, "eval/type", "MTY001",
                                 "numerator: argument must be a ratio");
}

mino_val *prim_denominator(mino_state *S, mino_val *args, mino_env *env)
{
    mino_val *x;
    (void)env;
    if (!mino_is_cons(args) || mino_is_cons(args->as.cons.cdr)) {
        return prim_throw_classified(S, "eval/arity", "MAR001",
                                     "denominator requires one argument");
    }
    x = args->as.cons.car;
    if (x == NULL)
        return prim_throw_classified(S, "eval/type", "MTY001",
                                     "denominator: nil");
    if (mino_type_of(x) == MINO_RATIO) {
        long long ll;
        if (mino_as_ll(x->as.ratio.denom, &ll)) return mino_int(S, ll);
        return x->as.ratio.denom;
    }
    return prim_throw_classified(S, "eval/type", "MTY001",
                                 "denominator: argument must be a ratio");
}

/* (ratio? x) */
mino_val *prim_ratio_p(mino_state *S, mino_val *args, mino_env *env)
{
    (void)env;
    if (!mino_is_cons(args) || mino_is_cons(args->as.cons.cdr)) {
        return prim_throw_classified(S, "eval/arity", "MAR001",
                                     "ratio? requires one argument");
    }
    {
        mino_val *x = args->as.cons.car;
        return (x != NULL && mino_type_of(x) == MINO_RATIO)
            ? mino_true(S) : mino_false(S);
    }
}

/* (rational? x) — true for int / bigint / ratio / bigdec. Per Clojure
 * BigDecimal is rational since its value `unscaled * 10^-scale` is an
 * exact rational number; only floats (IEEE-754) sit outside. */
mino_val *prim_rational_p(mino_state *S, mino_val *args, mino_env *env)
{
    (void)env;
    if (!mino_is_cons(args) || mino_is_cons(args->as.cons.cdr)) {
        return prim_throw_classified(S, "eval/arity", "MAR001",
                                     "rational? requires one argument");
    }
    {
        mino_val *x = args->as.cons.car;
        if (x == NULL) return mino_false(S);
        return (mino_val_int_p(x)
             || mino_type_of(x) == MINO_BIGINT
             || mino_type_of(x) == MINO_RATIO
             || mino_type_of(x) == MINO_BIGDEC) ? mino_true(S) : mino_false(S);
    }
}

/* (rationalize x) — coerce a float to the nearest representable ratio.
 * For numeric kinds that already exact (int, bigint, ratio), returns
 * the value unchanged. For floats, decomposes the IEEE-754 value into
 * mantissa * 2^exp and reduces. */
mino_val *prim_rationalize(mino_state *S, mino_val *args, mino_env *env)
{
    mino_val *x;
    (void)env;
    if (!mino_is_cons(args) || mino_is_cons(args->as.cons.cdr)) {
        return prim_throw_classified(S, "eval/arity", "MAR001",
                                     "rationalize requires one argument");
    }
    x = args->as.cons.car;
    if (x == NULL)
        return prim_throw_classified(S, "eval/type", "MTY001",
                                     "rationalize: nil");
    if (mino_val_int_p(x) || mino_type_of(x) == MINO_BIGINT
        || mino_type_of(x) == MINO_RATIO) {
        return x;
    }
    if (mino_type_of(x) == MINO_BIGDEC) {
        /* unscaled / 10^scale, reduced. Negative scale means
         * unscaled * 10^|scale| / 1 (an integer multiple). */
        mino_val *unscaled = x->as.bigdec.unscaled;
        int         scale    = x->as.bigdec.scale;
        if (scale == 0) return unscaled;
        if (scale < 0) {
            mpz_t pw;
            mino_val *out;
            if (mp_int_init(&pw) != MP_OK) {
                return prim_throw_classified(S, "eval/out-of-memory",
                    "MOM001", "out of memory");
            }
            if (mp_int_set_value(&pw, 10) != MP_OK
                || mp_int_expt(&pw, -scale, &pw) != MP_OK) {
                mp_int_clear(&pw);
                return prim_throw_classified(S, "eval/out-of-memory",
                    "MOM001", "out of memory");
            }
            out = mino_bigint_from_ll(S, 0);
            if (out == NULL) { mp_int_clear(&pw); return NULL; }
            mp_int_mul((mp_int)unscaled->as.bigint.mpz, &pw,
                       (mp_int)out->as.bigint.mpz);
            mp_int_clear(&pw);
            return out;
        }
        {
            mino_val *bd;
            mpz_t pw;
            if (mp_int_init(&pw) != MP_OK) {
                return prim_throw_classified(S, "eval/out-of-memory",
                    "MOM001", "out of memory");
            }
            if (mp_int_set_value(&pw, 10) != MP_OK
                || mp_int_expt(&pw, scale, &pw) != MP_OK) {
                mp_int_clear(&pw);
                return prim_throw_classified(S, "eval/out-of-memory",
                    "MOM001", "out of memory");
            }
            bd = mino_bigint_from_ll(S, 1);
            if (bd == NULL) { mp_int_clear(&pw); return NULL; }
            mp_int_copy(&pw, (mp_int)bd->as.bigint.mpz);
            mp_int_clear(&pw);
            return mino_ratio_make(S, unscaled, bd);
        }
    }
    if (mino_type_of(x) == MINO_FLOAT) {
        /* JVM Clojure's (rationalize d) for a double goes through
         * BigDecimal.valueOf(d), i.e. the shortest decimal that
         * round-trips. So (rationalize 1.1) is 11/10 (matching the
         * literal "1.1") rather than the binary mantissa fraction
         * 2476979795053773/2251799813685248. We mirror that here:
         * shortest-decimal-double -> bigdec -> ratio. */
        double      d = x->as.f;
        char        buf[64];
        mino_val *bd;
        mino_val *unscaled;
        int         scale;
        if (d != d) /* NaN */
            return prim_throw_classified(S, "eval/type", "MTY001",
                                         "rationalize: NaN");
        if (d == 0.0) return mino_int(S, 0);
        mino_double_shortest(d, buf, sizeof(buf));
        bd = mino_bigdec_from_string(S, buf);
        if (bd == NULL) {
            return prim_throw_classified(S, "eval/type", "MTY001",
                                         "rationalize: failed to convert double");
        }
        unscaled = bd->as.bigdec.unscaled;
        scale    = bd->as.bigdec.scale;
        if (scale <= 0) return unscaled;
        {
            /* (numerator unscaled, denominator 10^scale) reduced. */
            mpz_t       pw;
            mino_val *denom;
            if (mp_int_init(&pw) != MP_OK) {
                return prim_throw_classified(S, "eval/out-of-memory",
                                             "MOM001", "out of memory");
            }
            if (mp_int_set_value(&pw, 10) != MP_OK
                || mp_int_expt(&pw, scale, &pw) != MP_OK) {
                mp_int_clear(&pw);
                return prim_throw_classified(S, "eval/out-of-memory",
                                             "MOM001", "out of memory");
            }
            denom = mino_bigint_from_ll(S, 1);
            if (denom == NULL) { mp_int_clear(&pw); return NULL; }
            mp_int_copy(&pw, (mp_int)denom->as.bigint.mpz);
            mp_int_clear(&pw);
            return mino_ratio_make(S, unscaled, denom);
        }
    }
    return prim_throw_classified(S, "eval/type", "MTY001",
                                 "rationalize: argument must be numeric");
}

/* Tower-arithmetic helpers for ratios. Each takes any numeric pair
 * (a, b) where at least one operand is a ratio (or both are) and
 * returns a canonicalised ratio / integer. */

static mino_val *as_ratio_pair(mino_state *S, const mino_val *v,
                                 mino_val **out_num, mino_val **out_denom)
{
    if (v == NULL) return NULL;
    if (mino_type_of(v) == MINO_RATIO) {
        *out_num   = v->as.ratio.num;
        *out_denom = v->as.ratio.denom;
        return (mino_val *)v;
    }
    if (mino_val_int_p(v) || mino_type_of(v) == MINO_BIGINT) {
        mino_val *bn = to_bigint(S, v);
        if (bn == NULL) return NULL;
        *out_num   = bn;
        *out_denom = mino_bigint_from_ll(S, 1);
        return bn;
    }
    return NULL;
}

mino_val *mino_ratio_add(mino_state *S, const mino_val *a,
                           const mino_val *b)
{
    mino_val *an, *ad, *bn, *bd;
    mino_val *cross1, *cross2, *new_num, *new_den;
    if (as_ratio_pair(S, a, &an, &ad) == NULL ||
        as_ratio_pair(S, b, &bn, &bd) == NULL) {
        return NULL;
    }
    /* a/b + c/d = (a*d + c*b) / (b*d) */
    cross1  = mino_bigint_mul(S, an, bd); if (cross1 == NULL) return NULL;
    gc_pin(cross1);
    cross2  = mino_bigint_mul(S, bn, ad);
    gc_unpin(1);
    if (cross2 == NULL) return NULL;
    new_num = mino_bigint_add(S, cross1, cross2); if (new_num == NULL) return NULL;
    new_den = mino_bigint_mul(S, ad, bd); if (new_den == NULL) return NULL;
    return mino_ratio_make(S, new_num, new_den);
}

mino_val *mino_ratio_sub(mino_state *S, const mino_val *a,
                           const mino_val *b)
{
    mino_val *an, *ad, *bn, *bd;
    mino_val *cross1, *cross2, *new_num, *new_den;
    if (as_ratio_pair(S, a, &an, &ad) == NULL ||
        as_ratio_pair(S, b, &bn, &bd) == NULL) {
        return NULL;
    }
    cross1  = mino_bigint_mul(S, an, bd); if (cross1 == NULL) return NULL;
    cross2  = mino_bigint_mul(S, bn, ad); if (cross2 == NULL) return NULL;
    new_num = mino_bigint_sub(S, cross1, cross2); if (new_num == NULL) return NULL;
    new_den = mino_bigint_mul(S, ad, bd); if (new_den == NULL) return NULL;
    return mino_ratio_make(S, new_num, new_den);
}

mino_val *mino_ratio_mul(mino_state *S, const mino_val *a,
                           const mino_val *b)
{
    mino_val *an, *ad, *bn, *bd;
    mino_val *new_num, *new_den;
    if (as_ratio_pair(S, a, &an, &ad) == NULL ||
        as_ratio_pair(S, b, &bn, &bd) == NULL) {
        return NULL;
    }
    new_num = mino_bigint_mul(S, an, bn); if (new_num == NULL) return NULL;
    new_den = mino_bigint_mul(S, ad, bd); if (new_den == NULL) return NULL;
    return mino_ratio_make(S, new_num, new_den);
}

mino_val *mino_ratio_div(mino_state *S, const mino_val *a,
                           const mino_val *b)
{
    mino_val *an, *ad, *bn, *bd;
    mino_val *new_num, *new_den;
    if (as_ratio_pair(S, a, &an, &ad) == NULL ||
        as_ratio_pair(S, b, &bn, &bd) == NULL) {
        return NULL;
    }
    if (mp_int_compare_zero((mp_int)bn->as.bigint.mpz) == 0) {
        return prim_throw_classified(S, "eval/type", "MTY001",
                                     "divide by zero");
    }
    /* a/b / c/d = (a*d) / (b*c) */
    new_num = mino_bigint_mul(S, an, bd); if (new_num == NULL) return NULL;
    new_den = mino_bigint_mul(S, ad, bn); if (new_den == NULL) return NULL;
    return mino_ratio_make(S, new_num, new_den);
}
