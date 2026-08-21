/*
 * gc/layout.h -- shared GC substrate: header layout, tag enum,
 * generation + phase enums, range index entry, bump slab.
 *
 * Carved out of gc/internal.h so collection-side tracers (and any
 * other component that owns a per-tag tracer) can see gc_hdr_t,
 * the GC_T_* tag, and the gc_range_t shape without including the
 * full driver/barrier surface.
 *
 * Internal to the runtime; embedders should only use mino.h.
 */

#ifndef GC_LAYOUT_H
#define GC_LAYOUT_H

#include "mino_internal.h"

#include <stddef.h>
#include <stdint.h>

/* ------------------------------------------------------------------------- */
/* GC tag types                                                              */
/* ------------------------------------------------------------------------- */

enum {
    GC_T_RAW        = 1,
    GC_T_VAL        = 2,
    GC_T_ENV        = 3,
    GC_T_VEC_NODE   = 4,
    GC_T_HAMT_NODE  = 5,
    GC_T_HAMT_ENTRY = 6,
    GC_T_PTRARR     = 7,
    GC_T_VALARR     = 8,
    GC_T_RB_NODE    = 9,
    GC_T_BC         = 10,
    GC_T__COUNT     = 16   /* histogram array bound; covers all current
                            * tags + slack for one round of growth before
                            * the array needs resizing. */
};

/* Generation tags for the two-generation collector. The OLD value is
 * set when a nursery survivor outlives the promotion age. gc_alloc_typed
 * stamps every new allocation GC_GEN_YOUNG. */
enum {
    GC_GEN_YOUNG = 0,
    GC_GEN_OLD   = 1
};

/* Collector phase. IDLE: no cycle in flight. MINOR: young-only
 * mark-and-sweep; gc_mark_push filters OLD out of the frontier so
 * tracing stays proportional to young reachability. MAJOR_MARK: major
 * tracing, possibly sliced across many gc_major_step calls interleaved
 * with mutator progress; the SATB write barrier is armed. MAJOR_SWEEP:
 * one-shot STW sweep of dead OLD objects, always runs directly after
 * the final remark drain. */
enum {
    GC_PHASE_IDLE        = 0,
    GC_PHASE_MINOR       = 1,
    GC_PHASE_MAJOR_MARK  = 2,
    GC_PHASE_MAJOR_SWEEP = 3
};

/* ------------------------------------------------------------------------- */
/* Header layout                                                             */
/* ------------------------------------------------------------------------- */

/* Header layout on a 64-bit target: 1+1+1+1 bytes followed by 4 bytes
 * of padding, then 8-byte size and 8-byte next. Four single-byte fields
 * fit into the padding slot that existed for type_tag/mark alone; the
 * struct size is unchanged. The `bump` field tags headers carved from
 * a bump-allocator slab: those headers are never free()d on sweep --
 * the memory belongs to the aligned slab region, which is returned to
 * the OS once every header carved from it has been recycled (see
 * gc_bump_slab_t). Bump-origin headers DO round-trip through the
 * per-size-class freelist; gc_alloc_raw preserves the flag across the
 * pull-time memset. */
typedef struct gc_hdr {
    unsigned char  type_tag;
    unsigned char  mark;
    unsigned char  gen;
    unsigned char  age;
    unsigned char  dirty;  /* remset membership bit; see gc_write_barrier */
    unsigned char  bump;   /* 1 if carved from a bump slab; never free()d */
    size_t         size;
    struct gc_hdr *next;
} gc_hdr_t;

/* GC range: address span of one allocated payload for conservative scan. */
typedef struct {
    uintptr_t  start;
    uintptr_t  end;
    gc_hdr_t  *h;
} gc_range_t;

/* Slab for the bump allocator. payload[] starts at the byte after the
 * header and runs to MINO_BUMP_SLAB_BYTES total. Each slab occupies
 * exactly one MINO_BUMP_SLAB_BYTES-aligned region (VirtualAlloc's
 * allocation granularity on Windows, posix_memalign elsewhere), so a
 * header's owning slab is derivable by masking its address -- no
 * back-pointer in gc_hdr_t. `live` counts headers carved from this
 * slab that are still reachable-or-freelisted... precisely: headers
 * carved and not yet recycled (a recycled-with-size-class header
 * decremented at push; pulling it back off the freelist increments
 * again). When live reaches zero and the slab is not the one the
 * cursor is carving from, the slab is purged from the per-size-class
 * freelists and its region is returned to the OS. Aligned-allocation
 * failure at refill falls back to the calloc path (headers with
 * bump=0), so the mask lookup only ever sees aligned slabs.
 *
 * The bump path bypasses the per-size-class freelist arm of
 * gc_alloc_raw; freed headers from bump-allocated slabs route to
 * the freelist as today (the recycle path doesn't care which arm
 * originally produced the header). Slab tail bytes that cannot
 * hold the requested size on refill become permanent waste, which
 * is bounded by the slab size. */
#define MINO_BUMP_SLAB_BYTES (64u * 1024u)
#define MINO_BUMP_SLAB_MASK  (~((uintptr_t)MINO_BUMP_SLAB_BYTES - 1u))

typedef struct gc_bump_slab {
    struct gc_bump_slab *next;
    size_t               live;  /* carved-and-not-yet-recycled headers */
    unsigned char        cold;  /* 1 once retired to the cold list; a
                                 * freelist pull of one of its headers
                                 * revives it before the batched release
                                 * can free the region */
} gc_bump_slab_t;

/* Owning slab of a bump-carved header. Only valid for headers with
 * h->bump == 1; see the slab comment for the alignment contract. */
static inline gc_bump_slab_t *gc_bump_slab_of(const gc_hdr_t *h)
{
    return (gc_bump_slab_t *)((uintptr_t)h & MINO_BUMP_SLAB_MASK);
}

/* ------------------------------------------------------------------------- */
/* Per-tag tracer registration                                               */
/* ------------------------------------------------------------------------- */

/* gc_tracer_fn handles a single header by pushing every GC child it
 * holds onto the mark stack via gc_mark_child_push. One slot per
 * GC_T_* tag; gc_trace_children dispatches through this table. The
 * VAL tracer's inner switch over MINO_VECTOR / MINO_MAP / ... is
 * owned by the values component (its tracer fn lives there).
 * Component-owned tracers register themselves during state init,
 * before the first allocation. */
typedef void (*gc_tracer_fn)(mino_state *S, gc_hdr_t *h);

/* gc_finalizer_fn frees per-tag external resources owned by a
 * dying header (e.g. mpz allocation behind a MINO_BIGINT). Called
 * from gc_minor_sweep + gc_major_sweep_phase before the header
 * itself is freed. NULL slot means "nothing external to release"
 * and the sweep is a plain free(). */
typedef void (*gc_finalizer_fn)(mino_state *S, gc_hdr_t *h);

#endif /* GC_LAYOUT_H */
