/*
 * stencils/tailcall.c -- copy-and-patch stencil for OP_TAILCALL.
 *
 * OP_TAILCALL is a FINAL stencil: it exits the JIT region by
 * returning `&S->tail_call_sentinel` through the fn's natural ret.
 * The slow helper builds the args cons list head-first from the
 * register window, publishes (callee, args) on the sentinel, and
 * returns the sentinel pointer. mino_bc_run's caller -- the
 * trampoline loop in apply_callable -- spots the sentinel and
 * re-dispatches into the new (fn, args) pair without growing the C
 * stack.
 *
 * Operands the JIT patches:
 *   IMM_A -- fn-value register
 *   IMM_B -- argc
 */

#include "abi.h"

mino_val *stencil_op_tailcall(mino_val **regs,
                                mino_val **consts,
                                mino_state *S)
{
    (void)consts;
    return mino_jit_tailcall_slow(S, regs,
                                  (unsigned)IMM_A,
                                  (unsigned)IMM_B);
}
