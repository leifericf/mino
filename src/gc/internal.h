/*
 * gc_internal.h -- garbage collector internal types and declarations.
 *
 * Internal to the runtime; embedders should only use mino.h.
 *
 * Error classes emitted (see diag/diag_contract.h):
 *
 *   MINO_ERR_CORRUPT -- driver.c, barrier.c, major.c, minor.c, roots.c,
 *      trace.c.  Every abort() in this subsystem is a CORRUPT path:
 *      OOM with no try frame established, GC range index realloc
 *      inside a collection, an unexpected setjmp return inside
 *      gc_collect, or a write barrier fired with no recovery option.
 *      Reaching any of these means the heap is in an unrecoverable
 *      state.
 *   MINO_ERR_RECOVERABLE -- gc_alloc_typed with a try frame in place
 *      longjmps into the catch handler so user code can observe an
 *      OOM via :internal/MIN001.
 */

#ifndef GC_INTERNAL_H
#define GC_INTERNAL_H

#include "mino.h"

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
    GC_T_RB_NODE    = 9
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

/* GC event ring buffer (diagnostic; opt-in via MINO_GC_EVT=1).
 *
 * Records a low-perturbation trace of barrier/remset/sweep/promote
 * activity into a fixed-size, per-state ring. Only writes -- no I/O,
 * no locks, no allocation on the hot path. Dumped by
 * gc_evt_dump_around to stderr from the verify abort site so the
 * window around the failure can be reconstructed without a 20-million
 * line trace file (which itself hides the timing-sensitive bug).
 *
 * Record layout: payload fields are written first, seq last as a
 * commit marker. Readers treat an entry with seq < floor as empty.
 * Ring size is a power of two; slot = seq & (cap - 1). */
enum {
    GC_EVT_NONE      = 0,
    GC_EVT_WB        = 1,  /* a=container, b=old_value, c=new_value */
    GC_EVT_REMSET_ADD= 2,  /* a=container */
    GC_EVT_REMSET_RESET = 3,
    GC_EVT_REMSET_PURGE = 4,
    GC_EVT_PROMOTE   = 5,  /* a=hdr (young->old), aux=size */
    GC_EVT_FREE_YOUNG= 6,  /* a=hdr, aux=size */
    GC_EVT_MINOR_BEGIN = 7,
    GC_EVT_MINOR_END   = 8,
    GC_EVT_MAJOR_BEGIN = 9,
    GC_EVT_MAJOR_SWEEP = 10,
    GC_EVT_ALLOC     = 11  /* a=hdr, aux=(type_tag<<8)|gen, extra=size */
};

typedef struct {
    uint64_t  seq;     /* written last; 0 means empty slot */
    uint32_t  cycle;   /* minor cycle number at event time */
    uint8_t   kind;
    uint8_t   phase;
    uint16_t  extra;
    void     *a;
    void     *b;
    void     *c;
    uintptr_t aux;
} gc_evt_t;

#define GC_EVT_CAP_LOG2 16u
#define GC_EVT_CAP      (1u << GC_EVT_CAP_LOG2)
#define GC_EVT_CAP_MASK (GC_EVT_CAP - 1u)

/* Header layout on a 64-bit target: 1+1+1+1 bytes followed by 4 bytes
 * of padding, then 8-byte size and 8-byte next. Four single-byte fields
 * fit into the padding slot that existed for type_tag/mark alone; the
 * struct size is unchanged. */
typedef struct gc_hdr {
    unsigned char  type_tag;
    unsigned char  mark;
    unsigned char  gen;
    unsigned char  age;
    unsigned char  dirty;  /* remset membership bit; see gc_write_barrier */
    size_t         size;
    struct gc_hdr *next;
} gc_hdr_t;

/* GC range: address span of one allocated payload for conservative scan. */
typedef struct {
    uintptr_t  start;
    uintptr_t  end;
    gc_hdr_t  *h;
} gc_range_t;

/* GC pin/unpin macros.
 * Always increment gc_save_len so pin/unpin pairs stay balanced even
 * when the save array is full.  Only write the pointer when there is
 * space; beyond 64 the value is not pinned but the counter remains
 * correct, preventing underflow on the matching unpin.
 * Require a local variable named `S` of type mino_state_t *. */
#define GC_SAVE_MAX 64
#define gc_pin(v) \
    do { if (S->ctx->gc_save_len < GC_SAVE_MAX) S->ctx->gc_save[S->ctx->gc_save_len] = (v); \
         S->ctx->gc_save_len++; } while (0)
#define gc_unpin(n) \
    do { assert(S->ctx->gc_save_len >= (n)); \
         S->ctx->gc_save_len -= (n); } while (0)

/* ------------------------------------------------------------------------- */
/* Shared GC function declarations                                           */
/* ------------------------------------------------------------------------- */

/* driver.c: allocation and collection driver.
 * All gc_alloc/alloc_val returns are GC-owned. */
void  *gc_alloc_typed(mino_state_t *S, unsigned char tag, size_t size);
mino_val_t *alloc_val(mino_state_t *S, mino_type_t type);     /* GC-owned */
char  *dup_n(mino_state_t *S, const char *s, size_t len);     /* GC-owned copy */
void   gc_major_collect(mino_state_t *S);
void   gc_minor_collect(mino_state_t *S);
/* Incremental major state machine. gc_major_begin seeds the mark stack
 * from roots and transitions IDLE -> MAJOR_MARK. gc_major_step drains
 * up to budget_words headers (pass SIZE_MAX for an unbounded drain).
 * gc_major_remark performs the final conservative stack rescan and
 * drains any pending SATB-pushed previous values. gc_major_sweep
 * transitions MAJOR_MARK -> MAJOR_SWEEP, sweeps dead OLD, and returns
 * to IDLE. gc_major_collect chains all four back-to-back for a fully
 * STW major cycle. */
void   gc_major_begin(mino_state_t *S);
void   gc_major_step(mino_state_t *S, size_t budget_words);
void   gc_major_remark(mino_state_t *S);
void   gc_major_sweep_phase(mino_state_t *S);
/* Drive any in-flight major to IDLE with the mutator paused. No-op
 * when the phase is not MAJOR_MARK. Used by the OOM fallback and by
 * the public mino_gc_collect(MAJOR|FULL) entry point. */
void   gc_force_finish_major(mino_state_t *S);
void   gc_note_host_frame(mino_state_t *S, void *addr);

/* Free-list size class lookup. Returns -1 for variable-size allocations
 * that cannot be recycled. Shared between alloc (driver.c) and sweep
 * (major.c). */
int    gc_freelist_class(size_t size);

/* Mark-stack primitives (driver.c). Mark the header live and push it
 * for tracing; interior-pointer variant resolves a heap pointer to its
 * header first and is safe on stale/stack words. gc_drain_mark_stack pops
 * until empty, tracing each header's outgoing references.
 * gc_mark_push filters OLD headers out when gc_phase == GC_PHASE_MINOR
 * so minor marking stays proportional to young reachability.
 * gc_trace_children unconditionally pushes every child pointer held in
 * h into the mark stack; minor uses it to trace remembered-set
 * entries even though their header is OLD. */
void gc_mark_push(mino_state_t *S, gc_hdr_t *h);
void gc_drain_mark_stack(mino_state_t *S);
/* Drain the mark stack until its length drops to floor_len. Minor
 * uses this with the saved major-in-flight length so its drain only
 * processes entries it added on top; major's pending OLD entries
 * beneath the floor are preserved for the next gc_major_step. */
void gc_drain_mark_stack_to(mino_state_t *S, size_t floor_len);
void gc_trace_children(mino_state_t *S, gc_hdr_t *h);
/* Enqueue a header onto the mark stack with mark=1, bypassing the
 * minor-phase OLD filter. Used by minor's promotion hook to hand
 * newly-promoted OLD objects to major's mark frontier when a minor
 * runs nested inside MAJOR_MARK. */
void gc_major_enqueue_promoted(mino_state_t *S, gc_hdr_t *h);

/* runtime_gc_roots.c: range index over live headers plus root enumeration.
 * The range index backs gc_find_header_for_ptr, which resolves a raw
 * machine word to its owning header during conservative stack scans and
 * interior-pointer mark. gc_range_insert buffers new allocations in a
 * growable pending array; gc_range_merge_pending folds them into the
 * sorted main array at the top of each collection;
 * gc_range_compact_after_minor_mark drops freed YOUNG entries before
 * minor sweep so the index stays valid across cycles without a
 * rebuild-from-gc_all. gc_build_range_index is the fallback used when
 * the mutator hits OOM on pending growth. */
void      gc_build_range_index(mino_state_t *S);
void      gc_range_insert(mino_state_t *S, gc_hdr_t *h);
void      gc_range_merge_pending(mino_state_t *S);
void      gc_range_compact_after_minor_mark(mino_state_t *S);
gc_hdr_t *gc_find_header_for_ptr(mino_state_t *S, const void *p);
void      gc_mark_roots(mino_state_t *S);
void      gc_scan_stack(mino_state_t *S);

/* runtime_gc_major.c: full-heap sweep driven by gc_major_collect. Frees every
 * allocation whose mark bit is clear and resets the mark bit on
 * survivors; updates gc_bytes_live and gc_threshold. */
void gc_sweep(mino_state_t *S);

/* runtime_gc_barrier.c: write barrier and remembered-set machinery.
 * Call BEFORE storing new_value into a field owned by container. The
 * barrier handles two concerns:
 *   Remset (all phases): when the store creates an old->young edge,
 *     container is appended to the remembered set (deduped via
 *     container->dirty) so the next minor traces it.
 *   SATB (MAJOR_MARK only): old_value -- the previous slot contents
 *     overwritten by this store -- is pushed onto the mark stack so
 *     anything reachable at snapshot time survives the cycle, even if
 *     the mutator unlinks it before mark reaches container. Pass NULL
 *     for old_value when the slot was empty.
 * Pointer arguments are the PAYLOAD start of a GC-allocated object
 * (mino_val_t*, mino_env_t*, raw buffer from gc_alloc_typed) or a
 * singleton inside mino_state_t (nil, true, false, small-int cache,
 * sentinels). Singletons are recognised by their address being
 * embedded in the state struct and are treated as no-ops. NULL is
 * always a no-op. */
void gc_write_barrier(mino_state_t *S, void *container,
                      const void *old_value, const void *new_value);

/* runtime_gc_trace.c: GC event ring buffer + classifier (both opt-in
 * via MINO_GC_EVT=1 at state init). Recording sites call
 * gc_evt_record; when the ring is unallocated the call is a single
 * nullptr-check-and-return. gc_evt_dump_around walks the ring in
 * sequence order and prints only events that reference any of the
 * supplied pointers (or, if all three are NULL, the tail N events).
 * gc_classify_offender runs two reachability passes (precise-only,
 * then precise+conservative) to answer whether an offender header is
 * really reachable or only kept alive by conservative stack scan. */
void gc_evt_init(mino_state_t *S);
void gc_evt_free(mino_state_t *S);
void gc_evt_record_impl(mino_state_t *S, uint8_t kind, const void *a,
                        const void *b, const void *c, uintptr_t aux,
                        uint16_t extra);
void gc_evt_dump_around(mino_state_t *S, const void *p1, const void *p2,
                        const void *p3);

/* Macro wrapper: when MINO_GC_EVT=0 (the default), gc_evt_ring is
 * NULL and this expands to a single predictable pointer check with
 * no function call, keeping the hot path identical to the
 * uninstrumented binary modulo that one branch. When the ring is
 * enabled, we pay a real call through gc_evt_record_impl. */
#define gc_evt_record(S, kind, a, b, c, aux, extra) \
    do { \
        if ((S)->gc_evt_ring != NULL) { \
            gc_evt_record_impl((S), (kind), (a), (b), (c), (aux), (extra)); \
        } \
    } while (0)
/* Reachability classifier. Returns 1 if offender reachable without
 * conservative stack, 2 if reachable only via conservative stack, 0
 * if not reachable at all (bookkeeping corruption). Non-destructive:
 * saves and restores h->mark on every tracked header. */
int  gc_classify_offender(mino_state_t *S, gc_hdr_t *offender);

/* Tail-append helper used by every list-building loop. Barriers the
 * store first -- critical because mid-loop minor GC can promote tail
 * to OLD while the cell being appended is a fresh YOUNG allocation,
 * and an unbarriered OLD-to-YOUNG edge loses the cell at the next
 * minor. Also drives SATB when a major mark is in flight. Caller must
 * guarantee tail is non-NULL. */
void mino_cons_cdr_set(mino_state_t *S, mino_val_t *tail, mino_val_t *cell);

/* VALARR slot store: barriers the write, then updates arr[i]. Use at
 * every site that fills a GC_T_VALARR scratch buffer in a loop whose
 * body can allocate (user eval, recursive readers, quasiquote expand)
 * -- the scratch array may be promoted mid-loop and later stores of
 * fresh YOUNG values need remset coverage. Inline-friendly; a single
 * call replaces the raw `arr[i] = v;` assignment. */
void gc_valarr_set(mino_state_t *S, mino_val_t **arr, size_t i,
                   mino_val_t *v);

/* Clear every dirty bit and empty the remembered set. Called at the
 * end of every full cycle -- after a complete trace, the old-to-young
 * reference set is rebuilt by future barriers, not inherited. */
void gc_remset_reset(mino_state_t *S);

/* Append container to the remembered set if not already dirty. Used by
 * the minor collector to enqueue every just-promoted header -- a
 * one-cycle safety net that covers alloc-then-populate patterns where
 * the mutator sets pointers on a container after the minor that
 * promoted it. */
void gc_remset_add(mino_state_t *S, gc_hdr_t *container);

/* Used by major sweep to remove entries whose container is about to
 * be freed, while leaving remset entries for containers that survive
 * (their dirty bit intact) so the next minor still finds the YOUNG
 * edges the mutator installed during this major cycle. */
void gc_remset_purge_dead(mino_state_t *S);

/* GC marking (used by gc_major_collect and by root enumeration). */
void gc_mark_interior(mino_state_t *S, const void *p);

#endif /* GC_INTERNAL_H */
