/*
 * stencils/getglobal_cached.c -- copy-and-patch stencil for
 * OP_GETGLOBAL_CACHED.
 *
 * OP_GETGLOBAL_CACHED A Bx loads the resolved value of the symbol at
 * `bc->ic_slots[Bx]` into regs[A]. The slot mediates the dyn / env /
 * cached / resolve cascade shared with the interpreter's handler; on
 * first hit the slot is filled under a write barrier and reused while
 * `slot->gen == S->ic_gen`.
 *
 * The stencil routes every call through `mino_jit_getglobal_cached_slow`
 * which contains the same fast-path-then-resolve logic the interpreter
 * runs inline. Keeping the cache check inside the slow helper rather
 * than re-implementing it in the stencil keeps the stencil's bytes
 * hermetic (no exposure to `mino_state_t` / `mino_bc_ic_slot_t`
 * layouts) and lets a future cycle move the hot read inline once the
 * IC-slot ABI is stable enough to be reflected here.
 *
 * Operands the JIT patches:
 *   IMM_A   -- destination register
 *   IMM_BX  -- ic-slot index (16-bit unsigned)
 *   IMM_BC  -- pointer to the owning bc fn (pool slot, runtime fixed)
 */

#include "abi.h"

mino_stencil_chain_t stencil_op_getglobal_cached(mino_val_t **regs,
                                                  mino_val_t **consts,
                                                  mino_state_t *S)
{
    regs = mino_jit_getglobal_cached_slow(S, regs,
                                          (unsigned)IMM_A,
                                          IMM_BC,
                                          (unsigned)IMM_BX);
    MINO_STENCIL_CHAIN_RETURN(regs, consts, S);
}
