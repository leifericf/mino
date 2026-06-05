/*
 * eval/bc/state.h -- per-state bytecode-VM block.
 *
 * Holds the BC register stack (bc_regs / bc_regs_cap / bc_top) plus
 * the optional pointer-tagged-int profile counters. bc_regs is a
 * stencil-ABI anchor read by stencil bytes at the fixed offset
 * pinned in stencils/runtime_layout.h; the block sits first in
 * mino_state's POD-only head ahead of any libc-defined type, so the
 * offset is identical across all JIT targets. Keep every member of
 * this struct libc-free POD.
 *
 * Internal to the runtime; embedders should only use mino.h.
 */

#ifndef EVAL_BC_STATE_H
#define EVAL_BC_STATE_H

#include "mino_internal.h"

#include <stddef.h>

typedef struct bc_vm_state {
    /* Bytecode VM register stack. The bytecode VM runs each compiled
     * fn in a slot window inside this single stack; a fn entry pushes
     * n_regs slots, a fn exit pops them. Every slot in [0, bc_top) is
     * a live GC root walked by gc_mark_roots. Allocated lazily on
     * first compile + run; NULL until then. */
    mino_val    **bc_regs;          /* stencil-ABI anchor (runtime_layout.h) */
    size_t          bc_regs_cap;
    size_t          bc_top;

    /* Pointer-tagged int counters. Only maintained when
     * MINO_BC_PROFILE_COUNTS is defined. */
    size_t          bc_int_make_count;
    size_t          bc_int_alloc_avoided;
} bc_vm_state_t;

#endif /* EVAL_BC_STATE_H */
