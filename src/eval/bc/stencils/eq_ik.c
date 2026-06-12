/*
 * stencils/eq_ik.c -- copy-and-patch stencil for OP_EQ_IK.
 *
 * Mirrors lt_ik.c with `==`. Cross-type equality (a non-int lhs vs.
 * the tagged-int immediate) routes through prim_eq via the slow
 * helper so the comparison rules stay consistent.
 */

#include "abi.h"
#include "runtime_layout.h"

void *stencil_op_eq_ik(mino_val **regs, mino_val **consts,
                       mino_state *S)
{
    mino_val *lhs  = regs[IMM_B];
    mino_val *kimm = IMM_KIMM;
    if (__builtin_expect(MINO_IS_INT(lhs), 1)) {
        regs[IMM_A] = MINO_MAKE_BOOL(MINO_INT_VAL(lhs) == MINO_INT_VAL(kimm));
    } else {
        regs = mino_jit_binop_k_slow(S, regs, IMM_A, IMM_B, kimm,
                                     STENCIL_BINOP_EQ);
    }
    if (__builtin_expect(regs == NULL, 0)) return NULL;
    MINO_STENCIL_CHAIN_RETURN_PTR(regs, consts, S);
}
