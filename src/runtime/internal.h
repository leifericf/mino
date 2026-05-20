/*
 * internal.h -- mino_state struct, environments, var registry,
 * dynamic bindings, error reporting, module resolution.
 *
 * Pulls in the per-subsystem internal headers so any field on
 * mino_state has a complete type at struct-definition time. .c files
 * that only need a single subsystem's types can include just that
 * subsystem's header instead.
 *
 * Internal to the runtime; embedders should only use mino.h.
 *
 * Error classes emitted (see diag/diag_contract.h):
 *
 *   MINO_ERR_RECOVERABLE -- error.c (set_error, set_eval_diag,
 *      append_trace).  Standard path for runtime-detected user errors;
 *      lifts into :eval/..., :type/..., :arity/... diagnostic kinds.
 *   MINO_ERR_CORRUPT -- state.c (mino_state_new initial alloc).  No
 *      state exists to report through, so abort is the only option.
 *      module.c uses set_eval_diag for all module-loading failures
 *      (RECOVERABLE), never abort.
 */

#ifndef RUNTIME_INTERNAL_H
#define RUNTIME_INTERNAL_H

/* glibc gates PTHREAD_MUTEX_RECURSIVE and pthread_mutexattr_settype on
 * _XOPEN_SOURCE >= 500. macOS exposes them unconditionally; Windows
 * uses CRITICAL_SECTION. Define before any system header pulls in
 * features.h so glibc sees the request. */
#if defined(__linux__) && !defined(_XOPEN_SOURCE)
#  define _XOPEN_SOURCE 600
#endif

#include "mino_internal.h"
#include "diag.h"

#include "gc/internal.h"
#include "collections/internal.h"
#include "eval/internal.h"
#include "interop/internal.h"
#include "async/internal.h"

#include <assert.h>
#include <ctype.h>
#include <math.h>
#include <setjmp.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* pthread for state_lock + worker threads. Win32 path uses a
 * CRITICAL_SECTION wrapped in state_lock as void* (defined in state.c). */
#if !(defined(_WIN32) && defined(_MSC_VER))
#  include <pthread.h>
#endif
#include <time.h>

#include "runtime/value_assert.h"

#include "runtime/runtime_types.h"

#include "runtime/stm_state.h"

#include "runtime/thread_ctx.h"


#include "runtime/agent_queue.h"

/* gc_state_t alias — fields stay inline in struct mino_state until
 * the deferred byte-level decomposition lands. See
 * .local/cycle-4-followups.md. */
#include "gc/state.h"
#include "prim/stm_state.h"
#include "prim/agent_state.h"
#include "async/state.h"
#include "runtime/reader_printer_state.h"
#include "runtime/threading_state.h"
#include "runtime/ns_vars_state.h"
#include "eval/bc/state.h"
#include "eval/bc/jit/state.h"
#include "runtime/module_state.h"

/* ------------------------------------------------------------------------- */
/* Runtime state                                                             */
/* ------------------------------------------------------------------------- */

struct mino_state {
    /* Per-thread context.
     *
     * `main_ctx` is the embedded ctx for the OS thread that owns S
     * (calls mino_state_new and runs the bulk of work). Spawned host
     * threads allocate their own ctx and install it via
     * `mino_tls_ctx` for the duration of the worker run. Code that
     * needs the active ctx calls `mino_current_ctx(S)` which returns
     * the TLS ctx if set, else &main_ctx for the embedder thread. */
    mino_thread_ctx_t  main_ctx;

    /* === Garbage collection ============================================ */
    /* GC subsystem state lives in its own block defined in
     * src/gc/state.h. The block is embedded at the byte position
     * the inline gc_* fields used to occupy, so the
     * runtime_layout.h offsets for sf_*, ic_gen, bc_regs,
     * jit_invoke_ctx are unaffected. The secondary instrumentation
     * cluster (per-phase timers, pause ring, sampler rings) stays
     * inline past jit_hot_threshold, where adding new fields is
     * safe. */
    gc_state_t      gc;

    /* === Value caches: singletons, sentinels, interns, special forms === */

    /* Singletons */
    mino_val      nil_singleton;
    mino_val      true_singleton;
    mino_val      false_singleton;
    mino_val      empty_list_singleton;
    /* Trampoline sentinels reused across recur/tail-call to avoid
     * per-iteration allocation. Their args/fn fields are replaced in-place
     * and the containing eval loop consumes them before any other code
     * runs, so sharing one cell per kind is safe. */
    mino_val      recur_sentinel;
    mino_val      tail_call_sentinel;

    /* Small-integer cache: mino_int(S, n) returns the shared cell for
     * n in [MINO_SMALL_INT_LO, MINO_SMALL_INT_HI]. Arithmetic-heavy code
     * (fib, loops, reductions) produces many small-int results and
     * re-boxing them dominates allocation without this cache. */
    mino_val      small_ints[256];

    /* Intern tables */
    intern_table_t  sym_intern;
    intern_table_t  kw_intern;

    /* Record-type registry. Singly-linked list of MINO_TYPE values
     * keyed by interned ns/name, so re-eval of (defrecord Point ...)
     * returns the same type pointer. Pinned for state lifetime. */
    record_type_entry_t *record_types;

    /* Cached interned special-form symbols for O(1) pointer-eq dispatch.
     * Populated lazily on first eval_impl call. */
    int             sf_initialized;
    mino_val     *sf_quote;
    mino_val     *sf_quasiquote;
    mino_val     *sf_unquote;
    mino_val     *sf_unquote_splicing;
    mino_val     *sf_defmacro;
    mino_val     *sf_declare;
    mino_val     *sf_ns;
    mino_val     *sf_var;
    mino_val     *sf_def;
    mino_val     *sf_if;
    mino_val     *sf_do;
    mino_val     *sf_let;
    mino_val     *sf_let_star;
    mino_val     *sf_fn;
    mino_val     *sf_fn_star;
    mino_val     *sf_recur;
    mino_val     *sf_loop;
    mino_val     *sf_loop_star;
    mino_val     *sf_try;
    mino_val     *sf_binding;
    mino_val     *sf_lazy_seq;
    mino_val     *sf_new;
    mino_val     *sf_when;
    mino_val     *sf_and;
    mino_val     *sf_or;

    /* === Module system, execution limits, metadata ===================== */
    /* Lives in src/runtime/module_state.h. The block is embedded at
     * the byte position the inline limit_* / module_* / load_stack /
     * bundled_libs / extra_load_paths / meta_table fields used to
     * occupy. */
    module_state_t  module;

    /* Capability bitmask. Bit set per MINO_CAP_* constant in mino.h
     * when the corresponding `mino_install_<cap>` runs. Consulted by
     * the `mino-installed?` primitive for core.clj gates and by the
     * eval_symbol MNS002 diagnostic to enrich "unbound symbol" errors
     * with the capability that would enable the name. Defaults to 0
     * so a bare `mino_state_new` + `mino_install_minimal` runtime sees
     * no capabilities until the embedder opts in. */
    unsigned int    caps_installed;

    /* === Printer, reader, source diagnostics =========================== */
    /* print_depth stays inline so it packs with caps_installed (both
     * 4-byte ints) before the sub-struct, leaving the sub-struct at
     * an 8-aligned offset with no outer padding. */
    int             print_depth;
    /* Reader / source-cache state lives in
     * src/runtime/reader_printer_state.h. The block is embedded at
     * the byte position the inline reader_* / interned_* /
     * source_cache fields used to occupy. */
    reader_printer_state_t reader;

    /* Error reporting state moved to mino_thread_ctx_t (error_buf,
     * call_stack, call_depth, trace_added, last_diag). */

    /* === Namespaces, vars, host interop ================================ */
    /* Lives in src/runtime/ns_vars_state.h. The block is embedded at
     * the byte position the inline current_ns / ns_* / var_* /
     * ic_table / ic_gen / transient_owner_next / var_hash fields
     * used to occupy. ic_gen sits at the stencil-ABI-pinned offset
     * 47856; the embedded layout preserves it (verified by the
     * static_assert in eval/bc/jit/entry.c). */
    ns_vars_state_t ns_vars;

    /* Bytecode-VM state lives in src/eval/bc/state.h. The block is
     * embedded at the byte position the inline bc_* fields used to
     * occupy. bc_regs sits at the stencil-ABI-pinned offset 47888. */
    bc_vm_state_t   bc;

    /* JIT state lives in src/eval/bc/jit/state.h. The block is
     * embedded at the byte position the inline jit_* fields used to
     * occupy. jit_invoke_ctx sits at the stencil-ABI-pinned offset
     * 47936. */
    jit_state_t     jit;

    /* === Instrumentation: per-phase GC timers ============================
     *
     * Cumulative ns since state creation. gc_minor_mark_ns counts time
     * inside the minor mark phase (precise roots + remset + conservative
     * stack scan + drains); gc_minor_sweep_ns counts time in
     * gc_minor_sweep, which also performs age-based promotion as an
     * interleaved branch of the sweep loop. gc_major_mark_ns sums
     * gc_major_begin's mark_roots + every incremental gc_major_step +
     * gc_major_remark; gc_major_sweep_ns counts gc_major_sweep_phase.
     * gc_root_scan_ns is a sub-timer that measures only precise-root
     * enumeration (gc_mark_roots) across both collectors -- it overlaps
     * with the two _mark_ns fields rather than adding to them.
     *
     * mino's nursery is mark-and-sweep with age-based promotion (not a
     * copying semispace), so there is no flip phase and promotion is
     * interleaved into the minor sweep loop. The plan's gc_minor_flip_ns
     * and gc_minor_promote_ns counters are intentionally omitted;
     * promotion volume gets a separate byte-count surface in a later
     * release. Placed past jit_hot_threshold so the runtime_layout.h
     * offset constants the stencil bytes depend on do not shift. */
    size_t          gc_minor_mark_ns;
    size_t          gc_minor_sweep_ns;
    size_t          gc_major_mark_ns;
    size_t          gc_major_sweep_ns;
    size_t          gc_root_scan_ns;

    /* Write-barrier counters. satb_pushes ticks for each old_value
     * snapshot push during MAJOR_MARK (Yuasa); dijkstra_pushes ticks
     * for each new_value insertion push. mark_stack_overflows counts
     * silent-drop events in gc_mark_stack_push_raw when realloc fails
     * (the collector then leans on conservative stack scan as a
     * backstop). The plan's remset_overflows counter is intentionally
     * omitted: mino aborts on a remset realloc failure, so the event
     * is unobservable from the surviving runtime. */
    size_t          gc_barrier_satb_pushes;
    size_t          gc_barrier_dijkstra_pushes;
    size_t          gc_mark_stack_overflows;

    /* Generational promotion bookkeeping. bytes_promoted_minor is a
     * running total of bytes that flipped YOUNG -> OLD during minor
     * sweep; the rate (delta over a window) feeds nursery / promotion-
     * age tuning. young_age_bucket[i] increments whenever a YOUNG
     * header survives a minor cycle into age bucket i (0..7 mapped as
     * log2(age+1), clamped at 7). The accumulated histogram identifies
     * long-lived young objects that should bypass the nursery. */
    size_t          gc_bytes_promoted_minor;
    uint64_t        gc_young_age_bucket[8];

    /* Per-tag allocation counter. Indexed by GC_T_* tag (1..GC_T_BC,
     * with 0 and the slack tail reserved). Ticked once per gc_alloc_
     * typed call. Always-on: a single indexed store on the alloc path,
     * which is already paying for the header init / list-link cost. */
    uint64_t        gc_alloc_by_tag[GC_T__COUNT];

    /* BC compile-decline reason histogram. Each ok=0 path in compile.c
     * that the compiler attributes to a structural reason ticks one of
     * these buckets; the leaf overflow sites (consts table full, IC
     * slots cap reached, &c.) all fold into BC_DECLINE_OTHER so the
     * dashboard sees them as a single "internal limit" bucket without
     * needing to instrument every micro-site. Always-on; one indexed
     * store per decline. Exposed via (gc-stats) for now (cheaper than
     * a separate stats API) and via the mino.h surface below. */
    uint64_t        bc_declines[16];

    /* Collection-size histogram (env-gated by MINO_COLL_SIZE_STATS=1).
     * coll_size_hist[kind][bucket] -- kind is 0=vector, 1=map, 2=set;
     * bucket is clamp(floor(log2(size+1)), 0..31). Ticked at the
     * persistent / finalize entry points. Zero by default; only
     * populated when the env flag is on. Surfaced via (gc-stats). */
    int             coll_size_stats_enabled;  /* sniffed tri-state */
    uint64_t        coll_size_hist[3][32];

    /* Safepoint-based CPU sampler. MINO_SAMPLE=1 turns the sampler on;
     * MINO_SAMPLE_PERIOD sets the sampling rate (default 1000). Every
     * mino_bc_safepoint call bumps sampler_counter and, when the
     * counter modulo period hits zero, records the current bytecode
     * frame into sampler_ring. sampler_ring is allocated lazily on
     * the first sample (~1 MB at default cap 65536). The ring wraps,
     * preserving the most-recent SAMPLER_CAP samples for the dump. */
    int             sampler_enabled;  /* sniffed tri-state */
    unsigned        sampler_period;
    unsigned        sampler_ring_cap;
    unsigned        sampler_ring_idx;
    unsigned        sampler_ring_count;
    uint64_t        sampler_counter;
    struct mino_sample *sampler_ring;  /* malloc'd lazily */

    /* Allocation-site sampler (light). MINO_ALLOC_SAMPLE=1 +
     * MINO_ALLOC_SAMPLE_RATE=N (default 4096) records one alloc out
     * of every N into a tiny ring keyed by the immediate alloc-site
     * return address + tag + size bucket. Fixed-size ring (4096
     * entries × 16 B = 64 KB) makes this safe to flip on in dev
     * without recompile. */
    int             alloc_sampler_enabled;  /* sniffed tri-state */
    unsigned        alloc_sampler_rate;
    unsigned        alloc_sampler_ring_cap;
    unsigned        alloc_sampler_ring_idx;
    unsigned        alloc_sampler_ring_count;
    uint64_t        alloc_sampler_counter;
    struct mino_alloc_sample *alloc_sampler_ring;

    /* Pause-time distribution. gc_pause_ring is a circular buffer of
     * the last 256 pause durations (one entry per minor collect, per
     * major-slice, per force-finish, per fully-STW major); each value
     * saturates at UINT32_MAX (~4.29s). gc_pause_ring_idx is the next
     * write slot (wraps at 256). gc_pause_ring_count is the number of
     * valid entries written so far, clamped at 256. gc_pause_hist is
     * a log2 histogram of all pauses seen this state -- bucket i is
     * [2^i, 2^(i+1)) ns, bucket 23 catches anything >= 8.4ms. Combined
     * the ring gives recent-window percentiles while the histogram
     * gives a lifetime-wide distribution shape. */
    uint32_t        gc_pause_ring[256];
    unsigned        gc_pause_ring_idx;
    unsigned        gc_pause_ring_count;
    uint32_t        gc_pause_hist[24];

    /* Adaptive major-slice budget. gc_pause_target_ns is the desired
     * STW pause length (default 1 ms); MINO_GC_PAUSE_TARGET_NS overrides.
     * gc_budget_slices_since_adjust counts slices since the last damped
     * adjustment so successive adjustments can't snowball. The adaptive
     * helper in driver.c reads the recent 8-slice median pause off
     * gc_pause_ring and bumps gc_major_work_budget toward
     * gc_pause_target_ns within bounds [256, 65536] headers. Stress
     * mode (MINO_GC_STRESS=1) bypasses adaptive entirely. */
    size_t          gc_pause_target_ns;
    unsigned        gc_budget_slices_since_adjust;

    /* Host interop */
    int             interop_enabled;
    host_type_t    *host_types;
    size_t          host_types_len;
    size_t          host_types_cap;

    /* Eval current_form moved to mino_thread_ctx_t. */

    /* === Misc per-state: PRNG, sort, gensym, refs, fault injection ===== */

    /* Per-state PRNG (xorshift64*). Seeded lazily on first draw so two
     * runtimes initialised at the same instant get distinct sequences.
     * Per-state so cross-thread use doesn't race on libc rand(). */
    uint64_t        rand_state;

    /* Sort comparator */
    mino_val     *sort_comp_fn;
    mino_env     *sort_comp_env;

    /* Late-binding print-method hook. NULL during core bootstrap and any
     * state that never installed one; set via set-print-method! once the
     * multimethod is registered. When non-NULL, prim_pr / prim_prn route
     * each argument through this fn instead of calling mino_print
     * directly. The hook is expected to write to stdout as a side effect. */
    mino_val     *print_method_fn;

    /* Gensym counter */
    long            gensym_counter;

    /* `letfn*` special form symbol. Lives down here (rather than in
     * the sf_* block above) because that block sits at byte offsets
     * pinned by the JIT layout assertions in
     * src/eval/bc/stencils/runtime_layout.h. New special-form symbols
     * land after the layout-tracked region. */
    mino_val     *sf_letfn_star;

    /* Host-retained value refs */
    mino_ref     *ref_roots;

    /* Dynamic bindings, interrupt flag, and GC save stack moved to
     * mino_thread_ctx_t (dyn_stack, interrupted, gc_save, gc_save_len). */

    /* Cached parsed core.clj forms (avoids re-parsing on second
     * mino_install_clojure_core call within the same state). */
    mino_val    **core_forms;
    size_t          core_forms_len;

    /* Fault injection: when fi_alloc_countdown > 0, decrement on each
     * gc_alloc_typed call; when it reaches zero, simulate OOM. */
    long            fi_alloc_countdown;

    /* Fault injection for raw (non-GC) allocation paths such as the
     * clone serialization buffer. Same semantics as above. */
    long            fi_raw_countdown;

    /* === Host-thread runtime: grant, knobs, lock, futures, STW ========= */
    /* Lives in src/runtime/threading_state.h. The block is embedded
     * at the byte position the inline host-thread fields used to
     * occupy. The threading cluster sits past every stencil-ABI- and
     * JIT-pinned offset, so the extraction has no effect on the
     * runtime_layout.h constants. */
    threading_state_t threading;


    /* === STM (refs / dosync) ============================================ */
    /* STM subsystem state lives in src/prim/stm_state.h. The block
     * is embedded at the byte position the inline stm_* fields used
     * to occupy. */
    stm_subsystem_t stm;

    /* === Agent subsystem (send / send-off / await) ====================== */
    /* Lives in src/prim/agent_state.h. The block is embedded at the
     * byte position the inline agent_* fields used to occupy. */
    agent_subsystem_t   agent;

    /* === Async scheduler and timers ==================================== */
    /* Async subsystem state lives in src/async/state.h. The block is
     * embedded at the byte position the inline async_* fields used
     * to occupy. */
    async_state_t   async;

    /* Reader recursion depth. Bumped on every read_form entry,
     * checked against MINO_READER_MAX_DEPTH so pathological
     * input ('(' repeated 30k+ times) emits MRE011 instead of
     * stack-overflowing the embedder. Placed at the end of the
     * struct to keep the JIT's pinned offsets (ic_gen, bc_regs,
     * jit_invoke_ctx, etc. in src/eval/bc/stencils/runtime_layout.h)
     * stable across this addition. */
    int             reader_depth;

    /* Alloc-source counters: every gc_alloc_raw call increments exactly
     * one. freelist_hits = pulled from a per-size-class freelist;
     * calloc_size_class_miss = size matches a freelist class but the
     * list was empty (so we calloc'd a fresh header); calloc_no_class =
     * size has no freelist class at all (so we calloc by definition).
     * Used by (gc-stats) for allocator-source profiling. Placed at the
     * end alongside reader_depth to keep JIT-pinned offsets stable. */
    size_t          gc_alloc_freelist_hits;
    size_t          gc_alloc_calloc_size_class_miss;
    size_t          gc_alloc_calloc_no_class;

    /* Slab-backed bump allocator. Enabled by the MINO_BUMP_ALLOC env
     * var on state creation. When on, gc_alloc_raw's calloc arm is
     * replaced by a per-state bump from a linked list of fixed-size
     * slabs. Freelist arm is unchanged (still the hot path for
     * size-classed allocations). bump_enabled is the sticky toggle
     * read once at init. bump_cur/bump_end are the active slab's
     * cursor pair. bump_slabs is the head of every slab ever allocated.
     * bump_alloc_hits / bump_slab_refills count allocator events; the
     * counters are also surfaced via (gc-stats). */
    int             gc_bump_enabled;
    char           *gc_bump_cur;
    char           *gc_bump_end;
    struct gc_bump_slab *gc_bump_slabs;
    size_t          gc_bump_alloc_hits;
    size_t          gc_bump_slab_refills;

    /* Side-exit hand-off from JIT'd code back to the interpreter. The
     * JIT plants an OP_DEOPT_TO_INTERP stencil at the first PC its
     * compiled region cannot handle natively. The stencil routes
     * through mino_jit_deopt_exit which sets jit_deopt_pending = 1
     * and writes the resume PC to jit_deopt_pc, then returns NULL.
     * mino_jit_invoke checks the flag after the native call returns;
     * on deopt it clears the flag and tail-calls mino_bc_run_resume.
     *
     * jit_resume_saved_* hold the per-fn snapshots mino_bc_run captured
     * at fn entry (try_depth, bc_catch_depth, dyn_stack at fn entry).
     * mino_bc_run publishes them right before invoking the JIT so the
     * resume dispatch can pass them as the saved_* args to
     * bc_run_dispatch_from -- matching the bounds-check semantics the
     * interpreter path would use for the same body.
     *
     * Placed at the very tail to preserve JIT-pinned offsets for
     * ic_gen, bc_regs, jit_invoke_ctx (runtime_layout.h). */
    size_t                   jit_deopt_pc;
    int                      jit_deopt_pending;
    int                      jit_resume_saved_try_depth;
    int                      jit_resume_saved_bc_catch_depth;
    struct dyn_frame        *jit_resume_saved_dyn_stack;

    /* JIT slab pool: small fns (need_bytes <= MINO_JIT_SLAB_CUTOFF)
     * share host pages instead of mmap'ing one page per fn. Reduces
     * per-fn JIT memory footprint from one page-worth of waste to
     * (slab_size - aggregate-slot-bytes) per slab. Slabs are RX-
     * sealed between compiles and RW-flipped for the duration of
     * each fill via mprotect; mino_jit_slab_alloc/seal manage the
     * cycle. Tail-placed so the runtime_layout.h JIT-pinned offsets
     * stay stable. */
    struct mino_jit_slab    *jit_slabs;

    /* Per-tag GC dispatch tables. Tracer at gc_tracers[tag] handles
     * gc_trace_children for a header with that type_tag; finalizer at
     * gc_finalizers[tag] (NULL = no external resource) is called from
     * sweep paths before the header is freed. Component-owned tracers
     * register themselves via gc_register_tracer / gc_register_finalizer
     * during state init, before the first allocation. */
    gc_tracer_fn             gc_tracers[GC_T__COUNT];
    gc_finalizer_fn          gc_finalizers[GC_T__COUNT];
};

/* Resolve the active per-thread ctx for state S.
 *
 * Worker threads set TLS at spawn; the embedder's thread leaves it
 * NULL and falls through to &S->main_ctx. Inline so the fast path
 * is a TLS load + predictable branch. */
static inline mino_thread_ctx_t *mino_current_ctx(mino_state *S)
{
    return mino_tls_ctx != NULL ? mino_tls_ctx : &S->main_ctx;
}

#include "runtime/host_future.h"

#include "runtime/coordination.h"

/* Inline fast paths for the coordination primitives. Live here, not in
 * coordination.h, because they reach into struct mino_state and
 * mino_thread_ctx_t. Once mino_state is broken into per-subsystem
 * sub-structs these can move to coordination.h alongside their
 * declarations. */

/* Safepoint fast path: the branch inlines into every alloc /
 * eval-impl-entry / loop-recur site. */
static inline void mino_safepoint_poll(mino_state *S)
{
    if (mino_current_ctx(S)->should_yield) {
        mino_safepoint_park(S);
    }
}

/* mino_lock / mino_unlock take the recursive state_lock unconditionally.
 * This ensures correctness when multi_threaded flips mid-eval (a
 * conditional lock could race because the eval that called future-spawn
 * entered without taking the lock when multi_threaded was still 0).
 * Single-threaded states pay one uncontested mutex lock per eval entry,
 * which is on the order of tens of nanoseconds and dominated by other
 * eval costs. A fast-path skip is reintroducable later if safe-flip
 * sequencing can be proven.
 *
 * The outermost acquire / release also swap the BC register stack:
 * on outermost acquire, install this ctx's snapshot into S->bc.bc_regs/
 * cap/top so the worker resumes with its own frame slots, isolated
 * from any other worker that ran during the yield window. On
 * outermost release, snapshot back so the next acquirer can install
 * theirs. Without this, concurrent (fn [x] (thread-sleep) x) calls
 * share one bc_regs stack and clobber each other's args. */
static inline void mino_lock(mino_state *S)
{
    mino_thread_ctx_t *ctx;
    mino_state_lock_acquire(S);
    ctx = mino_current_ctx(S);
    if (ctx->lock_depth == 0) {
        if (ctx->bc_snapshot_valid) {
            S->bc.bc_regs     = ctx->bc_regs_storage;
            S->bc.bc_regs_cap = ctx->bc_regs_storage_cap;
            S->bc.bc_top      = ctx->bc_top_snapshot;
        } else {
            S->bc.bc_regs     = NULL;
            S->bc.bc_regs_cap = 0;
            S->bc.bc_top      = 0;
        }
    }
    ctx->lock_depth++;
}

static inline void mino_unlock(mino_state *S)
{
    mino_thread_ctx_t *ctx = mino_current_ctx(S);
    ctx->lock_depth--;
    if (ctx->lock_depth == 0) {
        ctx->bc_regs_storage     = S->bc.bc_regs;
        ctx->bc_regs_storage_cap = S->bc.bc_regs_cap;
        ctx->bc_top_snapshot     = S->bc.bc_top;
        ctx->bc_snapshot_valid   = 1;
    }
    mino_state_lock_release(S);
}

#include "runtime/error_diag.h"
#include "runtime/env_api.h"

#include "runtime/var_module.h"

/* ------------------------------------------------------------------------- */
/* Monotonic wall-clock nanoseconds. Uses CLOCK_MONOTONIC on POSIX,          */
/* QueryPerformanceCounter on Windows, clock() as coarse fallback.           */
/* Shared between prim_nano_time and gc_major_collect timing.                */
/* ------------------------------------------------------------------------- */

long long mino_monotonic_ns(void);

/* UUID helpers (defined in src/prim/string.c) -- declared here so the
 * reader can build a MINO_UUID directly for the `#uuid "..."` literal
 * without pulling in the full prim/internal.h. */
mino_val *mino_uuid_from_bytes(mino_state *S, const unsigned char *b);
int         mino_uuid_parse(const char *s, size_t len, unsigned char out[16]);

/* Regex constructor (defined in src/prim/regex.c) -- declared here so
 * the reader can build a MINO_REGEX for the `#"..."` literal. */
mino_val *mino_regex_from_source(mino_state *S, mino_val *source);

/* Inline truthiness for hot branch-dispatch paths (eval_if, eval_when,
 * eval_and, eval_or). The exported `mino_is_truthy` in src/mino.h
 * stays available for embedders; this internal sibling sidesteps the
 * function-call cost on the eval-side hot loop. The two must stay in
 * lockstep — adversarial tests catch divergence. */
static inline int mino_is_truthy_inline(const mino_val *v)
{
    if (v == NULL) return 0;
    if (mino_type_of(v) == MINO_NIL) return 0;
    if (mino_type_of(v) == MINO_BOOL) return mino_val_bool_get(v) != 0;
    return 1;
}

/* ------------------------------------------------------------------------- */
/* Ownership conventions used in the per-subsystem internal headers:         */
/*   GC-owned  — returned pointer is managed by the garbage collector.       */
/*               It survives until the next collection unless pinned or      */
/*               reachable from a rooted environment.  Callers that need     */
/*               a value to survive across allocation must gc_pin it.        */
/*   borrowed  — returned pointer aliases existing storage.  The caller      */
/*               must not free it and must not retain it past the next       */
/*               mutation of the owning container.                           */
/*   static    — returned pointer has program lifetime; never freed.         */
/*   malloc-owned — returned pointer must be freed by the caller or by a     */
/*               documented owner (e.g. dyn_binding_list_free).              */
/* Parameters marked "borrowed" are read-only and not retained.              */
/* Parameters marked "consumed" transfer ownership to the callee.            */
/* ------------------------------------------------------------------------- */

#endif /* RUNTIME_INTERNAL_H */
