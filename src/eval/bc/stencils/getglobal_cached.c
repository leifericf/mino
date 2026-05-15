/*
 * stencils/getglobal_cached.c -- copy-and-patch stencil for
 * OP_GETGLOBAL_CACHED.
 *
 * OP_GETGLOBAL_CACHED A Bx loads the resolved value of the symbol at
 * `bc->ic_slots[Bx]` into regs[A]. The slot mediates the dyn / env /
 * cached / resolve cascade shared with the interpreter's handler.
 *
 * The stencil inlines the cached-hit path: read the slot, verify the
 * cached value is present, verify the slot's gen matches the current
 * `S->ic_gen` (so a defn / ns-unmap / var_set_root / var_unintern
 * since fill misses), and verify no `(binding [...] ...)` is active
 * (the dyn-shadowing branch otherwise has precedence). On hit the
 * cached value is written straight into regs[A] -- no `bl`. On miss
 * the stencil falls through to mino_jit_getglobal_cached_slow, which
 * runs the full cascade and refills the slot under the GC write
 * barrier.
 *
 * The struct field reads route through runtime_layout.h's offset
 * accessor macros so the stencil compilation unit stays hermetic
 * (no canonical runtime headers). jit.c's
 * MINO_JIT_LAYOUT_ASSERT block guards every offset against the live
 * `offsetof(...)` value so any layout drift surfaces at jit.c
 * compile time, never as a stencil mis-read.
 *
 * Operands the JIT patches:
 *   IMM_A   -- destination register
 *   IMM_BX  -- ic-slot index (16-bit unsigned)
 *   IMM_BC  -- pointer to the owning bc fn (pool slot, runtime fixed)
 */

#include "abi.h"
#include "runtime_layout.h"

mino_stencil_chain_t stencil_op_getglobal_cached(mino_val_t **regs,
                                                  mino_val_t **consts,
                                                  mino_state_t *S)
{
    mino_bc_ic_slot_t *slot = &MINO_JIT_BC_IC_SLOTS(IMM_BC)[(unsigned)IMM_BX];
    mino_thread_ctx_t *ctx  = MINO_JIT_INVOKE_CTX(S);
    if (__builtin_expect(slot->cached != NULL
                         && slot->gen == MINO_JIT_STATE_IC_GEN(S)
                         && MINO_JIT_CTX_DYN_STACK(ctx) == NULL,
                         1)) {
        regs[IMM_A] = slot->cached;
    } else {
        regs = mino_jit_getglobal_cached_slow(S, regs,
                                              (unsigned)IMM_A,
                                              IMM_BC,
                                              (unsigned)IMM_BX);
    }
    MINO_STENCIL_CHAIN_RETURN(regs, consts, S);
}
