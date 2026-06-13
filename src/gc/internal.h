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

#include "mino_internal.h"

#include <stddef.h>
#include <stdint.h>

#include "gc/layout.h"   /* gc_hdr_t, GC_T_*, GC_GEN_*, GC_PHASE_*,
                          * gc_range_t, gc_bump_slab_t, gc_tracer_fn,
                          * gc_finalizer_fn */

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

/* GC pin/unpin macros.
 * Always increment gc_save_len so pin/unpin pairs stay balanced even
 * when the save array is full.  Only write the pointer when there is
 * space; beyond 64 the value is not pinned but the counter remains
 * correct, preventing underflow on the matching unpin. The assert
 * makes the overflow case loud in debug / sanitizer builds so the
 * silent-loss path doesn't quietly drop liveness protection on a
 * deeply-nested test; release builds keep the documented soft
 * behavior so a runaway pin doesn't crash the host -- a script
 * with a ~60-clause case form expands to deeply-nested cond, each
 * level pins fn through eval_apply_regular_call, and the standard
 * -O2 build (no -DNDEBUG) used to abort the embedder there.
 * Require a local variable named `S` of type mino_state *. */
#define GC_SAVE_MAX 64
/* Gate the overflow assert on whether we're in a sanitizer build.
 * Sanitizer builds want loud failure to flag liveness regressions;
 * release builds keep the documented soft-loss path so the
 * conservative C-stack scanner still covers values that escape
 * the pin array. The __has_feature check is nested inside its own
 * `defined` test because gcc evaluates the second half of an &&
 * syntactically even when the first half is false -- splitting
 * into nested #if keeps gcc preprocessing happy while still
 * detecting clang's ASan / TSan / UBSan flavours. */
#if defined(__has_feature)
#  if __has_feature(address_sanitizer) \
      || __has_feature(thread_sanitizer) \
      || __has_feature(undefined_behavior_sanitizer)
#    define MINO_GC_PIN_LOUD_ASSERT 1
#  else
#    define MINO_GC_PIN_LOUD_ASSERT 0
#  endif
#elif defined(__SANITIZE_ADDRESS__) \
    || defined(__SANITIZE_THREAD__) \
    || defined(__SANITIZE_UNDEFINED__)
#  define MINO_GC_PIN_LOUD_ASSERT 1
#else
#  define MINO_GC_PIN_LOUD_ASSERT 0
#endif
#if MINO_GC_PIN_LOUD_ASSERT
#  define gc_pin(v) \
    do { assert(mino_current_ctx(S)->gc_save_len < GC_SAVE_MAX); \
         if (mino_current_ctx(S)->gc_save_len < GC_SAVE_MAX) mino_current_ctx(S)->gc_save[mino_current_ctx(S)->gc_save_len] = (v); \
         mino_current_ctx(S)->gc_save_len++; } while (0)
#  define gc_unpin(n) \
    do { assert(mino_current_ctx(S)->gc_save_len >= (n)); \
         mino_current_ctx(S)->gc_save_len -= (n); } while (0)
#else
#  define gc_pin(v) \
    do { if (mino_current_ctx(S)->gc_save_len < GC_SAVE_MAX) mino_current_ctx(S)->gc_save[mino_current_ctx(S)->gc_save_len] = (v); \
         mino_current_ctx(S)->gc_save_len++; } while (0)
#  define gc_unpin(n) \
    do { mino_current_ctx(S)->gc_save_len -= (n); } while (0)
#endif

/* ------------------------------------------------------------------------- */
/* Shared GC function declarations                                           */
/* ------------------------------------------------------------------------- */

/* driver.c: allocation and collection driver.
 * All gc_alloc/alloc_val returns are GC-owned. The three _inner names
 * are the real functions; the public macros below add per-callsite
 * profiler recording when the binary is built with -DMINO_ALLOC_PROFILE=1. */
void  *gc_alloc_typed_inner(mino_state *S, unsigned char tag, size_t size);
mino_val *alloc_val_inner(mino_state *S, mino_type type);
char  *dup_n_inner(mino_state *S, const char *s, size_t len);

/* Raise the standard OOM mino diagnostic by longjmp'ing into the active
 * try frame (or abort if none). The same throw the GC allocator uses on
 * its own NULL return; callers that detect allocation failure outside
 * the GC path -- e.g. checked-size overflow before a raw malloc/realloc,
 * or a non-GC heap returning NULL -- route through here so the failure
 * shape is uniform. */
void gc_oom_throw(mino_state *S, const char *msg);

#ifdef MINO_ALLOC_PROFILE
void mino_alloc_profile_record(const char *file, int line,
                               unsigned char tag, size_t size);
#define gc_alloc_typed(S, T, SZ)                                          \
    (mino_alloc_profile_record(__FILE__, __LINE__,                        \
                               (unsigned char)(T), (size_t)(SZ)),         \
     gc_alloc_typed_inner((S), (T), (SZ)))
#define alloc_val(S, T)                                                   \
    (mino_alloc_profile_record(__FILE__, __LINE__,                        \
                               (unsigned char)2 /* GC_T_VAL */,           \
                               sizeof(mino_val)),                       \
     alloc_val_inner((S), (T)))
#define dup_n(S, P, N)                                                    \
    (mino_alloc_profile_record(__FILE__, __LINE__,                        \
                               (unsigned char)1 /* GC_T_RAW */,           \
                               (size_t)(N) + 1u),                         \
     dup_n_inner((S), (P), (N)))
#else
#define gc_alloc_typed(S, T, SZ) gc_alloc_typed_inner((S), (T), (SZ))
#define alloc_val(S, T)          alloc_val_inner((S), (T))
#define dup_n(S, P, N)           dup_n_inner((S), (P), (N))
#endif
void   gc_major_collect(mino_state *S);
void   gc_minor_collect(mino_state *S);
/* Incremental major state machine. gc_major_begin seeds the mark stack
 * from roots and transitions IDLE -> MAJOR_MARK. gc_major_step drains
 * up to budget_words headers (pass SIZE_MAX for an unbounded drain).
 * gc_major_remark re-walks all precise roots, performs the final
 * conservative stack rescan, and drains the mark stack to empty.
 * gc_major_sweep_phase transitions MAJOR_MARK -> IDLE, sweeps dead OLD,
 * and resets accounting. gc_major_collect chains all four back-to-back
 * for a fully STW major cycle. */
void   gc_major_begin(mino_state *S);
void   gc_major_step(mino_state *S, size_t budget_words);
void   gc_major_remark(mino_state *S);
void   gc_major_sweep_phase(mino_state *S);
/* Drive any in-flight major to IDLE with the mutator paused. No-op
 * when the phase is not MAJOR_MARK. Used by the OOM fallback and by
 * the public mino_gc_collect(MAJOR|FULL) entry point. */
void   gc_force_finish_major(mino_state *S);
void   gc_note_host_frame(mino_state *S, void *addr);

/* Free-list size class lookup. Returns -1 for variable-size allocations
 * that cannot be recycled. Shared between alloc (driver.c) and sweep
 * (major.c). */
int    gc_freelist_class(size_t size);

/* Call the per-tag finalizer for h (if registered), then route h to the
 * freelist (sized class), leave it in its bump slab (no size class,
 * bump-origin), or release it via free() (no size class, calloc-origin).
 * Shared between minor sweep (minor.c) and major sweep (major.c). */
void   gc_hdr_recycle(mino_state *S, gc_hdr_t *h);

/* True iff p lies inside the mino_state struct -- i.e. p is a singleton
 * or small-int cache entry rather than a GC allocation.  Shared between
 * barrier.c and driver.c (gc_mark_child_push).
 * Defined in barrier.c; declared here so driver.c can call it without
 * duplicating the range check. */
int gc_ptr_is_state_embedded(const mino_state *S, const void *p);

/* Mark-stack primitives (driver.c). Mark the header live and push it
 * for tracing; interior-pointer variant resolves a heap pointer to its
 * header first and is safe on stale/stack words. gc_drain_mark_stack pops
 * until empty, tracing each header's outgoing references.
 * gc_mark_push filters OLD headers out when gc_phase == GC_PHASE_MINOR
 * so minor marking stays proportional to young reachability.
 * gc_trace_children unconditionally pushes every child pointer held in
 * h into the mark stack; minor uses it to trace remembered-set
 * entries even though their header is OLD. */
void gc_mark_push(mino_state *S, gc_hdr_t *h);
void gc_drain_mark_stack(mino_state *S);
/* Drain the mark stack until its length drops to floor_len. Minor
 * uses this with the saved major-in-flight length so its drain only
 * processes entries it added on top; major's pending OLD entries
 * beneath the floor are preserved for the next gc_major_step. */
void gc_drain_mark_stack_to(mino_state *S, size_t floor_len);
void gc_trace_children(mino_state *S, gc_hdr_t *h);

/* Per-tag tracer + finalizer registration. Called from each
 * component's mino_<component>_register_gc_handlers(S) hook during
 * state init, before any allocation. Registering NULL is the same
 * as leaving the slot untouched; gc_trace_children skips empty
 * slots, so unhandled tags are no-ops. */
void gc_register_tracer(mino_state *S, unsigned char tag,
                        gc_tracer_fn fn);
void gc_register_finalizer(mino_state *S, unsigned char tag,
                           gc_finalizer_fn fn);

/* Wire every built-in tracer. Called from runtime/state.c::state_init
 * before the first allocation; component-owned tracers register
 * themselves alongside. */
void gc_register_default_tracers(mino_state *S);
/* Enqueue a header onto the mark stack with mark=1, bypassing the
 * minor-phase OLD filter. Used by minor's promotion hook to hand
 * newly-promoted OLD objects to major's mark frontier when a minor
 * runs nested inside MAJOR_MARK. */
void gc_major_enqueue_promoted(mino_state *S, gc_hdr_t *h);

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
void      gc_build_range_index(mino_state *S);
void      gc_range_insert(mino_state *S, gc_hdr_t *h);
void      gc_range_merge_pending(mino_state *S);
void      gc_range_compact_after_minor_mark(mino_state *S);
gc_hdr_t *gc_find_header_for_ptr(mino_state *S, const void *p);
void      gc_mark_roots(mino_state *S);
void      gc_scan_stack(mino_state *S);

/* runtime_gc_major.c: full-heap sweep driven by gc_major_collect. Frees every
 * allocation whose mark bit is clear and resets the mark bit on
 * survivors; updates gc_bytes_live and gc_threshold. */
void gc_sweep(mino_state *S);

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
 * (mino_val*, mino_env*, raw buffer from gc_alloc_typed) or a
 * singleton inside mino_state (nil, true, false, small-int cache,
 * sentinels). Singletons are recognised by their address being
 * embedded in the state struct and are treated as no-ops. NULL is
 * always a no-op. */
void gc_write_barrier(mino_state *S, void *container,
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
void gc_evt_init(mino_state *S);
void gc_evt_free(mino_state *S);
void gc_evt_record_impl(mino_state *S, uint8_t kind, const void *a,
                        const void *b, const void *c, uintptr_t aux,
                        uint16_t extra);
void gc_evt_dump_around(mino_state *S, const void *p1, const void *p2,
                        const void *p3);

/* Macro wrapper: when MINO_GC_EVT=0 (the default), gc_evt_ring is
 * NULL and this expands to a single predictable pointer check with
 * no function call, keeping the hot path identical to the
 * uninstrumented binary modulo that one branch. When the ring is
 * enabled, we pay a real call through gc_evt_record_impl. */
#define gc_evt_record(S, kind, a, b, c, aux, extra) \
    do { \
        if ((S)->gc.evt_ring != NULL) { \
            gc_evt_record_impl((S), (kind), (a), (b), (c), (aux), (extra)); \
        } \
    } while (0)
/* Reachability classifier. Returns 1 if offender reachable without
 * conservative stack, 2 if reachable only via conservative stack, 0
 * if not reachable at all (bookkeeping corruption). Non-destructive:
 * saves and restores h->mark on every tracked header. */
int  gc_classify_offender(mino_state *S, gc_hdr_t *offender);

/* Walk both generation lists and apply fn(h, user) to every header.
 * Used by the trace classifier and by gc_verify_remset_complete to
 * save, clear, and restore mark bits without duplicating the traversal.
 * Defined in trace.c; declared here so minor.c can call it. */
void gc_for_each_hdr(mino_state *S,
                     void (*fn)(gc_hdr_t *h, void *user),
                     void *user);

/* Count headers live on both generation lists.  Used to size mark-save
 * buffers before calling gc_for_each_hdr with gc_save_mark_fn.
 * Defined in trace.c; declared here so minor.c can call it. */
size_t gc_count_hdrs(mino_state *S);

/* Context struct and callback for saving/clearing all mark bits in one
 * gc_for_each_hdr pass.  Restore by writing ctx.marks[i] back into
 * ctx.hdrs[i]->mark for i in [0, ctx.idx).
 * Defined in trace.c; declared here so minor.c can call gc_save_mark_fn. */
struct gc_mark_save_ctx {
    gc_hdr_t     **hdrs;
    unsigned char *marks;
    size_t         idx;
    size_t         cap;
};
void gc_save_mark_fn(gc_hdr_t *h, void *user);

/* Tail-append helper used by every list-building loop. Barriers the
 * store first -- critical because mid-loop minor GC can promote tail
 * to OLD while the cell being appended is a fresh YOUNG allocation,
 * and an unbarriered OLD-to-YOUNG edge loses the cell at the next
 * minor. Also drives SATB when a major mark is in flight. Caller must
 * guarantee tail is non-NULL. */
void mino_cons_cdr_set(mino_state *S, mino_val *tail, mino_val *cell);

/* VALARR slot store: barriers the write, then updates arr[i]. Use at
 * every site that fills a GC_T_VALARR scratch buffer in a loop whose
 * body can allocate (user eval, recursive readers, quasiquote expand)
 * -- the scratch array may be promoted mid-loop and later stores of
 * fresh YOUNG values need remset coverage. Inline-friendly; a single
 * call replaces the raw `arr[i] = v;` assignment. */
void gc_valarr_set(mino_state *S, mino_val **arr, size_t i,
                   mino_val *v);

/* Clear every dirty bit and empty the remembered set. Called at the
 * end of every full cycle -- after a complete trace, the old-to-young
 * reference set is rebuilt by future barriers, not inherited. */
void gc_remset_reset(mino_state *S);

/* Append container to the remembered set if not already dirty. Used by
 * the minor collector to enqueue every just-promoted header -- a
 * one-cycle safety net that covers alloc-then-populate patterns where
 * the mutator sets pointers on a container after the minor that
 * promoted it. */
void gc_remset_add(mino_state *S, gc_hdr_t *container);

/* Used by major sweep to remove entries whose container is about to
 * be freed, while leaving remset entries for containers that survive
 * (their dirty bit intact) so the next minor still finds the YOUNG
 * edges the mutator installed during this major cycle. */
void gc_remset_purge_dead(mino_state *S);

/* GC marking (used by gc_major_collect and by root enumeration). */
void gc_mark_interior(mino_state *S, const void *p);

/* Append one pause sample to the 256-ring (saturating at UINT32_MAX
 * ns) and tick the log2 histogram bucket [2^i, 2^(i+1)) ns clamped to
 * bucket 23. Called from each collection / slice site that already
 * computed elapsed_ns. Lifetime histogram + recent-window ring
 * together support both percentile + distribution queries. */
void gc_record_pause(mino_state *S, size_t ns);

/* Allocation-site sampler entry. site is the immediate return address
 * captured at gc_alloc_typed_inner (the C-side alloc site, which lets
 * the dashboard map a hot allocator back to a specific source file
 * and line via addr2line). tag is the GC tag the caller asked for;
 * size_bucket is clamp(floor(log2(size+1)), 0..31). 16 bytes. */
typedef struct mino_alloc_sample {
    const void *site;
    uint8_t     tag;
    uint8_t     size_bucket;
    uint16_t    _pad;
    uint32_t    count;  /* aggregated at fold; unused for ring slot */
} mino_alloc_sample_t;

#endif /* GC_INTERNAL_H */
