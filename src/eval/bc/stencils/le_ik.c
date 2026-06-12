/*
 * stencils/le_ik.c -- copy-and-patch stencil for OP_LE_IK.
 *
 * Mirrors lt_ik.c with `<=`.
 */

#include "abi.h"
#include "runtime_layout.h"

void *stencil_op_le_ik(mino_val **regs, mino_val **consts,
                       mino_state *S)
{
    mino_val *lhs  = regs[IMM_B];
    mino_val *kimm = IMM_KIMM;
    if (__builtin_expect(MINO_IS_INT(lhs), 1)) {
        regs[IMM_A] = MINO_MAKE_BOOL(MINO_INT_VAL(lhs) <= MINO_INT_VAL(kimm));
    } else {
        regs = mino_jit_binop_k_slow(S, regs, IMM_A, IMM_B, kimm,
                                     STENCIL_BINOP_LE);
    }
    if (__builtin_expect(regs == NULL, 0)) return NULL;
    MINO_STENCIL_CHAIN_RETURN_PTR(regs, consts, S);
}
