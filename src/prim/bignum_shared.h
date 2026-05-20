/*
 * prim/bignum_shared.h -- internal helpers shared between bignum.c
 * (bigint, ratio) and bigdec.c.
 *
 * The bigdec implementation reaches for `to_bigint` (convert any
 * numeric value to a bigint), `bigint_alloc_zeroed` (mp_int factory),
 * and `bigint_wrap` (mp_int -> MINO_BIGINT cell) so that scale-shift
 * and div/rem can stage their work through the bigint payload.
 *
 * Internal to the prim subsystem; embedders should only use mino.h.
 */

#ifndef PRIM_BIGNUM_SHARED_H
#define PRIM_BIGNUM_SHARED_H

#include "mino_internal.h"
#include "imath.h"

/* mp_int factory used by bigdec arithmetic to allocate intermediate
 * scratch payloads. Caller frees with mp_int_clear + free on the
 * pointer when no longer needed. Returns NULL on OOM. */
mp_int bigint_alloc_zeroed(void);

/* Wrap an mp_int payload as a MINO_BIGINT GC value. The payload is
 * adopted (the resulting cell owns it). */
mino_val *bigint_wrap(mino_state *S, mp_int z);

/* Convert a numeric value to a MINO_BIGINT. Accepts MINO_INT or
 * MINO_BIGINT; returns the input on bigint identity, or a fresh
 * bigint cell for ints. Returns NULL on bad input or OOM. */
mino_val *to_bigint(mino_state *S, const mino_val *v);

/* Shortest round-tripping double->string formatter (printf-%g-style
 * fast path with NaN / Infinity canonical tokens). Out buffer must
 * hold at least 32 bytes. Shared so ratio.c and bigdec.c print using
 * the same float-rendering rules as bigint.c. */
int mino_double_shortest(double d, char *out, size_t outsz);

#endif /* PRIM_BIGNUM_SHARED_H */
