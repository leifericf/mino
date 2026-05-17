/*
 * stencils/quot_ii.c -- copy-and-patch stencil for OP_QUOT_II.
 *
 * mino's `quot` is truncating integer division (C's `/`). Inline fast
 * lane handles the tagged-int case (both operands tagged, divisor
 * non-zero, no MIN/-1 overflow); everything else falls through to
 * prim_quot via mino_jit_binop_slow.
 */

#include "abi.h"
#include "runtime_layout.h"

void stencil_op_quot_ii(mino_val_t **regs, mino_val_t **consts,
                         mino_state_t *S)
{
    mino_val_t *lhs = regs[IMM_B];
    mino_val_t *rhs = regs[IMM_C];
    if (__builtin_expect(MINO_IS_INT(lhs) && MINO_IS_INT(rhs), 1)) {
        long long a = MINO_INT_VAL(lhs);
        long long b = MINO_INT_VAL(rhs);
        if (__builtin_expect(b != 0
                             && !(a == MINO_INT_MIN && b == -1), 1)) {
            regs[IMM_A] = MINO_MAKE_INT(a / b);
        } else {
            regs = mino_jit_binop_slow(S, regs, IMM_A, IMM_B, IMM_C,
                                       STENCIL_BINOP_QUOT);
        }
    } else {
        regs = mino_jit_binop_slow(S, regs, IMM_A, IMM_B, IMM_C,
                                   STENCIL_BINOP_QUOT);
    }
    MINO_STENCIL_CHAIN_RETURN(regs, consts, S);
}
