/*
 * prim_numeric.c -- numeric, math, bitwise, coercion, and comparison primitives.
 */

#include "prim_internal.h"

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
/* ------------------------------------------------------------------------- */

mino_val_t *prim_add(mino_state_t *S, mino_val_t *args, mino_env_t *env)
{
    long long iacc = 0;
    (void)env;
    while (mino_is_cons(args)) {
        mino_val_t *a = args->as.cons.car;
        if (a != NULL && a->type == MINO_INT) {
            if (iadd_overflow(iacc, a->as.i, &iacc)) {
                return prim_throw_classified(S, "eval/overflow", "MOV001",
                    "integer overflow in +");
            }
            args = args->as.cons.cdr;
            continue;
        }
        if (a != NULL && a->type == MINO_FLOAT) {
            /* Promote to float and finish the tail in float mode. */
            double dacc = (double)iacc + a->as.f;
            args = args->as.cons.cdr;
            while (mino_is_cons(args)) {
                double x;
                if (!as_double(args->as.cons.car, &x)) {
                    return prim_throw_classified(S, "eval/type", "MTY001", "+ expects numbers");
                }
                dacc += x;
                args = args->as.cons.cdr;
            }
            return mino_float(S, dacc);
        }
        return prim_throw_classified(S, "eval/type", "MTY001", "+ expects numbers");
    }
    return mino_int(S, iacc);
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
    return prim_throw_classified(S, "eval/type", "MTY001",
        "dec expects a number");
}

mino_val_t *prim_sub(mino_state_t *S, mino_val_t *args, mino_env_t *env)
{
    mino_val_t *first;
    long long iacc;
    (void)env;
    if (!mino_is_cons(args)) {
        return prim_throw_classified(S, "eval/arity", "MAR001", "- requires at least one argument");
    }
    first = args->as.cons.car;
    if (first != NULL && first->type == MINO_INT) {
        iacc = first->as.i;
        args = args->as.cons.cdr;
        if (!mino_is_cons(args)) {
            long long neg;
            if (ineg_overflow(iacc, &neg)) {
                return prim_throw_classified(S, "eval/overflow", "MOV001",
                    "integer overflow in -");
            }
            return mino_int(S, neg);
        }
        while (mino_is_cons(args)) {
            mino_val_t *a = args->as.cons.car;
            if (a != NULL && a->type == MINO_INT) {
                if (isub_overflow(iacc, a->as.i, &iacc)) {
                    return prim_throw_classified(S, "eval/overflow", "MOV001",
                        "integer overflow in -");
                }
                args = args->as.cons.cdr;
                continue;
            }
            if (a != NULL && a->type == MINO_FLOAT) {
                double dacc = (double)iacc - a->as.f;
                args = args->as.cons.cdr;
                while (mino_is_cons(args)) {
                    double x;
                    if (!as_double(args->as.cons.car, &x)) {
                        return prim_throw_classified(S, "eval/type", "MTY001", "- expects numbers");
                    }
                    dacc -= x;
                    args = args->as.cons.cdr;
                }
                return mino_float(S, dacc);
            }
            return prim_throw_classified(S, "eval/type", "MTY001", "- expects numbers");
        }
        return mino_int(S, iacc);
    }
    if (first != NULL && first->type == MINO_FLOAT) {
        double dacc = first->as.f;
        args = args->as.cons.cdr;
        if (!mino_is_cons(args)) {
            return mino_float(S, -dacc);
        }
        while (mino_is_cons(args)) {
            double x;
            if (!as_double(args->as.cons.car, &x)) {
                return prim_throw_classified(S, "eval/type", "MTY001", "- expects numbers");
            }
            dacc -= x;
            args = args->as.cons.cdr;
        }
        return mino_float(S, dacc);
    }
    return prim_throw_classified(S, "eval/type", "MTY001", "- expects numbers");
}

mino_val_t *prim_mul(mino_state_t *S, mino_val_t *args, mino_env_t *env)
{
    long long iacc = 1;
    (void)env;
    while (mino_is_cons(args)) {
        mino_val_t *a = args->as.cons.car;
        if (a != NULL && a->type == MINO_INT) {
            if (imul_overflow(iacc, a->as.i, &iacc)) {
                return prim_throw_classified(S, "eval/overflow", "MOV001",
                    "integer overflow in *");
            }
            args = args->as.cons.cdr;
            continue;
        }
        if (a != NULL && a->type == MINO_FLOAT) {
            double dacc = (double)iacc * a->as.f;
            args = args->as.cons.cdr;
            while (mino_is_cons(args)) {
                double x;
                if (!as_double(args->as.cons.car, &x)) {
                    return prim_throw_classified(S, "eval/type", "MTY001", "* expects numbers");
                }
                dacc *= x;
                args = args->as.cons.cdr;
            }
            return mino_float(S, dacc);
        }
        return prim_throw_classified(S, "eval/type", "MTY001", "* expects numbers");
    }
    return mino_int(S, iacc);
}

/* ------------------------------------------------------------------------- */
/* Auto-promoting arithmetic (+' -' *' inc' dec')                            */
/*                                                                           */
/* Like +, -, * but overflow on a long result promotes the running           */
/* accumulator to a bigint rather than throwing. A float operand anywhere    */
/* collapses the tower to doubles (matching Clojure's +' behaviour).         */
/*                                                                           */
/* The accumulator is tracked in one of three tiers; tier transitions are    */
/* one-way: int -> bigint -> double (and int -> double direct).              */
/* ------------------------------------------------------------------------- */

typedef enum { TIER_INT = 0, TIER_BIGINT, TIER_FLOAT } tier_t;

/* Convert any int/bigint/float into a double. Used when collapsing to
 * TIER_FLOAT. bigint conversion may lose precision (cold path, matches
 * Clojure double coercion). */
static int numeric_as_double(const mino_val_t *v, double *out)
{
    if (v == NULL) return 0;
    if (v->type == MINO_INT)    { *out = (double)v->as.i; return 1; }
    if (v->type == MINO_FLOAT)  { *out = v->as.f;         return 1; }
    if (v->type == MINO_BIGINT) { *out = mino_bigint_to_double(v); return 1; }
    return 0;
}

/* (+' & args) */
mino_val_t *prim_addq(mino_state_t *S, mino_val_t *args, mino_env_t *env)
{
    long long iacc = 0;
    mino_val_t *bacc = NULL;
    double     dacc = 0.0;
    tier_t     tier = TIER_INT;
    (void)env;
    while (mino_is_cons(args)) {
        mino_val_t *a = args->as.cons.car;
        if (a == NULL)
            return prim_throw_classified(S, "eval/type", "MTY001",
                                         "+' expects numbers");
        switch (tier) {
        case TIER_INT:
            if (a->type == MINO_INT) {
                long long prev = iacc;
                if (iadd_overflow(iacc, a->as.i, &iacc)) {
                    mino_val_t *la = mino_bigint_from_ll(S, prev);
                    if (la == NULL) return NULL;
                    bacc = mino_bigint_add(S, la, a);
                    if (bacc == NULL) return NULL;
                    tier = TIER_BIGINT;
                }
            } else if (a->type == MINO_BIGINT) {
                mino_val_t *la = mino_bigint_from_ll(S, iacc);
                if (la == NULL) return NULL;
                bacc = mino_bigint_add(S, la, a);
                if (bacc == NULL) return NULL;
                tier = TIER_BIGINT;
            } else if (a->type == MINO_FLOAT) {
                dacc = (double)iacc + a->as.f;
                tier = TIER_FLOAT;
            } else {
                return prim_throw_classified(S, "eval/type", "MTY001",
                                             "+' expects numbers");
            }
            break;
        case TIER_BIGINT:
            if (a->type == MINO_INT || a->type == MINO_BIGINT) {
                bacc = mino_bigint_add(S, bacc, a);
                if (bacc == NULL) return NULL;
            } else if (a->type == MINO_FLOAT) {
                dacc = mino_bigint_to_double(bacc) + a->as.f;
                tier = TIER_FLOAT;
            } else {
                return prim_throw_classified(S, "eval/type", "MTY001",
                                             "+' expects numbers");
            }
            break;
        case TIER_FLOAT: {
            double x;
            if (!numeric_as_double(a, &x))
                return prim_throw_classified(S, "eval/type", "MTY001",
                                             "+' expects numbers");
            dacc += x;
            break;
        }
        }
        args = args->as.cons.cdr;
    }
    if (tier == TIER_FLOAT)  return mino_float(S, dacc);
    if (tier == TIER_BIGINT) return bacc;
    return mino_int(S, iacc);
}

/* (-' x) -> negation; (-' x y ...) -> successive subtraction. */
mino_val_t *prim_subq(mino_state_t *S, mino_val_t *args, mino_env_t *env)
{
    mino_val_t *first;
    long long iacc = 0;
    mino_val_t *bacc = NULL;
    double     dacc = 0.0;
    tier_t     tier = TIER_INT;
    (void)env;
    if (!mino_is_cons(args)) {
        return prim_throw_classified(S, "eval/arity", "MAR001",
                                     "-' requires at least one argument");
    }
    first = args->as.cons.car;
    if (first == NULL)
        return prim_throw_classified(S, "eval/type", "MTY001",
                                     "-' expects numbers");
    if (first->type == MINO_INT) {
        iacc = first->as.i;
    } else if (first->type == MINO_BIGINT) {
        /* bigint_binop always writes into a fresh result cell, so seeding
         * bacc with first directly is safe — the first subsequent sub
         * produces a new bigint and rebinds bacc. */
        bacc = (mino_val_t *)first;
        tier = TIER_BIGINT;
    } else if (first->type == MINO_FLOAT) {
        dacc = first->as.f;
        tier = TIER_FLOAT;
    } else {
        return prim_throw_classified(S, "eval/type", "MTY001",
                                     "-' expects numbers");
    }
    args = args->as.cons.cdr;
    /* Unary negation. */
    if (!mino_is_cons(args)) {
        if (tier == TIER_INT) {
            long long neg;
            if (ineg_overflow(iacc, &neg)) {
                mino_val_t *first_bi = mino_bigint_from_ll(S, iacc);
                if (first_bi == NULL) return NULL;
                return mino_bigint_neg(S, first_bi);
            }
            return mino_int(S, neg);
        }
        if (tier == TIER_BIGINT) return mino_bigint_neg(S, bacc);
        return mino_float(S, -dacc);
    }
    while (mino_is_cons(args)) {
        mino_val_t *a = args->as.cons.car;
        if (a == NULL)
            return prim_throw_classified(S, "eval/type", "MTY001",
                                         "-' expects numbers");
        switch (tier) {
        case TIER_INT:
            if (a->type == MINO_INT) {
                long long prev = iacc;
                if (isub_overflow(iacc, a->as.i, &iacc)) {
                    mino_val_t *la = mino_bigint_from_ll(S, prev);
                    if (la == NULL) return NULL;
                    bacc = mino_bigint_sub(S, la, a);
                    if (bacc == NULL) return NULL;
                    tier = TIER_BIGINT;
                }
            } else if (a->type == MINO_BIGINT) {
                mino_val_t *la = mino_bigint_from_ll(S, iacc);
                if (la == NULL) return NULL;
                bacc = mino_bigint_sub(S, la, a);
                if (bacc == NULL) return NULL;
                tier = TIER_BIGINT;
            } else if (a->type == MINO_FLOAT) {
                dacc = (double)iacc - a->as.f;
                tier = TIER_FLOAT;
            } else {
                return prim_throw_classified(S, "eval/type", "MTY001",
                                             "-' expects numbers");
            }
            break;
        case TIER_BIGINT:
            if (a->type == MINO_INT || a->type == MINO_BIGINT) {
                bacc = mino_bigint_sub(S, bacc, a);
                if (bacc == NULL) return NULL;
            } else if (a->type == MINO_FLOAT) {
                dacc = mino_bigint_to_double(bacc) - a->as.f;
                tier = TIER_FLOAT;
            } else {
                return prim_throw_classified(S, "eval/type", "MTY001",
                                             "-' expects numbers");
            }
            break;
        case TIER_FLOAT: {
            double x;
            if (!numeric_as_double(a, &x))
                return prim_throw_classified(S, "eval/type", "MTY001",
                                             "-' expects numbers");
            dacc -= x;
            break;
        }
        }
        args = args->as.cons.cdr;
    }
    if (tier == TIER_FLOAT)  return mino_float(S, dacc);
    if (tier == TIER_BIGINT) return bacc;
    return mino_int(S, iacc);
}

/* (*' & args) */
mino_val_t *prim_mulq(mino_state_t *S, mino_val_t *args, mino_env_t *env)
{
    long long iacc = 1;
    mino_val_t *bacc = NULL;
    double     dacc = 0.0;
    tier_t     tier = TIER_INT;
    (void)env;
    while (mino_is_cons(args)) {
        mino_val_t *a = args->as.cons.car;
        if (a == NULL)
            return prim_throw_classified(S, "eval/type", "MTY001",
                                         "*' expects numbers");
        switch (tier) {
        case TIER_INT:
            if (a->type == MINO_INT) {
                long long prev = iacc;
                if (imul_overflow(iacc, a->as.i, &iacc)) {
                    mino_val_t *la = mino_bigint_from_ll(S, prev);
                    if (la == NULL) return NULL;
                    bacc = mino_bigint_mul(S, la, a);
                    if (bacc == NULL) return NULL;
                    tier = TIER_BIGINT;
                }
            } else if (a->type == MINO_BIGINT) {
                mino_val_t *la = mino_bigint_from_ll(S, iacc);
                if (la == NULL) return NULL;
                bacc = mino_bigint_mul(S, la, a);
                if (bacc == NULL) return NULL;
                tier = TIER_BIGINT;
            } else if (a->type == MINO_FLOAT) {
                dacc = (double)iacc * a->as.f;
                tier = TIER_FLOAT;
            } else {
                return prim_throw_classified(S, "eval/type", "MTY001",
                                             "*' expects numbers");
            }
            break;
        case TIER_BIGINT:
            if (a->type == MINO_INT || a->type == MINO_BIGINT) {
                bacc = mino_bigint_mul(S, bacc, a);
                if (bacc == NULL) return NULL;
            } else if (a->type == MINO_FLOAT) {
                dacc = mino_bigint_to_double(bacc) * a->as.f;
                tier = TIER_FLOAT;
            } else {
                return prim_throw_classified(S, "eval/type", "MTY001",
                                             "*' expects numbers");
            }
            break;
        case TIER_FLOAT: {
            double x;
            if (!numeric_as_double(a, &x))
                return prim_throw_classified(S, "eval/type", "MTY001",
                                             "*' expects numbers");
            dacc *= x;
            break;
        }
        }
        args = args->as.cons.cdr;
    }
    if (tier == TIER_FLOAT)  return mino_float(S, dacc);
    if (tier == TIER_BIGINT) return bacc;
    return mino_int(S, iacc);
}

/* (inc' x) */
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
    if (x->type == MINO_BIGINT) {
        mino_val_t *one = mino_int(S, 1);
        return mino_bigint_add(S, x, one);
    }
    if (x->type == MINO_FLOAT) return mino_float(S, x->as.f + 1.0);
    return prim_throw_classified(S, "eval/type", "MTY001",
                                 "inc' expects a number");
}

/* (dec' x) */
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
    if (x->type == MINO_BIGINT) {
        mino_val_t *one = mino_int(S, 1);
        return mino_bigint_sub(S, x, one);
    }
    if (x->type == MINO_FLOAT) return mino_float(S, x->as.f - 1.0);
    return prim_throw_classified(S, "eval/type", "MTY001",
                                 "dec' expects a number");
}

mino_val_t *prim_div(mino_state_t *S, mino_val_t *args, mino_env_t *env)
{
    /* Division returns an integer when all operands are integers and the
     * result is exact, a float otherwise. */
    double acc;
    int all_int = 1;
    (void)env;
    if (!mino_is_cons(args)) {
        return prim_throw_classified(S, "eval/arity", "MAR001", "/ requires at least one argument");
    }
    if (args->as.cons.car == NULL
        || (args->as.cons.car->type != MINO_INT
            && args->as.cons.car->type != MINO_FLOAT)) {
        return prim_throw_classified(S, "eval/type", "MTY001", "/ expects numbers");
    }
    if (args->as.cons.car->type == MINO_FLOAT) all_int = 0;
    if (!as_double(args->as.cons.car, &acc)) {
        return prim_throw_classified(S, "eval/type", "MTY001", "/ expects numbers");
    }
    args = args->as.cons.cdr;
    if (!mino_is_cons(args)) {
        if (acc == 0.0) {
            if (all_int)
                return prim_throw_classified(S, "eval/type", "MTY001", "division by zero");
            return mino_float(S, 1.0 / acc);
        }
        return mino_float(S, 1.0 / acc);
    }
    while (mino_is_cons(args)) {
        double x;
        if (args->as.cons.car == NULL
            || (args->as.cons.car->type != MINO_INT
                && args->as.cons.car->type != MINO_FLOAT)) {
            return prim_throw_classified(S, "eval/type", "MTY001", "/ expects numbers");
        }
        if (args->as.cons.car->type == MINO_FLOAT) all_int = 0;
        if (!as_double(args->as.cons.car, &x)) {
            return prim_throw_classified(S, "eval/type", "MTY001", "/ expects numbers");
        }
        if (x == 0.0 && all_int) {
            return prim_throw_classified(S, "eval/type", "MTY001", "division by zero");
        }
        acc /= x;
        args = args->as.cons.cdr;
    }
    /* Return integer when all operands were ints and result is exact. */
    if (all_int && acc == (double)(long long)acc) {
        return mino_int(S, (long long)acc);
    }
    return mino_float(S, acc);
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

#define MATH_UNARY(cname, cfn, label)                                  \
    mino_val_t *cname(mino_state_t *S, mino_val_t *args, mino_env_t *env) \
    {                                                                   \
        double x;                                                       \
        (void)env;                                                      \
        if (!mino_is_cons(args) ||                                      \
            mino_is_cons(args->as.cons.cdr)) {                          \
            return prim_throw_classified(S, "eval/arity", "MAR001", label " requires one argument");  \
        }                                                               \
        if (!as_double(args->as.cons.car, &x)) {                       \
            return prim_throw_classified(S, "eval/type", "MTY001", label " expects a number");     \
        }                                                               \
        return mino_float(S, cfn(x));                                      \
    }

MATH_UNARY(prim_math_floor, floor, "math-floor")
MATH_UNARY(prim_math_ceil,  ceil,  "math-ceil")
MATH_UNARY(prim_math_round, round, "math-round")
MATH_UNARY(prim_math_sqrt,  sqrt,  "math-sqrt")
MATH_UNARY(prim_math_log,   log,   "math-log")
MATH_UNARY(prim_math_exp,   exp,   "math-exp")
MATH_UNARY(prim_math_sin,   sin,   "math-sin")
MATH_UNARY(prim_math_cos,   cos,   "math-cos")
MATH_UNARY(prim_math_tan,   tan,   "math-tan")

#undef MATH_UNARY

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
static mino_val_t *compare_chain(mino_state_t *S, mino_val_t *args, const char *name, int op)
{
    double prev;
    if (!mino_is_cons(args)) {
        return mino_true(S);
    }
    if (!mino_is_cons(args->as.cons.cdr)) {
        return mino_true(S);
    }
    if (!as_double(args->as.cons.car, &prev)) {
        char msg[64];
        snprintf(msg, sizeof(msg), "%s expects numbers", name);
        return prim_throw_classified(S, "eval/type", "MTY001", msg);
    }
    args = args->as.cons.cdr;
    while (mino_is_cons(args)) {
        double x;
        int    ok;
        if (!as_double(args->as.cons.car, &x)) {
            char msg[64];
            snprintf(msg, sizeof(msg), "%s expects numbers", name);
            return prim_throw_classified(S, "eval/type", "MTY001", msg);
        }
        switch (op) {
        case 0:  ok = prev <  x; break;
        case 1:  ok = prev <= x; break;
        case 2:  ok = prev >  x; break;
        default: ok = prev >= x; break;
        }
        if (!ok) {
            return mino_false(S);
        }
        prev = x;
        args = args->as.cons.cdr;
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
