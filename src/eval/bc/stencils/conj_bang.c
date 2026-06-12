/*
 * stencils/conj_bang.c -- copy-and-patch stencil for OP_CONJ_BANG.
 *
 * OP_CONJ_BANG A B C  ::=  regs[A] := (conj! regs[B] regs[C]).
 *
 * Arity-2 fast lane: tcoll in regs[B], item in regs[C], dst is A.
 * The slow helper routes a valid transient through mino_conj_bang
 * and falls through to prim_conj_bang on anything else.
 */

#include "abi.h"
#include "runtime_layout.h"

void *stencil_op_conj_bang(mino_val **regs,
                          mino_val **consts,
                          mino_state *S)
{
    regs = mino_jit_conj_bang_slow(S, regs,
                                   (unsigned)IMM_A,
                                   (unsigned)IMM_B,
                                   (unsigned)IMM_C);
    if (__builtin_expect(regs == NULL, 0)) return NULL;
    MINO_STENCIL_CHAIN_RETURN_PTR(regs, consts, S);
}
