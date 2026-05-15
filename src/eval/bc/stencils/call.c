/*
 * stencils/call.c -- copy-and-patch stencil for OP_CALL (uncached).
 *
 * OP_CALL A B C reads the callee out of regs[A], hands argv at
 * regs[A+1..A+B] to `apply_callable_argv`, and stores the result at
 * regs[C]. The uncached path is what the bytecode compiler emits
 * whenever it can't statically prove the head resolves to a global
 * (typically: head is itself an expression, or a local fn-value).
 *
 * The slow helper does the regs-base refresh after the call so a
 * GC during the callee leaves the chain pointing at the live
 * register window.
 *
 * Operands the JIT patches:
 *   IMM_A -- fn-value register
 *   IMM_B -- argc
 *   IMM_C -- destination register
 */

#include "abi.h"

mino_stencil_chain_t stencil_op_call(mino_val_t **regs,
                                      mino_val_t **consts,
                                      mino_state_t *S)
{
    regs = mino_jit_call_slow(S, regs,
                              (unsigned)IMM_A,
                              (unsigned)IMM_B,
                              (unsigned)IMM_C);
    MINO_STENCIL_CHAIN_RETURN(regs, consts, S);
}
