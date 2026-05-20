/*
 * numeric_math.c -- math.h wrappers (floor, ceil, round, sqrt, log,
 * exp, trig + hyperbolic, pow, atan2, signum, hypot, copy-sign,
 * next-up/down, ieee-remainder, to-radians/to-degrees, log10,
 * log1p, expm1, cbrt). Carved out of numeric.c.
 *
 * Internal to the prim subsystem; embedders should only use mino.h.
 */

#include "prim/internal.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

/* ------------------------------------------------------------------------- */
/* Math functions (thin wrappers around math.h)                              */
/* ------------------------------------------------------------------------- */

static inline mino_val *math_unary(mino_state *S, mino_val *args,
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

mino_val *prim_math_floor(mino_state *S, mino_val *args, mino_env *env)
{ (void)env; return math_unary(S, args, floor, "math-floor"); }

mino_val *prim_math_ceil(mino_state *S, mino_val *args, mino_env *env)
{ (void)env; return math_unary(S, args, ceil, "math-ceil"); }

mino_val *prim_math_round(mino_state *S, mino_val *args, mino_env *env)
{ (void)env; return math_unary(S, args, round, "math-round"); }

mino_val *prim_math_sqrt(mino_state *S, mino_val *args, mino_env *env)
{ (void)env; return math_unary(S, args, sqrt, "math-sqrt"); }

mino_val *prim_math_log(mino_state *S, mino_val *args, mino_env *env)
{ (void)env; return math_unary(S, args, log, "math-log"); }

mino_val *prim_math_exp(mino_state *S, mino_val *args, mino_env *env)
{ (void)env; return math_unary(S, args, exp, "math-exp"); }

mino_val *prim_math_sin(mino_state *S, mino_val *args, mino_env *env)
{ (void)env; return math_unary(S, args, sin, "math-sin"); }

mino_val *prim_math_cos(mino_state *S, mino_val *args, mino_env *env)
{ (void)env; return math_unary(S, args, cos, "math-cos"); }

mino_val *prim_math_tan(mino_state *S, mino_val *args, mino_env *env)
{ (void)env; return math_unary(S, args, tan, "math-tan"); }

mino_val *prim_math_pow(mino_state *S, mino_val *args, mino_env *env)
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

mino_val *prim_math_atan2(mino_state *S, mino_val *args, mino_env *env)
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

/* Extra unary math wrappers for the clojure.math namespace (Clojure
 * 1.11+). Each is a thin libm bridge identical in shape to the
 * existing math_unary calls above. */
mino_val *prim_math_asin(mino_state *S, mino_val *args, mino_env *env)
{ (void)env; return math_unary(S, args, asin, "math-asin"); }
mino_val *prim_math_acos(mino_state *S, mino_val *args, mino_env *env)
{ (void)env; return math_unary(S, args, acos, "math-acos"); }
mino_val *prim_math_atan(mino_state *S, mino_val *args, mino_env *env)
{ (void)env; return math_unary(S, args, atan, "math-atan"); }
mino_val *prim_math_sinh(mino_state *S, mino_val *args, mino_env *env)
{ (void)env; return math_unary(S, args, sinh, "math-sinh"); }
mino_val *prim_math_cosh(mino_state *S, mino_val *args, mino_env *env)
{ (void)env; return math_unary(S, args, cosh, "math-cosh"); }
mino_val *prim_math_tanh(mino_state *S, mino_val *args, mino_env *env)
{ (void)env; return math_unary(S, args, tanh, "math-tanh"); }
mino_val *prim_math_log10(mino_state *S, mino_val *args, mino_env *env)
{ (void)env; return math_unary(S, args, log10, "math-log10"); }
mino_val *prim_math_log1p(mino_state *S, mino_val *args, mino_env *env)
{ (void)env; return math_unary(S, args, log1p, "math-log1p"); }
mino_val *prim_math_expm1(mino_state *S, mino_val *args, mino_env *env)
{ (void)env; return math_unary(S, args, expm1, "math-expm1"); }
mino_val *prim_math_cbrt(mino_state *S, mino_val *args, mino_env *env)
{ (void)env; return math_unary(S, args, cbrt, "math-cbrt"); }
mino_val *prim_math_signum(mino_state *S, mino_val *args, mino_env *env)
{
    double x;
    (void)env;
    if (!mino_is_cons(args) || mino_is_cons(args->as.cons.cdr))
        return prim_throw_classified(S, "eval/arity", "MAR001", "math-signum requires one argument");
    if (!as_double(args->as.cons.car, &x))
        return prim_throw_classified(S, "eval/type", "MTY001", "math-signum expects a number");
    if (x != x) return mino_float(S, x);       /* NaN -> NaN */
    if (x == 0.0) return mino_float(S, x);     /* +0/-0 preserved */
    return mino_float(S, x > 0.0 ? 1.0 : -1.0);
}
mino_val *prim_math_to_radians(mino_state *S, mino_val *args, mino_env *env)
{
    double x;
    (void)env;
    if (!mino_is_cons(args) || mino_is_cons(args->as.cons.cdr))
        return prim_throw_classified(S, "eval/arity", "MAR001", "math-to-radians requires one argument");
    if (!as_double(args->as.cons.car, &x))
        return prim_throw_classified(S, "eval/type", "MTY001", "math-to-radians expects a number");
    return mino_float(S, x * (3.14159265358979323846 / 180.0));
}
mino_val *prim_math_to_degrees(mino_state *S, mino_val *args, mino_env *env)
{
    double x;
    (void)env;
    if (!mino_is_cons(args) || mino_is_cons(args->as.cons.cdr))
        return prim_throw_classified(S, "eval/arity", "MAR001", "math-to-degrees requires one argument");
    if (!as_double(args->as.cons.car, &x))
        return prim_throw_classified(S, "eval/type", "MTY001", "math-to-degrees expects a number");
    return mino_float(S, x * (180.0 / 3.14159265358979323846));
}
mino_val *prim_math_hypot(mino_state *S, mino_val *args, mino_env *env)
{
    double a, b;
    (void)env;
    if (!mino_is_cons(args) || !mino_is_cons(args->as.cons.cdr)
        || mino_is_cons(args->as.cons.cdr->as.cons.cdr))
        return prim_throw_classified(S, "eval/arity", "MAR001", "math-hypot requires two arguments");
    if (!as_double(args->as.cons.car, &a)
        || !as_double(args->as.cons.cdr->as.cons.car, &b))
        return prim_throw_classified(S, "eval/type", "MTY001", "math-hypot expects numbers");
    return mino_float(S, hypot(a, b));
}
mino_val *prim_math_copy_sign(mino_state *S, mino_val *args, mino_env *env)
{
    double mag, sgn;
    (void)env;
    if (!mino_is_cons(args) || !mino_is_cons(args->as.cons.cdr)
        || mino_is_cons(args->as.cons.cdr->as.cons.cdr))
        return prim_throw_classified(S, "eval/arity", "MAR001", "math-copy-sign requires two arguments");
    if (!as_double(args->as.cons.car, &mag)
        || !as_double(args->as.cons.cdr->as.cons.car, &sgn))
        return prim_throw_classified(S, "eval/type", "MTY001", "math-copy-sign expects numbers");
    return mino_float(S, copysign(mag, sgn));
}
mino_val *prim_math_next_up(mino_state *S, mino_val *args, mino_env *env)
{
    double x;
    (void)env;
    if (!mino_is_cons(args) || mino_is_cons(args->as.cons.cdr))
        return prim_throw_classified(S, "eval/arity", "MAR001", "math-next-up requires one argument");
    if (!as_double(args->as.cons.car, &x))
        return prim_throw_classified(S, "eval/type", "MTY001", "math-next-up expects a number");
    return mino_float(S, nextafter(x, +1.0/0.0));
}
mino_val *prim_math_next_down(mino_state *S, mino_val *args, mino_env *env)
{
    double x;
    (void)env;
    if (!mino_is_cons(args) || mino_is_cons(args->as.cons.cdr))
        return prim_throw_classified(S, "eval/arity", "MAR001", "math-next-down requires one argument");
    if (!as_double(args->as.cons.car, &x))
        return prim_throw_classified(S, "eval/type", "MTY001", "math-next-down expects a number");
    return mino_float(S, nextafter(x, -1.0/0.0));
}
mino_val *prim_math_ieee_remainder(mino_state *S, mino_val *args, mino_env *env)
{
    double a, b;
    (void)env;
    if (!mino_is_cons(args) || !mino_is_cons(args->as.cons.cdr)
        || mino_is_cons(args->as.cons.cdr->as.cons.cdr))
        return prim_throw_classified(S, "eval/arity", "MAR001", "math-ieee-remainder requires two arguments");
    if (!as_double(args->as.cons.car, &a)
        || !as_double(args->as.cons.cdr->as.cons.car, &b))
        return prim_throw_classified(S, "eval/type", "MTY001", "math-ieee-remainder expects numbers");
    return mino_float(S, remainder(a, b));
}
