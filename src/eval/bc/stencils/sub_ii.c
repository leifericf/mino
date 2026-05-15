/*
 * stencils/sub_ii.c -- copy-and-patch stencil for OP_SUB_II.
 *
 * Mirrors add_ii.c with subtraction. Operands of opposite sign whose
 * difference escapes the 60-bit signed inline range trigger the
 * slow path through prim_sub.
 */

#include "abi.h"
#include "runtime_layout.h"

void stencil_op_sub_ii(mino_val_t **regs, mino_val_t **consts,
                        mino_state_t *S)
{
    mino_val_t *lhs = regs[IMM_B];
    mino_val_t *rhs = regs[IMM_C];
    long long   r;
    if (__builtin_expect(MINO_IS_INT(lhs) && MINO_IS_INT(rhs), 1)) {
        r = MINO_INT_VAL(lhs) - MINO_INT_VAL(rhs);
        if (__builtin_expect(r >= MINO_INT_MIN && r <= MINO_INT_MAX, 1)) {
            regs[IMM_A] = MINO_MAKE_INT(r);
        } else {
            regs = mino_jit_binop_slow(S, regs, IMM_A, IMM_B, IMM_C,
                                       STENCIL_BINOP_SUB);
        }
    } else {
        regs = mino_jit_binop_slow(S, regs, IMM_A, IMM_B, IMM_C,
                                   STENCIL_BINOP_SUB);
    }
    MINO_STENCIL_CHAIN_RETURN(regs, consts, S);
}
