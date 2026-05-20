/*
 * numeric_coerce.c -- type coercions: int / long / short / byte /
 * char / float / double / parse-long / parse-double. Carved out of
 * numeric.c.
 *
 * Internal to the prim subsystem; embedders should only use mino.h.
 */

#include "prim/internal.h"

#include <ctype.h>
#include <errno.h>
#include <float.h>
#include <inttypes.h>
#include <limits.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------------------- */
/* Type coercion                                                             */
/* ------------------------------------------------------------------------- */

/* Helper for narrow integer casts: extract an integer value from any
 * numeric type, checking for NaN/infinity on floats and bigdecs. The
 * caller then range-checks the result against the target tier. */
static int extract_integer_for_cast(mino_state *S, mino_val *v,
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
static mino_val *narrow_cast(mino_state *S, mino_val *v,
                               long long lo, long long hi,
                               const char *opname)
{
    long long   ll;
    const char *err = NULL;
    char        buf[160];
    /* (cast \a) -> codepoint: chars don't take the float-bound path. */
    if (v != NULL && mino_type_of(v) == MINO_CHAR) {
        long long cp = (long long)mino_val_char_get(v);
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

mino_val *prim_int(mino_state *S, mino_val *args, mino_env *env)
{
    (void)env;
    if (!mino_is_cons(args) || mino_is_cons(args->as.cons.cdr)) {
        return prim_throw_classified(S, "eval/arity", "MAR001", "int requires one argument");
    }
    return narrow_cast(S, args->as.cons.car, -2147483648LL, 2147483647LL, "int");
}

mino_val *prim_long(mino_state *S, mino_val *args, mino_env *env)
{
    mino_val *v;
    long long ll;
    const char *err = NULL;
    (void)env;
    if (!mino_is_cons(args) || mino_is_cons(args->as.cons.cdr)) {
        return prim_throw_classified(S, "eval/arity", "MAR001", "long requires one argument");
    }
    v = args->as.cons.car;
    /* (long \a) -> 97: char value yields its Unicode codepoint. */
    if (v != NULL && mino_type_of(v) == MINO_CHAR) {
        return mino_int(S, (long long)mino_val_char_get(v));
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

mino_val *prim_short(mino_state *S, mino_val *args, mino_env *env)
{
    (void)env;
    if (!mino_is_cons(args) || mino_is_cons(args->as.cons.cdr)) {
        return prim_throw_classified(S, "eval/arity", "MAR001",
                                     "short requires one argument");
    }
    return narrow_cast(S, args->as.cons.car, -32768LL, 32767LL, "short");
}

mino_val *prim_byte(mino_state *S, mino_val *args, mino_env *env)
{
    (void)env;
    if (!mino_is_cons(args) || mino_is_cons(args->as.cons.cdr)) {
        return prim_throw_classified(S, "eval/arity", "MAR001",
                                     "byte requires one argument");
    }
    return narrow_cast(S, args->as.cons.car, -128LL, 127LL, "byte");
}

mino_val *prim_char(mino_state *S, mino_val *args, mino_env *env)
{
    mino_val *v;
    long long   ll;
    const char *err = NULL;
    (void)env;
    if (!mino_is_cons(args) || mino_is_cons(args->as.cons.cdr)) {
        return prim_throw_classified(S, "eval/arity", "MAR001",
                                     "char requires one argument");
    }
    v = args->as.cons.car;
    /* (char \a) is identity. */
    if (v != NULL && mino_type_of(v) == MINO_CHAR) {
        return v;
    }
    if (!extract_integer_for_cast(S, v, &ll, &err)) {
        char buf[160];
        snprintf(buf, sizeof(buf), "char: %s",
                 err ? err : "expected an integer codepoint or char");
        return prim_throw_classified(S, "eval/type", "MTY001", buf);
    }
    /* Unicode scalar value range: 0..0x10FFFF, excluding surrogates
     * 0xD800..0xDFFF. JVM Clojure accepts the surrogate range (its
     * Character is a UTF-16 code unit), but mino's MINO_CHAR is a
     * Unicode scalar value -- the strict canon. */
    if (ll < 0 || ll > 0x10FFFFLL) {
        char buf[160];
        snprintf(buf, sizeof(buf),
                 "char: codepoint %lld out of range (0..0x10FFFF)", ll);
        return prim_throw_classified(S, "eval/bounds", "MBD001", buf);
    }
    return mino_char(S, (int)ll);
}

mino_val *prim_float(mino_state *S, mino_val *args, mino_env *env)
{
    mino_val *v;
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

mino_val *prim_double(mino_state *S, mino_val *args, mino_env *env)
{
    mino_val *v;
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

mino_val *prim_parse_long(mino_state *S, mino_val *args, mino_env *env)
{
    mino_val *v;
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

mino_val *prim_parse_double(mino_state *S, mino_val *args, mino_env *env)
{
    mino_val *v;
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

