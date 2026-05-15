/*
 * stencils/lt_ik.c -- copy-and-patch stencil for OP_LT_IK.
 *
 * Mirrors add_ik.c with `<` and a bool store; the comparison has no
 * arith-overflow path.
 */

#include "abi.h"
#include "runtime_layout.h"

mino_stencil_chain_t stencil_op_lt_ik(mino_val_t **regs, mino_val_t **consts,
                                       mino_state_t *S)
{
    mino_val_t *lhs  = regs[IMM_B];
    mino_val_t *kimm = IMM_KIMM;
    if (__builtin_expect(MINO_IS_INT(lhs), 1)) {
        regs[IMM_A] = MINO_MAKE_BOOL(MINO_INT_VAL(lhs) < MINO_INT_VAL(kimm));
    } else {
        regs = mino_jit_binop_k_slow(S, regs, IMM_A, IMM_B, kimm,
                                     STENCIL_BINOP_LT);
    }
    MINO_STENCIL_CHAIN_RETURN(regs, consts, S);
}
