/*
 * stencils/add_ik.c -- copy-and-patch stencil for OP_ADD_IK.
 *
 * OP_ADD_IK A B sB is the immediate-rhs form of `(+ x N)` where N is a
 * signed 8-bit literal baked into the bytecode word. The JIT patches
 * MINO_STENCIL_IMM_KIMM to a pre-tagged mino_val_t* carrying the
 * literal so the stencil hands it to binop_int_fast exactly the way
 * the II form passes regs[c]. On a tag miss the slow helper conses
 * the literal directly onto the spine and dispatches to prim_add.
 */

#include "abi.h"

mino_val_t **stencil_op_add_ik(mino_val_t **regs, mino_val_t **consts,
                                mino_state_t *S)
{
    (void)consts;
    mino_val_t *kimm = IMM_KIMM;
    mino_val_t *r = binop_int_fast(S, regs[IMM_B], kimm,
                                   STENCIL_BINOP_ADD);
    if (__builtin_expect(r == NULL, 0)) {
        return mino_jit_binop_k_slow(S, regs, IMM_A, IMM_B, kimm,
                                     STENCIL_BINOP_ADD);
    }
    regs[IMM_A] = r;
    return regs;
}
