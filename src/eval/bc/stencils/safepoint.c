/*
 * stencils/safepoint.c -- copy-and-patch stencil for OP_SAFEPOINT_POLL.
 *
 * Backward-jump safepoint. The emit pass places one instance
 * immediately before every direct-emit OP_JMP / OP_JMPIFNOT whose
 * branch offset is negative, mirroring the interpreter's
 * poll-on-backward-jump rule: every loop back-edge must reach
 * mino_bc_safepoint so future-cancel lands, the state lock yields
 * under contention, and the GC's stop-the-world request is honoured.
 * The fused OP_LOOP_INT_* stencils carry their own in-register poll;
 * this stencil covers every loop shape the fusion matcher declines.
 *
 * The helper keeps a per-thread tick counter so the full poll runs
 * once per MINO_JIT_BACKJUMP_TICKS traversals; the common case is
 * one call + decrement + branch.
 *
 * On cancel with no try frame on the worker (the future-body norm)
 * the poll returns NULL instead of raising through longjmp. The
 * stencil must NOT chain that NULL into the next instance -- the
 * next stencil would dereference it -- so it exits the JIT region
 * with a real `return NULL`, the same mid-region ret the deopt
 * stencil uses. mino_jit_invoke hands the NULL to mino_bc_run,
 * which unwinds exactly like the interpreter's ok = 0 path (the
 * poll already recorded the cancel diagnostic). The success path
 * chains through the `void *` marker twin so both return paths
 * satisfy musttail's signature-match rule.
 */

#include "abi.h"

void *stencil_op_safepoint(mino_val **regs, mino_val **consts,
                            mino_state *S)
{
    regs = mino_jit_backjump_safepoint(S, regs);
    if (__builtin_expect(regs == NULL, 0)) {
        return NULL;
    }
    MINO_STENCIL_CHAIN_RETURN_PTR(regs, consts, S);
}
