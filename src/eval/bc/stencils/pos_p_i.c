/*
 * stencils/pos_p_i.c -- copy-and-patch stencil for OP_POS_P_I.
 *
 * Inline `pos?` for a tagged-int operand. Boxed ints / doubles /
 * non-numeric values land in prim_pos_p via the unop slow path.
 */

#include "abi.h"
#include "runtime_layout.h"

void stencil_op_pos_p_i(mino_val **regs,
                         mino_val **consts,
                         mino_state *S)
{
    mino_val *v = regs[IMM_B];
    if (__builtin_expect(MINO_IS_INT(v), 1)) {
        regs[IMM_A] = MINO_MAKE_BOOL(MINO_INT_VAL(v) > 0);
    } else {
        regs = mino_jit_unop_slow(S, regs, IMM_A, IMM_B,
                                  STENCIL_UNOP_POS_P);
    }
    MINO_STENCIL_CHAIN_RETURN(regs, consts, S);
}
