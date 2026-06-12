/*
 * stencils/lt_ii.c -- copy-and-patch stencil for OP_LT_II.
 *
 * Mirrors add_ii.c with `<` and a bool store. No arith overflow path
 * (the comparison result is binary); a tag miss bails to the slow
 * path through prim_lt.
 */

#include "abi.h"
#include "runtime_layout.h"

void *stencil_op_lt_ii(mino_val **regs, mino_val **consts,
                       mino_state *S)
{
    mino_val *lhs = regs[IMM_B];
    mino_val *rhs = regs[IMM_C];
    if (__builtin_expect(MINO_IS_INT(lhs) && MINO_IS_INT(rhs), 1)) {
        regs[IMM_A] = MINO_MAKE_BOOL(MINO_INT_VAL(lhs) < MINO_INT_VAL(rhs));
    } else {
        regs = mino_jit_binop_slow(S, regs, IMM_A, IMM_B, IMM_C,
                                   STENCIL_BINOP_LT);
    }
    if (__builtin_expect(regs == NULL, 0)) return NULL;
    MINO_STENCIL_CHAIN_RETURN_PTR(regs, consts, S);
}
