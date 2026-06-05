/*
 * eval/bc/jit/state.h -- per-state copy-and-patch JIT block.
 *
 * Holds the JIT executable-region list, the active JIT-invoke ctx,
 * the per-state JIT mode (AUTO / OFF / ON), and the hot threshold.
 * jit_invoke_ctx is a stencil-ABI anchor read by stencil bytes at
 * the fixed offset pinned in stencils/runtime_layout.h; the block
 * sits in mino_state's POD-only head ahead of any libc-defined
 * type, so the offset is identical across all JIT targets. Keep
 * every member of this struct libc-free POD.
 *
 * Internal to the runtime; embedders should only use mino.h.
 */

#ifndef EVAL_BC_JIT_STATE_H
#define EVAL_BC_JIT_STATE_H

#include "mino_internal.h"

struct mino_jit_region;
struct mino_thread_ctx;

typedef struct jit_state {
    /* JIT executable-region list (CPJIT). Walked by mino_state_free
     * to munmap every region. Empty until the first JIT compile. */
    struct mino_jit_region *jit_regions;

    /* Active JIT-invoke ctx. Published by mino_jit_invoke before it
     * jumps into native code and restored on return. Lets stencil-
     * emitted code reach the calling thread's ctx via a single
     * fixed-offset load from S. NULL outside an active JIT region. */
    struct mino_thread_ctx *jit_invoke_ctx;  /* stencil-ABI anchor (runtime_layout.h) */

    /* Per-state JIT mode: AUTO (default) / OFF / ON. */
    int             jit_mode;

    /* Per-state hot threshold (call count before AUTO triggers a compile). */
    unsigned        jit_hot_threshold;
} jit_state_t;

#endif /* EVAL_BC_JIT_STATE_H */
