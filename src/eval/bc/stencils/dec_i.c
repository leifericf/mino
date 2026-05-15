/*
 * stencils/dec_i.c -- copy-and-patch stencil for OP_DEC_I.
 *
 * Mirrors inc_i.c with UNOP_DEC; prim_dec covers the overflow path.
 */

#include "abi.h"

mino_stencil_chain_t stencil_op_dec_i(mino_val_t **regs, mino_val_t **consts,
                                       mino_state_t *S)
{
    mino_val_t *r = unop_int_fast(S, regs[IMM_B], STENCIL_UNOP_DEC);
    if (__builtin_expect(r == NULL, 0)) {
        regs = mino_jit_unop_slow(S, regs, IMM_A, IMM_B,
                                  STENCIL_UNOP_DEC);
    } else {
        regs[IMM_A] = r;
    }
    MINO_STENCIL_CHAIN_RETURN(regs, consts, S);
}
