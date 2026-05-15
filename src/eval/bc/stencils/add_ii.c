/*
 * stencils/add_ii.c -- copy-and-patch stencil for OP_ADD_II.
 *
 * OP_ADD_II A B C is the specialised int+int form of `(+ a b)`. The
 * stencil inlines the tagged-int fast lane: verify both operands
 * carry the INT tag, add their values in 64-bit signed, verify the
 * result fits the 60-bit signed inline-int range, and store the
 * tagged result in regs[A]. On a tag miss (boxed int, non-numeric)
 * or arith overflow the stencil falls through to
 * mino_jit_binop_slow which routes through prim_add for the boxed-
 * int / bigint-promotion / diagnostic paths.
 */

#include "abi.h"
#include "runtime_layout.h"

void stencil_op_add_ii(mino_val_t **regs, mino_val_t **consts,
                        mino_state_t *S)
{
    mino_val_t *lhs = regs[IMM_B];
    mino_val_t *rhs = regs[IMM_C];
    long long   r;
    if (__builtin_expect(MINO_IS_INT(lhs) && MINO_IS_INT(rhs), 1)) {
        r = MINO_INT_VAL(lhs) + MINO_INT_VAL(rhs);
        if (__builtin_expect(r >= MINO_INT_MIN && r <= MINO_INT_MAX, 1)) {
            regs[IMM_A] = MINO_MAKE_INT(r);
        } else {
            regs = mino_jit_binop_slow(S, regs, IMM_A, IMM_B, IMM_C,
                                       STENCIL_BINOP_ADD);
        }
    } else {
        regs = mino_jit_binop_slow(S, regs, IMM_A, IMM_B, IMM_C,
                                   STENCIL_BINOP_ADD);
    }
    MINO_STENCIL_CHAIN_RETURN(regs, consts, S);
}
