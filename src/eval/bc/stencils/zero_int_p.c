/*
 * stencils/zero_int_p.c -- copy-and-patch stencil for OP_ZERO_INT_P.
 *
 * OP_ZERO_INT_P A B is the specialised int form of `(zero? x)`. The
 * stencil inlines the tagged-int fast lane: verify the operand
 * carries the INT tag, compare its value to zero, and store the
 * tagged-bool result. On tag miss the stencil falls through to
 * `mino_jit_unop_slow` for the prim_zero_p path which handles
 * boxed ints, doubles, and the non-numeric diagnostic.
 */

#include "abi.h"
#include "runtime_layout.h"

mino_stencil_chain_t stencil_op_zero_int_p(mino_val_t **regs,
                                            mino_val_t **consts,
                                            mino_state_t *S)
{
    mino_val_t *v = regs[IMM_B];
    if (__builtin_expect(MINO_IS_INT(v), 1)) {
        regs[IMM_A] = MINO_MAKE_BOOL(MINO_INT_VAL(v) == 0);
    } else {
        regs = mino_jit_unop_slow(S, regs, IMM_A, IMM_B,
                                  STENCIL_UNOP_ZERO_P);
    }
    MINO_STENCIL_CHAIN_RETURN(regs, consts, S);
}
