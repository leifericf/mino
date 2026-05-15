/*
 * stencils/env_bind.c -- copy-and-patch stencil for OP_ENV_BIND.
 *
 * Publishes regs[A] under the symbol at bc->consts[Bx] in the
 * current JIT-invoke env. The compiler emits OP_ENV_BIND alongside
 * OP_PUSH_ENV / OP_POP_ENV when a let / fn-param scope needs to be
 * visible to a nested OP_CLOSURE that captures the current env.
 *
 * Operands the JIT patches:
 *   IMM_A  -- source register
 *   IMM_BX -- symbol const-pool index
 *   IMM_BC -- pointer to the owning bc fn
 */

#include "abi.h"

void stencil_op_env_bind(mino_val_t **regs,
                          mino_val_t **consts,
                          mino_state_t *S)
{
    regs = mino_jit_env_bind_slow(S, regs,
                                  (unsigned)IMM_A,
                                  IMM_BC,
                                  (unsigned)IMM_BX);
    MINO_STENCIL_CHAIN_RETURN(regs, consts, S);
}
