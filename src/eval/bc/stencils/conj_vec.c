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

mino_stencil_chain_t stencil_op_conj_vec(mino_val_t **regs,
                                          mino_val_t **consts,
                                          mino_state_t *S)
{
    regs = mino_jit_conj_vec_slow(S, regs,
                                  (unsigned)IMM_A,
                                  (unsigned)IMM_B,
                                  (unsigned)IMM_C);
    MINO_STENCIL_CHAIN_RETURN(regs, consts, S);
}
