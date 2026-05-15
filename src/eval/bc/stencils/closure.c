/*
 * stencils/closure.c -- copy-and-patch stencil for OP_CLOSURE.
 *
 * OP_CLOSURE A Bx builds a fresh closure value from the child fn at
 * bc->consts[Bx], capturing the current JIT-invoke env, and stores
 * the result at regs[A]. The slow helper mirrors the interpreter's
 * OP_CLOSURE handler so the resulting closure is bit-identical to
 * the interpreter-built one for the same input.
 *
 * Operands the JIT patches:
 *   IMM_A  -- destination register
 *   IMM_BX -- child fn const-pool index
 *   IMM_BC -- pointer to the owning bc fn
 */

#include "abi.h"

mino_stencil_chain_t stencil_op_closure(mino_val_t **regs,
                                         mino_val_t **consts,
                                         mino_state_t *S)
{
    regs = mino_jit_closure_slow(S, regs,
                                 (unsigned)IMM_A,
                                 IMM_BC,
                                 (unsigned)IMM_BX);
    MINO_STENCIL_CHAIN_RETURN(regs, consts, S);
}
