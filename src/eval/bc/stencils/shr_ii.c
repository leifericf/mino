/*
 * stencils/shr_ii.c -- copy-and-patch stencil for OP_SHR_II.
 *
 * Inline arithmetic `bit-shift-right` for tagged-int operands. Shift
 * amount (rhs) must be in [0, 63]. Signed right shift on a tagged
 * value always stays in the tagged range (the magnitude can only
 * shrink), so no post-shift range check is needed.
 */

#include "abi.h"
#include "runtime_layout.h"

void stencil_op_shr_ii(mino_val_t **regs, mino_val_t **consts,
                        mino_state_t *S)
{
    mino_val_t *lhs = regs[IMM_B];
    mino_val_t *rhs = regs[IMM_C];
    if (__builtin_expect(MINO_IS_INT(lhs) && MINO_IS_INT(rhs), 1)) {
        long long a = MINO_INT_VAL(lhs);
        long long b = MINO_INT_VAL(rhs);
        if (__builtin_expect(b >= 0 && b < 64, 1)) {
            regs[IMM_A] = MINO_MAKE_INT(a >> b);
        } else {
            regs = mino_jit_binop_slow(S, regs, IMM_A, IMM_B, IMM_C,
                                       STENCIL_BINOP_SHR);
        }
    } else {
        regs = mino_jit_binop_slow(S, regs, IMM_A, IMM_B, IMM_C,
                                   STENCIL_BINOP_SHR);
    }
    MINO_STENCIL_CHAIN_RETURN(regs, consts, S);
}
