/*
 * stencils/even_p_i.c -- copy-and-patch stencil for OP_EVEN_P_I.
 *
 * Inline `even?` for a tagged-int operand. Boxed ints (via bigint) /
 * non-integer values land in prim_even_p via the unop slow path.
 */

#include "abi.h"
#include "runtime_layout.h"

void stencil_op_even_p_i(mino_val_t **regs,
                          mino_val_t **consts,
                          mino_state_t *S)
{
    mino_val_t *v = regs[IMM_B];
    if (__builtin_expect(MINO_IS_INT(v), 1)) {
        regs[IMM_A] = MINO_MAKE_BOOL((MINO_INT_VAL(v) & 1) == 0);
    } else {
        regs = mino_jit_unop_slow(S, regs, IMM_A, IMM_B,
                                  STENCIL_UNOP_EVEN_P);
    }
    MINO_STENCIL_CHAIN_RETURN(regs, consts, S);
}
