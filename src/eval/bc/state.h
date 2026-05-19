/*
 * eval/bc/state.h -- per-state bytecode-VM block.
 *
 * Holds the BC register stack (bc_regs / bc_regs_cap / bc_top) plus
 * the optional pointer-tagged-int profile counters. bc_regs sits at
 * the stencil-ABI-pinned offset 47888 inside mino_state; the
 * embedded sub-struct is placed so the absolute offset is byte-stable.
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
    mino_val_t    **bc_regs;          /* stencil-ABI-pinned offset 47888 */
    size_t          bc_regs_cap;
    size_t          bc_top;

    /* Pointer-tagged int counters. Only maintained when
     * MINO_BC_PROFILE_COUNTS is defined. */
    size_t          bc_int_make_count;
    size_t          bc_int_alloc_avoided;
} bc_vm_state_t;

#endif /* EVAL_BC_STATE_H */
