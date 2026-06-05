/*
 * gc/state.h -- per-state GC subsystem block.
 *
 * gc_state_t holds the per-state mark/sweep machinery: generation
 * lists, range index, mark stack, remembered set, threshold and
 * tuning knobs, and the optional event ring. The secondary
 * instrumentation cluster (per-phase timers, pause ring, barrier
 * counters, alloc-by-tag histogram) stays inline in mino_state.
 *
 * Internal to the runtime; embedders should only use mino.h.
 */

#ifndef GC_STATE_H
#define GC_STATE_H

#include "mino_internal.h"
#include "gc/layout.h"
#include "runtime/runtime_types.h"   /* root_env_t */

#include <stddef.h>
#include <stdint.h>

typedef struct gc_state {
    /* Generation lists: singly-linked partitions of every live header
     * by generation. Alloc prepends to all_young; minor sweep walks
     * only young (promotion moves a header between lists); major sweep
     * walks old to free dead, and young only to clear mark bits set by
     * cross-gen tracing. */
    gc_hdr_t       *all_young;
    gc_hdr_t       *all_old;
    size_t          bytes_alloc;
    size_t          bytes_live;
    size_t          threshold;
    int             stress;

    /* Root environments registered via mino_register_root_env. */
    root_env_t     *root_envs;

    /* Range index over live headers: addresses span -> owning header,
     * used by the conservative stack scan and interior-pointer mark. */
    gc_range_t     *ranges;
    size_t          ranges_len;
    size_t          ranges_cap;
    size_t          ranges_valid;
    /* Allocations between collections land here instead of memmove-ing
     * into the sorted main array on every alloc. Merged at the next
     * collection via sort-then-merge. */
    gc_range_t     *ranges_pending;
    size_t          ranges_pending_len;
    size_t          ranges_pending_cap;

    size_t          collections_minor;
    size_t          collections_major;
    size_t          total_freed;
    size_t          total_ns;       /* cumulative ns spent in gc_major_collect */
    size_t          max_ns;         /* largest single-collection ns */

    /* Generational bookkeeping: bytes_young + bytes_old == bytes_alloc.
     * old_baseline captures bytes_old right after the last major sweep;
     * future major cycles trigger when bytes_old exceeds baseline by the
     * growth-tenths factor. */
    size_t          bytes_young;
    size_t          bytes_old;
    size_t          old_baseline;

    /* Remembered set: every old-gen header that observed a store of a
     * young-gen pointer since the last minor or major cycle. */
    gc_hdr_t      **remset;
    size_t          remset_len;
    size_t          remset_cap;
    size_t          remset_high_water;

    /* Collector tuning parameters. */
    size_t          nursery_bytes;
    unsigned        promotion_age;
    unsigned        major_growth_tenths;

    /* Incremental major parameters. work_budget bounds headers popped
     * per gc_major_step slice; alloc_quantum is the allocation volume
     * required between steps; step_alloc is the running count. */
    size_t          major_work_budget;
    size_t          major_alloc_quantum;
    size_t          major_step_alloc;

    int             phase;
    gc_hdr_t      **mark_stack;
    size_t          mark_stack_len;
    size_t          mark_stack_cap;
    size_t          mark_stack_high_water;

    gc_hdr_t       *freelists[4];   /* per-size-class recycling */

    /* Cached [min, max) bounds of all managed allocations. */
    uintptr_t       heap_min;
    uintptr_t       heap_max;

    /* GC event ring buffer (diagnostic only; opt-in via MINO_GC_EVT=1
     * at state init). Allocated lazily; NULL otherwise and every
     * recording site is a no-op. */
    gc_evt_t       *evt_ring;
    uint64_t        evt_seq;
} gc_state_t;

#endif /* GC_STATE_H */
