/*
 * stencils/odd_p_i.c -- copy-and-patch stencil for OP_ODD_P_I.
 *
 * Inline `odd?` for a tagged-int operand. See even_p_i.c.
 */

#include "abi.h"
#include "runtime_layout.h"

void stencil_op_odd_p_i(mino_val **regs,
                         mino_val **consts,
                         mino_state *S)
{
    mino_val *v = regs[IMM_B];
    if (__builtin_expect(MINO_IS_INT(v), 1)) {
        regs[IMM_A] = MINO_MAKE_BOOL((MINO_INT_VAL(v) & 1) != 0);
    } else {
        regs = mino_jit_unop_slow(S, regs, IMM_A, IMM_B,
                                  STENCIL_UNOP_ODD_P);
    }
    MINO_STENCIL_CHAIN_RETURN(regs, consts, S);
}
