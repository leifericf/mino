/*
 * stencils/sub_ik.c -- copy-and-patch stencil for OP_SUB_IK.
 *
 * Mirrors add_ik.c with subtraction.
 */

#include "abi.h"
#include "runtime_layout.h"

void stencil_op_sub_ik(mino_val **regs, mino_val **consts,
                        mino_state *S)
{
    mino_val *lhs  = regs[IMM_B];
    mino_val *kimm = IMM_KIMM;
    long long   r;
    if (__builtin_expect(MINO_IS_INT(lhs), 1)) {
        r = MINO_INT_VAL(lhs) - MINO_INT_VAL(kimm);
        if (__builtin_expect(r >= MINO_INT_MIN && r <= MINO_INT_MAX, 1)) {
            regs[IMM_A] = MINO_MAKE_INT(r);
        } else {
            regs = mino_jit_binop_k_slow(S, regs, IMM_A, IMM_B, kimm,
                                         STENCIL_BINOP_SUB);
        }
    } else {
        regs = mino_jit_binop_k_slow(S, regs, IMM_A, IMM_B, kimm,
                                     STENCIL_BINOP_SUB);
    }
    MINO_STENCIL_CHAIN_RETURN(regs, consts, S);
}
