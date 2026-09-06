/*
 * bignum.h -- value-model interface for the arbitrary-precision numeric
 * tower (MINO_BIGINT, MINO_RATIO, MINO_BIGDEC).
 *
 * The operations are implemented in prim/bignum.c, prim/ratio.c and
 * prim/bigdec.c, but the interface lives at the value layer because the
 * consumers are value-layer paths: the finalizer in values/gc_handlers.c
 * frees a dying bigint cell, the printer prints one, and the equality /
 * hash paths in values/val.c and collections/map_hash.c compare and hash
 * them. Declaring the interface here keeps those callers off an upward
 * include into prim/.
 *
 * Internal to the runtime; embedders should only use mino.h.
 */

#ifndef VALUES_BIGNUM_H
#define VALUES_BIGNUM_H

#include "values/internal.h"

#include <stdio.h>
#include <stdint.h>

/* ------------------------------------------------------------------------- */
/* MINO_BIGINT support.                                                      */
/* ------------------------------------------------------------------------- */

void     mino_bigint_free(mino_val *v);
void     mino_bigint_print(mino_state *S, const mino_val *v, FILE *out);
int      mino_bigint_equals(const mino_val *a, const mino_val *b);
int      mino_bigint_equals_ll(const mino_val *a, long long n);
int      mino_bigint_cmp(const mino_val *a, const mino_val *b);
uint32_t mino_bigint_hash(const mino_val *v);
mino_val *mino_bigint_from_string_n(mino_state *S, const char *s, size_t len);
/* Digits-only base-aware parse (2..36) for the reader's hex / radix
 * literal paths; `negative` applies the sign carried by the token. */
mino_val *mino_bigint_from_digits_base(mino_state *S, const char *s,
                                       size_t len, int base, int negative);
char    *mino_bigint_to_cstr(const mino_val *v);   /* malloc; caller frees */
int      mino_as_ll(const mino_val *v, long long *out);

/* Bigint arithmetic helpers for the promoting tower primitives. Each
 * accepts MINO_INT or MINO_BIGINT operands (callers classify first) and
 * returns a GC-owned MINO_BIGINT, or NULL on allocation failure (error
 * raised via prim_throw_classified). */
mino_val *mino_bigint_add(mino_state *S, const mino_val *a,
                            const mino_val *b);
mino_val *mino_bigint_sub(mino_state *S, const mino_val *a,
                            const mino_val *b);
mino_val *mino_bigint_mul(mino_state *S, const mino_val *a,
                            const mino_val *b);
mino_val *mino_bigint_neg(mino_state *S, const mino_val *a);
mino_val *mino_bigint_quot(mino_state *S, const mino_val *a,
                             const mino_val *b);
mino_val *mino_bigint_rem(mino_state *S, const mino_val *a,
                            const mino_val *b);
mino_val *mino_bigint_mod(mino_state *S, const mino_val *a,
                            const mino_val *b);
double   mino_bigint_to_double(const mino_val *v);
/* Returns non-zero if v (MINO_BIGINT) is odd. */
int      mino_bigint_is_odd(const mino_val *v);
/* Returns <0 / 0 / >0 for the sign of v (MINO_BIGINT). */
int      mino_bigint_sign(const mino_val *v);
/* Extract the low 64 bits of v (MINO_BIGINT) as a two's-complement
 * uint64_t into *out. Matches unchecked-long wrapping semantics. */
void     mino_bigint_to_bits64(const mino_val *v, uint64_t *out);

/* ------------------------------------------------------------------------- */
/* MINO_RATIO support.                                                       */
/* ------------------------------------------------------------------------- */

mino_val *mino_ratio_make(mino_state *S, mino_val *num, mino_val *denom);
mino_val *mino_ratio_make_unchecked(mino_state *S, mino_val *num,
                                      mino_val *denom);
void     mino_ratio_print(mino_state *S, const mino_val *v, FILE *out);
int      mino_ratio_equals(const mino_val *a, const mino_val *b);
int      mino_ratio_cmp(const mino_val *a, const mino_val *b);
uint32_t mino_ratio_hash(const mino_val *v);
double   mino_ratio_to_double(const mino_val *v);
mino_val *mino_ratio_add(mino_state *S, const mino_val *a,
                           const mino_val *b);
mino_val *mino_ratio_sub(mino_state *S, const mino_val *a,
                           const mino_val *b);
mino_val *mino_ratio_mul(mino_state *S, const mino_val *a,
                           const mino_val *b);
mino_val *mino_ratio_div(mino_state *S, const mino_val *a,
                           const mino_val *b);

/* ------------------------------------------------------------------------- */
/* MINO_BIGDEC support.                                                      */
/* ------------------------------------------------------------------------- */

mino_val *mino_bigdec_make(mino_state *S, mino_val *unscaled, int scale);
mino_val *mino_bigdec_quot(mino_state *S, const mino_val *a,
                             const mino_val *b);
mino_val *mino_bigdec_rem(mino_state *S, const mino_val *a,
                            const mino_val *b);
mino_val *mino_bigdec_mod(mino_state *S, const mino_val *a,
                            const mino_val *b);
void     mino_bigdec_print(mino_state *S, const mino_val *v, FILE *out);
int      mino_bigdec_equals(const mino_val *a, const mino_val *b);
int      mino_bigdec_cmp(const mino_val *a, const mino_val *b);
uint32_t mino_bigdec_hash(const mino_val *v);
double   mino_bigdec_to_double(const mino_val *v);
mino_val *mino_bigdec_add(mino_state *S, const mino_val *a,
                            const mino_val *b);
mino_val *mino_bigdec_sub(mino_state *S, const mino_val *a,
                            const mino_val *b);
mino_val *mino_bigdec_mul(mino_state *S, const mino_val *a,
                            const mino_val *b);
mino_val *mino_bigdec_div(mino_state *S, const mino_val *a,
                            const mino_val *b);
mino_val *mino_bigdec_neg(mino_state *S, const mino_val *a);
mino_val *mino_bigdec_apply_math_context(mino_state *S, mino_val *bd);

#endif /* VALUES_BIGNUM_H */
