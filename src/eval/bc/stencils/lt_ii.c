/*
 * stencils/lt_ii.c -- copy-and-patch stencil for OP_LT_II.
 *
 * Mirrors add_ii.c with STENCIL_BINOP_LT and prim_lt; the fast lane
 * returns mino_true / mino_false directly without allocating.
 */

#include "abi.h"

mino_stencil_chain_t stencil_op_lt_ii(mino_val_t **regs, mino_val_t **consts,
                                       mino_state_t *S)
{
    mino_val_t *r = binop_int_fast(S, regs[IMM_B], regs[IMM_C],
                                   STENCIL_BINOP_LT);
    if (__builtin_expect(r == NULL, 0)) {
        regs = mino_jit_binop_slow(S, regs, IMM_A, IMM_B, IMM_C,
                                   STENCIL_BINOP_LT);
    } else {
        regs[IMM_A] = r;
    }
    MINO_STENCIL_CHAIN_RETURN(regs, consts, S);
}
