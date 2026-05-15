/*
 * stencils/lt_ii.c -- copy-and-patch stencil for OP_LT_II.
 *
 * OP_LT_II A B C is the specialised int<int form of `(< a b)`. Same
 * shape as the arith stencils -- tagged-int fast lane via
 * binop_int_fast, cons-spine fallback via mino_jit_binop_slow --
 * with BINOP_LT as the subop. The fast lane returns the mino_true /
 * mino_false sentinel without allocating; only the cold path
 * (non-int operands) hits the prim.
 *
 * Build: compiled as part of the stencil pipeline only.
 */

#include "abi.h"

mino_val_t **stencil_op_lt_ii(mino_val_t **regs, mino_val_t **consts,
                               mino_state_t *S)
{
    (void)consts;
    mino_val_t *r = binop_int_fast(S, regs[IMM_B], regs[IMM_C],
                                   STENCIL_BINOP_LT);
    if (__builtin_expect(r == NULL, 0)) {
        return mino_jit_binop_slow(S, regs, IMM_A, IMM_B, IMM_C,
                                   STENCIL_BINOP_LT);
    }
    regs[IMM_A] = r;
    return regs;
}
