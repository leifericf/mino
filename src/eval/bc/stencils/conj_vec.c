/*
 * stencils/conj_vec.c -- copy-and-patch stencil for OP_CONJ_VEC.
 *
 * Trampoline into mino_jit_conj_vec_slow which carries the
 * MINO_VECTOR vec_conj1 fast lane and the prim_conj fallback.
 *
 * Operands:
 *   IMM_A -- destination reg
 *   IMM_B -- coll reg
 *   IMM_C -- item reg
 */

#include "abi.h"
#include "runtime_layout.h"

void *stencil_op_conj_vec(mino_val **regs,
                          mino_val **consts,
                          mino_state *S)
{
    regs = mino_jit_conj_vec_slow(S, regs,
                                  (unsigned)IMM_A,
                                  (unsigned)IMM_B,
                                  (unsigned)IMM_C);
    if (__builtin_expect(regs == NULL, 0)) return NULL;
    MINO_STENCIL_CHAIN_RETURN_PTR(regs, consts, S);
}
