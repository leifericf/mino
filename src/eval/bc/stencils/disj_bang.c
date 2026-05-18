/*
 * stencils/disj_bang.c -- copy-and-patch stencil for OP_DISJ_BANG.
 *
 * OP_DISJ_BANG A B C  ::=  regs[A] := (disj! regs[B] regs[C]).
 *
 * Arity-2 fast lane: tcoll in regs[B], item in regs[C], dst is A.
 */

#include "abi.h"
#include "runtime_layout.h"

void stencil_op_disj_bang(mino_val_t **regs,
                          mino_val_t **consts,
                          mino_state_t *S)
{
    regs = mino_jit_disj_bang_slow(S, regs,
                                   (unsigned)IMM_A,
                                   (unsigned)IMM_B,
                                   (unsigned)IMM_C);
    MINO_STENCIL_CHAIN_RETURN(regs, consts, S);
}
