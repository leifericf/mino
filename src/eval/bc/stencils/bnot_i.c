/*
 * stencils/bnot_i.c -- copy-and-patch stencil for OP_BNOT_I.
 *
 * Inline `bit-not` for a tagged-int operand. `~a` on a value in the
 * 60-bit signed tagged range cannot escape the range (the operation
 * is a sign-preserving complement that keeps the magnitude bounded).
 * Non-int operands land in prim_bit_not via the unop slow path.
 */

#include "abi.h"
#include "runtime_layout.h"

void stencil_op_bnot_i(mino_val_t **regs,
                        mino_val_t **consts,
                        mino_state_t *S)
{
    mino_val_t *v = regs[IMM_B];
    if (__builtin_expect(MINO_IS_INT(v), 1)) {
        regs[IMM_A] = MINO_MAKE_INT(~MINO_INT_VAL(v));
    } else {
        regs = mino_jit_unop_slow(S, regs, IMM_A, IMM_B,
                                  STENCIL_UNOP_BNOT);
    }
    MINO_STENCIL_CHAIN_RETURN(regs, consts, S);
}
