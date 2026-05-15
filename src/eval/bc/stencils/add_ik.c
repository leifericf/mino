/*
 * stencils/add_ik.c -- copy-and-patch stencil for OP_ADD_IK.
 *
 * OP_ADD_IK A B sC is the immediate-rhs form of `(+ x N)` where N is a
 * signed 8-bit literal baked into the bytecode word. The JIT patches
 * MINO_STENCIL_IMM_KIMM to a pre-tagged mino_val_t* carrying the
 * literal so the stencil treats it as another tagged-int operand
 * without re-checking its tag. The lhs (regs[B]) still needs the tag
 * check; on a miss or arith overflow the stencil falls through to
 * mino_jit_binop_k_slow.
 */

#include "abi.h"
#include "runtime_layout.h"

mino_stencil_chain_t stencil_op_add_ik(mino_val_t **regs, mino_val_t **consts,
                                        mino_state_t *S)
{
    mino_val_t *lhs  = regs[IMM_B];
    mino_val_t *kimm = IMM_KIMM;
    long long   r;
    if (__builtin_expect(MINO_IS_INT(lhs), 1)) {
        r = MINO_INT_VAL(lhs) + MINO_INT_VAL(kimm);
        if (__builtin_expect(r >= MINO_INT_MIN && r <= MINO_INT_MAX, 1)) {
            regs[IMM_A] = MINO_MAKE_INT(r);
        } else {
            regs = mino_jit_binop_k_slow(S, regs, IMM_A, IMM_B, kimm,
                                         STENCIL_BINOP_ADD);
        }
    } else {
        regs = mino_jit_binop_k_slow(S, regs, IMM_A, IMM_B, kimm,
                                     STENCIL_BINOP_ADD);
    }
    MINO_STENCIL_CHAIN_RETURN(regs, consts, S);
}
