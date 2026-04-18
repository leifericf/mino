/*
 * prim_numeric.c -- numeric, math, bitwise, coercion, and comparison primitives.
 */

#include "prim_internal.h"

#include <math.h>
#include <string.h>
#include <stdio.h>
#include <errno.h>

/* ------------------------------------------------------------------------- */
/* Arithmetic                                                                */
/* ------------------------------------------------------------------------- */

mino_val_t *prim_add(mino_state_t *S, mino_val_t *args, mino_env_t *env)
{
    (void)env;
    if (args_have_float(args)) {
        double acc = 0.0;
        while (mino_is_cons(args)) {
            double x;
            if (!as_double(args->as.cons.car, &x)) {
                return prim_throw_error(S, "+ expects numbers");
            }
            acc += x;
            args = args->as.cons.cdr;
        }
        return mino_float(S, acc);
    } else {
        long long acc = 0;
        while (mino_is_cons(args)) {
            long long x;
            if (!as_long(args->as.cons.car, &x)) {
                return prim_throw_error(S, "+ expects numbers");
            }
            acc += x;
            args = args->as.cons.cdr;
        }
        return mino_int(S, acc);
    }
}

mino_val_t *prim_sub(mino_state_t *S, mino_val_t *args, mino_env_t *env)
{
    (void)env;
    if (!mino_is_cons(args)) {
        return prim_throw_error(S, "- requires at least one argument");
    }
    if (args_have_float(args)) {
        double acc;
        if (!as_double(args->as.cons.car, &acc)) {
            return prim_throw_error(S, "- expects numbers");
        }
        args = args->as.cons.cdr;
        if (!mino_is_cons(args)) {
            return mino_float(S, -acc);
        }
        while (mino_is_cons(args)) {
            double x;
            if (!as_double(args->as.cons.car, &x)) {
                return prim_throw_error(S, "- expects numbers");
            }
            acc -= x;
            args = args->as.cons.cdr;
        }
        return mino_float(S, acc);
    } else {
        long long acc;
        if (!as_long(args->as.cons.car, &acc)) {
            return prim_throw_error(S, "- expects numbers");
        }
        args = args->as.cons.cdr;
        if (!mino_is_cons(args)) {
            return mino_int(S, -acc);
        }
        while (mino_is_cons(args)) {
            long long x;
            if (!as_long(args->as.cons.car, &x)) {
                return prim_throw_error(S, "- expects numbers");
            }
            acc -= x;
            args = args->as.cons.cdr;
        }
        return mino_int(S, acc);
    }
}

mino_val_t *prim_mul(mino_state_t *S, mino_val_t *args, mino_env_t *env)
{
    (void)env;
    if (args_have_float(args)) {
        double acc = 1.0;
        while (mino_is_cons(args)) {
            double x;
            if (!as_double(args->as.cons.car, &x)) {
                return prim_throw_error(S, "* expects numbers");
            }
            acc *= x;
            args = args->as.cons.cdr;
        }
        return mino_float(S, acc);
    } else {
        long long acc = 1;
        while (mino_is_cons(args)) {
            long long x;
            if (!as_long(args->as.cons.car, &x)) {
                return prim_throw_error(S, "* expects numbers");
            }
            acc *= x;
            args = args->as.cons.cdr;
        }
        return mino_int(S, acc);
    }
}

mino_val_t *prim_div(mino_state_t *S, mino_val_t *args, mino_env_t *env)
{
    /* Division returns an integer when all operands are integers and the
     * result is exact, a float otherwise. */
    double acc;
    int all_int = 1;
    (void)env;
    if (!mino_is_cons(args)) {
        return prim_throw_error(S, "/ requires at least one argument");
    }
    if (args->as.cons.car == NULL
        || (args->as.cons.car->type != MINO_INT
            && args->as.cons.car->type != MINO_FLOAT)) {
        return prim_throw_error(S, "/ expects numbers");
    }
    if (args->as.cons.car->type == MINO_FLOAT) all_int = 0;
    if (!as_double(args->as.cons.car, &acc)) {
        return prim_throw_error(S, "/ expects numbers");
    }
    args = args->as.cons.cdr;
    if (!mino_is_cons(args)) {
        if (acc == 0.0) {
            if (all_int)
                return prim_throw_error(S, "division by zero");
            return mino_float(S, 1.0 / acc);
        }
        return mino_float(S, 1.0 / acc);
    }
    while (mino_is_cons(args)) {
        double x;
        if (args->as.cons.car == NULL
            || (args->as.cons.car->type != MINO_INT
                && args->as.cons.car->type != MINO_FLOAT)) {
            return prim_throw_error(S, "/ expects numbers");
        }
        if (args->as.cons.car->type == MINO_FLOAT) all_int = 0;
        if (!as_double(args->as.cons.car, &x)) {
            return prim_throw_error(S, "/ expects numbers");
        }
        if (x == 0.0 && all_int) {
            return prim_throw_error(S, "division by zero");
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
        return prim_throw_error(S, "mod requires two arguments");
    }
    if (!as_double(args->as.cons.car, &a) ||
        !as_double(args->as.cons.cdr->as.cons.car, &b)) {
        return prim_throw_error(S, "mod expects numbers");
    }
    if (b == 0.0) {
        if (args->as.cons.car->type == MINO_INT &&
            args->as.cons.cdr->as.cons.car->type == MINO_INT)
            return prim_throw_error(S, "mod: division by zero");
        return mino_float(S, fmod(a, b));
    }
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
        return prim_throw_error(S, "rem requires two arguments");
    }
    if (!as_double(args->as.cons.car, &a) ||
        !as_double(args->as.cons.cdr->as.cons.car, &b)) {
        return prim_throw_error(S, "rem expects numbers");
    }
    if (b == 0.0) {
        if (args->as.cons.car->type == MINO_INT &&
            args->as.cons.cdr->as.cons.car->type == MINO_INT)
            return prim_throw_error(S, "rem: division by zero");
        return mino_float(S, fmod(a, b));
    }
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
        return prim_throw_error(S, "quot requires two arguments");
    }
    if (!as_double(args->as.cons.car, &a) ||
        !as_double(args->as.cons.cdr->as.cons.car, &b)) {
        return prim_throw_error(S, "quot expects numbers");
    }
    if (b == 0.0) {
        if (args->as.cons.car->type == MINO_INT &&
            args->as.cons.cdr->as.cons.car->type == MINO_INT)
            return prim_throw_error(S, "quot: division by zero");
        {
            double qq = a / b;
            return mino_float(S, qq >= 0 ? floor(qq) : ceil(qq));
        }
    }
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
            return prim_throw_error(S, label " requires one argument");  \
        }                                                               \
        if (!as_double(args->as.cons.car, &x)) {                       \
            return prim_throw_error(S, label " expects a number");     \
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
        return prim_throw_error(S, "math-pow requires two arguments");
    }
    if (!as_double(args->as.cons.car, &base) ||
        !as_double(args->as.cons.cdr->as.cons.car, &exponent)) {
        return prim_throw_error(S, "math-pow expects numbers");
    }
    return mino_float(S, pow(base, exponent));
}

mino_val_t *prim_math_atan2(mino_state_t *S, mino_val_t *args, mino_env_t *env)
{
    double y, x;
    (void)env;
    if (!mino_is_cons(args) || !mino_is_cons(args->as.cons.cdr) ||
        mino_is_cons(args->as.cons.cdr->as.cons.cdr)) {
        return prim_throw_error(S, "math-atan2 requires two arguments");
    }
    if (!as_double(args->as.cons.car, &y) ||
        !as_double(args->as.cons.cdr->as.cons.car, &x)) {
        return prim_throw_error(S, "math-atan2 expects numbers");
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
        return prim_throw_error(S, "bit-and requires two arguments");
    }
    if (!as_long(args->as.cons.car, &a) ||
        !as_long(args->as.cons.cdr->as.cons.car, &b)) {
        return prim_throw_error(S, "bit-and expects integers");
    }
    return mino_int(S, a & b);
}

mino_val_t *prim_bit_or(mino_state_t *S, mino_val_t *args, mino_env_t *env)
{
    long long a, b;
    (void)env;
    if (!mino_is_cons(args) || !mino_is_cons(args->as.cons.cdr) ||
        mino_is_cons(args->as.cons.cdr->as.cons.cdr)) {
        return prim_throw_error(S, "bit-or requires two arguments");
    }
    if (!as_long(args->as.cons.car, &a) ||
        !as_long(args->as.cons.cdr->as.cons.car, &b)) {
        return prim_throw_error(S, "bit-or expects integers");
    }
    return mino_int(S, a | b);
}

mino_val_t *prim_bit_xor(mino_state_t *S, mino_val_t *args, mino_env_t *env)
{
    long long a, b;
    (void)env;
    if (!mino_is_cons(args) || !mino_is_cons(args->as.cons.cdr) ||
        mino_is_cons(args->as.cons.cdr->as.cons.cdr)) {
        return prim_throw_error(S, "bit-xor requires two arguments");
    }
    if (!as_long(args->as.cons.car, &a) ||
        !as_long(args->as.cons.cdr->as.cons.car, &b)) {
        return prim_throw_error(S, "bit-xor expects integers");
    }
    return mino_int(S, a ^ b);
}

mino_val_t *prim_bit_not(mino_state_t *S, mino_val_t *args, mino_env_t *env)
{
    long long a;
    (void)env;
    if (!mino_is_cons(args) || mino_is_cons(args->as.cons.cdr)) {
        return prim_throw_error(S, "bit-not requires one argument");
    }
    if (!as_long(args->as.cons.car, &a)) {
        return prim_throw_error(S, "bit-not expects an integer");
    }
    return mino_int(S, ~a);
}

mino_val_t *prim_bit_shift_left(mino_state_t *S, mino_val_t *args, mino_env_t *env)
{
    long long a, b;
    (void)env;
    if (!mino_is_cons(args) || !mino_is_cons(args->as.cons.cdr) ||
        mino_is_cons(args->as.cons.cdr->as.cons.cdr)) {
        return prim_throw_error(S, "bit-shift-left requires two arguments");
    }
    if (!as_long(args->as.cons.car, &a) ||
        !as_long(args->as.cons.cdr->as.cons.car, &b)) {
        return prim_throw_error(S, "bit-shift-left expects integers");
    }
    return mino_int(S, a << b);
}

mino_val_t *prim_bit_shift_right(mino_state_t *S, mino_val_t *args, mino_env_t *env)
{
    long long a, b;
    (void)env;
    if (!mino_is_cons(args) || !mino_is_cons(args->as.cons.cdr) ||
        mino_is_cons(args->as.cons.cdr->as.cons.cdr)) {
        return prim_throw_error(S, "bit-shift-right requires two arguments");
    }
    if (!as_long(args->as.cons.car, &a) ||
        !as_long(args->as.cons.cdr->as.cons.car, &b)) {
        return prim_throw_error(S, "bit-shift-right expects integers");
    }
    return mino_int(S, a >> b);
}

mino_val_t *prim_unsigned_bit_shift_right(mino_state_t *S, mino_val_t *args, mino_env_t *env)
{
    long long a, b;
    (void)env;
    if (!mino_is_cons(args) || !mino_is_cons(args->as.cons.cdr) ||
        mino_is_cons(args->as.cons.cdr->as.cons.cdr)) {
        return prim_throw_error(S, "unsigned-bit-shift-right requires two arguments");
    }
    if (!as_long(args->as.cons.car, &a) ||
        !as_long(args->as.cons.cdr->as.cons.car, &b)) {
        return prim_throw_error(S, "unsigned-bit-shift-right expects integers");
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
        return prim_throw_error(S, "int requires one argument");
    }
    v = args->as.cons.car;
    if (v != NULL && v->type == MINO_INT) return v;
    if (v != NULL && v->type == MINO_FLOAT) return mino_int(S, (long long)v->as.f);
    return prim_throw_error(S, "int: expected a number");
}

mino_val_t *prim_float(mino_state_t *S, mino_val_t *args, mino_env_t *env)
{
    mino_val_t *v;
    (void)env;
    if (!mino_is_cons(args) || mino_is_cons(args->as.cons.cdr)) {
        return prim_throw_error(S, "float requires one argument");
    }
    v = args->as.cons.car;
    if (v != NULL && v->type == MINO_FLOAT) return v;
    if (v != NULL && v->type == MINO_INT) return mino_float(S, (double)v->as.i);
    return prim_throw_error(S, "float: expected a number");
}

mino_val_t *prim_parse_long(mino_state_t *S, mino_val_t *args, mino_env_t *env)
{
    mino_val_t *v;
    const char *s;
    char *end;
    long long result;
    (void)env;
    if (!mino_is_cons(args) || mino_is_cons(args->as.cons.cdr)) {
        return prim_throw_error(S, "parse-long requires one argument");
    }
    v = args->as.cons.car;
    if (v == NULL || v->type != MINO_STRING)
        return prim_throw_error(S, "parse-long: argument must be a string");
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
        return prim_throw_error(S, "parse-double requires one argument");
    }
    v = args->as.cons.car;
    if (v == NULL || v->type != MINO_STRING)
        return prim_throw_error(S, "parse-double: argument must be a string");
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
        return prim_throw_error(S, "compare requires two arguments");
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
    return prim_throw_error(S, "compare: cannot compare values of different types");
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
        return prim_throw_error(S, "identical? requires 2 arguments");
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
        return prim_throw_error(S, msg);
    }
    args = args->as.cons.cdr;
    while (mino_is_cons(args)) {
        double x;
        int    ok;
        if (!as_double(args->as.cons.car, &x)) {
            char msg[64];
            snprintf(msg, sizeof(msg), "%s expects numbers", name);
            return prim_throw_error(S, msg);
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
        return prim_throw_error(S, "NaN? requires one argument");
    }
    v = args->as.cons.car;
    if (v == NULL || v->type == MINO_NIL)
        return prim_throw_error(S, "NaN? requires a number");
    return (v->type == MINO_FLOAT && isnan(v->as.f))
           ? mino_true(S) : mino_false(S);
}

mino_val_t *prim_infinite_p(mino_state_t *S, mino_val_t *args, mino_env_t *env)
{
    mino_val_t *v;
    (void)env;
    if (!mino_is_cons(args) || mino_is_cons(args->as.cons.cdr)) {
        return prim_throw_error(S, "infinite? requires one argument");
    }
    v = args->as.cons.car;
    if (v == NULL || v->type == MINO_NIL)
        return prim_throw_error(S, "infinite? requires a number");
    return (v->type == MINO_FLOAT && isinf(v->as.f))
           ? mino_true(S) : mino_false(S);
}
