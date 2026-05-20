/*
 * stencils/inc_i.c -- copy-and-patch stencil for OP_INC_I.
 *
 * OP_INC_I A B is the specialised int form of `(inc x)`. The stencil
 * inlines the tagged-int fast lane: verify the operand carries the
 * INT tag, add one, verify the result fits the 60-bit signed inline
 * range, and store the freshly-tagged result. On tag miss or
 * overflow the stencil falls through to `mino_jit_unop_slow` which
 * routes through the prim_inc path so non-int operands surface a
 * diagnostic and overflow values box to a heap MINO_INT.
 */

#include "abi.h"
#include "runtime_layout.h"

void stencil_op_inc_i(mino_val **regs, mino_val **consts,
                       mino_state *S)
{
    mino_val *v = regs[IMM_B];
    long long   r;
    if (__builtin_expect(MINO_IS_INT(v), 1)) {
        r = MINO_INT_VAL(v) + 1;
        if (__builtin_expect(r <= MINO_INT_MAX, 1)) {
            regs[IMM_A] = MINO_MAKE_INT(r);
        } else {
            regs = mino_jit_unop_slow(S, regs, IMM_A, IMM_B,
                                      STENCIL_UNOP_INC);
        }
    } else {
        regs = mino_jit_unop_slow(S, regs, IMM_A, IMM_B,
                                  STENCIL_UNOP_INC);
    }
    MINO_STENCIL_CHAIN_RETURN(regs, consts, S);
}
