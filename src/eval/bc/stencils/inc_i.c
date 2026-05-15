/*
 * stencils/inc_i.c -- copy-and-patch stencil for OP_INC_I.
 *
 * OP_INC_I A B is the specialised int form of `(inc x)`. The stencil
 * tries unop_int_fast with subop UNOP_INC; on a tag miss or overflow
 * it falls back to the cons-spine + prim_inc path through
 * mino_jit_unop_slow.
 */

#include "abi.h"

mino_stencil_chain_t stencil_op_inc_i(mino_val_t **regs, mino_val_t **consts,
                                       mino_state_t *S)
{
    mino_val_t *r = unop_int_fast(S, regs[IMM_B], STENCIL_UNOP_INC);
    if (__builtin_expect(r == NULL, 0)) {
        regs = mino_jit_unop_slow(S, regs, IMM_A, IMM_B,
                                  STENCIL_UNOP_INC);
    } else {
        regs[IMM_A] = r;
    }
    MINO_STENCIL_CHAIN_RETURN(regs, consts, S);
}
