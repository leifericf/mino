/*
 * stencils/eq_ii.c -- copy-and-patch stencil for OP_EQ_II.
 *
 * Mirrors lt_ii.c with `==` and prim_eq on the slow path. The inline
 * fast lane is sound for two tagged ints; mixed-type equality (one
 * tagged, one boxed / non-numeric) routes through the slow helper
 * which honours the cross-type comparison rules.
 */

#include "abi.h"
#include "runtime_layout.h"

mino_stencil_chain_t stencil_op_eq_ii(mino_val_t **regs, mino_val_t **consts,
                                       mino_state_t *S)
{
    mino_val_t *lhs = regs[IMM_B];
    mino_val_t *rhs = regs[IMM_C];
    if (__builtin_expect(MINO_IS_INT(lhs) && MINO_IS_INT(rhs), 1)) {
        regs[IMM_A] = MINO_MAKE_BOOL(MINO_INT_VAL(lhs) == MINO_INT_VAL(rhs));
    } else {
        regs = mino_jit_binop_slow(S, regs, IMM_A, IMM_B, IMM_C,
                                   STENCIL_BINOP_EQ);
    }
    MINO_STENCIL_CHAIN_RETURN(regs, consts, S);
}
