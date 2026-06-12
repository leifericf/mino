/*
 * stencils/shl_ii.c -- copy-and-patch stencil for OP_SHL_II.
 *
 * Inline `bit-shift-left` for tagged-int operands. The shift amount
 * (rhs) must be in [0, 63] -- larger amounts are UB in C for signed
 * shifts and the prim re-routes through `mino_int_wrap`-with-throw
 * for the diagnostic. The shifted result might escape the 60-bit
 * tagged range, so the range check is the same belt-and-braces guard
 * the arith stencils use.
 */

#include "abi.h"
#include "runtime_layout.h"

void *stencil_op_shl_ii(mino_val **regs, mino_val **consts,
                        mino_state *S)
{
    mino_val *lhs = regs[IMM_B];
    mino_val *rhs = regs[IMM_C];
    if (__builtin_expect(MINO_IS_INT(lhs) && MINO_IS_INT(rhs), 1)) {
        long long a = MINO_INT_VAL(lhs);
        long long b = MINO_INT_VAL(rhs);
        if (__builtin_expect(b >= 0 && b < 64, 1)) {
            long long r = (long long)((unsigned long long)a << b);
            if (__builtin_expect(r >= MINO_INT_MIN
                                 && r <= MINO_INT_MAX, 1)) {
                regs[IMM_A] = MINO_MAKE_INT(r);
            } else {
                regs = mino_jit_binop_slow(S, regs, IMM_A, IMM_B, IMM_C,
                                           STENCIL_BINOP_SHL);
            }
        } else {
            regs = mino_jit_binop_slow(S, regs, IMM_A, IMM_B, IMM_C,
                                       STENCIL_BINOP_SHL);
        }
    } else {
        regs = mino_jit_binop_slow(S, regs, IMM_A, IMM_B, IMM_C,
                                   STENCIL_BINOP_SHL);
    }
    if (__builtin_expect(regs == NULL, 0)) return NULL;
    MINO_STENCIL_CHAIN_RETURN_PTR(regs, consts, S);
}
