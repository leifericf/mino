/*
 * stencils/deopt_to_interp.c -- side-exit stencil for the JIT.
 *
 * The JIT compile path plants this stencil after the supported native
 * prefix when the eligibility classifier returns OK_WITH_DEOPT (a fn
 * whose first unstenciled op sits at PC > 0). The stencil body sets
 * the state's `jit_deopt_pending` flag and writes the resume PC to
 * `jit_deopt_pc`, then returns NULL.
 *
 * The returned NULL doubles as the deopt sentinel: mino_jit_invoke
 * detects the (NULL, pending) pair, clears the flag, and tail-calls
 * mino_bc_run_resume to drive the interpreter from `jit_deopt_pc` to
 * the body's OP_RETURN.
 *
 * The resume PC is patched into the stencil via the existing IMM_BX
 * pool slot. Bx is 16-bit so deopt PCs above 65535 are rejected by
 * the classifier before they reach this stencil.
 *
 * Build: compiled as part of the stencil pipeline only. Final stencil
 * (preserves trailing `ret`), so no chain-continue tail.
 */

#include "abi.h"

extern mino_val *mino_jit_deopt_exit(mino_state *S, unsigned long resume_pc);

void *stencil_op_deopt_to_interp(mino_val **regs,
                                 mino_val **consts,
                                 mino_state *S)
{
    (void)regs;
    (void)consts;
    return mino_jit_deopt_exit(S, IMM_BX);
}
