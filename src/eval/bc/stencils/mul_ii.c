/*
 * stencils/mul_ii.c -- copy-and-patch stencil for OP_MUL_II.
 *
 * OP_MUL_II A B C is the specialised int*int form of `(* a b)`. Same
 * shape as add_ii.c with BINOP_MUL as the subop.
 *
 * Build: compiled as part of the stencil pipeline only.
 */

#include "abi.h"

mino_val_t **stencil_op_mul_ii(mino_val_t **regs, mino_val_t **consts,
                                mino_state_t *S)
{
    (void)consts;
    mino_val_t *r = binop_int_fast(S, regs[IMM_B], regs[IMM_C],
                                   STENCIL_BINOP_MUL);
    if (__builtin_expect(r == NULL, 0)) {
        return mino_jit_binop_slow(S, regs, IMM_A, IMM_B, IMM_C,
                                   STENCIL_BINOP_MUL);
    }
    regs[IMM_A] = r;
    return regs;
}
