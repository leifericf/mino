/*
 * prim_numeric.c -- numeric, math, bitwise, coercion, and comparison primitives.
 */

#include "prim/internal.h"

#include <math.h>
#include <string.h>
#include <stdio.h>
#include <errno.h>
#include <limits.h>

/* Integer-overflow-safe arithmetic helpers. GCC >= 5 and Clang >= 3.9
 * expose __builtin_*_overflow; MSVC and older compilers fall back to
 * explicit range-based preconditions. Every helper returns 1 on
 * overflow (caller throws) and 0 on success.
 *
 * The signed C-integer rules we rely on:
 *   - a + b overflows iff (b > 0 && a > LLONG_MAX - b) ||
 *                         (b < 0 && a < LLONG_MIN - b)
 *   - a - b overflows iff (b > 0 && a < LLONG_MIN + b) ||
 *                         (b < 0 && a > LLONG_MAX + b)
 *   - a * b: split by sign; never perform the multiply (or divide
 *     LLONG_MIN by -1) before we have proved it safe. */
#if defined(__has_builtin)
  #if __has_builtin(__builtin_add_overflow)
    #define MINO_HAVE_BUILTIN_OVERFLOW 1
  #endif
#elif defined(__GNUC__) && (__GNUC__ >= 5)
  #define MINO_HAVE_BUILTIN_OVERFLOW 1
#endif

static int iadd_overflow(long long a, long long b, long long *out)
{
#ifdef MINO_HAVE_BUILTIN_OVERFLOW
    return __builtin_add_overflow(a, b, out) ? 1 : 0;
#else
    if ((b > 0 && a > LLONG_MAX - b) ||
        (b < 0 && a < LLONG_MIN - b)) {
        return 1;
    }
    *out = a + b;
    return 0;
#endif
}

static int isub_overflow(long long a, long long b, long long *out)
{
#ifdef MINO_HAVE_BUILTIN_OVERFLOW
    return __builtin_sub_overflow(a, b, out) ? 1 : 0;
#else
    if ((b > 0 && a < LLONG_MIN + b) ||
        (b < 0 && a > LLONG_MAX + b)) {
        return 1;
    }
    *out = a - b;
    return 0;
#endif
}

static int imul_overflow(long long a, long long b, long long *out)
{
#ifdef MINO_HAVE_BUILTIN_OVERFLOW
    return __builtin_mul_overflow(a, b, out) ? 1 : 0;
#else
    if (a == 0 || b == 0) { *out = 0; return 0; }
    if (a > 0) {
        if (b > 0) {
            if (a > LLONG_MAX / b) return 1;
        } else {
            if (b < LLONG_MIN / a) return 1;
        }
    } else {
        /* a < 0 */
        if (b > 0) {
            if (a < LLONG_MIN / b) return 1;
        } else {
            /* Both negative: LLONG_MIN / -1 is itself UB, so catch
             * LLONG_MIN directly before dividing. */
            if (a == LLONG_MIN || b == LLONG_MIN) return 1;
            if (a < LLONG_MAX / b) return 1;
        }
    }
    *out = a * b;
    return 0;
#endif
}

static int ineg_overflow(long long a, long long *out)
{
    /* Only LLONG_MIN lacks a representable negation. */
    if (a == LLONG_MIN) return 1;
    *out = -a;
    return 0;
}

/* ------------------------------------------------------------------------- */
/* Arithmetic                                                                */
/*                                                                           */
/* `+`, `-`, `*`, `/` perform tier-dispatched arithmetic across the five     */
/* numeric tiers — int, bigint, ratio, bigdec, float. The accumulator        */
/* tracks the current tier and promotes one-way as higher-tier operands       */
/* arrive. When ratio meets bigdec in the same expression, both collapse to  */
/* float since the exact ratio→bigdec conversion needs an explicit precision */
/* (Clojure exposes that via with-precision; we punt for now and document    */
/* the behaviour).                                                           */
/* ------------------------------------------------------------------------- */

typedef enum { OP_ADD, OP_SUB, OP_MUL, OP_DIV } tower_op_t;

static double tower_to_double(const mino_val_t *v)
{
    if (v == NULL) return 0.0;
    switch (v->type) {
    case MINO_INT:    return (double)v->as.i;
    case MINO_FLOAT:  return v->as.f;
    case MINO_BIGINT: return mino_bigint_to_double(v);
    case MINO_RATIO:  return mino_ratio_to_double(v);
    case MINO_BIGDEC: return mino_bigdec_to_double(v);
    default:          return 0.0;
    }
}

/* The accumulator state for a tower walk. Exactly one of int_set,
 * bigint_acc, ratio_acc, bigdec_acc, float_set is "active" depending
 * on tier. */
typedef struct {
    enum { TT_INT, TT_BIGINT, TT_RATIO, TT_BIGDEC, TT_FLOAT } tier;
    long long   iacc;
    mino_val_t *vacc;  /* bigint, ratio, or bigdec accumulator */
    double      dacc;
} tower_acc_t;

static int classify_or_throw(mino_state_t *S, const mino_val_t *v,
                             const char *opname, int *out_tier)
{
    if (v == NULL) goto err;
    switch (v->type) {
    case MINO_INT:    *out_tier = TT_INT;    return 1;
    case MINO_BIGINT: *out_tier = TT_BIGINT; return 1;
    case MINO_RATIO:  *out_tier = TT_RATIO;  return 1;
    case MINO_BIGDEC: *out_tier = TT_BIGDEC; return 1;
    case MINO_FLOAT:  *out_tier = TT_FLOAT;  return 1;
    default: break;
    }
err: {
    char buf[64];
    snprintf(buf, sizeof(buf), "%s expects numbers", opname);
    prim_throw_classified(S, "eval/type", "MTY001", buf);
    return 0;
}
}

/* Apply a tower op when both sides are accumulator-tier-coerced.
 * Returns NULL on error (already raised). */
static mino_val_t *tower_op_at_tier(mino_state_t *S, tower_op_t op,
                                    int tier, mino_val_t *a, mino_val_t *b,
                                    const char *opname)
{
    (void)opname;
    switch (tier) {
    case TT_BIGINT:
        switch (op) {
        case OP_ADD: return mino_bigint_add(S, a, b);
        case OP_SUB: return mino_bigint_sub(S, a, b);
        case OP_MUL: return mino_bigint_mul(S, a, b);
        case OP_DIV: return mino_ratio_div(S, a, b); /* may yield ratio */
        }
        break;
    case TT_RATIO:
        switch (op) {
        case OP_ADD: return mino_ratio_add(S, a, b);
        case OP_SUB: return mino_ratio_sub(S, a, b);
        case OP_MUL: return mino_ratio_mul(S, a, b);
        case OP_DIV: return mino_ratio_div(S, a, b);
        }
        break;
    case TT_BIGDEC:
        switch (op) {
        case OP_ADD: return mino_bigdec_add(S, a, b);
        case OP_SUB: return mino_bigdec_sub(S, a, b);
        case OP_MUL: return mino_bigdec_mul(S, a, b);
        case OP_DIV: return prim_throw_classified(S, "eval/type", "MTY001",
                                "bigdec division requires explicit precision (with-precision unimplemented)");
        }
        break;
    }
    return prim_throw_classified(S, "internal", "MIN001", "tower_op_at_tier");
}

/* Promote the accumulator to a higher tier in-place. */
static int promote_acc(mino_state_t *S, tower_acc_t *acc, int new_tier,
                       const char *opname)
{
    while ((int)acc->tier < new_tier) {
        switch (acc->tier) {
        case TT_INT:
            if (new_tier == TT_BIGINT) {
                acc->vacc = mino_bigint_from_ll(S, acc->iacc);
                if (acc->vacc == NULL) return 0;
                acc->tier = TT_BIGINT;
                continue;
            }
            if (new_tier == TT_RATIO) {
                mino_val_t *bn = mino_bigint_from_ll(S, acc->iacc);
                mino_val_t *bd;
                if (bn == NULL) return 0;
                bd = mino_bigint_from_ll(S, 1);
                if (bd == NULL) return 0;
                acc->vacc = mino_ratio_make_unchecked(S, bn, bd);
                if (acc->vacc == NULL) return 0;
                acc->tier = TT_RATIO;
                continue;
            }
            if (new_tier == TT_BIGDEC) {
                mino_val_t *u = mino_bigint_from_ll(S, acc->iacc);
                if (u == NULL) return 0;
                acc->vacc = mino_bigdec_make(S, u, 0);
                if (acc->vacc == NULL) return 0;
                acc->tier = TT_BIGDEC;
                continue;
            }
            if (new_tier == TT_FLOAT) {
                acc->dacc = (double)acc->iacc;
                acc->tier = TT_FLOAT;
                continue;
            }
            break;
        case TT_BIGINT:
            if (new_tier == TT_RATIO) {
                mino_val_t *bd = mino_bigint_from_ll(S, 1);
                if (bd == NULL) return 0;
                acc->vacc = mino_ratio_make_unchecked(S, acc->vacc, bd);
                if (acc->vacc == NULL) return 0;
                acc->tier = TT_RATIO;
                continue;
            }
            if (new_tier == TT_BIGDEC) {
                acc->vacc = mino_bigdec_make(S, acc->vacc, 0);
                if (acc->vacc == NULL) return 0;
                acc->tier = TT_BIGDEC;
                continue;
            }
            if (new_tier == TT_FLOAT) {
                acc->dacc = mino_bigint_to_double(acc->vacc);
                acc->vacc = NULL;
                acc->tier = TT_FLOAT;
                continue;
            }
            break;
        case TT_RATIO:
            if (new_tier == TT_BIGDEC || new_tier == TT_FLOAT) {
                /* Ratio meets bigdec: collapse to float (documented). */
                acc->dacc = mino_ratio_to_double(acc->vacc);
                acc->vacc = NULL;
                acc->tier = TT_FLOAT;
                continue;
            }
            break;
        case TT_BIGDEC:
            if (new_tier == TT_FLOAT) {
                acc->dacc = mino_bigdec_to_double(acc->vacc);
                acc->vacc = NULL;
                acc->tier = TT_FLOAT;
                continue;
            }
            break;
        case TT_FLOAT: return 1;
        }
        break;
    }
    (void)opname;
    return 1;
}

static mino_val_t *coerce_at_tier(mino_state_t *S, mino_val_t *v, int tier,
                                  const char *opname)
{
    /* Convert v to a value at `tier` (or the appropriate sub-tier). */
    int vt;
    if (!classify_or_throw(S, v, opname, &vt)) return NULL;
    switch (tier) {
    case TT_BIGINT:
        if (v->type == MINO_INT)    return mino_bigint_from_ll(S, v->as.i);
        if (v->type == MINO_BIGINT) return v;
        break;
    case TT_RATIO: {
        mino_val_t *bn, *bd;
        if (v->type == MINO_RATIO)  return v;
        if (v->type == MINO_INT) {
            bn = mino_bigint_from_ll(S, v->as.i);
            if (bn == NULL) return NULL;
            bd = mino_bigint_from_ll(S, 1);
            if (bd == NULL) return NULL;
            return mino_ratio_make_unchecked(S, bn, bd);
        }
        if (v->type == MINO_BIGINT) {
            bd = mino_bigint_from_ll(S, 1);
            if (bd == NULL) return NULL;
            return mino_ratio_make_unchecked(S, (mino_val_t *)v, bd);
        }
        break;
    }
    case TT_BIGDEC: {
        if (v->type == MINO_BIGDEC) return v;
        if (v->type == MINO_INT) {
            mino_val_t *u = mino_bigint_from_ll(S, v->as.i);
            if (u == NULL) return NULL;
            return mino_bigdec_make(S, u, 0);
        }
        if (v->type == MINO_BIGINT) {
            return mino_bigdec_make(S, (mino_val_t *)v, 0);
        }
        break;
    }
    }
    /* Should be unreachable in well-classified args. */
    return prim_throw_classified(S, "internal", "MIN001",
                                 "coerce_at_tier: unsupported tier transition");
}

/* Driver. Walks args, classifies each, promotes accumulator on tier
 * increase, and applies the per-tier op. Returns the final value. */
static mino_val_t *tower_reduce(mino_state_t *S, mino_val_t *args,
                                tower_op_t op, int promote_long_overflow,
                                const char *opname)
{
    tower_acc_t acc;
    int seeded = 0;
    int (*overflow_op)(long long, long long, long long *) =
        (op == OP_ADD) ? iadd_overflow
        : (op == OP_SUB) ? isub_overflow
        : (op == OP_MUL) ? imul_overflow : NULL;
    long long identity = (op == OP_MUL) ? 1 : 0;
    acc.tier = TT_INT;
    acc.iacc = identity;
    acc.vacc = NULL;
    acc.dacc = 0.0;
    while (mino_is_cons(args)) {
        mino_val_t *a = args->as.cons.car;
        int at;
        if (!classify_or_throw(S, a, opname, &at)) return NULL;
        if (op == OP_DIV && !seeded) {
            /* Seed with first operand directly, then continue with division.
             * For div we always need the operand-1 path. */
            seeded = 1;
            switch (at) {
            case TT_INT:    acc.iacc = a->as.i; break;
            case TT_FLOAT:  acc.dacc = a->as.f; acc.tier = TT_FLOAT; break;
            case TT_BIGINT: acc.vacc = (mino_val_t *)a; acc.tier = TT_BIGINT; break;
            case TT_RATIO:  acc.vacc = (mino_val_t *)a; acc.tier = TT_RATIO; break;
            case TT_BIGDEC: acc.vacc = (mino_val_t *)a; acc.tier = TT_BIGDEC; break;
            }
            args = args->as.cons.cdr;
            continue;
        }
        /* Promote the running accumulator if the new operand is at a
         * higher tier. Ratio meeting bigdec triggers a float collapse. */
        if (at > (int)acc.tier ||
            (acc.tier == TT_RATIO && at == TT_BIGDEC) ||
            (acc.tier == TT_BIGDEC && at == TT_RATIO)) {
            int target = at;
            if ((acc.tier == TT_RATIO && at == TT_BIGDEC) ||
                (acc.tier == TT_BIGDEC && at == TT_RATIO)) {
                target = TT_FLOAT;
            }
            if (!promote_acc(S, &acc, target, opname)) return NULL;
        }
        /* Coerce the operand to acc's tier and apply. */
        switch (acc.tier) {
        case TT_INT: {
            long long x = a->as.i;
            long long out;
            if (op == OP_ADD || op == OP_SUB || op == OP_MUL) {
                if (overflow_op(acc.iacc, x, &out)) {
                    if (promote_long_overflow) {
                        mino_val_t *la = mino_bigint_from_ll(S, acc.iacc);
                        mino_val_t *lb = mino_bigint_from_ll(S, x);
                        if (la == NULL || lb == NULL) return NULL;
                        acc.vacc = tower_op_at_tier(S, op, TT_BIGINT, la, lb, opname);
                        if (acc.vacc == NULL) return NULL;
                        acc.tier = TT_BIGINT;
                        break;
                    }
                    return prim_throw_classified(S, "eval/overflow", "MOV001",
                        op == OP_ADD ? "integer overflow in +"
                        : op == OP_SUB ? "integer overflow in -"
                        : "integer overflow in *");
                }
                acc.iacc = out;
            } else { /* OP_DIV */
                if (x == 0) return prim_throw_classified(S, "eval/type", "MTY001",
                                                          "division by zero");
                /* For exact int/int division, prefer ratio when not exact
                 * (Clojure's `/` returns a Ratio for non-exact int/int). */
                if (acc.iacc % x == 0) {
                    acc.iacc /= x;
                } else {
                    /* Promote to ratio. */
                    mino_val_t *bn = mino_bigint_from_ll(S, acc.iacc);
                    mino_val_t *bd = mino_bigint_from_ll(S, x);
                    if (bn == NULL || bd == NULL) return NULL;
                    acc.vacc = mino_ratio_make(S, bn, bd);
                    if (acc.vacc == NULL) return NULL;
                    acc.tier = TT_RATIO;
                }
            }
            break;
        }
        case TT_BIGINT: {
            mino_val_t *operand = coerce_at_tier(S, a, TT_BIGINT, opname);
            if (operand == NULL) return NULL;
            if (op == OP_DIV) {
                /* int/bigint division: promote to ratio. */
                acc.vacc = mino_ratio_div(S, acc.vacc, operand);
                if (acc.vacc == NULL) return NULL;
                /* mino_ratio_div may have collapsed to int / bigint. */
                if (acc.vacc->type == MINO_INT) {
                    acc.iacc = acc.vacc->as.i;
                    acc.vacc = NULL;
                    acc.tier = TT_INT;
                } else if (acc.vacc->type == MINO_BIGINT) {
                    acc.tier = TT_BIGINT;
                } else {
                    acc.tier = TT_RATIO;
                }
            } else {
                acc.vacc = tower_op_at_tier(S, op, TT_BIGINT, acc.vacc, operand, opname);
                if (acc.vacc == NULL) return NULL;
            }
            break;
        }
        case TT_RATIO: {
            mino_val_t *operand = coerce_at_tier(S, a, TT_RATIO, opname);
            if (operand == NULL) return NULL;
            acc.vacc = tower_op_at_tier(S, op, TT_RATIO, acc.vacc, operand, opname);
            if (acc.vacc == NULL) return NULL;
            /* Result may have collapsed back to int/bigint. */
            if (acc.vacc->type == MINO_INT) {
                acc.iacc = acc.vacc->as.i; acc.vacc = NULL; acc.tier = TT_INT;
            } else if (acc.vacc->type == MINO_BIGINT) {
                acc.tier = TT_BIGINT;
            }
            break;
        }
        case TT_BIGDEC: {
            mino_val_t *operand = coerce_at_tier(S, a, TT_BIGDEC, opname);
            if (operand == NULL) return NULL;
            acc.vacc = tower_op_at_tier(S, op, TT_BIGDEC, acc.vacc, operand, opname);
            if (acc.vacc == NULL) return NULL;
            break;
        }
        case TT_FLOAT: {
            double x = tower_to_double(a);
            switch (op) {
            case OP_ADD: acc.dacc += x; break;
            case OP_SUB: acc.dacc -= x; break;
            case OP_MUL: acc.dacc *= x; break;
            case OP_DIV: acc.dacc /= x; break;
            }
            break;
        }
        }
        seeded = 1;
        args = args->as.cons.cdr;
    }
    /* Pack the result. */
    switch (acc.tier) {
    case TT_INT:    return mino_int(S, acc.iacc);
    case TT_FLOAT:  return mino_float(S, acc.dacc);
    default:        return acc.vacc;
    }
}

mino_val_t *prim_add(mino_state_t *S, mino_val_t *args, mino_env_t *env)
{
    (void)env;
    return tower_reduce(S, args, OP_ADD, 0, "+");
}

/* (inc x) -- x + 1. Fast path for the dominant integer case; falls back
 * to the generic + primitive for non-ints so semantics stay identical. */
mino_val_t *prim_inc(mino_state_t *S, mino_val_t *args, mino_env_t *env)
{
    mino_val_t *x;
    (void)env;
    if (!mino_is_cons(args) || mino_is_cons(args->as.cons.cdr)) {
        return prim_throw_classified(S, "eval/arity", "MAR001",
            "inc requires exactly 1 argument");
    }
    x = args->as.cons.car;
    if (x != NULL && x->type == MINO_INT) {
        if (x->as.i == LLONG_MAX) {
            return prim_throw_classified(S, "eval/overflow", "MOV001",
                "integer overflow in inc");
        }
        return mino_int(S, x->as.i + 1);
    }
    if (x != NULL && x->type == MINO_FLOAT) {
        return mino_float(S, x->as.f + 1.0);
    }
    if (x != NULL && (x->type == MINO_BIGINT || x->type == MINO_RATIO
                      || x->type == MINO_BIGDEC)) {
        /* Defer to the tower-aware (+) by passing (x 1). */
        mino_val_t *one = mino_int(S, 1);
        mino_val_t *pair = mino_cons(S, x, mino_cons(S, one, mino_nil(S)));
        return prim_add(S, pair, env);
    }
    return prim_throw_classified(S, "eval/type", "MTY001",
        "inc expects a number");
}

mino_val_t *prim_dec(mino_state_t *S, mino_val_t *args, mino_env_t *env)
{
    mino_val_t *x;
    (void)env;
    if (!mino_is_cons(args) || mino_is_cons(args->as.cons.cdr)) {
        return prim_throw_classified(S, "eval/arity", "MAR001",
            "dec requires exactly 1 argument");
    }
    x = args->as.cons.car;
    if (x != NULL && x->type == MINO_INT) {
        if (x->as.i == LLONG_MIN) {
            return prim_throw_classified(S, "eval/overflow", "MOV001",
                "integer overflow in dec");
        }
        return mino_int(S, x->as.i - 1);
    }
    if (x != NULL && x->type == MINO_FLOAT) {
        return mino_float(S, x->as.f - 1.0);
    }
    if (x != NULL && (x->type == MINO_BIGINT || x->type == MINO_RATIO
                      || x->type == MINO_BIGDEC)) {
        mino_val_t *one = mino_int(S, 1);
        mino_val_t *pair = mino_cons(S, x, mino_cons(S, one, mino_nil(S)));
        return prim_sub(S, pair, env);
    }
    return prim_throw_classified(S, "eval/type", "MTY001",
        "dec expects a number");
}

/* Apply the n-ary subtract/divide step starting from a seeded acc and
 * walking the rest. Reused by prim_sub / prim_div / prim_subq.
 * promote_long_overflow=1 promotes int-overflow to bigint instead of
 * throwing (used by `-'`); 0 throws (used by `-`). */
static mino_val_t *tower_reduce_seeded(mino_state_t *S, mino_val_t *seed,
                                       mino_val_t *rest, tower_op_t op,
                                       int promote_long_overflow,
                                       const char *opname)
{
    tower_acc_t a;
    int seed_tier;
    if (!classify_or_throw(S, seed, opname, &seed_tier)) return NULL;
    a.iacc = 0; a.dacc = 0; a.vacc = NULL;
    switch (seed_tier) {
    case TT_INT:    a.iacc = seed->as.i; a.tier = TT_INT;    break;
    case TT_FLOAT:  a.dacc = seed->as.f; a.tier = TT_FLOAT;  break;
    case TT_BIGINT: a.vacc = seed;       a.tier = TT_BIGINT; break;
    case TT_RATIO:  a.vacc = seed;       a.tier = TT_RATIO;  break;
    case TT_BIGDEC: a.vacc = seed;       a.tier = TT_BIGDEC; break;
    default:        a.tier = TT_INT;     break;
    }
    while (mino_is_cons(rest)) {
        mino_val_t *x = rest->as.cons.car;
        int xt;
        if (!classify_or_throw(S, x, opname, &xt)) return NULL;
        if (xt > (int)a.tier ||
            (a.tier == TT_RATIO  && xt == TT_BIGDEC) ||
            (a.tier == TT_BIGDEC && xt == TT_RATIO)) {
            int target = xt;
            if ((a.tier == TT_RATIO  && xt == TT_BIGDEC) ||
                (a.tier == TT_BIGDEC && xt == TT_RATIO)) target = TT_FLOAT;
            if (!promote_acc(S, &a, target, opname)) return NULL;
        }
        switch (a.tier) {
        case TT_INT: {
            long long out;
            if (op == OP_SUB) {
                if (isub_overflow(a.iacc, x->as.i, &out)) {
                    if (promote_long_overflow) {
                        mino_val_t *la = mino_bigint_from_ll(S, a.iacc);
                        mino_val_t *lb = mino_bigint_from_ll(S, x->as.i);
                        if (la == NULL || lb == NULL) return NULL;
                        a.vacc = mino_bigint_sub(S, la, lb);
                        if (a.vacc == NULL) return NULL;
                        a.tier = TT_BIGINT;
                        break;
                    }
                    return prim_throw_classified(S, "eval/overflow", "MOV001",
                                                 "integer overflow in -");
                }
                a.iacc = out;
            } else { /* OP_DIV */
                if (x->as.i == 0)
                    return prim_throw_classified(S, "eval/type", "MTY001",
                                                 "division by zero");
                if (a.iacc % x->as.i == 0) {
                    a.iacc /= x->as.i;
                } else {
                    mino_val_t *bn = mino_bigint_from_ll(S, a.iacc);
                    mino_val_t *bd = mino_bigint_from_ll(S, x->as.i);
                    if (bn == NULL || bd == NULL) return NULL;
                    a.vacc = mino_ratio_make(S, bn, bd);
                    if (a.vacc == NULL) return NULL;
                    if (a.vacc->type == MINO_INT) {
                        a.iacc = a.vacc->as.i; a.vacc = NULL; a.tier = TT_INT;
                    } else if (a.vacc->type == MINO_BIGINT) {
                        a.tier = TT_BIGINT;
                    } else {
                        a.tier = TT_RATIO;
                    }
                }
            }
            break;
        }
        case TT_BIGINT: {
            mino_val_t *opd = coerce_at_tier(S, x, TT_BIGINT, opname);
            if (opd == NULL) return NULL;
            if (op == OP_SUB) {
                a.vacc = mino_bigint_sub(S, a.vacc, opd);
            } else {
                a.vacc = mino_ratio_div(S, a.vacc, opd);
                if (a.vacc != NULL) {
                    if (a.vacc->type == MINO_INT) {
                        a.iacc = a.vacc->as.i; a.vacc = NULL; a.tier = TT_INT;
                    } else if (a.vacc->type == MINO_BIGINT) {
                        a.tier = TT_BIGINT;
                    } else {
                        a.tier = TT_RATIO;
                    }
                }
            }
            if (a.vacc == NULL && a.tier != TT_INT && a.tier != TT_BIGINT) return NULL;
            break;
        }
        case TT_RATIO: {
            mino_val_t *opd = coerce_at_tier(S, x, TT_RATIO, opname);
            if (opd == NULL) return NULL;
            a.vacc = (op == OP_SUB)
                ? mino_ratio_sub(S, a.vacc, opd)
                : mino_ratio_div(S, a.vacc, opd);
            if (a.vacc == NULL) return NULL;
            if (a.vacc->type == MINO_INT) {
                a.iacc = a.vacc->as.i; a.vacc = NULL; a.tier = TT_INT;
            } else if (a.vacc->type == MINO_BIGINT) {
                a.tier = TT_BIGINT;
            }
            break;
        }
        case TT_BIGDEC: {
            mino_val_t *opd = coerce_at_tier(S, x, TT_BIGDEC, opname);
            if (opd == NULL) return NULL;
            if (op == OP_SUB) {
                a.vacc = mino_bigdec_sub(S, a.vacc, opd);
            } else {
                return prim_throw_classified(S, "eval/type", "MTY001",
                    "bigdec division requires explicit precision (with-precision unimplemented)");
            }
            if (a.vacc == NULL) return NULL;
            break;
        }
        case TT_FLOAT: {
            double dx = tower_to_double(x);
            if (op == OP_SUB) a.dacc -= dx;
            else {
                /* IEEE division: x/0 yields Inf or -Inf for float; only
                 * the all-integer case throws division-by-zero (handled
                 * in the TT_INT branch above). Float div mirrors the
                 * historical mino behaviour. */
                a.dacc /= dx;
            }
            break;
        }
        }
        rest = rest->as.cons.cdr;
    }
    switch (a.tier) {
    case TT_INT:    return mino_int(S, a.iacc);
    case TT_FLOAT:  return mino_float(S, a.dacc);
    default:        return a.vacc;
    }
}

mino_val_t *prim_sub(mino_state_t *S, mino_val_t *args, mino_env_t *env)
{
    mino_val_t *first;
    (void)env;
    if (!mino_is_cons(args))
        return prim_throw_classified(S, "eval/arity", "MAR001",
                                     "- requires at least one argument");
    first = args->as.cons.car;
    /* Unary: negate. */
    if (!mino_is_cons(args->as.cons.cdr)) {
        if (first == NULL)
            return prim_throw_classified(S, "eval/type", "MTY001",
                                         "- expects numbers");
        if (first->type == MINO_INT) {
            long long neg;
            if (ineg_overflow(first->as.i, &neg))
                return prim_throw_classified(S, "eval/overflow", "MOV001",
                                             "integer overflow in -");
            return mino_int(S, neg);
        }
        if (first->type == MINO_FLOAT)  return mino_float(S, -first->as.f);
        if (first->type == MINO_BIGINT) return mino_bigint_neg(S, first);
        if (first->type == MINO_RATIO) {
            mino_val_t *zero_n = mino_bigint_from_ll(S, 0);
            mino_val_t *zero_d = mino_bigint_from_ll(S, 1);
            mino_val_t *zero;
            if (zero_n == NULL || zero_d == NULL) return NULL;
            zero = mino_ratio_make_unchecked(S, zero_n, zero_d);
            if (zero == NULL) return NULL;
            return mino_ratio_sub(S, zero, first);
        }
        if (first->type == MINO_BIGDEC) return mino_bigdec_neg(S, first);
        return prim_throw_classified(S, "eval/type", "MTY001",
                                     "- expects numbers");
    }
    return tower_reduce_seeded(S, first, args->as.cons.cdr, OP_SUB, 0, "-");
}

mino_val_t *prim_mul(mino_state_t *S, mino_val_t *args, mino_env_t *env)
{
    (void)env;
    return tower_reduce(S, args, OP_MUL, 0, "*");
}

/* ------------------------------------------------------------------------- */
/* Auto-promoting arithmetic (+' -' *' inc' dec')                            */
/*                                                                           */
/* Same shape as +, -, *, inc, dec, but a long-overflow promotes the         */
/* running accumulator to bigint instead of throwing. The implementations    */
/* delegate to the shared tower-dispatch core (tower_reduce /                */
/* tower_reduce_seeded) with the promote_long_overflow flag set, so          */
/* ratio / bigdec / float operands work identically to plain +, -, *, /.    */
/* ------------------------------------------------------------------------- */

/* (+' & args) — tower-aware add with long overflow promoting to bigint
 * instead of throwing. */
mino_val_t *prim_addq(mino_state_t *S, mino_val_t *args, mino_env_t *env)
{
    (void)env;
    return tower_reduce(S, args, OP_ADD, 1, "+'");
}

/* (-' x) -> negation; (-' x y ...) -> successive subtraction with
 * long overflow promoting to bigint. */
mino_val_t *prim_subq(mino_state_t *S, mino_val_t *args, mino_env_t *env)
{
    mino_val_t *first;
    (void)env;
    if (!mino_is_cons(args))
        return prim_throw_classified(S, "eval/arity", "MAR001",
                                     "-' requires at least one argument");
    first = args->as.cons.car;
    /* Unary: negate (overflow at LLONG_MIN promotes to bigint). */
    if (!mino_is_cons(args->as.cons.cdr)) {
        if (first == NULL)
            return prim_throw_classified(S, "eval/type", "MTY001",
                                         "-' expects numbers");
        if (first->type == MINO_INT) {
            long long neg;
            if (ineg_overflow(first->as.i, &neg)) {
                mino_val_t *bi = mino_bigint_from_ll(S, first->as.i);
                if (bi == NULL) return NULL;
                return mino_bigint_neg(S, bi);
            }
            return mino_int(S, neg);
        }
        if (first->type == MINO_FLOAT)  return mino_float(S, -first->as.f);
        if (first->type == MINO_BIGINT) return mino_bigint_neg(S, first);
        if (first->type == MINO_RATIO) {
            mino_val_t *zero_n = mino_bigint_from_ll(S, 0);
            mino_val_t *zero_d = mino_bigint_from_ll(S, 1);
            mino_val_t *zero;
            if (zero_n == NULL || zero_d == NULL) return NULL;
            zero = mino_ratio_make_unchecked(S, zero_n, zero_d);
            if (zero == NULL) return NULL;
            return mino_ratio_sub(S, zero, first);
        }
        if (first->type == MINO_BIGDEC) return mino_bigdec_neg(S, first);
        return prim_throw_classified(S, "eval/type", "MTY001",
                                     "-' expects numbers");
    }
    return tower_reduce_seeded(S, first, args->as.cons.cdr, OP_SUB, 1, "-'");
}

/* (*' & args) — tower-aware multiply with long-overflow promotion. */
mino_val_t *prim_mulq(mino_state_t *S, mino_val_t *args, mino_env_t *env)
{
    (void)env;
    return tower_reduce(S, args, OP_MUL, 1, "*'");
}

/* (inc' x) — overflow at LLONG_MAX promotes to bigint. */
mino_val_t *prim_incq(mino_state_t *S, mino_val_t *args, mino_env_t *env)
{
    mino_val_t *x;
    (void)env;
    if (!mino_is_cons(args) || mino_is_cons(args->as.cons.cdr)) {
        return prim_throw_classified(S, "eval/arity", "MAR001",
                                     "inc' requires exactly 1 argument");
    }
    x = args->as.cons.car;
    if (x == NULL)
        return prim_throw_classified(S, "eval/type", "MTY001",
                                     "inc' expects a number");
    if (x->type == MINO_INT) {
        if (x->as.i == LLONG_MAX) {
            mino_val_t *lhs = mino_bigint_from_ll(S, x->as.i);
            mino_val_t *one;
            if (lhs == NULL) return NULL;
            one = mino_int(S, 1);
            return mino_bigint_add(S, lhs, one);
        }
        return mino_int(S, x->as.i + 1);
    }
    if (x->type == MINO_FLOAT) return mino_float(S, x->as.f + 1.0);
    /* For bigint / ratio / bigdec, route through (+' x 1) which handles
     * tier promotion correctly. */
    {
        mino_val_t *one = mino_int(S, 1);
        mino_val_t *pair = mino_cons(S, x, mino_cons(S, one, mino_nil(S)));
        return prim_addq(S, pair, env);
    }
}

/* (dec' x) — overflow at LLONG_MIN promotes to bigint. */
mino_val_t *prim_decq(mino_state_t *S, mino_val_t *args, mino_env_t *env)
{
    mino_val_t *x;
    (void)env;
    if (!mino_is_cons(args) || mino_is_cons(args->as.cons.cdr)) {
        return prim_throw_classified(S, "eval/arity", "MAR001",
                                     "dec' requires exactly 1 argument");
    }
    x = args->as.cons.car;
    if (x == NULL)
        return prim_throw_classified(S, "eval/type", "MTY001",
                                     "dec' expects a number");
    if (x->type == MINO_INT) {
        if (x->as.i == LLONG_MIN) {
            mino_val_t *lhs = mino_bigint_from_ll(S, x->as.i);
            mino_val_t *one;
            if (lhs == NULL) return NULL;
            one = mino_int(S, 1);
            return mino_bigint_sub(S, lhs, one);
        }
        return mino_int(S, x->as.i - 1);
    }
    if (x->type == MINO_FLOAT) return mino_float(S, x->as.f - 1.0);
    {
        mino_val_t *one = mino_int(S, 1);
        mino_val_t *pair = mino_cons(S, x, mino_cons(S, one, mino_nil(S)));
        return prim_subq(S, pair, env);
    }
}

mino_val_t *prim_div(mino_state_t *S, mino_val_t *args, mino_env_t *env)
{
    mino_val_t *first;
    (void)env;
    if (!mino_is_cons(args)) {
        return prim_throw_classified(S, "eval/arity", "MAR001",
                                     "/ requires at least one argument");
    }
    first = args->as.cons.car;
    /* Unary: 1 / x. Compute by dividing 1 by first, dispatching on tier. */
    if (!mino_is_cons(args->as.cons.cdr)) {
        mino_val_t *one  = mino_int(S, 1);
        if (one == NULL) return NULL;
        return tower_reduce_seeded(S, one, args, OP_DIV, 0, "/");
    }
    return tower_reduce_seeded(S, first, args->as.cons.cdr, OP_DIV, 0, "/");
}

mino_val_t *prim_mod(mino_state_t *S, mino_val_t *args, mino_env_t *env)
{
    double a, b, r;
    (void)env;
    if (!mino_is_cons(args) || !mino_is_cons(args->as.cons.cdr) ||
        mino_is_cons(args->as.cons.cdr->as.cons.cdr)) {
        return prim_throw_classified(S, "eval/arity", "MAR001", "mod requires two arguments");
    }
    if (!as_double(args->as.cons.car, &a) ||
        !as_double(args->as.cons.cdr->as.cons.car, &b)) {
        return prim_throw_classified(S, "eval/type", "MTY001", "mod expects numbers");
    }
    if (isnan(a) || isinf(a))
        return prim_throw_classified(S, "eval/type", "MTY001", "mod: NaN or Infinite dividend");
    if (isnan(b))
        return prim_throw_classified(S, "eval/type", "MTY001", "mod: NaN divisor");
    if (b == 0.0)
        return prim_throw_classified(S, "eval/type", "MTY001", "mod: division by zero");
    if (isinf(b))
        return mino_float(S, NAN);
    r = fmod(a, b);
    /* Floored modulo: result has same sign as divisor. */
    if (r != 0.0 && ((r < 0.0) != (b < 0.0))) r += b;
    /* Return int if both args are ints. */
    if (args->as.cons.car->type == MINO_INT &&
        args->as.cons.cdr->as.cons.car->type == MINO_INT) {
        return mino_int(S, (long long)r);
    }
    return mino_float(S, r);
}

mino_val_t *prim_rem(mino_state_t *S, mino_val_t *args, mino_env_t *env)
{
    double a, b, r;
    (void)env;
    if (!mino_is_cons(args) || !mino_is_cons(args->as.cons.cdr) ||
        mino_is_cons(args->as.cons.cdr->as.cons.cdr)) {
        return prim_throw_classified(S, "eval/arity", "MAR001", "rem requires two arguments");
    }
    if (!as_double(args->as.cons.car, &a) ||
        !as_double(args->as.cons.cdr->as.cons.car, &b)) {
        return prim_throw_classified(S, "eval/type", "MTY001", "rem expects numbers");
    }
    if (isnan(a) || isinf(a))
        return prim_throw_classified(S, "eval/type", "MTY001", "rem: NaN or Infinite dividend");
    if (isnan(b))
        return prim_throw_classified(S, "eval/type", "MTY001", "rem: NaN divisor");
    if (b == 0.0)
        return prim_throw_classified(S, "eval/type", "MTY001", "rem: division by zero");
    if (isinf(b))
        return mino_float(S, NAN);
    r = fmod(a, b);
    if (args->as.cons.car->type == MINO_INT &&
        args->as.cons.cdr->as.cons.car->type == MINO_INT) {
        return mino_int(S, (long long)r);
    }
    return mino_float(S, r);
}

mino_val_t *prim_quot(mino_state_t *S, mino_val_t *args, mino_env_t *env)
{
    double a, b, q;
    (void)env;
    if (!mino_is_cons(args) || !mino_is_cons(args->as.cons.cdr) ||
        mino_is_cons(args->as.cons.cdr->as.cons.cdr)) {
        return prim_throw_classified(S, "eval/arity", "MAR001", "quot requires two arguments");
    }
    if (!as_double(args->as.cons.car, &a) ||
        !as_double(args->as.cons.cdr->as.cons.car, &b)) {
        return prim_throw_classified(S, "eval/type", "MTY001", "quot expects numbers");
    }
    if (isnan(a) || isinf(a))
        return prim_throw_classified(S, "eval/type", "MTY001", "quot: NaN or Infinite dividend");
    if (isnan(b))
        return prim_throw_classified(S, "eval/type", "MTY001", "quot: NaN divisor");
    if (b == 0.0)
        return prim_throw_classified(S, "eval/type", "MTY001", "quot: division by zero");
    if (isinf(b))
        return mino_float(S, 0.0);
    q = a / b;
    q = q >= 0 ? floor(q) : ceil(q);
    if (args->as.cons.car->type == MINO_INT &&
        args->as.cons.cdr->as.cons.car->type == MINO_INT) {
        return mino_int(S, (long long)q);
    }
    return mino_float(S, q);
}

/* ------------------------------------------------------------------------- */
/* Math functions (thin wrappers around math.h)                              */
/* ------------------------------------------------------------------------- */

static inline mino_val_t *math_unary(mino_state_t *S, mino_val_t *args,
                                     double (*fn)(double), const char *label)
{
    char    msg[64];
    double  x;
    if (!mino_is_cons(args) || mino_is_cons(args->as.cons.cdr)) {
        snprintf(msg, sizeof(msg), "%s requires one argument", label);
        return prim_throw_classified(S, "eval/arity", "MAR001", msg);
    }
    if (!as_double(args->as.cons.car, &x)) {
        snprintf(msg, sizeof(msg), "%s expects a number", label);
        return prim_throw_classified(S, "eval/type", "MTY001", msg);
    }
    return mino_float(S, fn(x));
}

mino_val_t *prim_math_floor(mino_state_t *S, mino_val_t *args, mino_env_t *env)
{ (void)env; return math_unary(S, args, floor, "math-floor"); }

mino_val_t *prim_math_ceil(mino_state_t *S, mino_val_t *args, mino_env_t *env)
{ (void)env; return math_unary(S, args, ceil, "math-ceil"); }

mino_val_t *prim_math_round(mino_state_t *S, mino_val_t *args, mino_env_t *env)
{ (void)env; return math_unary(S, args, round, "math-round"); }

mino_val_t *prim_math_sqrt(mino_state_t *S, mino_val_t *args, mino_env_t *env)
{ (void)env; return math_unary(S, args, sqrt, "math-sqrt"); }

mino_val_t *prim_math_log(mino_state_t *S, mino_val_t *args, mino_env_t *env)
{ (void)env; return math_unary(S, args, log, "math-log"); }

mino_val_t *prim_math_exp(mino_state_t *S, mino_val_t *args, mino_env_t *env)
{ (void)env; return math_unary(S, args, exp, "math-exp"); }

mino_val_t *prim_math_sin(mino_state_t *S, mino_val_t *args, mino_env_t *env)
{ (void)env; return math_unary(S, args, sin, "math-sin"); }

mino_val_t *prim_math_cos(mino_state_t *S, mino_val_t *args, mino_env_t *env)
{ (void)env; return math_unary(S, args, cos, "math-cos"); }

mino_val_t *prim_math_tan(mino_state_t *S, mino_val_t *args, mino_env_t *env)
{ (void)env; return math_unary(S, args, tan, "math-tan"); }

mino_val_t *prim_math_pow(mino_state_t *S, mino_val_t *args, mino_env_t *env)
{
    double base, exponent;
    (void)env;
    if (!mino_is_cons(args) || !mino_is_cons(args->as.cons.cdr) ||
        mino_is_cons(args->as.cons.cdr->as.cons.cdr)) {
        return prim_throw_classified(S, "eval/arity", "MAR001", "math-pow requires two arguments");
    }
    if (!as_double(args->as.cons.car, &base) ||
        !as_double(args->as.cons.cdr->as.cons.car, &exponent)) {
        return prim_throw_classified(S, "eval/type", "MTY001", "math-pow expects numbers");
    }
    return mino_float(S, pow(base, exponent));
}

mino_val_t *prim_math_atan2(mino_state_t *S, mino_val_t *args, mino_env_t *env)
{
    double y, x;
    (void)env;
    if (!mino_is_cons(args) || !mino_is_cons(args->as.cons.cdr) ||
        mino_is_cons(args->as.cons.cdr->as.cons.cdr)) {
        return prim_throw_classified(S, "eval/arity", "MAR001", "math-atan2 requires two arguments");
    }
    if (!as_double(args->as.cons.car, &y) ||
        !as_double(args->as.cons.cdr->as.cons.car, &x)) {
        return prim_throw_classified(S, "eval/type", "MTY001", "math-atan2 expects numbers");
    }
    return mino_float(S, atan2(y, x));
}

/* ------------------------------------------------------------------------- */
/* Bitwise operations                                                        */
/* ------------------------------------------------------------------------- */

mino_val_t *prim_bit_and(mino_state_t *S, mino_val_t *args, mino_env_t *env)
{
    long long a, b;
    (void)env;
    if (!mino_is_cons(args) || !mino_is_cons(args->as.cons.cdr) ||
        mino_is_cons(args->as.cons.cdr->as.cons.cdr)) {
        return prim_throw_classified(S, "eval/arity", "MAR001", "bit-and requires two arguments");
    }
    if (!as_long(args->as.cons.car, &a) ||
        !as_long(args->as.cons.cdr->as.cons.car, &b)) {
        return prim_throw_classified(S, "eval/type", "MTY001", "bit-and expects integers");
    }
    return mino_int(S, a & b);
}

mino_val_t *prim_bit_or(mino_state_t *S, mino_val_t *args, mino_env_t *env)
{
    long long a, b;
    (void)env;
    if (!mino_is_cons(args) || !mino_is_cons(args->as.cons.cdr) ||
        mino_is_cons(args->as.cons.cdr->as.cons.cdr)) {
        return prim_throw_classified(S, "eval/arity", "MAR001", "bit-or requires two arguments");
    }
    if (!as_long(args->as.cons.car, &a) ||
        !as_long(args->as.cons.cdr->as.cons.car, &b)) {
        return prim_throw_classified(S, "eval/type", "MTY001", "bit-or expects integers");
    }
    return mino_int(S, a | b);
}

mino_val_t *prim_bit_xor(mino_state_t *S, mino_val_t *args, mino_env_t *env)
{
    long long a, b;
    (void)env;
    if (!mino_is_cons(args) || !mino_is_cons(args->as.cons.cdr) ||
        mino_is_cons(args->as.cons.cdr->as.cons.cdr)) {
        return prim_throw_classified(S, "eval/arity", "MAR001", "bit-xor requires two arguments");
    }
    if (!as_long(args->as.cons.car, &a) ||
        !as_long(args->as.cons.cdr->as.cons.car, &b)) {
        return prim_throw_classified(S, "eval/type", "MTY001", "bit-xor expects integers");
    }
    return mino_int(S, a ^ b);
}

mino_val_t *prim_bit_not(mino_state_t *S, mino_val_t *args, mino_env_t *env)
{
    long long a;
    (void)env;
    if (!mino_is_cons(args) || mino_is_cons(args->as.cons.cdr)) {
        return prim_throw_classified(S, "eval/arity", "MAR001", "bit-not requires one argument");
    }
    if (!as_long(args->as.cons.car, &a)) {
        return prim_throw_classified(S, "eval/type", "MTY001", "bit-not expects an integer");
    }
    return mino_int(S, ~a);
}

/* 64-bit shift amounts must live in [0, 63]; any other value is UB per
 * C99 (shift exponent negative or >= type width). Validate once in a
 * shared helper so each prim enforces the same bounds. */
#define MINO_SHIFT_WIDTH 64

static int shift_amount_ok(long long b)
{
    return b >= 0 && b < MINO_SHIFT_WIDTH;
}

mino_val_t *prim_bit_shift_left(mino_state_t *S, mino_val_t *args, mino_env_t *env)
{
    long long a, b;
    (void)env;
    if (!mino_is_cons(args) || !mino_is_cons(args->as.cons.cdr) ||
        mino_is_cons(args->as.cons.cdr->as.cons.cdr)) {
        return prim_throw_classified(S, "eval/arity", "MAR001", "bit-shift-left requires two arguments");
    }
    if (!as_long(args->as.cons.car, &a) ||
        !as_long(args->as.cons.cdr->as.cons.car, &b)) {
        return prim_throw_classified(S, "eval/type", "MTY001", "bit-shift-left expects integers");
    }
    if (!shift_amount_ok(b)) {
        return prim_throw_classified(S, "eval/bounds", "MBD001",
            "bit-shift-left shift amount must be in [0, 63]");
    }
    /* Route the shift through unsigned so that (bit-shift-left 1 63)
     * yields the usual -9223372036854775808 wrap result without tripping
     * signed-overflow UB. */
    return mino_int(S, (long long)((unsigned long long)a << b));
}

mino_val_t *prim_bit_shift_right(mino_state_t *S, mino_val_t *args, mino_env_t *env)
{
    long long a, b;
    (void)env;
    if (!mino_is_cons(args) || !mino_is_cons(args->as.cons.cdr) ||
        mino_is_cons(args->as.cons.cdr->as.cons.cdr)) {
        return prim_throw_classified(S, "eval/arity", "MAR001", "bit-shift-right requires two arguments");
    }
    if (!as_long(args->as.cons.car, &a) ||
        !as_long(args->as.cons.cdr->as.cons.car, &b)) {
        return prim_throw_classified(S, "eval/type", "MTY001", "bit-shift-right expects integers");
    }
    if (!shift_amount_ok(b)) {
        return prim_throw_classified(S, "eval/bounds", "MBD001",
            "bit-shift-right shift amount must be in [0, 63]");
    }
    /* Signed right shift of a negative value is implementation-defined;
     * all our supported targets (GCC/Clang/MSVC on x86_64 and ARM64)
     * produce arithmetic shift, which is the Clojure-expected behavior. */
    return mino_int(S, a >> b);
}

mino_val_t *prim_unsigned_bit_shift_right(mino_state_t *S, mino_val_t *args, mino_env_t *env)
{
    long long a, b;
    (void)env;
    if (!mino_is_cons(args) || !mino_is_cons(args->as.cons.cdr) ||
        mino_is_cons(args->as.cons.cdr->as.cons.cdr)) {
        return prim_throw_classified(S, "eval/arity", "MAR001", "unsigned-bit-shift-right requires two arguments");
    }
    if (!as_long(args->as.cons.car, &a) ||
        !as_long(args->as.cons.cdr->as.cons.car, &b)) {
        return prim_throw_classified(S, "eval/type", "MTY001", "unsigned-bit-shift-right expects integers");
    }
    if (!shift_amount_ok(b)) {
        return prim_throw_classified(S, "eval/bounds", "MBD001",
            "unsigned-bit-shift-right shift amount must be in [0, 63]");
    }
    return mino_int(S, (long long)((unsigned long long)a >> b));
}

/* ------------------------------------------------------------------------- */
/* Type coercion                                                             */
/* ------------------------------------------------------------------------- */

mino_val_t *prim_int(mino_state_t *S, mino_val_t *args, mino_env_t *env)
{
    mino_val_t *v;
    (void)env;
    if (!mino_is_cons(args) || mino_is_cons(args->as.cons.cdr)) {
        return prim_throw_classified(S, "eval/arity", "MAR001", "int requires one argument");
    }
    v = args->as.cons.car;
    if (v != NULL && v->type == MINO_INT) return v;
    if (v != NULL && v->type == MINO_FLOAT) return mino_int(S, (long long)v->as.f);
    if (v != NULL && v->type == MINO_BIGINT) {
        long long ll;
        if (mino_as_ll(v, &ll)) return mino_int(S, ll);
        return prim_throw_classified(S, "eval/overflow", "MOV001",
                                     "int: bigint value out of long range");
    }
    if (v != NULL && v->type == MINO_RATIO) {
        /* Truncate toward zero: numerator / denominator using integer div. */
        return mino_int(S, (long long)mino_ratio_to_double(v));
    }
    if (v != NULL && v->type == MINO_BIGDEC) {
        return mino_int(S, (long long)mino_bigdec_to_double(v));
    }
    /* (int \a) -> 97: char value yields its Unicode codepoint. */
    if (v != NULL && v->type == MINO_CHAR) {
        return mino_int(S, (long long)v->as.ch);
    }
    /* (int "a") -> 97: single-char string to char code (legacy). */
    if (v != NULL && v->type == MINO_STRING && v->as.s.len == 1) {
        return mino_int(S, (long long)(unsigned char)v->as.s.data[0]);
    }
    return prim_throw_classified(S, "eval/type", "MTY001", "int: expected a number");
}

mino_val_t *prim_float(mino_state_t *S, mino_val_t *args, mino_env_t *env)
{
    mino_val_t *v;
    (void)env;
    if (!mino_is_cons(args) || mino_is_cons(args->as.cons.cdr)) {
        return prim_throw_classified(S, "eval/arity", "MAR001", "float requires one argument");
    }
    v = args->as.cons.car;
    if (v != NULL && v->type == MINO_FLOAT) return v;
    if (v != NULL && v->type == MINO_INT) return mino_float(S, (double)v->as.i);
    if (v != NULL && v->type == MINO_BIGINT)
        return mino_float(S, mino_bigint_to_double(v));
    if (v != NULL && v->type == MINO_RATIO)
        return mino_float(S, mino_ratio_to_double(v));
    if (v != NULL && v->type == MINO_BIGDEC)
        return mino_float(S, mino_bigdec_to_double(v));
    return prim_throw_classified(S, "eval/type", "MTY001", "float: expected a number");
}

mino_val_t *prim_parse_long(mino_state_t *S, mino_val_t *args, mino_env_t *env)
{
    mino_val_t *v;
    const char *s;
    char *end;
    long long result;
    (void)env;
    if (!mino_is_cons(args) || mino_is_cons(args->as.cons.cdr)) {
        return prim_throw_classified(S, "eval/arity", "MAR001", "parse-long requires one argument");
    }
    v = args->as.cons.car;
    if (v == NULL || v->type != MINO_STRING)
        return prim_throw_classified(S, "eval/type", "MTY001", "parse-long: argument must be a string");
    s = v->as.s.data;
    if (v->as.s.len == 0 || isspace((unsigned char)s[0]))
        return mino_nil(S);
    errno = 0;
    result = strtoll(s, &end, 10);
    if (end == s || *end != '\0' || errno == ERANGE)
        return mino_nil(S);
    return mino_int(S, result);
}

mino_val_t *prim_parse_double(mino_state_t *S, mino_val_t *args, mino_env_t *env)
{
    mino_val_t *v;
    const char *s;
    char *end;
    double result;
    (void)env;
    if (!mino_is_cons(args) || mino_is_cons(args->as.cons.cdr)) {
        return prim_throw_classified(S, "eval/arity", "MAR001", "parse-double requires one argument");
    }
    v = args->as.cons.car;
    if (v == NULL || v->type != MINO_STRING)
        return prim_throw_classified(S, "eval/type", "MTY001", "parse-double: argument must be a string");
    s = v->as.s.data;
    if (v->as.s.len == 0 || isspace((unsigned char)s[0]))
        return mino_nil(S);
    errno = 0;
    result = strtod(s, &end);
    if (end == s || *end != '\0')
        return mino_nil(S);
    return mino_float(S, result);
}

/* ------------------------------------------------------------------------- */
/* Comparison                                                                */
/* ------------------------------------------------------------------------- */

/* (compare a b) -- general comparison returning -1, 0, or 1.
 * Compares numbers, strings, keywords, symbols, and nil. */
mino_val_t *prim_compare(mino_state_t *S, mino_val_t *args, mino_env_t *env)
{
    mino_val_t *a, *b;
    (void)env;
    if (!mino_is_cons(args) || !mino_is_cons(args->as.cons.cdr) ||
        mino_is_cons(args->as.cons.cdr->as.cons.cdr)) {
        return prim_throw_classified(S, "eval/arity", "MAR001", "compare requires two arguments");
    }
    a = args->as.cons.car;
    b = args->as.cons.cdr->as.cons.car;
    /* nil sorts before everything */
    if ((a == NULL || (a->type == MINO_NIL)) &&
        (b == NULL || (b->type == MINO_NIL))) return mino_int(S, 0);
    if (a == NULL || a->type == MINO_NIL) return mino_int(S, -1);
    if (b == NULL || b->type == MINO_NIL) return mino_int(S, 1);
    /* numbers */
    {
        double da, db;
        if (as_double(a, &da) && as_double(b, &db)) {
            return mino_int(S, da < db ? -1 : da > db ? 1 : 0);
        }
    }
    /* strings, keywords, symbols -- lexicographic */
    if ((a->type == MINO_STRING || a->type == MINO_KEYWORD ||
         a->type == MINO_SYMBOL) && a->type == b->type) {
        int cmp = strcmp(a->as.s.data, b->as.s.data);
        return mino_int(S, cmp < 0 ? -1 : cmp > 0 ? 1 : 0);
    }
    return prim_throw_classified(S, "eval/type", "MTY001", "compare: cannot compare values of different types");
}

mino_val_t *prim_eq(mino_state_t *S, mino_val_t *args, mino_env_t *env)
{
    (void)env;
    if (!mino_is_cons(args)) {
        return mino_true(S);
    }
    {
        mino_val_t *first = args->as.cons.car;
        args = args->as.cons.cdr;
        while (mino_is_cons(args)) {
            if (!mino_eq_force(S, first, args->as.cons.car)) {
                return mino_false(S);
            }
            args = args->as.cons.cdr;
        }
    }
    return mino_true(S);
}

/* (== & nums) — numeric equality across all five tiers. Unlike `=`,
 * which is type-strict on the numeric tower (so `(= 1 1.0)` is false),
 * `==` returns true whenever the values are numerically equal regardless
 * of representation. Implementation pairs each arg by tier:
 *   - Both INT/BIGINT: exact integer equality (via mino_eq_force which
 *     already handles int↔bigint).
 *   - Both same-type ratio/bigdec: same-type comparison.
 *   - Mixed otherwise: convert to double and compare (matches Clojure's
 *     float-promotion semantics; loses precision for huge bigints, which
 *     mirrors Clojure's behaviour). */
static int num_pair_eq(const mino_val_t *a, const mino_val_t *b)
{
    if (a == NULL || b == NULL) return 0;
    /* Exact integer comparison. */
    if ((a->type == MINO_INT || a->type == MINO_BIGINT) &&
        (b->type == MINO_INT || b->type == MINO_BIGINT)) {
        if (a->type == MINO_INT && b->type == MINO_INT) return a->as.i == b->as.i;
        if (a->type == MINO_BIGINT && b->type == MINO_BIGINT)
            return mino_bigint_equals(a, b);
        if (a->type == MINO_INT) return mino_bigint_equals_ll(b, a->as.i);
        return mino_bigint_equals_ll(a, b->as.i);
    }
    /* Same-tier ratio. */
    if (a->type == MINO_RATIO && b->type == MINO_RATIO)
        return mino_ratio_equals(a, b);
    /* Same-tier bigdec: compare by value (not representation), since
     * == is numeric equality. */
    if (a->type == MINO_BIGDEC && b->type == MINO_BIGDEC)
        return mino_bigdec_cmp(a, b) == 0;
    /* Cross-tier with float involved: collapse to double and compare. */
    {
        double da = tower_to_double(a);
        double db = tower_to_double(b);
        return da == db;
    }
}

mino_val_t *prim_num_eq(mino_state_t *S, mino_val_t *args, mino_env_t *env)
{
    (void)env;
    if (!mino_is_cons(args)) return mino_true(S);
    {
        mino_val_t *first = args->as.cons.car;
        int t;
        if (!classify_or_throw(S, first, "==", &t)) return NULL;
        args = args->as.cons.cdr;
        while (mino_is_cons(args)) {
            mino_val_t *next = args->as.cons.car;
            int nt;
            if (!classify_or_throw(S, next, "==", &nt)) return NULL;
            if (!num_pair_eq(first, next)) return mino_false(S);
            args = args->as.cons.cdr;
        }
    }
    return mino_true(S);
}

mino_val_t *prim_identical(mino_state_t *S, mino_val_t *args,
                           mino_env_t *env)
{
    mino_val_t *a, *b;
    (void)env;
    if (!mino_is_cons(args) || !mino_is_cons(args->as.cons.cdr)) {
        return prim_throw_classified(S, "eval/arity", "MAR001", "identical? requires 2 arguments");
    }
    a = args->as.cons.car;
    b = args->as.cons.cdr->as.cons.car;
    return (a == b) ? mino_true(S) : mino_false(S);
}

/*
 * Chained numeric comparison. `op` selects the relation:
 *   0: <    1: <=    2: >    3: >=
 * Returns true if each successive pair satisfies the relation (and
 * trivially true on zero or one argument).
 */
/* Cross-tier numeric three-way compare. Returns -1, 0, or 1; -2 if
 * either operand isn't numeric. */
static int tower_cmp(const mino_val_t *a, const mino_val_t *b)
{
    if (a == NULL || b == NULL) return -2;
    /* Same-type fast paths. */
    if (a->type == b->type) {
        switch (a->type) {
        case MINO_INT:
            return a->as.i < b->as.i ? -1 : a->as.i > b->as.i ? 1 : 0;
        case MINO_FLOAT:
            return a->as.f < b->as.f ? -1 : a->as.f > b->as.f ? 1 : 0;
        case MINO_BIGINT:
            return mino_bigint_cmp(a, b);
        case MINO_RATIO:
            return mino_ratio_cmp(a, b);
        case MINO_BIGDEC:
            return mino_bigdec_cmp(a, b);
        default: return -2;
        }
    }
    /* Mixed int/bigint: compare via bigint_equals_ll + magnitude check. */
    if ((a->type == MINO_INT && b->type == MINO_BIGINT) ||
        (a->type == MINO_BIGINT && b->type == MINO_INT)) {
        long long ll;
        if (a->type == MINO_INT) {
            if (mino_bigint_equals_ll(b, a->as.i)) return 0;
            /* Compare magnitudes via double; for long-fitting bigints
             * the comparison is exact, otherwise the bigint dominates. */
            if (mino_as_ll(b, &ll))
                return a->as.i < ll ? -1 : a->as.i > ll ? 1 : 0;
            return mino_bigint_to_double(b) > 0 ? -1 : 1;
        } else {
            if (mino_bigint_equals_ll(a, b->as.i)) return 0;
            if (mino_as_ll(a, &ll))
                return ll < b->as.i ? -1 : ll > b->as.i ? 1 : 0;
            return mino_bigint_to_double(a) > 0 ? 1 : -1;
        }
    }
    /* Cross-tier with float / bigdec / ratio: collapse to double. */
    {
        double da = tower_to_double(a);
        double db = tower_to_double(b);
        if (a->type < 0) return -2; /* unreachable defensive */
        return da < db ? -1 : da > db ? 1 : 0;
    }
}

static mino_val_t *compare_chain(mino_state_t *S, mino_val_t *args, const char *name, int op)
{
    if (!mino_is_cons(args)) return mino_true(S);
    if (!mino_is_cons(args->as.cons.cdr)) return mino_true(S);
    {
        const mino_val_t *prev = args->as.cons.car;
        args = args->as.cons.cdr;
        while (mino_is_cons(args)) {
            const mino_val_t *cur = args->as.cons.car;
            int cmp = tower_cmp(prev, cur);
            int ok;
            if (cmp == -2) {
                char msg[64];
                snprintf(msg, sizeof(msg), "%s expects numbers", name);
                return prim_throw_classified(S, "eval/type", "MTY001", msg);
            }
            switch (op) {
            case 0:  ok = cmp <  0; break;
            case 1:  ok = cmp <= 0; break;
            case 2:  ok = cmp >  0; break;
            default: ok = cmp >= 0; break;
            }
            if (!ok) return mino_false(S);
            prev = cur;
            args = args->as.cons.cdr;
        }
    }
    return mino_true(S);
}

mino_val_t *prim_lt(mino_state_t *S, mino_val_t *args, mino_env_t *env)
{
    (void)env;
    return compare_chain(S, args, "<", 0);
}

mino_val_t *prim_lte(mino_state_t *S, mino_val_t *args, mino_env_t *env)
{
    (void)env;
    return compare_chain(S, args, "<=", 1);
}

mino_val_t *prim_gt(mino_state_t *S, mino_val_t *args, mino_env_t *env)
{
    (void)env;
    return compare_chain(S, args, ">", 2);
}

mino_val_t *prim_gte(mino_state_t *S, mino_val_t *args, mino_env_t *env)
{
    (void)env;
    return compare_chain(S, args, ">=", 3);
}

mino_val_t *prim_nan_p(mino_state_t *S, mino_val_t *args, mino_env_t *env)
{
    mino_val_t *v;
    (void)env;
    if (!mino_is_cons(args) || mino_is_cons(args->as.cons.cdr)) {
        return prim_throw_classified(S, "eval/arity", "MAR001", "NaN? requires one argument");
    }
    v = args->as.cons.car;
    if (v == NULL || v->type == MINO_NIL)
        return prim_throw_classified(S, "eval/type", "MTY001", "NaN? requires a number");
    return (v->type == MINO_FLOAT && isnan(v->as.f))
           ? mino_true(S) : mino_false(S);
}

mino_val_t *prim_infinite_p(mino_state_t *S, mino_val_t *args, mino_env_t *env)
{
    mino_val_t *v;
    (void)env;
    if (!mino_is_cons(args) || mino_is_cons(args->as.cons.cdr)) {
        return prim_throw_classified(S, "eval/arity", "MAR001", "infinite? requires one argument");
    }
    v = args->as.cons.car;
    if (v == NULL || v->type == MINO_NIL)
        return prim_throw_classified(S, "eval/type", "MTY001", "infinite? requires a number");
    return (v->type == MINO_FLOAT && isinf(v->as.f))
           ? mino_true(S) : mino_false(S);
}

const mino_prim_def k_prims_numeric[] = {
    {"+",   prim_add,
     "Returns the sum of the arguments."},
    {"inc", prim_inc,
     "Returns x plus 1."},
    {"dec", prim_dec,
     "Returns x minus 1."},
    {"-",   prim_sub,
     "Returns the difference of the arguments. With one arg, returns the negation."},
    {"*",   prim_mul,
     "Returns the product of the arguments."},
    {"/",   prim_div,
     "Returns the quotient of the arguments."},
    {"=",   prim_eq,
     "Returns true if all arguments are equal."},
    {"==",  prim_num_eq,
     "Returns true if all arguments are numerically equal across "
     "the numeric tower (int, bigint, ratio, bigdec, float)."},
    {"identical?", prim_identical,
     "Returns true if the arguments are the same object."},
    {"<",   prim_lt,
     "Returns true if nums are in monotonically increasing order."},
    {"<=",  prim_lte,
     "Returns true if nums are in monotonically non-decreasing order."},
    {">",   prim_gt,
     "Returns true if nums are in monotonically decreasing order."},
    {">=",  prim_gte,
     "Returns true if nums are in monotonically non-increasing order."},
    {"mod", prim_mod,
     "Returns the modulus of dividing num by div. Truncates toward negative infinity."},
    {"rem", prim_rem,
     "Returns the remainder of dividing num by div."},
    {"quot", prim_quot,
     "Returns the quotient of dividing num by div, truncated toward zero."},
    {"math-floor", prim_math_floor,
     "Returns the largest integer not greater than n."},
    {"math-ceil",  prim_math_ceil,
     "Returns the smallest integer not less than n."},
    {"math-round", prim_math_round,
     "Returns the closest integer to n."},
    {"math-sqrt",  prim_math_sqrt,
     "Returns the square root of n."},
    {"math-pow",   prim_math_pow,
     "Returns base raised to the power of exp."},
    {"math-log",   prim_math_log,
     "Returns the natural logarithm of n."},
    {"math-exp",   prim_math_exp,
     "Returns e raised to the power of n."},
    {"math-sin",   prim_math_sin,
     "Returns the sine of n (in radians)."},
    {"math-cos",   prim_math_cos,
     "Returns the cosine of n (in radians)."},
    {"math-tan",   prim_math_tan,
     "Returns the tangent of n (in radians)."},
    {"math-atan2", prim_math_atan2,
     "Returns the angle in radians between the positive x-axis and the point (x, y)."},
    {"bit-and", prim_bit_and,
     "Returns the bitwise AND of the arguments."},
    {"bit-or",  prim_bit_or,
     "Returns the bitwise OR of the arguments."},
    {"bit-xor", prim_bit_xor,
     "Returns the bitwise XOR of the arguments."},
    {"bit-not", prim_bit_not,
     "Returns the bitwise complement of n."},
    {"bit-shift-left", prim_bit_shift_left,
     "Returns n shifted left by count bits."},
    {"bit-shift-right", prim_bit_shift_right,
     "Returns n arithmetically shifted right by count bits."},
    {"unsigned-bit-shift-right", prim_unsigned_bit_shift_right,
     "Returns n logically shifted right by count bits."},
    {"compare", prim_compare,
     "Returns a negative, zero, or positive integer comparing x and y."},
    {"NaN?",    prim_nan_p,
     "Returns true if x is NaN."},
    {"infinite?", prim_infinite_p,
     "Returns true if x is positive or negative infinity."},
    {"int",   prim_int,
     "Coerces x to an integer."},
    {"float", prim_float,
     "Coerces x to a float."},
    {"parse-long",   prim_parse_long,
     "Parses a string into a long integer, or returns nil on failure."},
    {"parse-double", prim_parse_double,
     "Parses a string into a double, or returns nil on failure."},
    {"+'",   prim_addq,
     "Like +, but promotes to bigint on overflow rather than throwing."},
    {"-'",   prim_subq,
     "Like -, but promotes to bigint on overflow rather than throwing."},
    {"*'",   prim_mulq,
     "Like *, but promotes to bigint on overflow rather than throwing."},
    {"inc'", prim_incq,
     "Like inc, but promotes to bigint on overflow rather than throwing."},
    {"dec'", prim_decq,
     "Like dec, but promotes to bigint on overflow rather than throwing."},
};

const size_t k_prims_numeric_count =
    sizeof(k_prims_numeric) / sizeof(k_prims_numeric[0]);
