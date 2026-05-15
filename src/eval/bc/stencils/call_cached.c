/*
 * stencils/call_cached.c -- copy-and-patch stencil for
 * OP_CALL_CACHED.
 *
 * Two-word op: word-1 carries A=arg_base / B=argc / C=dst, word-2
 * carries the IC slot index in Bx. The stencil inlines the IC-slot
 * hit check (same shape as OP_GETGLOBAL_CACHED): on hit it hands
 * the pre-resolved callee to mino_jit_call_resolved_slow which goes
 * straight to apply_callable_argv. On miss (slot unfilled, gen
 * stale, or dyn binding active) it falls through to the existing
 * mino_jit_call_cached_slow which runs the full IC cascade.
 *
 * The actual call into apply_callable_argv is unavoidable -- it
 * walks the callable's dispatch table and may invoke arbitrary
 * mino code that triggers GC. The inline saving here is purely the
 * IC-resolve step on the hit branch.
 *
 * The chain-ABI macro pins x2=S at the trailing ret so the chain
 * branch hits the next stencil with the right state pointer even
 * though apply_callable_argv clobbered every caller-saved register.
 *
 * Operands the JIT patches:
 *   IMM_A   -- arg_base (8-bit reg)
 *   IMM_B   -- argc (8-bit)
 *   IMM_C   -- dst reg (8-bit)
 *   IMM_BX2 -- slot index (16-bit, from word-2)
 *   IMM_BC  -- owning bc fn pointer (pool slot)
 */

#include "abi.h"
#include "runtime_layout.h"

mino_stencil_chain_t stencil_op_call_cached(mino_val_t **regs,
                                             mino_val_t **consts,
                                             mino_state_t *S)
{
    mino_bc_ic_slot_t *slot = &MINO_JIT_BC_IC_SLOTS(IMM_BC)[(unsigned)IMM_BX2];
    mino_thread_ctx_t *ctx  = MINO_JIT_INVOKE_CTX(S);
    if (__builtin_expect(slot->cached != NULL
                         && slot->gen == MINO_JIT_STATE_IC_GEN(S)
                         && MINO_JIT_CTX_DYN_STACK(ctx) == NULL,
                         1)) {
        regs = mino_jit_call_resolved_slow(S, regs,
                                           slot->cached,
                                           (unsigned)IMM_A,
                                           (unsigned)IMM_B,
                                           (unsigned)IMM_C);
    } else {
        regs = mino_jit_call_cached_slow(S, regs,
                                         (unsigned)IMM_A,
                                         (unsigned)IMM_B,
                                         (unsigned)IMM_C,
                                         IMM_BC,
                                         (unsigned)IMM_BX2);
    }
    MINO_STENCIL_CHAIN_RETURN(regs, consts, S);
}
