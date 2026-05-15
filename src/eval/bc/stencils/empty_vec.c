/*
 * stencils/empty_vec.c -- copy-and-patch stencil for OP_EMPTY_VEC.
 *
 * Trampoline into mino_jit_empty_vec_slow which carries the
 * MINO_VECTOR + .len == 0 fast lane and the prim_empty_p fallback.
 *
 * Operands:
 *   IMM_A -- destination reg
 *   IMM_B -- coll reg
 */

#include "abi.h"
#include "runtime_layout.h"

void stencil_op_empty_vec(mino_val_t **regs,
                           mino_val_t **consts,
                           mino_state_t *S)
{
    regs = mino_jit_empty_vec_slow(S, regs,
                                   (unsigned)IMM_A,
                                   (unsigned)IMM_B);
    MINO_STENCIL_CHAIN_RETURN(regs, consts, S);
}
