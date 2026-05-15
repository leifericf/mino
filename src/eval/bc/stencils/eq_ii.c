/*
 * stencils/eq_ii.c -- copy-and-patch stencil for OP_EQ_II.
 *
 * Mirrors lt_ii.c with STENCIL_BINOP_EQ and prim_eq. The fast lane is
 * sound for two tagged ints; mixed-type equality routes through the
 * prim to honour cross-type comparison rules.
 */

#include "abi.h"

mino_stencil_chain_t stencil_op_eq_ii(mino_val_t **regs, mino_val_t **consts,
                                       mino_state_t *S)
{
    mino_val_t *r = binop_int_fast(S, regs[IMM_B], regs[IMM_C],
                                   STENCIL_BINOP_EQ);
    if (__builtin_expect(r == NULL, 0)) {
        regs = mino_jit_binop_slow(S, regs, IMM_A, IMM_B, IMM_C,
                                   STENCIL_BINOP_EQ);
    } else {
        regs[IMM_A] = r;
    }
    MINO_STENCIL_CHAIN_RETURN(regs, consts, S);
}
