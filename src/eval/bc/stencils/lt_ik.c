/*
 * stencils/lt_ik.c -- copy-and-patch stencil for OP_LT_IK.
 *
 * Mirrors add_ik.c with STENCIL_BINOP_LT and prim_lt.
 */

#include "abi.h"

mino_stencil_chain_t stencil_op_lt_ik(mino_val_t **regs, mino_val_t **consts,
                                       mino_state_t *S)
{
    mino_val_t *kimm = IMM_KIMM;
    mino_val_t *r = binop_int_fast(S, regs[IMM_B], kimm,
                                   STENCIL_BINOP_LT);
    if (__builtin_expect(r == NULL, 0)) {
        regs = mino_jit_binop_k_slow(S, regs, IMM_A, IMM_B, kimm,
                                     STENCIL_BINOP_LT);
    } else {
        regs[IMM_A] = r;
    }
    MINO_STENCIL_CHAIN_RETURN(regs, consts, S);
}
