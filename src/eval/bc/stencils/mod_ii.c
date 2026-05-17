/*
 * stencils/mod_ii.c -- copy-and-patch stencil for OP_MOD_II.
 *
 * mino's `mod` follows JVM Clojure: the sign of the result matches
 * the sign of the divisor. Inline fast lane handles the tagged-int
 * case (both operands tagged, divisor non-zero, no MIN/-1 overflow);
 * everything else lands in mino_jit_binop_slow which routes through
 * prim_mod for the boxed / bigint / double / diagnostic paths.
 */

#include "abi.h"
#include "runtime_layout.h"

void stencil_op_mod_ii(mino_val_t **regs, mino_val_t **consts,
                        mino_state_t *S)
{
    mino_val_t *lhs = regs[IMM_B];
    mino_val_t *rhs = regs[IMM_C];
    if (__builtin_expect(MINO_IS_INT(lhs) && MINO_IS_INT(rhs), 1)) {
        long long a = MINO_INT_VAL(lhs);
        long long b = MINO_INT_VAL(rhs);
        if (__builtin_expect(b != 0
                             && !(a == MINO_INT_MIN && b == -1), 1)) {
            long long r = a % b;
            if (r != 0 && ((r < 0) != (b < 0))) r += b;
            regs[IMM_A] = MINO_MAKE_INT(r);
        } else {
            regs = mino_jit_binop_slow(S, regs, IMM_A, IMM_B, IMM_C,
                                       STENCIL_BINOP_MOD);
        }
    } else {
        regs = mino_jit_binop_slow(S, regs, IMM_A, IMM_B, IMM_C,
                                   STENCIL_BINOP_MOD);
    }
    MINO_STENCIL_CHAIN_RETURN(regs, consts, S);
}
