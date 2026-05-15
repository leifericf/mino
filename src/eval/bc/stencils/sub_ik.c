/*
 * stencils/sub_ik.c -- copy-and-patch stencil for OP_SUB_IK.
 *
 * Mirrors add_ik.c with STENCIL_BINOP_SUB and prim_sub.
 */

#include "abi.h"

mino_val_t **stencil_op_sub_ik(mino_val_t **regs, mino_val_t **consts,
                                mino_state_t *S)
{
    (void)consts;
    mino_val_t *kimm = IMM_KIMM;
    mino_val_t *r = binop_int_fast(S, regs[IMM_B], kimm,
                                   STENCIL_BINOP_SUB);
    if (__builtin_expect(r == NULL, 0)) {
        return mino_jit_binop_k_slow(S, regs, IMM_A, IMM_B, kimm,
                                     STENCIL_BINOP_SUB);
    }
    regs[IMM_A] = r;
    return regs;
}
