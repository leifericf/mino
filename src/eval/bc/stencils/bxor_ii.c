/*
 * stencils/bxor_ii.c -- copy-and-patch stencil for OP_BXOR_II.
 *
 * Inline `bit-xor` for tagged-int operands. Same shape as band_ii.c.
 */

#include "abi.h"
#include "runtime_layout.h"

void *stencil_op_bxor_ii(mino_val **regs, mino_val **consts,
                         mino_state *S)
{
    mino_val *lhs = regs[IMM_B];
    mino_val *rhs = regs[IMM_C];
    if (__builtin_expect(MINO_IS_INT(lhs) && MINO_IS_INT(rhs), 1)) {
        long long a = MINO_INT_VAL(lhs);
        long long b = MINO_INT_VAL(rhs);
        regs[IMM_A] = MINO_MAKE_INT(a ^ b);
    } else {
        regs = mino_jit_binop_slow(S, regs, IMM_A, IMM_B, IMM_C,
                                   STENCIL_BINOP_BXOR);
    }
    if (__builtin_expect(regs == NULL, 0)) return NULL;
    MINO_STENCIL_CHAIN_RETURN_PTR(regs, consts, S);
}
