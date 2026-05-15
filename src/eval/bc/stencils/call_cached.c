/*
 * stencils/call_cached.c -- copy-and-patch stencil for
 * OP_CALL_CACHED.
 *
 * Two-word op: word-1 carries A=arg_base / B=argc / C=dst, word-2
 * carries the IC slot index in Bx. The stencil bundles both into one
 * slow-path call. The slow helper:
 *
 *   1. resolves the callee through the bc's ic-slot cascade (the
 *      same path OP_GETGLOBAL_CACHED uses);
 *   2. dispatches via `apply_callable_argv` with the args at
 *      regs[arg_base..arg_base + argc - 1];
 *   3. stores the result at regs[dst] and returns the refreshed
 *      regs base so a GC during the callee leaves the chain's
 *      window pointer current.
 *
 * The chain-ABI macro pins x2=S at the trailing ret so the chain
 * branch hits the next stencil with the right state pointer even
 * though `apply_callable_argv` clobbered every caller-saved
 * register on the way in.
 *
 * Operands the JIT patches:
 *   IMM_A   -- arg_base (8-bit reg)
 *   IMM_B   -- argc (8-bit)
 *   IMM_C   -- dst reg (8-bit)
 *   IMM_BX2 -- slot index (16-bit, from word-2)
 *   IMM_BC  -- owning bc fn pointer (pool slot)
 */

#include "abi.h"

mino_stencil_chain_t stencil_op_call_cached(mino_val_t **regs,
                                             mino_val_t **consts,
                                             mino_state_t *S)
{
    regs = mino_jit_call_cached_slow(S, regs,
                                     (unsigned)IMM_A,    /* arg_base */
                                     (unsigned)IMM_B,    /* argc     */
                                     (unsigned)IMM_C,    /* dst      */
                                     IMM_BC,
                                     (unsigned)IMM_BX2); /* slot     */
    MINO_STENCIL_CHAIN_RETURN(regs, consts, S);
}
