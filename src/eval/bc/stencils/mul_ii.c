/*
 * stencils/mul_ii.c -- copy-and-patch stencil for OP_MUL_II.
 *
 * Mirrors add_ii.c with multiplication. Two 60-bit signed inputs can
 * produce up to a 120-bit signed result; long long is only 64 bits,
 * so the raw `*` would silently overflow before the range check.
 * `__builtin_smulll_overflow` is a GCC / Clang builtin that does the
 * widening internally and returns a non-zero flag on overflow; the
 * stencil bails to the slow path on both the overflow flag and a
 * range escape past MINO_INT_MAX / MIN.
 */

#include "abi.h"
#include "runtime_layout.h"

void stencil_op_mul_ii(mino_val **regs, mino_val **consts,
                        mino_state *S)
{
    mino_val *lhs = regs[IMM_B];
    mino_val *rhs = regs[IMM_C];
    long long   r;
    int         ovf = 1;
    if (__builtin_expect(MINO_IS_INT(lhs) && MINO_IS_INT(rhs), 1)) {
        ovf = __builtin_smulll_overflow(MINO_INT_VAL(lhs),
                                        MINO_INT_VAL(rhs),
                                        &r);
        if (__builtin_expect(!ovf
                             && r >= MINO_INT_MIN
                             && r <= MINO_INT_MAX, 1)) {
            regs[IMM_A] = MINO_MAKE_INT(r);
        } else {
            regs = mino_jit_binop_slow(S, regs, IMM_A, IMM_B, IMM_C,
                                       STENCIL_BINOP_MUL);
        }
    } else {
        regs = mino_jit_binop_slow(S, regs, IMM_A, IMM_B, IMM_C,
                                   STENCIL_BINOP_MUL);
    }
    MINO_STENCIL_CHAIN_RETURN(regs, consts, S);
}
