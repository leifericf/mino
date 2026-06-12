/*
 * stencils/band_ii.c -- copy-and-patch stencil for OP_BAND_II.
 *
 * Inline `bit-and` for tagged-int operands. The tagged-int
 * representation embeds the value in the high 61 bits; the bitwise
 * AND of two tagged values cleanly produces another tagged value
 * (their tag bits agree), so the inline fast lane is a single AND
 * with no overflow guard. Type miss falls through to prim_bit_and
 * via mino_jit_binop_slow.
 */

#include "abi.h"
#include "runtime_layout.h"

void *stencil_op_band_ii(mino_val **regs, mino_val **consts,
                         mino_state *S)
{
    mino_val *lhs = regs[IMM_B];
    mino_val *rhs = regs[IMM_C];
    if (__builtin_expect(MINO_IS_INT(lhs) && MINO_IS_INT(rhs), 1)) {
        long long a = MINO_INT_VAL(lhs);
        long long b = MINO_INT_VAL(rhs);
        regs[IMM_A] = MINO_MAKE_INT(a & b);
    } else {
        regs = mino_jit_binop_slow(S, regs, IMM_A, IMM_B, IMM_C,
                                   STENCIL_BINOP_BAND);
    }
    if (__builtin_expect(regs == NULL, 0)) return NULL;
    MINO_STENCIL_CHAIN_RETURN_PTR(regs, consts, S);
}
