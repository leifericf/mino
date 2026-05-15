/*
 * stencils/add_ik.c -- copy-and-patch stencil for OP_ADD_IK.
 *
 * OP_ADD_IK A B sB is the immediate-rhs form of `(+ x N)` where N is a
 * signed 8-bit literal baked into the bytecode word. The JIT patches
 * MINO_STENCIL_IMM_KIMM to a pre-tagged mino_val_t* carrying the
 * literal so the stencil hands it to binop_int_fast exactly the way
 * the II form passes regs[c].
 */

#include "abi.h"

mino_stencil_chain_t stencil_op_add_ik(mino_val_t **regs, mino_val_t **consts,
                                        mino_state_t *S)
{
    mino_val_t *kimm = IMM_KIMM;
    mino_val_t *r = binop_int_fast(S, regs[IMM_B], kimm,
                                   STENCIL_BINOP_ADD);
    if (__builtin_expect(r == NULL, 0)) {
        regs = mino_jit_binop_k_slow(S, regs, IMM_A, IMM_B, kimm,
                                     STENCIL_BINOP_ADD);
    } else {
        regs[IMM_A] = r;
    }
    MINO_STENCIL_CHAIN_RETURN(regs, consts, S);
}
