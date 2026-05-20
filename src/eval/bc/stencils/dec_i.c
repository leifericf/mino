/*
 * stencils/dec_i.c -- copy-and-patch stencil for OP_DEC_I.
 *
 * Mirrors inc_i.c with subtraction. The 60-bit signed inline-int
 * range underflows at MINO_INT_MIN; on tag miss or underflow the
 * stencil falls through to `mino_jit_unop_slow` for the prim_dec
 * path.
 */

#include "abi.h"
#include "runtime_layout.h"

void stencil_op_dec_i(mino_val **regs, mino_val **consts,
                       mino_state *S)
{
    mino_val *v = regs[IMM_B];
    long long   r;
    if (__builtin_expect(MINO_IS_INT(v), 1)) {
        r = MINO_INT_VAL(v) - 1;
        if (__builtin_expect(r >= MINO_INT_MIN, 1)) {
            regs[IMM_A] = MINO_MAKE_INT(r);
        } else {
            regs = mino_jit_unop_slow(S, regs, IMM_A, IMM_B,
                                      STENCIL_UNOP_DEC);
        }
    } else {
        regs = mino_jit_unop_slow(S, regs, IMM_A, IMM_B,
                                  STENCIL_UNOP_DEC);
    }
    MINO_STENCIL_CHAIN_RETURN(regs, consts, S);
}
