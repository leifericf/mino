/*
 * numeric.c -- numeric, math, bitwise, coercion, and comparison primitives.
 */

#include "prim/internal.h"
#include "imath.h"

#include <math.h>
#include <string.h>
#include <stdio.h>
#include <errno.h>
#include <limits.h>
#include <float.h>

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

/* Forward decl: defined later alongside prim_compare. */
static int is_compare_number(const mino_val_t *v);

double tower_to_double(const mino_val_t *v)
{
    if (v == NULL) return 0.0;
    switch (mino_type_of(v)) {
    case MINO_INT:     return (double)mino_val_int_get(v);
    case MINO_FLOAT:   return v->as.f;
    case MINO_FLOAT32: return v->as.f;
    case MINO_BIGINT:  return mino_bigint_to_double(v);
    case MINO_RATIO:   return mino_ratio_to_double(v);
    case MINO_BIGDEC:  return mino_bigdec_to_double(v);
    default:           return 0.0;
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
    switch (mino_type_of(v)) {
    case MINO_INT:     *out_tier = TT_INT;    return 1;
    case MINO_BIGINT:  *out_tier = TT_BIGINT; return 1;
    case MINO_RATIO:   *out_tier = TT_RATIO;  return 1;
    case MINO_BIGDEC:  *out_tier = TT_BIGDEC; return 1;
    case MINO_FLOAT:   *out_tier = TT_FLOAT;  return 1;
    case MINO_FLOAT32: *out_tier = TT_FLOAT;  return 1;
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
        case OP_DIV: return mino_bigdec_div(S, a, b);
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
            if (new_tier == TT_BIGDEC) {
                /* Ratio meets bigdec: widen to bigdec via exact
                 * num/denom division (matches JVM Clojure contagion:
                 * bigdec is "higher" than ratio). */
                mino_val_t *num_bd = mino_bigdec_make(S,
                    acc->vacc->as.ratio.num, 0);
                mino_val_t *den_bd;
                if (num_bd == NULL) return 0;
                den_bd = mino_bigdec_make(S,
                    acc->vacc->as.ratio.denom, 0);
                if (den_bd == NULL) return 0;
                acc->vacc = mino_bigdec_div(S, num_bd, den_bd);
                if (acc->vacc == NULL) return 0;
                acc->tier = TT_BIGDEC;
                continue;
            }
            if (new_tier == TT_FLOAT) {
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
        if (mino_val_int_p(v))    return mino_bigint_from_ll(S, mino_val_int_get(v));
        if (mino_type_of(v) == MINO_BIGINT) return v;
        break;
    case TT_RATIO: {
        mino_val_t *bn, *bd;
        if (mino_type_of(v) == MINO_RATIO)  return v;
        if (mino_val_int_p(v)) {
            bn = mino_bigint_from_ll(S, mino_val_int_get(v));
            if (bn == NULL) return NULL;
            bd = mino_bigint_from_ll(S, 1);
            if (bd == NULL) return NULL;
            return mino_ratio_make_unchecked(S, bn, bd);
        }
        if (mino_type_of(v) == MINO_BIGINT) {
            bd = mino_bigint_from_ll(S, 1);
            if (bd == NULL) return NULL;
            return mino_ratio_make_unchecked(S, (mino_val_t *)v, bd);
        }
        break;
    }
    case TT_BIGDEC: {
        if (mino_type_of(v) == MINO_BIGDEC) return v;
        if (mino_val_int_p(v)) {
            mino_val_t *u = mino_bigint_from_ll(S, mino_val_int_get(v));
            if (u == NULL) return NULL;
            return mino_bigdec_make(S, u, 0);
        }
        if (mino_type_of(v) == MINO_BIGINT) {
            return mino_bigdec_make(S, (mino_val_t *)v, 0);
        }
        if (mino_type_of(v) == MINO_RATIO) {
            /* Widen the ratio to bigdec via exact division. */
            mino_val_t *num_bd, *den_bd;
            num_bd = mino_bigdec_make(S, v->as.ratio.num, 0);
            if (num_bd == NULL) return NULL;
            den_bd = mino_bigdec_make(S, v->as.ratio.denom, 0);
            if (den_bd == NULL) return NULL;
            return mino_bigdec_div(S, num_bd, den_bd);
        }
        break;
    }
    }
    /* Should be unreachable in well-classified args. */
    return prim_throw_classified(S, "internal", "MIN001",
                                 "coerce_at_tier: unsupported tier transition");
}

/* tower_seed_div -- (/ x ...) seeds the accumulator with the first
 * operand directly so subsequent operands divide into it. */
static void tower_seed_div(tower_acc_t *acc, const mino_val_t *a, int at)
{
    switch (at) {
    case TT_INT:    acc->iacc = mino_val_int_get(a); break;
    case TT_FLOAT:  acc->dacc = a->as.f; acc->tier = TT_FLOAT; break;
    case TT_BIGINT: acc->vacc = (mino_val_t *)a; acc->tier = TT_BIGINT; break;
    case TT_RATIO:  acc->vacc = (mino_val_t *)a; acc->tier = TT_RATIO; break;
    case TT_BIGDEC: acc->vacc = (mino_val_t *)a; acc->tier = TT_BIGDEC; break;
    }
}

/* tower_apply_int -- INT-tier step. add/sub/mul behavior depends on
 * `strict`: when strict, long-overflow throws (matching JVM Clojure's
 * unprimed arithmetic contracts); when not, the acc auto-promotes
 * to bigint (matching Clojure's primed forms). div promotes to ratio
 * when the quotient is not exact. Returns 0 on success, -1 on a
 * runtime error (diag set or thrown). */
static int tower_apply_int(mino_state_t *S, tower_acc_t *acc,
                           const mino_val_t *a, tower_op_t op,
                           const char *opname, int strict)
{
    long long x = mino_val_int_get(a);
    int (*overflow_op)(long long, long long, long long *) =
        (op == OP_ADD) ? iadd_overflow
        : (op == OP_SUB) ? isub_overflow
        : (op == OP_MUL) ? imul_overflow : NULL;
    if (op == OP_ADD || op == OP_SUB || op == OP_MUL) {
        long long out;
        if (overflow_op(acc->iacc, x, &out)) {
            if (strict) {
                char buf[64];
                snprintf(buf, sizeof(buf), "integer overflow");
                prim_throw_classified(S, "eval/contract", "MCT001", buf);
                return -1;
            }
            {
                mino_val_t *la = mino_bigint_from_ll(S, acc->iacc);
                mino_val_t *lb = mino_bigint_from_ll(S, x);
                if (la == NULL || lb == NULL) return -1;
                acc->vacc = tower_op_at_tier(S, op, TT_BIGINT, la, lb, opname);
                if (acc->vacc == NULL) return -1;
                acc->tier = TT_BIGINT;
                return 0;
            }
        }
        acc->iacc = out;
        return 0;
    }
    /* OP_DIV. */
    if (x == 0) {
        prim_throw_classified(S, "eval/type", "MTY001",
                              "division by zero");
        return -1;
    }
    /* For exact int/int division keep an int; otherwise promote to ratio.
     * (Clojure's `/` returns a Ratio for non-exact int/int.) */
    if (acc->iacc % x == 0) {
        acc->iacc /= x;
        return 0;
    }
    {
        mino_val_t *bn = mino_bigint_from_ll(S, acc->iacc);
        mino_val_t *bd = mino_bigint_from_ll(S, x);
        if (bn == NULL || bd == NULL) return -1;
        acc->vacc = mino_ratio_make(S, bn, bd);
        if (acc->vacc == NULL) return -1;
        acc->tier = TT_RATIO;
    }
    return 0;
}

/* tower_apply_bigint -- BIGINT-tier step. div promotes to ratio (and
 * may collapse back to int / bigint inside mino_ratio_div). */
static int tower_apply_bigint(mino_state_t *S, tower_acc_t *acc,
                              const mino_val_t *a, tower_op_t op,
                              const char *opname)
{
    mino_val_t *operand = coerce_at_tier(S, (mino_val_t *)a, TT_BIGINT, opname);
    if (operand == NULL) return -1;
    if (op == OP_DIV) {
        acc->vacc = mino_ratio_div(S, acc->vacc, operand);
        if (acc->vacc == NULL) return -1;
        if (mino_val_int_p(acc->vacc)) {
            acc->iacc = mino_val_int_get(acc->vacc);
            acc->vacc = NULL;
            acc->tier = TT_INT;
        } else if (mino_type_of(acc->vacc) == MINO_BIGINT) {
            acc->tier = TT_BIGINT;
        } else {
            acc->tier = TT_RATIO;
        }
        return 0;
    }
    acc->vacc = tower_op_at_tier(S, op, TT_BIGINT, acc->vacc, operand, opname);
    return acc->vacc == NULL ? -1 : 0;
}

/* tower_apply_ratio -- RATIO-tier step. Result may have collapsed back
 * to int or bigint. */
static int tower_apply_ratio(mino_state_t *S, tower_acc_t *acc,
                             const mino_val_t *a, tower_op_t op,
                             const char *opname)
{
    mino_val_t *operand = coerce_at_tier(S, (mino_val_t *)a, TT_RATIO, opname);
    if (operand == NULL) return -1;
    acc->vacc = tower_op_at_tier(S, op, TT_RATIO, acc->vacc, operand, opname);
    if (acc->vacc == NULL) return -1;
    if (mino_val_int_p(acc->vacc)) {
        acc->iacc = mino_val_int_get(acc->vacc); acc->vacc = NULL; acc->tier = TT_INT;
    } else if (mino_type_of(acc->vacc) == MINO_BIGINT) {
        acc->tier = TT_BIGINT;
    }
    return 0;
}

/* tower_apply_bigdec -- BIGDEC-tier step. */
static int tower_apply_bigdec(mino_state_t *S, tower_acc_t *acc,
                              const mino_val_t *a, tower_op_t op,
                              const char *opname)
{
    mino_val_t *operand = coerce_at_tier(S, (mino_val_t *)a, TT_BIGDEC, opname);
    if (operand == NULL) return -1;
    acc->vacc = tower_op_at_tier(S, op, TT_BIGDEC, acc->vacc, operand, opname);
    return acc->vacc == NULL ? -1 : 0;
}

/* tower_apply_float -- FLOAT-tier step. The four ops are inlined since
 * they correspond directly to a single C double op; no error path. */
static int tower_apply_float(tower_acc_t *acc, const mino_val_t *a,
                             tower_op_t op)
{
    double x = tower_to_double((mino_val_t *)a);
    switch (op) {
    case OP_ADD: acc->dacc += x; break;
    case OP_SUB: acc->dacc -= x; break;
    case OP_MUL: acc->dacc *= x; break;
    case OP_DIV: acc->dacc /= x; break;
    }
    return 0;
}

/* Promote acc and dispatch one operand. Returns 0 on success, -1 on
 * error (diag set or thrown). Shared between cons-spine and argv
 * tower_reduce / tower_reduce_seeded entry points. */
static int tower_advance(mino_state_t *S, tower_acc_t *acc, mino_val_t *a,
                         tower_op_t op, const char *opname, int strict)
{
    int at;
    int step;
    if (!classify_or_throw(S, a, opname, &at)) return -1;
    /* Promote the running accumulator if the new operand is at a higher
     * tier. Ratio meeting bigdec widens to bigdec via exact ratio→bigdec
     * conversion (matches JVM Clojure's contagion: bigdec is "higher"
     * than ratio). */
    if (at > (int)acc->tier
        || (acc->tier == TT_RATIO && at == TT_BIGDEC)
        || (acc->tier == TT_BIGDEC && at == TT_RATIO)) {
        int target = at;
        if (acc->tier == TT_RATIO && at == TT_BIGDEC) target = TT_BIGDEC;
        if (acc->tier == TT_BIGDEC && at == TT_RATIO) target = TT_BIGDEC;
        if (!promote_acc(S, acc, target, opname)) return -1;
    }
    switch (acc->tier) {
    case TT_INT:    step = tower_apply_int(S, acc, a, op, opname, strict); break;
    case TT_BIGINT: step = tower_apply_bigint(S, acc, a, op, opname); break;
    case TT_RATIO:  step = tower_apply_ratio(S, acc, a, op, opname); break;
    case TT_BIGDEC: step = tower_apply_bigdec(S, acc, a, op, opname); break;
    case TT_FLOAT:  step = tower_apply_float(acc, a, op); break;
    default:        step = -1; break;
    }
    return step;
}

/* Pack the final accumulator into a result val. */
static mino_val_t *tower_finish(mino_state_t *S, const tower_acc_t *acc)
{
    switch (acc->tier) {
    case TT_INT:    return mino_int(S, acc->iacc);
    case TT_FLOAT:  return mino_float(S, acc->dacc);
    default:        return acc->vacc;
    }
}

static void tower_acc_init(tower_acc_t *acc, tower_op_t op)
{
    acc->tier = TT_INT;
    acc->iacc = (op == OP_MUL) ? 1 : 0;
    acc->vacc = NULL;
    acc->dacc = 0.0;
}

/* Driver. Walks args, classifies each, promotes accumulator on tier
 * increase, and dispatches to the per-tier step. Returns the final
 * value, or NULL on error with diag already set. */
static mino_val_t *tower_reduce(mino_state_t *S, mino_val_t *args,
                                tower_op_t op, const char *opname,
                                int strict)
{
    tower_acc_t acc;
    int         seeded = 0;
    tower_acc_init(&acc, op);
    while (mino_is_cons(args)) {
        mino_val_t *a = args->as.cons.car;
        if (op == OP_DIV && !seeded) {
            int at;
            if (!classify_or_throw(S, a, opname, &at)) return NULL;
            tower_seed_div(&acc, a, at);
            seeded = 1;
            args = args->as.cons.cdr;
            continue;
        }
        if (tower_advance(S, &acc, a, op, opname, strict) != 0) return NULL;
        seeded = 1;
        args = args->as.cons.cdr;
    }
    return tower_finish(S, &acc);
}

/* argv-ABI sibling of tower_reduce. Shape-equivalent; no cons walk. */
static mino_val_t *tower_reduce_argv(mino_state_t *S, mino_val_t **argv,
                                     int argc, tower_op_t op,
                                     const char *opname, int strict)
{
    tower_acc_t acc;
    int         i = 0;
    tower_acc_init(&acc, op);
    if (op == OP_DIV && argc > 0) {
        int at;
        if (!classify_or_throw(S, argv[0], opname, &at)) return NULL;
        tower_seed_div(&acc, argv[0], at);
        i = 1;
    }
    for (; i < argc; i++) {
        if (tower_advance(S, &acc, argv[i], op, opname, strict) != 0)
            return NULL;
    }
    return tower_finish(S, &acc);
}

mino_val_t *prim_add(mino_state_t *S, mino_val_t *args, mino_env_t *env)
{
    (void)env;
    return tower_reduce(S, args, OP_ADD, "+", 1);
}

mino_val_t *prim_addp(mino_state_t *S, mino_val_t *args, mino_env_t *env)
{
    (void)env;
    return tower_reduce(S, args, OP_ADD, "+'", 0);
}

/* Forward decl: prim_subp is defined alongside prim_sub later, but
 * prim_dec_impl needs to call it from this location. */
mino_val_t *prim_subp(mino_state_t *S, mino_val_t *args, mino_env_t *env);

/* (inc x) -- x + 1. Fast path for the dominant integer case; behavior
 * on long overflow at LLONG_MAX depends on `strict`: throw (default
 * inc, matches JVM Clojure) or auto-promote to bigint (inc'). Non-int
 * operands defer to the generic + primitive so tier semantics stay
 * identical. */
static mino_val_t *prim_inc_step(mino_state_t *S, mino_val_t *x,
                                 mino_env_t *env, int strict)
{
    if (x != NULL && mino_val_int_p(x)) {
        if (mino_val_int_get(x) == LLONG_MAX) {
            if (strict) {
                return prim_throw_classified(S, "eval/contract", "MCT001",
                                             "integer overflow");
            }
            {
                mino_val_t *lhs = mino_bigint_from_ll(S, mino_val_int_get(x));
                mino_val_t *one;
                if (lhs == NULL) return NULL;
                one = mino_int(S, 1);
                return mino_bigint_add(S, lhs, one);
            }
        }
        return mino_int(S, mino_val_int_get(x) + 1);
    }
    if (x != NULL && (mino_type_of(x) == MINO_FLOAT || mino_type_of(x) == MINO_FLOAT32)) {
        /* JVM Clojure's `(inc (float 1))` returns a Double; arithmetic
         * with floats always promotes to double. Match that. */
        return mino_float(S, x->as.f + 1.0);
    }
    if (x != NULL && (mino_type_of(x) == MINO_BIGINT || mino_type_of(x) == MINO_RATIO
                      || mino_type_of(x) == MINO_BIGDEC)) {
        /* Defer to the tower-aware (+) by passing (x 1). The bigint /
         * ratio / bigdec tiers don't overflow long, so the strict flag
         * doesn't change behavior here -- both inc and inc' route to
         * the strict (+) path safely. */
        mino_val_t *one = mino_int(S, 1);
        mino_val_t *pair = mino_cons(S, x, mino_cons(S, one, mino_nil(S)));
        return strict ? prim_add(S, pair, env) : prim_addp(S, pair, env);
    }
    return prim_throw_classified(S, "eval/type", "MTY001",
        "inc expects a number");
}

static mino_val_t *prim_inc_impl(mino_state_t *S, mino_val_t *args,
                                 mino_env_t *env, int strict)
{
    if (!mino_is_cons(args) || mino_is_cons(args->as.cons.cdr)) {
        return prim_throw_classified(S, "eval/arity", "MAR001",
            "inc requires exactly 1 argument");
    }
    return prim_inc_step(S, args->as.cons.car, env, strict);
}

mino_val_t *prim_inc_argv(mino_state_t *S, mino_val_t **argv, int argc,
                          mino_env_t *env)
{
    if (argc != 1) {
        return prim_throw_classified(S, "eval/arity", "MAR001",
            "inc requires exactly 1 argument");
    }
    return prim_inc_step(S, argv[0], env, 1);
}

mino_val_t *prim_incp_argv(mino_state_t *S, mino_val_t **argv, int argc,
                           mino_env_t *env)
{
    if (argc != 1) {
        return prim_throw_classified(S, "eval/arity", "MAR001",
            "inc' requires exactly 1 argument");
    }
    return prim_inc_step(S, argv[0], env, 0);
}

mino_val_t *prim_inc(mino_state_t *S, mino_val_t *args, mino_env_t *env)
{
    return prim_inc_impl(S, args, env, 1);
}

mino_val_t *prim_incp(mino_state_t *S, mino_val_t *args, mino_env_t *env)
{
    return prim_inc_impl(S, args, env, 0);
}

static mino_val_t *prim_dec_step(mino_state_t *S, mino_val_t *x,
                                 mino_env_t *env, int strict)
{
    if (x != NULL && mino_val_int_p(x)) {
        if (mino_val_int_get(x) == LLONG_MIN) {
            if (strict) {
                return prim_throw_classified(S, "eval/contract", "MCT001",
                                             "integer overflow");
            }
            {
                mino_val_t *lhs = mino_bigint_from_ll(S, mino_val_int_get(x));
                mino_val_t *one;
                if (lhs == NULL) return NULL;
                one = mino_int(S, 1);
                return mino_bigint_sub(S, lhs, one);
            }
        }
        return mino_int(S, mino_val_int_get(x) - 1);
    }
    if (x != NULL && (mino_type_of(x) == MINO_FLOAT || mino_type_of(x) == MINO_FLOAT32)) {
        return mino_float(S, x->as.f - 1.0);
    }
    if (x != NULL && (mino_type_of(x) == MINO_BIGINT || mino_type_of(x) == MINO_RATIO
                      || mino_type_of(x) == MINO_BIGDEC)) {
        mino_val_t *one = mino_int(S, 1);
        mino_val_t *pair = mino_cons(S, x, mino_cons(S, one, mino_nil(S)));
        return strict ? prim_sub(S, pair, env) : prim_subp(S, pair, env);
    }
    return prim_throw_classified(S, "eval/type", "MTY001",
        "dec expects a number");
}

static mino_val_t *prim_dec_impl(mino_state_t *S, mino_val_t *args,
                                 mino_env_t *env, int strict)
{
    if (!mino_is_cons(args) || mino_is_cons(args->as.cons.cdr)) {
        return prim_throw_classified(S, "eval/arity", "MAR001",
            "dec requires exactly 1 argument");
    }
    return prim_dec_step(S, args->as.cons.car, env, strict);
}

mino_val_t *prim_dec_argv(mino_state_t *S, mino_val_t **argv, int argc,
                          mino_env_t *env)
{
    if (argc != 1) {
        return prim_throw_classified(S, "eval/arity", "MAR001",
            "dec requires exactly 1 argument");
    }
    return prim_dec_step(S, argv[0], env, 1);
}

mino_val_t *prim_decp_argv(mino_state_t *S, mino_val_t **argv, int argc,
                           mino_env_t *env)
{
    if (argc != 1) {
        return prim_throw_classified(S, "eval/arity", "MAR001",
            "dec' requires exactly 1 argument");
    }
    return prim_dec_step(S, argv[0], env, 0);
}

mino_val_t *prim_dec(mino_state_t *S, mino_val_t *args, mino_env_t *env)
{
    return prim_dec_impl(S, args, env, 1);
}

mino_val_t *prim_decp(mino_state_t *S, mino_val_t *args, mino_env_t *env)
{
    return prim_dec_impl(S, args, env, 0);
}

/* Seed the accumulator with a single classified operand. */
static int tower_seed(mino_state_t *S, tower_acc_t *a, mino_val_t *seed,
                      const char *opname)
{
    int seed_tier;
    if (!classify_or_throw(S, seed, opname, &seed_tier)) return -1;
    a->iacc = 0; a->dacc = 0; a->vacc = NULL;
    switch (seed_tier) {
    case TT_INT:    a->iacc = mino_val_int_get(seed); a->tier = TT_INT;    break;
    case TT_FLOAT:  a->dacc = seed->as.f; a->tier = TT_FLOAT;  break;
    case TT_BIGINT: a->vacc = seed;       a->tier = TT_BIGINT; break;
    case TT_RATIO:  a->vacc = seed;       a->tier = TT_RATIO;  break;
    case TT_BIGDEC: a->vacc = seed;       a->tier = TT_BIGDEC; break;
    default:        a->tier = TT_INT;     break;
    }
    return 0;
}

/* Apply one step of (- ...) or (/ ...) against a seeded acc. Returns
 * 0 on success, -1 on error. Shared by cons-spine and argv variants. */
static int tower_seeded_step(mino_state_t *S, tower_acc_t *a, mino_val_t *x,
                             tower_op_t op, const char *opname, int strict)
{
    int xt;
    if (!classify_or_throw(S, x, opname, &xt)) return -1;
    if (xt > (int)a->tier ||
        (a->tier == TT_RATIO  && xt == TT_BIGDEC) ||
        (a->tier == TT_BIGDEC && xt == TT_RATIO)) {
        int target = xt;
        if (a->tier == TT_RATIO  && xt == TT_BIGDEC) target = TT_BIGDEC;
        if (a->tier == TT_BIGDEC && xt == TT_RATIO)  target = TT_BIGDEC;
        if (!promote_acc(S, a, target, opname)) return -1;
    }
    switch (a->tier) {
        case TT_INT: {
            long long out;
            if (op == OP_SUB) {
                if (isub_overflow(a->iacc, mino_val_int_get(x), &out)) {
                    if (strict) {
                        prim_throw_classified(S, "eval/contract", "MCT001",
                                              "integer overflow");
                        return -1;
                    }
                    {
                        mino_val_t *la = mino_bigint_from_ll(S, a->iacc);
                        mino_val_t *lb = mino_bigint_from_ll(S, mino_val_int_get(x));
                        if (la == NULL || lb == NULL) return -1;
                        a->vacc = mino_bigint_sub(S, la, lb);
                        if (a->vacc == NULL) return -1;
                        a->tier = TT_BIGINT;
                        break;
                    }
                }
                a->iacc = out;
            } else { /* OP_DIV */
                if (mino_val_int_get(x) == 0) {
                    prim_throw_classified(S, "eval/type", "MTY001",
                                          "division by zero");
                    return -1;
                }
                if (a->iacc % mino_val_int_get(x) == 0) {
                    a->iacc /= mino_val_int_get(x);
                } else {
                    mino_val_t *bn = mino_bigint_from_ll(S, a->iacc);
                    mino_val_t *bd = mino_bigint_from_ll(S, mino_val_int_get(x));
                    if (bn == NULL || bd == NULL) return -1;
                    a->vacc = mino_ratio_make(S, bn, bd);
                    if (a->vacc == NULL) return -1;
                    if (mino_val_int_p(a->vacc)) {
                        a->iacc = mino_val_int_get(a->vacc); a->vacc = NULL; a->tier = TT_INT;
                    } else if (mino_type_of(a->vacc) == MINO_BIGINT) {
                        a->tier = TT_BIGINT;
                    } else {
                        a->tier = TT_RATIO;
                    }
                }
            }
            break;
        }
        case TT_BIGINT: {
            mino_val_t *opd = coerce_at_tier(S, x, TT_BIGINT, opname);
            if (opd == NULL) return -1;
            if (op == OP_SUB) {
                a->vacc = mino_bigint_sub(S, a->vacc, opd);
            } else {
                a->vacc = mino_ratio_div(S, a->vacc, opd);
                if (a->vacc != NULL) {
                    if (mino_val_int_p(a->vacc)) {
                        a->iacc = mino_val_int_get(a->vacc); a->vacc = NULL; a->tier = TT_INT;
                    } else if (mino_type_of(a->vacc) == MINO_BIGINT) {
                        a->tier = TT_BIGINT;
                    } else {
                        a->tier = TT_RATIO;
                    }
                }
            }
            if (a->vacc == NULL && a->tier != TT_INT && a->tier != TT_BIGINT) return -1;
            break;
        }
        case TT_RATIO: {
            mino_val_t *opd = coerce_at_tier(S, x, TT_RATIO, opname);
            if (opd == NULL) return -1;
            a->vacc = (op == OP_SUB)
                ? mino_ratio_sub(S, a->vacc, opd)
                : mino_ratio_div(S, a->vacc, opd);
            if (a->vacc == NULL) return -1;
            if (mino_val_int_p(a->vacc)) {
                a->iacc = mino_val_int_get(a->vacc); a->vacc = NULL; a->tier = TT_INT;
            } else if (mino_type_of(a->vacc) == MINO_BIGINT) {
                a->tier = TT_BIGINT;
            }
            break;
        }
        case TT_BIGDEC: {
            mino_val_t *opd = coerce_at_tier(S, x, TT_BIGDEC, opname);
            if (opd == NULL) return -1;
            if (op == OP_SUB) {
                a->vacc = mino_bigdec_sub(S, a->vacc, opd);
            } else {
                a->vacc = mino_bigdec_div(S, a->vacc, opd);
            }
            if (a->vacc == NULL) return -1;
            break;
        }
        case TT_FLOAT: {
            double dx = tower_to_double(x);
            if (op == OP_SUB) a->dacc -= dx;
            else {
                /* IEEE division: x/0 yields Inf or -Inf for float; only
                 * the all-integer case throws division-by-zero (handled
                 * in the TT_INT branch above). Float div mirrors the
                 * historical mino behaviour. */
                a->dacc /= dx;
            }
            break;
        }
    }
    return 0;
}

/* Driver. Walks `rest` cons spine, applying tower_seeded_step against
 * the seeded accumulator. Returns the final value, or NULL on error
 * with diag already set. */
static mino_val_t *tower_reduce_seeded(mino_state_t *S, mino_val_t *seed,
                                       mino_val_t *rest, tower_op_t op,
                                       const char *opname, int strict)
{
    tower_acc_t a;
    if (tower_seed(S, &a, seed, opname) != 0) return NULL;
    while (mino_is_cons(rest)) {
        mino_val_t *x = rest->as.cons.car;
        if (tower_seeded_step(S, &a, x, op, opname, strict) != 0) return NULL;
        rest = rest->as.cons.cdr;
    }
    return tower_finish(S, &a);
}

/* argv-ABI sibling of tower_reduce_seeded. */
static mino_val_t *tower_reduce_seeded_argv(mino_state_t *S, mino_val_t *seed,
                                            mino_val_t **argv, int argc,
                                            tower_op_t op, const char *opname,
                                            int strict)
{
    tower_acc_t a;
    int i;
    if (tower_seed(S, &a, seed, opname) != 0) return NULL;
    for (i = 0; i < argc; i++) {
        if (tower_seeded_step(S, &a, argv[i], op, opname, strict) != 0)
            return NULL;
    }
    return tower_finish(S, &a);
}

/* Unary (-) / (-'): negate. Shared by cons-spine and argv variants. */
static mino_val_t *prim_sub_negate(mino_state_t *S, mino_val_t *first,
                                   int strict, const char *opname)
{
    if (first == NULL) {
        char buf[64];
        snprintf(buf, sizeof(buf), "%s expects numbers", opname);
        return prim_throw_classified(S, "eval/type", "MTY001", buf);
    }
    if (mino_val_int_p(first)) {
        long long neg;
        if (ineg_overflow(mino_val_int_get(first), &neg)) {
            if (strict) {
                return prim_throw_classified(S, "eval/contract", "MCT001",
                                             "integer overflow");
            }
            /* Negating LLONG_MIN doesn't fit in long; promote. */
            {
                mino_val_t *bi = mino_bigint_from_ll(S, mino_val_int_get(first));
                if (bi == NULL) return NULL;
                return mino_bigint_neg(S, bi);
            }
        }
        return mino_int(S, neg);
    }
    if (mino_type_of(first) == MINO_FLOAT || mino_type_of(first) == MINO_FLOAT32)
        return mino_float(S, -first->as.f);
    if (mino_type_of(first) == MINO_BIGINT) return mino_bigint_neg(S, first);
    if (mino_type_of(first) == MINO_RATIO) {
        mino_val_t *zero_n = mino_bigint_from_ll(S, 0);
        mino_val_t *zero_d = mino_bigint_from_ll(S, 1);
        mino_val_t *zero;
        if (zero_n == NULL || zero_d == NULL) return NULL;
        zero = mino_ratio_make_unchecked(S, zero_n, zero_d);
        if (zero == NULL) return NULL;
        return mino_ratio_sub(S, zero, first);
    }
    if (mino_type_of(first) == MINO_BIGDEC) return mino_bigdec_neg(S, first);
    {
        char buf[64];
        snprintf(buf, sizeof(buf), "%s expects numbers", opname);
        return prim_throw_classified(S, "eval/type", "MTY001", buf);
    }
}

static mino_val_t *prim_sub_impl(mino_state_t *S, mino_val_t *args,
                                 mino_env_t *env, int strict, const char *opname)
{
    mino_val_t *first;
    (void)env;
    if (!mino_is_cons(args)) {
        char buf[64];
        snprintf(buf, sizeof(buf), "%s requires at least one argument", opname);
        return prim_throw_classified(S, "eval/arity", "MAR001", buf);
    }
    first = args->as.cons.car;
    if (!mino_is_cons(args->as.cons.cdr))
        return prim_sub_negate(S, first, strict, opname);
    return tower_reduce_seeded(S, first, args->as.cons.cdr, OP_SUB, opname, strict);
}

mino_val_t *prim_sub(mino_state_t *S, mino_val_t *args, mino_env_t *env)
{
    return prim_sub_impl(S, args, env, 1, "-");
}

mino_val_t *prim_subp(mino_state_t *S, mino_val_t *args, mino_env_t *env)
{
    return prim_sub_impl(S, args, env, 0, "-'");
}

mino_val_t *prim_mul(mino_state_t *S, mino_val_t *args, mino_env_t *env)
{
    (void)env;
    return tower_reduce(S, args, OP_MUL, "*", 1);
}

mino_val_t *prim_mulp(mino_state_t *S, mino_val_t *args, mino_env_t *env)
{
    (void)env;
    return tower_reduce(S, args, OP_MUL, "*'", 0);
}

mino_val_t *prim_add_argv(mino_state_t *S, mino_val_t **argv, int argc,
                          mino_env_t *env)
{
    (void)env;
    return tower_reduce_argv(S, argv, argc, OP_ADD, "+", 1);
}

mino_val_t *prim_addp_argv(mino_state_t *S, mino_val_t **argv, int argc,
                           mino_env_t *env)
{
    (void)env;
    return tower_reduce_argv(S, argv, argc, OP_ADD, "+'", 0);
}

mino_val_t *prim_mul_argv(mino_state_t *S, mino_val_t **argv, int argc,
                          mino_env_t *env)
{
    (void)env;
    return tower_reduce_argv(S, argv, argc, OP_MUL, "*", 1);
}

mino_val_t *prim_mulp_argv(mino_state_t *S, mino_val_t **argv, int argc,
                           mino_env_t *env)
{
    (void)env;
    return tower_reduce_argv(S, argv, argc, OP_MUL, "*'", 0);
}

static mino_val_t *prim_sub_argv_impl(mino_state_t *S, mino_val_t **argv,
                                      int argc, int strict, const char *opname)
{
    if (argc == 0) {
        char buf[64];
        snprintf(buf, sizeof(buf), "%s requires at least one argument", opname);
        return prim_throw_classified(S, "eval/arity", "MAR001", buf);
    }
    if (argc == 1) return prim_sub_negate(S, argv[0], strict, opname);
    return tower_reduce_seeded_argv(S, argv[0], argv + 1, argc - 1,
                                    OP_SUB, opname, strict);
}

mino_val_t *prim_sub_argv(mino_state_t *S, mino_val_t **argv, int argc,
                          mino_env_t *env)
{
    (void)env;
    return prim_sub_argv_impl(S, argv, argc, 1, "-");
}

mino_val_t *prim_subp_argv(mino_state_t *S, mino_val_t **argv, int argc,
                           mino_env_t *env)
{
    (void)env;
    return prim_sub_argv_impl(S, argv, argc, 0, "-'");
}

/* ------------------------------------------------------------------------- */
/* unchecked-* family - fast int64 with two's-complement wraparound.         */
/*                                                                           */
/* These ops opt out of the tower / overflow path entirely. Operands must   */
/* be ints; non-int operands throw eval/type. Wraparound is computed via    */
/* unsigned arithmetic (well-defined in C) and the result is cast back to   */
/* long long. Names match Clojure's surface: unchecked-add /                 */
/* unchecked-subtract / unchecked-multiply are binary, unchecked-inc /       */
/* unchecked-dec / unchecked-negate are unary.                              */
/* ------------------------------------------------------------------------- */

static int unchecked_grab_long(mino_val_t *v, long long *out)
{
    if (v == NULL || !mino_val_int_p(v)) return 0;
    *out = mino_val_int_get(v);
    return 1;
}

static long long uwrap_add(long long a, long long b)
{
    return (long long)((unsigned long long)a + (unsigned long long)b);
}

static long long uwrap_sub(long long a, long long b)
{
    return (long long)((unsigned long long)a - (unsigned long long)b);
}

static long long uwrap_mul(long long a, long long b)
{
    return (long long)((unsigned long long)a * (unsigned long long)b);
}

static int unchecked_two_int(mino_state_t *S, mino_val_t *args,
                             const char *opname,
                             long long *a, long long *b)
{
    char msg[80];
    mino_val_t *xa;
    mino_val_t *xb;
    if (!mino_is_cons(args) || !mino_is_cons(args->as.cons.cdr) ||
        mino_is_cons(args->as.cons.cdr->as.cons.cdr)) {
        snprintf(msg, sizeof(msg), "%s requires exactly 2 arguments", opname);
        prim_throw_classified(S, "eval/arity", "MAR001", msg);
        return 0;
    }
    xa = args->as.cons.car;
    xb = args->as.cons.cdr->as.cons.car;
    if (!unchecked_grab_long(xa, a) || !unchecked_grab_long(xb, b)) {
        snprintf(msg, sizeof(msg), "%s expects ints", opname);
        prim_throw_classified(S, "eval/type", "MTY001", msg);
        return 0;
    }
    return 1;
}

mino_val_t *prim_unchecked_add(mino_state_t *S, mino_val_t *args, mino_env_t *env)
{
    long long a, b;
    (void)env;
    if (!unchecked_two_int(S, args, "unchecked-add", &a, &b)) return NULL;
    return mino_int(S, uwrap_add(a, b));
}

mino_val_t *prim_unchecked_sub(mino_state_t *S, mino_val_t *args, mino_env_t *env)
{
    long long a, b;
    (void)env;
    if (!unchecked_two_int(S, args, "unchecked-subtract", &a, &b)) return NULL;
    return mino_int(S, uwrap_sub(a, b));
}

mino_val_t *prim_unchecked_mul(mino_state_t *S, mino_val_t *args, mino_env_t *env)
{
    long long a, b;
    (void)env;
    if (!unchecked_two_int(S, args, "unchecked-multiply", &a, &b)) return NULL;
    return mino_int(S, uwrap_mul(a, b));
}

mino_val_t *prim_unchecked_inc(mino_state_t *S, mino_val_t *args, mino_env_t *env)
{
    long long x;
    (void)env;
    if (!mino_is_cons(args) || mino_is_cons(args->as.cons.cdr))
        return prim_throw_classified(S, "eval/arity", "MAR001",
            "unchecked-inc requires exactly 1 argument");
    if (!unchecked_grab_long(args->as.cons.car, &x))
        return prim_throw_classified(S, "eval/type", "MTY001",
            "unchecked-inc expects an int");
    return mino_int(S, uwrap_add(x, 1));
}

mino_val_t *prim_unchecked_dec(mino_state_t *S, mino_val_t *args, mino_env_t *env)
{
    long long x;
    (void)env;
    if (!mino_is_cons(args) || mino_is_cons(args->as.cons.cdr))
        return prim_throw_classified(S, "eval/arity", "MAR001",
            "unchecked-dec requires exactly 1 argument");
    if (!unchecked_grab_long(args->as.cons.car, &x))
        return prim_throw_classified(S, "eval/type", "MTY001",
            "unchecked-dec expects an int");
    return mino_int(S, uwrap_sub(x, 1));
}

mino_val_t *prim_unchecked_negate(mino_state_t *S, mino_val_t *args, mino_env_t *env)
{
    long long x;
    (void)env;
    if (!mino_is_cons(args) || mino_is_cons(args->as.cons.cdr))
        return prim_throw_classified(S, "eval/arity", "MAR001",
            "unchecked-negate requires exactly 1 argument");
    if (!unchecked_grab_long(args->as.cons.car, &x))
        return prim_throw_classified(S, "eval/type", "MTY001",
            "unchecked-negate expects an int");
    return mino_int(S, uwrap_sub(0, x));
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
        return tower_reduce_seeded(S, one, args, OP_DIV, "/", 0);
    }
    return tower_reduce_seeded(S, first, args->as.cons.cdr, OP_DIV, "/", 0);
}

mino_val_t *prim_div_argv(mino_state_t *S, mino_val_t **argv, int argc,
                          mino_env_t *env)
{
    (void)env;
    if (argc == 0) {
        return prim_throw_classified(S, "eval/arity", "MAR001",
                                     "/ requires at least one argument");
    }
    if (argc == 1) {
        /* (/ x) → 1 / x. */
        mino_val_t *one = mino_int(S, 1);
        if (one == NULL) return NULL;
        return tower_reduce_seeded_argv(S, one, argv, 1, OP_DIV, "/", 0);
    }
    return tower_reduce_seeded_argv(S, argv[0], argv + 1, argc - 1,
                                    OP_DIV, "/", 0);
}

/* mod/rem/quot dispatcher.
 *
 * The three operations share a common structure: classify both operands
 * and dispatch on the higher tier. Each tier preserves the result type
 * (Clojure contagion):
 *   FLOAT  -- fmod-based double computation
 *   BIGDEC -- align scales, integer-divide unscaled bigints, wrap back
 *   RATIO  -- cross-multiply num/denom into bigints, integer-divide
 *   BIGINT -- mp_int_div via mino_bigint_quot/rem/mod
 *   INT    -- C / and %, with LLONG_MIN / -1 overflow promoted to bigint
 *
 * RATIO meeting BIGDEC collapses to FLOAT (mirrors `+` / `-` etc.).
 */
typedef enum { MQR_QUOT, MQR_REM, MQR_MOD } mqr_op_t;

static const char *mqr_name(mqr_op_t op) {
    return op == MQR_QUOT ? "quot" : op == MQR_REM ? "rem" : "mod";
}

static mino_val_t *mqr_float(mino_state_t *S, double a, double b, mqr_op_t op,
                             const char *opname)
{
    double r;
    if (isnan(a) || isinf(a)) {
        char buf[64];
        snprintf(buf, sizeof(buf), "%s: NaN or Infinite dividend", opname);
        return prim_throw_classified(S, "eval/type", "MTY001", buf);
    }
    if (isnan(b)) {
        char buf[64];
        snprintf(buf, sizeof(buf), "%s: NaN divisor", opname);
        return prim_throw_classified(S, "eval/type", "MTY001", buf);
    }
    if (b == 0.0) {
        char buf[64];
        snprintf(buf, sizeof(buf), "%s: division by zero", opname);
        return prim_throw_classified(S, "eval/type", "MTY001", buf);
    }
    if (op == MQR_QUOT) {
        if (isinf(b)) return mino_float(S, 0.0);
        r = a / b;
        return mino_float(S, r >= 0 ? floor(r) : ceil(r));
    }
    if (isinf(b)) return mino_float(S, NAN);
    r = fmod(a, b);
    if (op == MQR_MOD && r != 0.0 && ((r < 0.0) != (b < 0.0))) r += b;
    return mino_float(S, r);
}

/* Coerce a ratio operand into an effective (num, denom) pair of bigints.
 * For non-ratio inputs (int / bigint) returns (operand-as-bigint, 1). */
static int as_ratio_pair_bigints(mino_state_t *S, const mino_val_t *v,
                                 mino_val_t **num_out, mino_val_t **denom_out)
{
    if (mino_type_of(v) == MINO_RATIO) {
        *num_out   = v->as.ratio.num;
        *denom_out = v->as.ratio.denom;
        return 1;
    }
    if (mino_val_int_p(v)) {
        *num_out = mino_bigint_from_ll(S, mino_val_int_get(v));
        if (*num_out == NULL) return 0;
        *denom_out = mino_bigint_from_ll(S, 1);
        return *denom_out != NULL;
    }
    if (mino_type_of(v) == MINO_BIGINT) {
        *num_out = (mino_val_t *)v;
        *denom_out = mino_bigint_from_ll(S, 1);
        return *denom_out != NULL;
    }
    prim_throw_classified(S, "internal", "MIN001",
                          "as_ratio_pair_bigints: unexpected type");
    return 0;
}

/* Promote MINO_INT to MINO_BIGINT. Used in the ratio path to keep
 * integer-valued results in the bigint tier (Clojure's BigInt contagion
 * rule -- once arithmetic enters the bignum tier the result stays
 * there, even when the value happens to fit a long). */
static mino_val_t *bigint_or_self(mino_state_t *S, mino_val_t *v)
{
    if (v == NULL) return NULL;
    if (mino_val_int_p(v)) return mino_bigint_from_ll(S, mino_val_int_get(v));
    return v;
}

/* Ratio path. Produce bigint quot, ratio rem/mod (which may collapse). */
static mino_val_t *mqr_ratio_inner(mino_state_t *S, const mino_val_t *a,
                                   const mino_val_t *b, mqr_op_t op);

static mino_val_t *mqr_ratio(mino_state_t *S, const mino_val_t *a,
                             const mino_val_t *b, mqr_op_t op)
{
    mino_val_t *r = mqr_ratio_inner(S, a, b, op);
    if (r == NULL) return NULL;
    /* Once arithmetic crosses through the ratio tier its integer
     * results live in the bigint tier (matching Clojure's BigInt
     * contagion). Don't downgrade to MINO_INT on the way out. */
    return bigint_or_self(S, r);
}

static mino_val_t *mqr_ratio_inner(mino_state_t *S, const mino_val_t *a,
                                   const mino_val_t *b, mqr_op_t op)
{
    mino_val_t *na, *da, *nb, *db, *cross_num, *cross_den, *q;
    if (!as_ratio_pair_bigints(S, a, &na, &da)) return NULL;
    if (!as_ratio_pair_bigints(S, b, &nb, &db)) return NULL;
    /* a / b = (na * db) / (da * nb). The integer quotient is
     * trunc((na * db) / (da * nb)). */
    cross_num = mino_bigint_mul(S, na, db);
    if (cross_num == NULL) return NULL;
    cross_den = mino_bigint_mul(S, da, nb);
    if (cross_den == NULL) return NULL;
    q = mino_bigint_quot(S, cross_num, cross_den);
    if (q == NULL) return NULL;
    if (op == MQR_QUOT) return q;
    /* rem = a - q*b, mod = adjust(rem, b). Both compose in the ratio
     * tier and may collapse to bigint when the result is integer. */
    {
        mino_val_t *qb_num = mino_bigint_mul(S, q, nb);
        mino_val_t *qb;
        mino_val_t *rem;
        if (qb_num == NULL) return NULL;
        qb = mino_ratio_make(S, qb_num, db);
        if (qb == NULL) return NULL;
        if (mino_type_of(a) == MINO_RATIO) {
            mino_val_t *qb_at_ratio = qb;
            if (mino_type_of(qb) != MINO_RATIO) {
                /* qb collapsed to int / bigint; promote for ratio_sub. */
                mino_val_t *qb_num2;
                mino_val_t *one = mino_bigint_from_ll(S, 1);
                if (one == NULL) return NULL;
                qb_num2 = (mino_val_int_p(qb))
                    ? mino_bigint_from_ll(S, mino_val_int_get(qb))
                    : qb;
                if (qb_num2 == NULL) return NULL;
                qb_at_ratio = mino_ratio_make_unchecked(S, qb_num2, one);
                if (qb_at_ratio == NULL) return NULL;
            }
            rem = mino_ratio_sub(S, a, qb_at_ratio);
        } else if (mino_type_of(qb) == MINO_RATIO) {
            /* a is int / bigint, qb is ratio. Sub via ratio. */
            mino_val_t *one = mino_bigint_from_ll(S, 1);
            mino_val_t *a_num;
            mino_val_t *a_at_ratio;
            if (one == NULL) return NULL;
            a_num = (mino_val_int_p(a))
                ? mino_bigint_from_ll(S, mino_val_int_get(a)) : (mino_val_t *)a;
            if (a_num == NULL) return NULL;
            a_at_ratio = mino_ratio_make_unchecked(S, a_num, one);
            if (a_at_ratio == NULL) return NULL;
            rem = mino_ratio_sub(S, a_at_ratio, qb);
        } else {
            /* Both bigint-tier. */
            mino_val_t *a_bn = (mino_val_int_p(a))
                ? mino_bigint_from_ll(S, mino_val_int_get(a)) : (mino_val_t *)a;
            mino_val_t *qb_bn = (mino_val_int_p(qb))
                ? mino_bigint_from_ll(S, mino_val_int_get(qb)) : qb;
            if (a_bn == NULL || qb_bn == NULL) return NULL;
            rem = mino_bigint_sub(S, a_bn, qb_bn);
        }
        if (rem == NULL) return NULL;
        if (op == MQR_REM) return rem;
        /* mod: if rem != 0 and signs differ from b, rem += b. */
        {
            int sr;
            int sb;
            if (mino_val_int_p(rem)) {
                sr = (mino_val_int_get(rem) > 0) - (mino_val_int_get(rem) < 0);
            } else if (mino_type_of(rem) == MINO_BIGINT) {
                sr = mp_int_compare_zero((mp_int)rem->as.bigint.mpz);
            } else { /* MINO_RATIO: sign matches numerator (denom positive) */
                sr = mp_int_compare_zero((mp_int)rem->as.ratio.num->as.bigint.mpz);
            }
            /* Sign of b: ratio b sign matches numerator. */
            if (mino_type_of(b) == MINO_RATIO) {
                sb = mp_int_compare_zero((mp_int)b->as.ratio.num->as.bigint.mpz);
            } else if (mino_val_int_p(b)) {
                sb = (mino_val_int_get(b) > 0) - (mino_val_int_get(b) < 0);
            } else {
                sb = mp_int_compare_zero((mp_int)b->as.bigint.mpz);
            }
            if (sr == 0 || ((sr < 0) == (sb < 0))) return rem;
            /* rem + b. Promote both to ratio if either is ratio. */
            if (mino_type_of(rem) == MINO_RATIO || mino_type_of(b) == MINO_RATIO) {
                mino_val_t *one = mino_bigint_from_ll(S, 1);
                /* Init to NULL so older GCC's -Wmaybe-uninitialized
                 * flow analysis sees a definite assignment. The
                 * conditional branches below cover every reachable
                 * path; the NULL check before the final use catches
                 * any allocator failure. */
                mino_val_t *r_at = NULL, *b_at = NULL;
                if (one == NULL) return NULL;
                if (mino_type_of(rem) == MINO_RATIO) {
                    r_at = rem;
                } else {
                    mino_val_t *rn = (mino_val_int_p(rem))
                        ? mino_bigint_from_ll(S, mino_val_int_get(rem)) : rem;
                    if (rn == NULL) return NULL;
                    r_at = mino_ratio_make_unchecked(S, rn, one);
                }
                if (mino_type_of(b) == MINO_RATIO) {
                    b_at = (mino_val_t *)b;
                } else {
                    mino_val_t *bn = (mino_val_int_p(b))
                        ? mino_bigint_from_ll(S, mino_val_int_get(b)) : (mino_val_t *)b;
                    mino_val_t *one2 = mino_bigint_from_ll(S, 1);
                    if (bn == NULL || one2 == NULL) return NULL;
                    b_at = mino_ratio_make_unchecked(S, bn, one2);
                }
                if (r_at == NULL || b_at == NULL) return NULL;
                return mino_ratio_add(S, r_at, b_at);
            }
            /* Both bigint-tier. */
            {
                mino_val_t *r_bn = (mino_val_int_p(rem))
                    ? mino_bigint_from_ll(S, mino_val_int_get(rem)) : rem;
                mino_val_t *b_bn = (mino_val_int_p(b))
                    ? mino_bigint_from_ll(S, mino_val_int_get(b)) : (mino_val_t *)b;
                if (r_bn == NULL || b_bn == NULL) return NULL;
                return mino_bigint_add(S, r_bn, b_bn);
            }
        }
    }
}

static mino_val_t *prim_mqr(mino_state_t *S, mino_val_t *args, mqr_op_t op)
{
    const char *opname = mqr_name(op);
    mino_val_t *xv, *yv;
    int xt, yt, max_tier;
    if (!mino_is_cons(args) || !mino_is_cons(args->as.cons.cdr) ||
        mino_is_cons(args->as.cons.cdr->as.cons.cdr)) {
        char buf[64];
        snprintf(buf, sizeof(buf), "%s requires two arguments", opname);
        return prim_throw_classified(S, "eval/arity", "MAR001", buf);
    }
    xv = args->as.cons.car;
    yv = args->as.cons.cdr->as.cons.car;
    if (!classify_or_throw(S, xv, opname, &xt)) return NULL;
    if (!classify_or_throw(S, yv, opname, &yt)) return NULL;
    max_tier = xt > yt ? xt : yt;

    /* Ratio meeting bigdec collapses to float. */
    if ((xt == TT_RATIO && yt == TT_BIGDEC) ||
        (xt == TT_BIGDEC && yt == TT_RATIO)) {
        return mqr_float(S, tower_to_double(xv), tower_to_double(yv), op, opname);
    }
    if (max_tier == TT_FLOAT) {
        return mqr_float(S, tower_to_double(xv), tower_to_double(yv), op, opname);
    }
    if (max_tier == TT_BIGDEC) {
        mino_val_t *xb = coerce_at_tier(S, xv, TT_BIGDEC, opname);
        mino_val_t *yb = coerce_at_tier(S, yv, TT_BIGDEC, opname);
        if (xb == NULL || yb == NULL) return NULL;
        if (op == MQR_QUOT) return mino_bigdec_quot(S, xb, yb);
        if (op == MQR_REM)  return mino_bigdec_rem(S, xb, yb);
        return mino_bigdec_mod(S, xb, yb);
    }
    if (max_tier == TT_RATIO) {
        return mqr_ratio(S, xv, yv, op);
    }
    if (max_tier == TT_BIGINT) {
        mino_val_t *xb = coerce_at_tier(S, xv, TT_BIGINT, opname);
        mino_val_t *yb = coerce_at_tier(S, yv, TT_BIGINT, opname);
        if (xb == NULL || yb == NULL) return NULL;
        if (op == MQR_QUOT) return mino_bigint_quot(S, xb, yb);
        if (op == MQR_REM)  return mino_bigint_rem(S, xb, yb);
        return mino_bigint_mod(S, xb, yb);
    }
    /* Both INT. */
    {
        long long a = mino_val_int_get(xv), b = mino_val_int_get(yv);
        if (b == 0) {
            char buf[64];
            snprintf(buf, sizeof(buf), "%s: division by zero", opname);
            return prim_throw_classified(S, "eval/type", "MTY001", buf);
        }
        if (a == LLONG_MIN && b == -1) {
            /* Overflow: promote to bigint. */
            mino_val_t *ba = mino_bigint_from_ll(S, a);
            mino_val_t *bb = mino_bigint_from_ll(S, b);
            if (ba == NULL || bb == NULL) return NULL;
            if (op == MQR_QUOT) return mino_bigint_quot(S, ba, bb);
            if (op == MQR_REM)  return mino_bigint_rem(S, ba, bb);
            return mino_bigint_mod(S, ba, bb);
        }
        if (op == MQR_QUOT) return mino_int(S, a / b);
        {
            long long r = a % b; /* C truncated remainder, sign of a. */
            if (op == MQR_MOD && r != 0 && ((r < 0) != (b < 0))) r += b;
            return mino_int(S, r);
        }
    }
}

mino_val_t *prim_mod(mino_state_t *S, mino_val_t *args, mino_env_t *env)
{
    (void)env;
    return prim_mqr(S, args, MQR_MOD);
}

mino_val_t *prim_rem(mino_state_t *S, mino_val_t *args, mino_env_t *env)
{
    (void)env;
    return prim_mqr(S, args, MQR_REM);
}

mino_val_t *prim_quot(mino_state_t *S, mino_val_t *args, mino_env_t *env)
{
    (void)env;
    return prim_mqr(S, args, MQR_QUOT);
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

/* Helper for narrow integer casts: extract an integer value from any
 * numeric type, checking for NaN/infinity on floats and bigdecs. The
 * caller then range-checks the result against the target tier. */
static int extract_integer_for_cast(mino_state_t *S, mino_val_t *v,
                                    long long *out, const char **err)
{
    (void)S;
    if (v == NULL) { *err = "expected a number"; return 0; }
    if (mino_val_int_p(v)) { *out = mino_val_int_get(v); return 1; }
    if (mino_type_of(v) == MINO_FLOAT || mino_type_of(v) == MINO_FLOAT32) {
        double d = v->as.f;
        if (d != d) { *err = "NaN cannot be coerced to integer"; return 0; }
        if (d > 9.2233720368547748e18 || d < -9.2233720368547758e18) {
            *err = "value out of long range"; return 0;
        }
        *out = (long long)d;
        return 1;
    }
    if (mino_type_of(v) == MINO_BIGINT) {
        if (mino_as_ll(v, out)) return 1;
        *err = "bigint value out of long range"; return 0;
    }
    if (mino_type_of(v) == MINO_RATIO) {
        double d = mino_ratio_to_double(v);
        if (d > 9.2233720368547748e18 || d < -9.2233720368547758e18) {
            *err = "ratio value out of long range"; return 0;
        }
        *out = (long long)d;
        return 1;
    }
    if (mino_type_of(v) == MINO_BIGDEC) {
        double d = mino_bigdec_to_double(v);
        if (d != d) { *err = "NaN cannot be coerced to integer"; return 0; }
        if (d > 9.2233720368547748e18 || d < -9.2233720368547758e18) {
            *err = "bigdec value out of long range"; return 0;
        }
        *out = (long long)d;
        return 1;
    }
    *err = "expected a number";
    return 0;
}

/* Helper for narrow integer casts with int32-or-smaller targets: if
 * the input is a float or bigdec, compares the *double value* against
 * the target tier's bounds before truncation. JVM Clojure's int / short /
 * byte all check the double value itself, so values like
 * -128.000001 throw for byte even though they truncate to the
 * in-range -128. */
static mino_val_t *narrow_cast(mino_state_t *S, mino_val_t *v,
                               long long lo, long long hi,
                               const char *opname)
{
    long long   ll;
    const char *err = NULL;
    char        buf[160];
    /* (cast \a) -> codepoint: chars don't take the float-bound path. */
    if (v != NULL && mino_type_of(v) == MINO_CHAR) {
        long long cp = (long long)v->as.ch;
        if (cp < lo || cp > hi) {
            snprintf(buf, sizeof(buf), "%s: value out of range", opname);
            return prim_throw_classified(S, "eval/type", "MTY001", buf);
        }
        return mino_int(S, cp);
    }
    if (v != NULL && (mino_type_of(v) == MINO_FLOAT || mino_type_of(v) == MINO_FLOAT32)) {
        double d = v->as.f;
        if (d != d) {
            snprintf(buf, sizeof(buf), "%s: NaN cannot be coerced to integer", opname);
            return prim_throw_classified(S, "eval/type", "MTY001", buf);
        }
        if (d < (double)lo || d > (double)hi) {
            snprintf(buf, sizeof(buf), "%s: value out of range", opname);
            return prim_throw_classified(S, "eval/type", "MTY001", buf);
        }
        return mino_int(S, (long long)d);
    }
    if (v != NULL && mino_type_of(v) == MINO_BIGDEC) {
        double d = mino_bigdec_to_double(v);
        if (d != d) {
            snprintf(buf, sizeof(buf), "%s: NaN cannot be coerced to integer", opname);
            return prim_throw_classified(S, "eval/type", "MTY001", buf);
        }
        if (d < (double)lo || d > (double)hi) {
            snprintf(buf, sizeof(buf), "%s: value out of range", opname);
            return prim_throw_classified(S, "eval/type", "MTY001", buf);
        }
        return mino_int(S, (long long)d);
    }
    if (!extract_integer_for_cast(S, v, &ll, &err)) {
        snprintf(buf, sizeof(buf), "%s: %s", opname, err ? err : "expected a number");
        return prim_throw_classified(S, "eval/type", "MTY001", buf);
    }
    if (ll < lo || ll > hi) {
        snprintf(buf, sizeof(buf), "%s: value out of range", opname);
        return prim_throw_classified(S, "eval/type", "MTY001", buf);
    }
    return mino_int(S, ll);
}

mino_val_t *prim_int(mino_state_t *S, mino_val_t *args, mino_env_t *env)
{
    (void)env;
    if (!mino_is_cons(args) || mino_is_cons(args->as.cons.cdr)) {
        return prim_throw_classified(S, "eval/arity", "MAR001", "int requires one argument");
    }
    return narrow_cast(S, args->as.cons.car, -2147483648LL, 2147483647LL, "int");
}

mino_val_t *prim_long(mino_state_t *S, mino_val_t *args, mino_env_t *env)
{
    mino_val_t *v;
    long long ll;
    const char *err = NULL;
    (void)env;
    if (!mino_is_cons(args) || mino_is_cons(args->as.cons.cdr)) {
        return prim_throw_classified(S, "eval/arity", "MAR001", "long requires one argument");
    }
    v = args->as.cons.car;
    /* (long \a) -> 97: char value yields its Unicode codepoint. */
    if (v != NULL && mino_type_of(v) == MINO_CHAR) {
        return mino_int(S, (long long)v->as.ch);
    }
    if (!extract_integer_for_cast(S, v, &ll, &err)) {
        char buf[160];
        snprintf(buf, sizeof(buf), "long: %s", err ? err : "expected a number");
        return prim_throw_classified(S, "eval/type", "MTY001", buf);
    }
    /* mino's MINO_INT is int64 already, so the range check is the same
     * one extract_integer_for_cast already did against long range. */
    return mino_int(S, ll);
}

mino_val_t *prim_short(mino_state_t *S, mino_val_t *args, mino_env_t *env)
{
    (void)env;
    if (!mino_is_cons(args) || mino_is_cons(args->as.cons.cdr)) {
        return prim_throw_classified(S, "eval/arity", "MAR001",
                                     "short requires one argument");
    }
    return narrow_cast(S, args->as.cons.car, -32768LL, 32767LL, "short");
}

mino_val_t *prim_byte(mino_state_t *S, mino_val_t *args, mino_env_t *env)
{
    (void)env;
    if (!mino_is_cons(args) || mino_is_cons(args->as.cons.cdr)) {
        return prim_throw_classified(S, "eval/arity", "MAR001",
                                     "byte requires one argument");
    }
    return narrow_cast(S, args->as.cons.car, -128LL, 127LL, "byte");
}

mino_val_t *prim_float(mino_state_t *S, mino_val_t *args, mino_env_t *env)
{
    mino_val_t *v;
    double      d;
    (void)env;
    if (!mino_is_cons(args) || mino_is_cons(args->as.cons.cdr)) {
        return prim_throw_classified(S, "eval/arity", "MAR001", "float requires one argument");
    }
    v = args->as.cons.car;
    if (v == NULL) {
        return prim_throw_classified(S, "eval/type", "MTY001", "float: expected a number");
    }
    if      (mino_type_of(v) == MINO_FLOAT)   d = v->as.f;
    else if (mino_type_of(v) == MINO_FLOAT32) d = v->as.f;
    else if (mino_val_int_p(v))     d = (double)mino_val_int_get(v);
    else if (mino_type_of(v) == MINO_BIGINT)  d = mino_bigint_to_double(v);
    else if (mino_type_of(v) == MINO_RATIO)   d = mino_ratio_to_double(v);
    else if (mino_type_of(v) == MINO_BIGDEC)  d = mino_bigdec_to_double(v);
    else
        return prim_throw_classified(S, "eval/type", "MTY001", "float: expected a number");
    /* Narrow the contract to the 32-bit float range: values outside
     * [-FLT_MAX, FLT_MAX] (including +/- infinity) throw; underflow
     * rounds toward zero. NaN passes through. The result is tagged
     * MINO_FLOAT32 so `double?` returns false on it. */
    if (d == d) {
        if (d > FLT_MAX || d < -FLT_MAX) {
            return prim_throw_classified(S, "eval/type", "MTY001",
                                         "float: value out of float range");
        }
    }
    return mino_float32(S, d);
}

mino_val_t *prim_double(mino_state_t *S, mino_val_t *args, mino_env_t *env)
{
    mino_val_t *v;
    (void)env;
    if (!mino_is_cons(args) || mino_is_cons(args->as.cons.cdr)) {
        return prim_throw_classified(S, "eval/arity", "MAR001",
                                     "double requires one argument");
    }
    v = args->as.cons.car;
    if (v == NULL)
        return prim_throw_classified(S, "eval/type", "MTY001", "double: expected a number");
    if (mino_type_of(v) == MINO_FLOAT)   return v;
    if (mino_type_of(v) == MINO_FLOAT32) return mino_float(S, v->as.f);
    if (mino_val_int_p(v))     return mino_float(S, (double)mino_val_int_get(v));
    if (mino_type_of(v) == MINO_BIGINT)  return mino_float(S, mino_bigint_to_double(v));
    if (mino_type_of(v) == MINO_RATIO)   return mino_float(S, mino_ratio_to_double(v));
    if (mino_type_of(v) == MINO_BIGDEC)  return mino_float(S, mino_bigdec_to_double(v));
    return prim_throw_classified(S, "eval/type", "MTY001", "double: expected a number");
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
    if (v == NULL || mino_type_of(v) != MINO_STRING)
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
    if (v == NULL || mino_type_of(v) != MINO_STRING)
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

/* Numeric "family" for cross-type compare. Numbers (long/float/bigint/
 * ratio/bigdec) compare uniformly via their numeric value. Returns 1
 * for numbers, 0 otherwise. */
static int is_compare_number(const mino_val_t *v)
{
    if (v == NULL) return 0;
    return mino_val_int_p(v) || mino_type_of(v) == MINO_FLOAT
        || mino_type_of(v) == MINO_FLOAT32
        || mino_type_of(v) == MINO_BIGINT || mino_type_of(v) == MINO_RATIO
        || mino_type_of(v) == MINO_BIGDEC;
}

/* (compare a b) -- general comparison returning -1, 0, or 1.
 * For values of the same type the natural same-type comparison
 * applies; for values of different types within the canon-recognized
 * set, the tier order above provides a total order matching
 * Clojure's `compare`. */
mino_val_t *prim_compare(mino_state_t *S, mino_val_t *args, mino_env_t *env)
{
    /* Mirrors Clojure: nil is less than anything (and equal to itself);
     * otherwise compareTo on the first arg, which throws on
     * incompatible types. We model "compareTo" by allowing same-family
     * comparisons for numbers (across long/float/bigint/ratio/bigdec),
     * chars, strings, symbols, keywords, bools, and vectors
     * (lexicographic, recursing through prim_compare). All other
     * cross-type pairs raise an "incomparable" error. */
    mino_val_t *a, *b;
    int a_nil, b_nil;
    (void)env;
    if (!mino_is_cons(args) || !mino_is_cons(args->as.cons.cdr) ||
        mino_is_cons(args->as.cons.cdr->as.cons.cdr)) {
        return prim_throw_classified(S, "eval/arity", "MAR001", "compare requires two arguments");
    }
    a = args->as.cons.car;
    b = args->as.cons.cdr->as.cons.car;
    a_nil = (a == NULL || mino_type_of(a) == MINO_NIL);
    b_nil = (b == NULL || mino_type_of(b) == MINO_NIL);
    if (a_nil && b_nil) return mino_int(S, 0);
    if (a_nil)          return mino_int(S, -1);
    if (b_nil)          return mino_int(S,  1);
    if (mino_type_of(a) == MINO_BOOL && mino_type_of(b) == MINO_BOOL) {
        return mino_int(S, a->as.b < b->as.b ? -1 :
                           a->as.b > b->as.b ? 1 : 0);
    }
    if (is_compare_number(a) && is_compare_number(b)) {
        /* Use the full numeric tower coercion so bigints, ratios, and
         * bigdecs all reduce to a comparable double. as_double only
         * knows about long/double, which would mis-classify e.g.
         * (compare 0 -100N) as cross-type. */
        double da = tower_to_double(a);
        double db = tower_to_double(b);
        return mino_int(S, da < db ? -1 : da > db ? 1 : 0);
    }
    if (a->type == b->type && mino_type_of(a) == MINO_STRING) {
        int cmp = strcmp(a->as.s.data, b->as.s.data);
        return mino_int(S, cmp < 0 ? -1 : cmp > 0 ? 1 : 0);
    }
    if (a->type == b->type
        && (mino_type_of(a) == MINO_KEYWORD || mino_type_of(a) == MINO_SYMBOL)) {
        /* Defer to val_compare so namespaced symbols/keywords compare
         * as Clojure does: unqualified before any qualified, and
         * within the same ns, by name. Plain strcmp would put `:cat`
         * after `:animal/cat` because 'c' > 'a'. */
        int cmp = val_compare(a, b);
        return mino_int(S, cmp < 0 ? -1 : cmp > 0 ? 1 : 0);
    }
    if (mino_type_of(a) == MINO_CHAR && mino_type_of(b) == MINO_CHAR) {
        int cmp = a->as.ch - b->as.ch;
        return mino_int(S, cmp < 0 ? -1 : cmp > 0 ? 1 : 0);
    }
    {
        /* Vectors and map entries compare lexicographically as
         * sequences. A MAP_ENTRY behaves like a 2-element vector for
         * compare purposes (matches JVM Clojure where MapEntry's
         * compareTo delegates to AbstractVector). */
        int a_vec = (mino_type_of(a) == MINO_VECTOR || mino_type_of(a) == MINO_MAP_ENTRY);
        int b_vec = (mino_type_of(b) == MINO_VECTOR || mino_type_of(b) == MINO_MAP_ENTRY);
        if (a_vec && b_vec) {
            size_t la = mino_type_of(a) == MINO_VECTOR ? a->as.vec.len : 2;
            size_t lb = mino_type_of(b) == MINO_VECTOR ? b->as.vec.len : 2;
            size_t n  = la < lb ? la : lb;
            size_t i;
            for (i = 0; i < n; i++) {
                mino_val_t *ea = mino_type_of(a) == MINO_VECTOR
                    ? vec_nth(a, i)
                    : (i == 0 ? a->as.map_entry.k : a->as.map_entry.v);
                mino_val_t *eb = mino_type_of(b) == MINO_VECTOR
                    ? vec_nth(b, i)
                    : (i == 0 ? b->as.map_entry.k : b->as.map_entry.v);
                mino_val_t *recur_args =
                    mino_cons(S, ea, mino_cons(S, eb, mino_nil(S)));
                mino_val_t *r = prim_compare(S, recur_args, env);
                if (r == NULL) return NULL;
                if (mino_val_int_p(r) && mino_val_int_get(r) != 0) return r;
            }
            if (la == lb) return mino_int(S, 0);
            return mino_int(S, la < lb ? -1 : 1);
        }
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
    if ((mino_val_int_p(a) || mino_type_of(a) == MINO_BIGINT) &&
        (mino_val_int_p(b) || mino_type_of(b) == MINO_BIGINT)) {
        if (mino_val_int_p(a) && mino_val_int_p(b)) return mino_val_int_get(a) == mino_val_int_get(b);
        if (mino_type_of(a) == MINO_BIGINT && mino_type_of(b) == MINO_BIGINT)
            return mino_bigint_equals(a, b);
        if (mino_val_int_p(a)) return mino_bigint_equals_ll(b, mino_val_int_get(a));
        return mino_bigint_equals_ll(a, mino_val_int_get(b));
    }
    /* Same-tier ratio. */
    if (mino_type_of(a) == MINO_RATIO && mino_type_of(b) == MINO_RATIO)
        return mino_ratio_equals(a, b);
    /* Same-tier bigdec: compare by value (not representation), since
     * == is numeric equality. */
    if (mino_type_of(a) == MINO_BIGDEC && mino_type_of(b) == MINO_BIGDEC)
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
        switch (mino_type_of(a)) {
        case MINO_INT:
            return mino_val_int_get(a) < mino_val_int_get(b) ? -1 : mino_val_int_get(a) > mino_val_int_get(b) ? 1 : 0;
        case MINO_FLOAT:
        case MINO_FLOAT32:
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
    if ((mino_val_int_p(a) && mino_type_of(b) == MINO_BIGINT) ||
        (mino_type_of(a) == MINO_BIGINT && mino_val_int_p(b))) {
        long long ll;
        if (mino_val_int_p(a)) {
            if (mino_bigint_equals_ll(b, mino_val_int_get(a))) return 0;
            /* Compare magnitudes via double; for long-fitting bigints
             * the comparison is exact, otherwise the bigint dominates. */
            if (mino_as_ll(b, &ll))
                return mino_val_int_get(a) < ll ? -1 : mino_val_int_get(a) > ll ? 1 : 0;
            return mino_bigint_to_double(b) > 0 ? -1 : 1;
        } else {
            if (mino_bigint_equals_ll(a, mino_val_int_get(b))) return 0;
            if (mino_as_ll(a, &ll))
                return ll < mino_val_int_get(b) ? -1 : ll > mino_val_int_get(b) ? 1 : 0;
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

/* `(<)`, `(<=)`, `(>)`, `(>=)` -- relational chains.
 *
 *   - Non-numeric operands (including `nil`) throw, matching Clojure's
 *     NPE / "cannot compare" semantics.
 *   - Any NaN operand short-circuits to `false` for every relation
 *     (NaN is unordered).
 *   - Otherwise return true iff each successive pair satisfies the
 *     relation; trivially true on zero or one argument.
 */
static int has_nan(const mino_val_t *v)
{
    return v != NULL && (mino_type_of(v) == MINO_FLOAT || mino_type_of(v) == MINO_FLOAT32)
        && isnan(v->as.f);
}

static mino_val_t *compare_chain(mino_state_t *S, mino_val_t *args, const char *name, int op)
{
    if (!mino_is_cons(args)) return mino_true(S);
    if (!mino_is_cons(args->as.cons.cdr)) {
        /* Single-arg form: Clojure short-circuits and never inspects
         * the operand's type, so even non-numeric arguments return
         * true. */
        return mino_true(S);
    }
    {
        const mino_val_t *prev = args->as.cons.car;
        if (!is_compare_number(prev)) {
            char msg[64];
            snprintf(msg, sizeof(msg), "%s expects numbers", name);
            return prim_throw_classified(S, "eval/type", "MTY001", msg);
        }
        args = args->as.cons.cdr;
        while (mino_is_cons(args)) {
            const mino_val_t *cur = args->as.cons.car;
            int cmp;
            int ok;
            if (!is_compare_number(cur)) {
                char msg[64];
                snprintf(msg, sizeof(msg), "%s expects numbers", name);
                return prim_throw_classified(S, "eval/type", "MTY001", msg);
            }
            if (has_nan(prev) || has_nan(cur)) return mino_false(S);
            cmp = tower_cmp(prev, cur);
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

/* argv-ABI sibling of compare_chain. */
static mino_val_t *compare_chain_argv(mino_state_t *S, mino_val_t **argv,
                                      int argc, const char *name, int op)
{
    int i;
    const mino_val_t *prev;
    if (argc <= 1) return mino_true(S);
    prev = argv[0];
    if (!is_compare_number(prev)) {
        char msg[64];
        snprintf(msg, sizeof(msg), "%s expects numbers", name);
        return prim_throw_classified(S, "eval/type", "MTY001", msg);
    }
    for (i = 1; i < argc; i++) {
        const mino_val_t *cur = argv[i];
        int cmp;
        int ok;
        if (!is_compare_number(cur)) {
            char msg[64];
            snprintf(msg, sizeof(msg), "%s expects numbers", name);
            return prim_throw_classified(S, "eval/type", "MTY001", msg);
        }
        if (has_nan(prev) || has_nan(cur)) return mino_false(S);
        cmp = tower_cmp(prev, cur);
        switch (op) {
        case 0:  ok = cmp <  0; break;
        case 1:  ok = cmp <= 0; break;
        case 2:  ok = cmp >  0; break;
        default: ok = cmp >= 0; break;
        }
        if (!ok) return mino_false(S);
        prev = cur;
    }
    return mino_true(S);
}

mino_val_t *prim_lt_argv(mino_state_t *S, mino_val_t **argv, int argc,
                         mino_env_t *env)
{
    (void)env;
    return compare_chain_argv(S, argv, argc, "<", 0);
}

mino_val_t *prim_lte_argv(mino_state_t *S, mino_val_t **argv, int argc,
                          mino_env_t *env)
{
    (void)env;
    return compare_chain_argv(S, argv, argc, "<=", 1);
}

mino_val_t *prim_gt_argv(mino_state_t *S, mino_val_t **argv, int argc,
                         mino_env_t *env)
{
    (void)env;
    return compare_chain_argv(S, argv, argc, ">", 2);
}

mino_val_t *prim_gte_argv(mino_state_t *S, mino_val_t **argv, int argc,
                          mino_env_t *env)
{
    (void)env;
    return compare_chain_argv(S, argv, argc, ">=", 3);
}

mino_val_t *prim_nan_p(mino_state_t *S, mino_val_t *args, mino_env_t *env)
{
    mino_val_t *v;
    (void)env;
    if (!mino_is_cons(args) || mino_is_cons(args->as.cons.cdr)) {
        return prim_throw_classified(S, "eval/arity", "MAR001", "NaN? requires one argument");
    }
    v = args->as.cons.car;
    if (!is_compare_number(v))
        return prim_throw_classified(S, "eval/type", "MTY001", "NaN? requires a number");
    return ((mino_type_of(v) == MINO_FLOAT || mino_type_of(v) == MINO_FLOAT32)
            && isnan(v->as.f))
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
    if (v == NULL || mino_type_of(v) == MINO_NIL)
        return prim_throw_classified(S, "eval/type", "MTY001", "infinite? requires a number");
    return ((mino_type_of(v) == MINO_FLOAT || mino_type_of(v) == MINO_FLOAT32)
            && isinf(v->as.f))
           ? mino_true(S) : mino_false(S);
}

const mino_prim_def k_prims_numeric[] = {
    {"+",   prim_add,
     "Returns the sum of the arguments. Throws on long overflow; "
     "use +' to auto-promote to bigint.", prim_add_argv},
    {"+'",  prim_addp,
     "Returns the sum of the arguments. Auto-promotes to bigint on "
     "long overflow; use + to throw on overflow.", prim_addp_argv},
    {"inc", prim_inc,
     "Returns x plus 1. Throws on long overflow; use inc' to "
     "auto-promote.", prim_inc_argv},
    {"inc'", prim_incp,
     "Returns x plus 1. Auto-promotes to bigint on long overflow.",
     prim_incp_argv},
    {"dec", prim_dec,
     "Returns x minus 1. Throws on long overflow; use dec' to "
     "auto-promote.", prim_dec_argv},
    {"dec'", prim_decp,
     "Returns x minus 1. Auto-promotes to bigint on long overflow.",
     prim_decp_argv},
    {"-",   prim_sub,
     "Returns the difference of the arguments. With one arg, returns "
     "the negation. Throws on long overflow; use -' to auto-promote.",
     prim_sub_argv},
    {"-'",  prim_subp,
     "Returns the difference of the arguments. With one arg, returns "
     "the negation. Auto-promotes to bigint on long overflow.",
     prim_subp_argv},
    {"*",   prim_mul,
     "Returns the product of the arguments. Throws on long overflow; "
     "use *' to auto-promote.", prim_mul_argv},
    {"*'",  prim_mulp,
     "Returns the product of the arguments. Auto-promotes to bigint "
     "on long overflow.", prim_mulp_argv},
    {"/",   prim_div,
     "Returns the quotient of the arguments.", prim_div_argv},
    {"=",   prim_eq,
     "Returns true if all arguments are equal."},
    {"==",  prim_num_eq,
     "Returns true if all arguments are numerically equal across "
     "the numeric tower (int, bigint, ratio, bigdec, float)."},
    {"identical?", prim_identical,
     "Returns true if the arguments are the same object."},
    {"<",   prim_lt,
     "Returns true if nums are in monotonically increasing order.",
     prim_lt_argv},
    {"<=",  prim_lte,
     "Returns true if nums are in monotonically non-decreasing order.",
     prim_lte_argv},
    {">",   prim_gt,
     "Returns true if nums are in monotonically decreasing order.",
     prim_gt_argv},
    {">=",  prim_gte,
     "Returns true if nums are in monotonically non-increasing order.",
     prim_gte_argv},
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
     "Coerces x to an int (32-bit integer). Throws on out-of-range "
     "values, NaN, or infinity. Returns the value as a MINO_INT (mino "
     "has only one integer tier); only the contract narrows."},
    {"long",  prim_long,
     "Coerces x to a long (64-bit integer). Throws on out-of-range "
     "values, NaN, or infinity, including bigint and bigdec out of "
     "long range."},
    {"short", prim_short,
     "Coerces x to a short (16-bit integer). Throws on out-of-range "
     "values, NaN, or infinity. Returns the value as a long since mino "
     "has only one integer tier."},
    {"byte",  prim_byte,
     "Coerces x to a byte (8-bit integer). Throws on out-of-range "
     "values, NaN, or infinity. Returns the value as a long since mino "
     "has only one integer tier."},
    {"float", prim_float,
     "Coerces x to a 32-bit float (returns a MINO_FLOAT32). Throws "
     "on out-of-float32-range, +/-Infinity. NaN passes through. "
     "Underflow rounds toward zero."},
    {"double", prim_double,
     "Coerces x to a 64-bit double (returns a MINO_FLOAT). Identity "
     "on existing doubles."},
    {"parse-long",   prim_parse_long,
     "Parses a string into a long integer, or returns nil on failure."},
    {"parse-double", prim_parse_double,
     "Parses a string into a double, or returns nil on failure."},
    {"unchecked-add",      prim_unchecked_add,
     "Returns x + y as a long with two's-complement wraparound. "
     "Operands must be ints. Opt-in fast path for code that knows "
     "overflow can't occur or wants wraparound semantics."},
    {"unchecked-subtract", prim_unchecked_sub,
     "Returns x - y as a long with two's-complement wraparound. "
     "Operands must be ints."},
    {"unchecked-multiply", prim_unchecked_mul,
     "Returns x * y as a long with two's-complement wraparound. "
     "Operands must be ints."},
    {"unchecked-inc",      prim_unchecked_inc,
     "Returns x + 1 as a long with two's-complement wraparound. "
     "Argument must be an int."},
    {"unchecked-dec",      prim_unchecked_dec,
     "Returns x - 1 as a long with two's-complement wraparound. "
     "Argument must be an int."},
    {"unchecked-negate",   prim_unchecked_negate,
     "Returns -x as a long with two's-complement wraparound. "
     "Argument must be an int."},
    {"unchecked-divide-int", prim_quot,
     "Returns the truncating integer division of x by y. Both must be ints. "
     "Aliased to quot — the truncating semantic matches canon's "
     "unchecked-divide-int (no overflow check; on the JVM this is the "
     "primitive idiv instruction)."},
};

const size_t k_prims_numeric_count =
    sizeof(k_prims_numeric) / sizeof(k_prims_numeric[0]);
