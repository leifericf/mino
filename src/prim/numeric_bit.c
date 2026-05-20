/*
 * numeric_bit.c -- bit-and / bit-or / bit-xor / bit-not /
 * bit-shift-left / bit-shift-right / unsigned-bit-shift-right.
 * Carved out of numeric.c.
 *
 * Internal to the prim subsystem; embedders should only use mino.h.
 */

#include "prim/internal.h"

#include <stdio.h>
#include <string.h>


/* ------------------------------------------------------------------------- */
/* Bitwise operations                                                        */
/* ------------------------------------------------------------------------- */

mino_val *prim_bit_and(mino_state *S, mino_val *args, mino_env *env)
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
    return mino_int_wrap(S, a & b);
}

mino_val *prim_bit_or(mino_state *S, mino_val *args, mino_env *env)
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
    return mino_int_wrap(S, a | b);
}

mino_val *prim_bit_xor(mino_state *S, mino_val *args, mino_env *env)
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
    return mino_int_wrap(S, a ^ b);
}

mino_val *prim_bit_not(mino_state *S, mino_val *args, mino_env *env)
{
    long long a;
    (void)env;
    if (!mino_is_cons(args) || mino_is_cons(args->as.cons.cdr)) {
        return prim_throw_classified(S, "eval/arity", "MAR001", "bit-not requires one argument");
    }
    if (!as_long(args->as.cons.car, &a)) {
        return prim_throw_classified(S, "eval/type", "MTY001", "bit-not expects an integer");
    }
    return mino_int_wrap(S, ~a);
}

/* 64-bit shift amounts must live in [0, 63]; any other value is UB per
 * C99 (shift exponent negative or >= type width). Validate once in a
 * shared helper so each prim enforces the same bounds. */
#define MINO_SHIFT_WIDTH 64

static int shift_amount_ok(long long b)
{
    return b >= 0 && b < MINO_SHIFT_WIDTH;
}

mino_val *prim_bit_shift_left(mino_state *S, mino_val *args, mino_env *env)
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
     * signed-overflow UB. The shift is defined to wrap, never promote,
     * so build the result via the deterministic int constructor. */
    return mino_int_wrap(S, (long long)((unsigned long long)a << b));
}

mino_val *prim_bit_shift_right(mino_state *S, mino_val *args, mino_env *env)
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
    return mino_int_wrap(S, a >> b);
}

mino_val *prim_unsigned_bit_shift_right(mino_state *S, mino_val *args, mino_env *env)
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
    return mino_int_wrap(S, (long long)((unsigned long long)a >> b));
}

