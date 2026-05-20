/*
 * stencils/ushr_ii.c -- copy-and-patch stencil for OP_USHR_II.
 *
 * Inline unsigned `bit-shift-right` for tagged-int operands. A
 * negative lhs's unsigned interpretation has the high bits set; the
 * shifted result might land outside the 60-bit tagged range (e.g.
 * `(unsigned-bit-shift-right -1 1)` is 0x7fff...). The range check
 * mirrors the arith stencils.
 */

#include "abi.h"
#include "runtime_layout.h"

void stencil_op_ushr_ii(mino_val **regs, mino_val **consts,
                         mino_state *S)
{
    mino_val *lhs = regs[IMM_B];
    mino_val *rhs = regs[IMM_C];
    if (__builtin_expect(MINO_IS_INT(lhs) && MINO_IS_INT(rhs), 1)) {
        long long a = MINO_INT_VAL(lhs);
        long long b = MINO_INT_VAL(rhs);
        if (__builtin_expect(b >= 0 && b < 64, 1)) {
            long long r = (long long)((unsigned long long)a >> b);
            if (__builtin_expect(r >= MINO_INT_MIN
                                 && r <= MINO_INT_MAX, 1)) {
                regs[IMM_A] = MINO_MAKE_INT(r);
            } else {
                regs = mino_jit_binop_slow(S, regs, IMM_A, IMM_B, IMM_C,
                                           STENCIL_BINOP_USHR);
            }
        } else {
            regs = mino_jit_binop_slow(S, regs, IMM_A, IMM_B, IMM_C,
                                       STENCIL_BINOP_USHR);
        }
    } else {
        regs = mino_jit_binop_slow(S, regs, IMM_A, IMM_B, IMM_C,
                                   STENCIL_BINOP_USHR);
    }
    MINO_STENCIL_CHAIN_RETURN(regs, consts, S);
}
