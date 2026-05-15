/*
 * stencils/mul_ii.c -- copy-and-patch stencil for OP_MUL_II.
 *
 * Mirrors add_ii.c with STENCIL_BINOP_MUL and prim_mul.
 */

#include "abi.h"

mino_stencil_chain_t stencil_op_mul_ii(mino_val_t **regs, mino_val_t **consts,
                                        mino_state_t *S)
{
    mino_val_t *r = binop_int_fast(S, regs[IMM_B], regs[IMM_C],
                                   STENCIL_BINOP_MUL);
    if (__builtin_expect(r == NULL, 0)) {
        regs = mino_jit_binop_slow(S, regs, IMM_A, IMM_B, IMM_C,
                                   STENCIL_BINOP_MUL);
    } else {
        regs[IMM_A] = r;
    }
    MINO_STENCIL_CHAIN_RETURN(regs, consts, S);
}
