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
    /*
     * gc_all_young and gc_all_old are singly-linked lists partitioning
     * every live header by generation. Alloc prepends to the young
     * list; minor sweep walks only young (promotion moves a header
     * between lists); major sweep walks old to free dead, and young
     * only to clear mark bits major set during cross-gen tracing.
     * Keeping the two separated caps minor-sweep cost at O(young-live)
     * instead of O(total-heap). */
    gc_hdr_t       *gc_all_young;
    gc_hdr_t       *gc_all_old;
    size_t          gc_bytes_alloc;
    size_t          gc_bytes_live;
    size_t          gc_threshold;
    int             gc_stress;
    /* gc_depth and gc_stack_bottom moved to mino_thread_ctx_t. */
    root_env_t     *gc_root_envs;
    gc_range_t     *gc_ranges;
    size_t          gc_ranges_len;
    size_t          gc_ranges_cap;
    size_t          gc_ranges_valid;
    /* Allocations between collections land here instead of memmove-ing
     * into the sorted main array on every alloc. Merged at the next
     * collection via sort-then-merge (cheaper than re-qsorting n+K
     * entries from scratch). Grows dynamically; sized by
     * gc_ranges_pending_cap. */
    gc_range_t     *gc_ranges_pending;
    size_t          gc_ranges_pending_len;
    size_t          gc_ranges_pending_cap;
    size_t          gc_collections_minor;
    size_t          gc_collections_major;
    size_t          gc_total_freed;
    size_t          gc_total_ns;       /* cumulative ns spent in gc_major_collect */
    size_t          gc_max_ns;         /* largest single-collection ns */
    /* Generational bookkeeping. Maintained continuously on every
     * allocation, sweep, and promotion: gc_bytes_young + gc_bytes_old
     * equals gc_bytes_alloc. gc_old_baseline captures gc_bytes_old
     * right after the last major sweep; future major cycles trigger
     * when gc_bytes_old exceeds baseline by the growth tenths factor. */
    size_t          gc_bytes_young;
    size_t          gc_bytes_old;
    size_t          gc_old_baseline;
    /* Remembered set: every old-gen header that observed a store of a
     * young-gen pointer since the last minor or major cycle. The array
     * doubles as needed. Each member has gc_hdr_t::dirty = 1 while
     * present, so repeated stores to the same container are deduped. */
    gc_hdr_t      **gc_remset;
    size_t          gc_remset_len;
    size_t          gc_remset_cap;
    /* High-water mark of gc_remset_len across this state's lifetime.
     * Exposed via mino_gc_stats so embedders can size remset-sensitive
     * workloads without instrumenting the runtime. */
    size_t          gc_remset_high_water;
    /* Collector tuning parameters. gc_nursery_bytes triggers a minor
     * collection when exceeded. gc_promotion_age is the number of
     * minor survivals before a young object flips to old. Both have
     * defaults from state_init; a future mino_gc_set_param lets a
     * host override them. */
    size_t          gc_nursery_bytes;
    unsigned        gc_promotion_age;
    /* Major trigger threshold: gc_bytes_old must exceed
     * gc_old_baseline * gc_major_growth_tenths / 10 (floored at
     * gc_threshold) before a major cycle is started. Tenths precision
     * keeps the setter integer-only while covering 1.1x through 4.0x. */
    unsigned        gc_major_growth_tenths;
    /* Incremental major parameters. gc_major_work_budget bounds the
     * headers popped per gc_major_step slice; gc_major_alloc_quantum is
     * the allocation volume that has to accumulate between steps before
     * the alloc path fires another slice. gc_major_step_alloc holds
     * the running count since the previous step. All three are shared
     * between the driver (gc/driver.c) and gc_major_step itself. */
    size_t          gc_major_work_budget;
    size_t          gc_major_alloc_quantum;
    size_t          gc_major_step_alloc;
    int             gc_phase;
    gc_hdr_t      **gc_mark_stack;
    size_t          gc_mark_stack_len;
    size_t          gc_mark_stack_cap;
    /* High-water mark of gc_mark_stack_len across this state's
     * lifetime. Exposed via mino_gc_stats. */
    size_t          gc_mark_stack_high_water;
    gc_hdr_t       *gc_freelists[4];   /* per-size-class recycling */
    /* Cached [min, max) bounds of all managed allocations. Lets the
     * conservative stack scan reject non-pointer words before doing a
     * binary search through the range index. */
    uintptr_t       gc_heap_min;
    uintptr_t       gc_heap_max;

    /* GC event ring buffer (diagnostic only). Allocated lazily when
     * MINO_GC_EVT=1 is set at state init; NULL otherwise and every
     * recording site is a no-op. See gc_evt_t. */
    gc_evt_t       *gc_evt_ring;
    uint64_t        gc_evt_seq;

    /* === Value caches: singletons, sentinels, interns, special forms === */

    /* Singletons */
    mino_val_t      nil_singleton;
    mino_val_t      true_singleton;
    mino_val_t      false_singleton;
    mino_val_t      empty_list_singleton;
    /* Trampoline sentinels reused across recur/tail-call to avoid
     * per-iteration allocation. Their args/fn fields are replaced in-place
     * and the containing eval loop consumes them before any other code
     * runs, so sharing one cell per kind is safe. */
    mino_val_t      recur_sentinel;
    mino_val_t      tail_call_sentinel;

    /* Small-integer cache: mino_int(S, n) returns the shared cell for
     * n in [MINO_SMALL_INT_LO, MINO_SMALL_INT_HI]. Arithmetic-heavy code
     * (fib, loops, reductions) produces many small-int results and
     * re-boxing them dominates allocation without this cache. */
    mino_val_t      small_ints[256];

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
    mino_val_t     *sf_quote;
    mino_val_t     *sf_quasiquote;
    mino_val_t     *sf_unquote;
    mino_val_t     *sf_unquote_splicing;
    mino_val_t     *sf_defmacro;
    mino_val_t     *sf_declare;
    mino_val_t     *sf_ns;
    mino_val_t     *sf_var;
    mino_val_t     *sf_def;
    mino_val_t     *sf_if;
    mino_val_t     *sf_do;
    mino_val_t     *sf_let;
    mino_val_t     *sf_let_star;
    mino_val_t     *sf_fn;
    mino_val_t     *sf_fn_star;
    mino_val_t     *sf_recur;
    mino_val_t     *sf_loop;
    mino_val_t     *sf_loop_star;
    mino_val_t     *sf_try;
    mino_val_t     *sf_binding;
    mino_val_t     *sf_lazy_seq;
    mino_val_t     *sf_new;
    mino_val_t     *sf_when;
    mino_val_t     *sf_and;
    mino_val_t     *sf_or;

    /* === Module system, execution limits, metadata ===================== */

    /* Execution limits (config knobs; set once by host, read by ctx). */
    size_t          limit_steps;
    size_t          limit_heap;

    /* Module system */
    mino_resolve_fn module_resolver;
    void           *module_resolver_ctx;
    module_entry_t *module_cache;
    size_t          module_cache_len;
    size_t          module_cache_cap;
    /* Load stack: names of modules currently being loaded (for cycle
     * detection). A module is on this stack between require entry and
     * the point where it's added to module_cache. */
    char          **load_stack;
    size_t          load_stack_len;
    size_t          load_stack_cap;
    /* Bundled-stdlib registry: name -> static C-string source pointer.
     * Populated by mino_install_clojure_<name> hooks. Consulted by
     * (require ...) before the disk resolver, so brew/scoop installs
     * with no lib/ on disk still load the bundled namespaces. The
     * source pointers are static literals in generated headers and
     * are never freed; only the array storage is malloc-owned. */
    bundled_lib_entry_t *bundled_libs;
    size_t               bundled_libs_len;
    size_t               bundled_libs_cap;
    /* Extra load paths registered at runtime via (add-load-path! ...).
     * Consulted by the main.c resolver after the project_paths and
     * before the cwd fallback. Each entry is a malloc-owned dup;
     * freed at state teardown. */
    char               **extra_load_paths;
    size_t               extra_load_paths_len;
    size_t               extra_load_paths_cap;

    /* Metadata */
    meta_entry_t   *meta_table;
    size_t          meta_table_len;
    size_t          meta_table_cap;

    /* Capability bitmask. Bit set per MINO_CAP_* constant in mino.h
     * when the corresponding `mino_install_<cap>` runs. Consulted by
     * the `mino-installed?` primitive for core.clj gates and by the
     * eval_symbol MNS002 diagnostic to enrich "unbound symbol" errors
     * with the capability that would enable the name. Defaults to 0
     * so a bare `mino_state_new` + `mino_install_minimal` runtime sees
     * no capabilities until the embedder opts in. */
    unsigned int    caps_installed;

    /* === Printer, reader, source diagnostics =========================== */

    /* Printer */
    int             print_depth;

    /* Error reporting state moved to mino_thread_ctx_t (error_buf,
     * call_stack, call_depth, trace_added, last_diag). */

    /* Reader */
    const char     *reader_file;
    int             reader_line;
    int             reader_col;
    const char     *reader_dialect;   /* "mino" */
    /* :read-cond mode for reader conditionals.
     *   0 = allow (default; match S->reader_dialect / default)
     *   1 = preserve (return a reader-conditional record)
     *   2 = disallow (error on any #? or #?@) */
    int             reader_cond_mode;
    /* Transient flag: set by `#?(...)` when no branch matched so the
     * read returned NULL silently (the "no form produced" signal that
     * lists/vectors/maps handle by skipping). Wrap-one reader macros
     * (`@`, `'`, `` ` ``, `~`, `~@`, `#'`) check this when they get a
     * silent NULL inner so the diagnostic names the actual cause
     * instead of the misleading "expected form after @". Reset at the
     * entry of every `read_form` call. */
    int             reader_last_cond_empty;

    /* Filename intern table. Strings are malloc-owned, freed at state
     * teardown. Held here (not process-global) so two runtimes on two
     * threads don't race on a shared table. */
    const char    **interned_files;
    size_t          interned_files_len;
    size_t          interned_files_cap;

    /* Var-name intern table (ns + name for MINO_VAR). Same rationale
     * as interned_files: strings outlive the state's vars but not the
     * state itself. */
    const char    **interned_var_strs;
    size_t          interned_var_strs_len;
    size_t          interned_var_strs_cap;
    /* Open-addressing hash mirror over interned_var_strs, keyed on the
     * string contents. Each slot stores the canonical pointer; NULL
     * marks an empty slot. cap is always a power of two; resize when
     * load factor exceeds 0.7. Linear scans dominated cold-start
     * install before this index existed (~640 vars => 400k strcmps). */
    const char    **interned_var_strs_hash;
    size_t          interned_var_strs_hash_cap;

    /* Source cache for diagnostic rendering. */
    #define MINO_SOURCE_CACHE_SIZE 4
    struct {
        const char *file;   /* interned filename */
        char       *text;   /* malloc-owned full source text */
        size_t      len;    /* length of text */
    } source_cache[4];

    /* === Namespaces, vars, host interop ================================ */

    /* Namespace */
    const char     *current_ns;       /* from (ns ...), default "user" */
    ns_alias_t     *ns_aliases;
    size_t          ns_alias_len;
    size_t          ns_alias_cap;

    /* Per-namespace root env table. Each entry's env owns the ns's
     * value bindings (def, refer). Every ns env except clojure.core has
     * parent → clojure.core, so unqualified lookup walks ns → clojure.core. */
    ns_env_entry_t *ns_env_table;
    size_t          ns_env_len;
    size_t          ns_env_cap;
    mino_env_t     *mino_core_env;    /* root env for clojure.core; parent NULL */
    /* Ambient namespace for free-var resolution inside the active fn body.
     * apply_callable sets this to the fn's defining ns; eval_symbol's
     * fall-through consults it after current_ns_env, so `(ns ...)`
     * mutations inside a body don't lose access to the macros and helpers
     * the body was written against. */
    const char     *fn_ambient_ns;

    /* Var registry */
    var_entry_t    *var_registry;
    size_t          var_registry_len;
    size_t          var_registry_cap;
    /* Monomorphic inline call cache. Keyed on the call form pointer
     * plus the head symbol's interned data pointer. The form and the
     * callable in each filled slot are walked by gc_mark_runtime_globals,
     * so the GC keeps both alive for the cache lifetime; this prevents
     * the freed-and-recycled-form aliasing problem. Var redefinition
     * bumps `ic_gen`, invalidating every slot in one shot. Allocated
     * lazily on first hit-prone call. */
    struct ic_slot {
        mino_val_t *form;          /* call form pointer, NULL = empty */
        const char *head_data;     /* sym->as.s.data, interned */
        mino_val_t *callable;
        unsigned    gen_at_fill;
    } *ic_table;
    size_t          ic_cap;
    unsigned        ic_gen;
    /* Monotonic owner-ID generator for transient batch mutators
     * (src/collections/transient.c). Each new (transient ...) call
     * pre-increments and reads this counter; vec nodes whose owner
     * field equals the resulting ID are mutated in place by the
     * matching transient. Using an ID rather than the transient
     * mino_val_t pointer avoids the address-reuse hazard after a
     * transient is GC'd. The vec node `owner` field is 32 bits, so
     * the runtime caps mintable IDs at 2^32 - 1; transients past
     * that cap (astronomically unlikely in a real process) fall
     * back to the wrapper path. */
    uint32_t        transient_owner_next;
    /* Open-addressing hash mirror keyed on the (interned-ns*, interned-name*)
     * pointer pair. var_intern / var_find / var_unintern hit this first;
     * the linear var_registry remains the source of truth (and the GC
     * root-walk target). cap is always a power of two; len counts
     * occupied slots. ns == NULL marks an empty slot. */
    var_hash_slot_t *var_hash;
    size_t          var_hash_cap;
    size_t          var_hash_len;

    /* Bytecode VM register stack. The bytecode VM (src/eval/bc/vm.c)
     * runs each compiled fn in a slot window inside this single stack;
     * a fn entry pushes n_regs slots, a fn exit pops them. Every slot
     * in [0, bc_top) is a live GC root walked by gc_mark_roots so the
     * GC keeps the values reachable across collections triggered from
     * inside the VM. Allocated lazily on first compile + run; NULL
     * until then. */
    mino_val_t    **bc_regs;
    size_t          bc_regs_cap;
    size_t          bc_top;

    /* Pointer-tagged int counters: bc_int_make_count counts every call
     * to mino_int(S, n); bc_int_alloc_avoided counts those that
     * returned a tagged value instead of allocating a boxed MINO_INT
     * cell. Only maintained when MINO_BC_PROFILE_COUNTS is defined --
     * the increments live on the hottest path in the VM, and steady-
     * state runs leave them off so the arith fast lane doesn't pay
     * for two unconditional writes per tagged-int production. */
    size_t          bc_int_make_count;
    size_t          bc_int_alloc_avoided;

    /* JIT executable-region list (CPJIT). Each entry is one mmap'd
     * page-aligned region that the runtime materialised through the
     * copy-and-patch pipeline. mino_state_free walks the list and
     * munmaps every region so the OS reclaims the executable pages
     * at state teardown. Empty until the first JIT compile. Field
     * stays present even with MINO_CPJIT disabled (always NULL); the
     * cleanup path checks for non-NULL before crossing the JIT-only
     * code so embedders linking against a non-JIT build pay only the
     * pointer slot. */
    struct mino_jit_region *jit_regions;

    /* Active JIT-invoke ctx. Published by mino_jit_invoke before it
     * jumps into the native region and restored on return. Lets
     * stencil-emitted code reach the calling thread's ctx (for
     * `dyn_stack`, `jit_invoke_env`, etc.) via a single fixed-offset
     * load from S, with no Darwin TLVP relocation in the stencil
     * bytes -- the stencil_extract tool does not handle TLV-class
     * relocations today, so the inline TLS sequence
     * `mino_current_ctx(S)` would emit can't survive the
     * copy-and-patch round-trip. Save / restore around the call
     * supports re-entry from a nested JIT'd callee. NULL outside an
     * active JIT region. */
    struct mino_thread_ctx *jit_invoke_ctx;

    /* Per-state JIT mode: AUTO (default) / OFF / ON. Threaded
     * through the fn.c JIT trigger so a single embedded runtime can
     * disable JIT for one VM and leave it enabled for another in
     * the same process. See mino.h::mino_jit_mode_t. Placed after
     * jit_invoke_ctx so the runtime_layout.h offset constants the
     * stencil bytes depend on do not shift. */
    int             jit_mode;
    /* Per-state hot threshold (call count before AUTO triggers a
     * compile). Defaults to MINO_JIT_THRESHOLD; overridable via
     * mino_state_set_jit_hot_threshold so embedders can soften /
     * tighten the warm-up window per workload. */
    unsigned        jit_hot_threshold;

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

    /* (Removed in v0.362.0: gc_barrier_clear_only was a one-cycle
     * verification counter for the SATB-drop audit. Field kept
     * out of the struct now that the drop has shipped.) */

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
    mino_val_t     *sort_comp_fn;
    mino_env_t     *sort_comp_env;

    /* Late-binding print-method hook. NULL during core bootstrap and any
     * state that never installed one; set via set-print-method! once the
     * multimethod is registered. When non-NULL, prim_pr / prim_prn route
     * each argument through this fn instead of calling mino_print
     * directly. The hook is expected to write to stdout as a side effect. */
    mino_val_t     *print_method_fn;

    /* Gensym counter */
    long            gensym_counter;

    /* `letfn*` special form symbol. Lives down here (rather than in
     * the sf_* block above) because that block sits at byte offsets
     * pinned by the JIT layout assertions in
     * src/eval/bc/stencils/runtime_layout.h. New special-form symbols
     * land after the layout-tracked region. */
    mino_val_t     *sf_letfn_star;

    /* Host-retained value refs */
    mino_ref_t     *ref_roots;

    /* Dynamic bindings, interrupt flag, and GC save stack moved to
     * mino_thread_ctx_t (dyn_stack, interrupted, gc_save, gc_save_len). */

    /* Cached parsed core.clj forms (avoids re-parsing on second
     * mino_install_core call within the same state). */
    mino_val_t    **core_forms;
    size_t          core_forms_len;

    /* Fault injection: when fi_alloc_countdown > 0, decrement on each
     * gc_alloc_typed call; when it reaches zero, simulate OOM. */
    long            fi_alloc_countdown;

    /* Fault injection for raw (non-GC) allocation paths such as the
     * clone serialization buffer. Same semantics as above. */
    long            fi_raw_countdown;

    /* === Host-thread runtime: grant, knobs, lock, futures, STW ========= */

    /* Host-thread grant.
     *
     * thread_limit is the host-granted ceiling on concurrent host
     * threads. Default 1 (single-threaded; future/promise/etc. throw
     * :mino/unsupported with a message naming the grant API). Set via
     * mino_set_thread_limit. Standalone `./mino` grants cpu_count right
     * after mino_install_all so REPL users get the canonical surface
     * by default; embedders opt in per state.
     *
     * thread_count is the live worker count, incremented at spawn,
     * decremented at join. Mutated under worker_list_lock (NOT
     * state_lock), so a tight embedder loop holding state_lock cannot
     * stall a worker's exit-decrement. multi_threaded flips to 1 the
     * first time a spawn actually runs; single-threaded states pay
     * none of the inter-thread coordination cost. The full
     * implementation
     * (per-thread context refactor, GC STW machinery, atom CAS upgrade)
     * lands across upcoming versions; v0.84.x is the API surface plus
     * thrown stubs that distinguish "host has not granted threads"
     * from "host granted but runtime impl is in flight." */
    int             thread_limit;
    /* thread_count is mutated by host_threads under worker_list_lock
     * but read without a lock from gc_tick_should_suppress and
     * mino_thread_count (relaxed observability counter -- see
     * runtime/state.c). Plain int was UB under the C standard and
     * TSan flagged the race. All accesses go through __atomic_*
     * with RELAXED ordering: the counter approximation tolerates
     * stale reads, and the lock-side writes pair with the
     * lock-side reads for the cases that need a tight value. */
    int             thread_count;
    int             multi_threaded;

    /* Embed-distinctive thread knobs. NULL/0 leaves the
     * spawn-per-future default behaviour intact. See mino.h for the
     * surface. thread_pool, when non-NULL, redirects spawn from
     * pthread_create to pool->submit_fn. thread_start_fn / thread_end_fn
     * fire on the worker thread for the spawn-per-future path only;
     * pool-managed work items run under the pool's own lifecycle.
     * thread_stack_size is applied via pthread_attr when set. */
    struct mino_thread_pool *thread_pool;
    void          (*thread_start_fn)(mino_state_t *S, void *ctx);
    void          (*thread_end_fn)(mino_state_t *S, void *ctx);
    void           *thread_factory_ctx;
    size_t          thread_stack_size;

    /* Per-state mutex held across worker-thread eval.
     *
     * Worker threads acquire `state_lock` before running eval and
     * release after, serializing all evals (workers + embedder
     * thread) within a single state. This is a coarse model that
     * gives correct semantics for futures/promises/threads without
     * a per-subsystem lock matrix. Cross-state work runs fully
     * concurrent (each state has its own lock); a host pool sharing
     * N workers across M states gets per-state mutual exclusion but
     * pool-wide parallelism.
     *
     * Initialized in state_init unconditionally (mutex_init is
     * cheap). `mino_lock(S)` / `mino_unlock(S)` no-op when
     * !multi_threaded so single-threaded states pay nothing. */
#if defined(_WIN32) && defined(_MSC_VER)
    void           *state_lock;        /* CRITICAL_SECTION; see state.c */
#else
    pthread_mutex_t state_lock;
#endif

    /* Outstanding futures. Singly-linked; quiesce walks this to join
     * worker threads before state teardown. The struct mino_future
     * definition is below (after struct mino_state). */
    mino_future_t *future_list_head;

    /* Linked list of live worker ctxs. Walked during GC root
     * scanning so blocked workers' pinned values stay live.
     * Mutated under worker_list_lock (see below) -- NOT state_lock.
     * The split keeps worker bookkeeping off the heavy eval lock so a
     * tight embedder loop that holds state_lock can't starve workers
     * at their entry-link / exit-detach steps.
     *
     * Lock order (when both are needed): state_lock outer,
     * worker_list_lock inner. Workers at entry/exit acquire
     * worker_list_lock alone. The spawn path and GC root scan reach
     * worker_list_lock from inside state_lock. */
    mino_thread_ctx_t *worker_ctxs_head;

    /* Brief mutex guarding worker_ctxs_head and thread_count.
     * Non-recursive; held only across the linked-list mutation +
     * counter step. See comment above worker_ctxs_head for the lock
     * order. Initialized in state_init unconditionally; cheap to
     * carry in single-threaded mode (one mutex_init at state-create
     * time, never contended). */
#if defined(_WIN32) && defined(_MSC_VER)
    void           *worker_list_lock;  /* CRITICAL_SECTION; see state.c */
#else
    pthread_mutex_t worker_list_lock;
#endif

    /* Stop-the-world request for major GC.
     *
     * Set by `gc_request_stw` before running a major collection;
     * mino_safepoint_propagate_stw walks the live thread set and
     * sets `should_yield` on each ctx. Cleared after GC by
     * `gc_release_stw`. In single-threaded mode there is exactly
     * one ctx (S->main_ctx) and the GC is itself the mutator, so
     * the propagation is a single-store no-op. The flag is declared
     * volatile so future multi-threaded code can safely read it
     * from worker threads without explicit fences (the ordering
     * invariants are enforced via the same __atomic_* primitives
     * used by atom CAS). */
    volatile int    stw_request;

    /* === STM (refs / dosync) ============================================ */

    /* Global STM commit lock. Held only across the commit phase of a
     * transaction (read-set validation + writes + version bumps); watch
     * dispatch runs outside it. Lazy-initialized on the first call to
     * mino_install_stm so embedders that never opt into STM pay only
     * the bool tracking cost. Coarse-grained on purpose: mino's typical
     * embedded workload is single-digit refs and a handful of worker
     * threads, where contention from a single mutex is far cheaper than
     * the per-ref pthread_mutex_t alternative. */
#if defined(_WIN32) && defined(_MSC_VER)
    void           *stm_commit_lock;   /* CRITICAL_SECTION; lazy */
#else
    pthread_mutex_t stm_commit_lock;
#endif
    int             stm_lock_inited;

    /* Monotonic counter for tx-ref IDs. Refs get a stable identity
     * value at construction; not currently used for lock ordering
     * (single global commit lock) but kept reserved for a future
     * fine-grained lock matrix. */
    uint64_t        stm_next_ref_id;

    /* Monotonic counter for agent IDs. Mirrors stm_next_ref_id so
     * the agent print form (#agent[ID VAL]) can distinguish two
     * agents that happen to hold the same value. Without it,
     * (= (pr-str a1) (pr-str a2)) was true for distinct agents
     * with equal state -- a debugging foot-gun and a divergence
     * from the ref print form. */
    uint64_t        agent_next_id;

    /* Flipped to 1 by (shutdown-agents). After shutdown, every send
     * / send-off throws MST008 instead of running its action; the
     * worker drains the remaining queued actions and then exits.
     * Idempotent: calling twice is a no-op. */
    int             agents_shutdown;

    /* Per-state agent workers + run queues (split into POOLED and
     * SOLO; see agent_pool_kind_t above).
     *
     * One worker thread per pool serves all agents in this state.
     * Each worker counts against thread_limit, so a host that never
     * grants threads (default thread_limit == 1) cannot use agents
     * -- send/send-off throw MTH001 in that case, the same shape
     * future/promise/thread
     * already use. Standalone `./mino` raises thread_limit to cpu_count
     * after install_all so the standalone REPL works out of the box.
     *
     * The worker holds state_lock for the duration of each action's
     * eval. Per-state eval-lock serialization means parallelism across
     * agents within one state isn't a goal here; the worker exists to
     * decouple send (returns immediately) from action execution and to
     * give await meaningful blocking semantics. mino's eval lock means
     * single-action-per-state regardless of pool size; Phase 2 may add
     * a send vs send-off split for IO-bound actions that release the
     * lock during blocking calls.
     *
     * Each pool's run_head / run_tail form a singly-linked FIFO of
     * (agent, fn, extra, dyn_snap) tuples. agent_mu serializes access
     * to all pool queues and to per-agent in_flight counters. agent_cv
     * is shared by the workers (waiting for their pool's work) and by
     * await callers (waiting for an agent's in_flight to reach zero);
     * broadcasts happen on enqueue and after each action completes.
     *
     * Each pool's worker is lazy-spawned on first send/send-off into
     * that pool, and joined by shutdown-agents. Queues live entirely
     * outside the GC heap (malloc-owned nodes); GC tracing reaches
     * the held mino_val_t pointers via gc_mark_agent_runq. */
#if defined(_WIN32) && defined(_MSC_VER)
    CRITICAL_SECTION     agent_mu;
    CONDITION_VARIABLE   agent_cv;
#else
    pthread_mutex_t      agent_mu;
    pthread_cond_t       agent_cv;
#endif
    /* Per-pool state. worker_alive: worker thread is currently in its
     * loop (mutated under agent_mu); cleared before the worker exits
     * so the next send observes "no live worker" and re-spawns.
     * worker_pending_join: worker holds a joinable handle for a thread
     * that has exited (or is exiting) but has not been joined yet
     * (mutated under state_lock). Cleared after the join. The worker
     * exits lazily when its run-queue drains so a long-idle agent
     * worker doesn't keep S->thread_count > 0 and suppress GC for the
     * rest of the state's lifetime; the spawn cost on a subsequent
     * burst is the price. */
    struct {
        agent_action_node_t *run_head;
        agent_action_node_t *run_tail;
#if defined(_WIN32) && defined(_MSC_VER)
        HANDLE               worker;
#else
        pthread_t            worker;
#endif
        int                  worker_alive;
        int                  worker_pending_join;
    } agent_pool[AGENT_POOL_COUNT];

    /* agent_mu_inited: has agent_mu/agent_cv been pthread_*_init'd
     * yet (lazy on first send into either pool). */
    int                  agent_mu_inited;

    /* === Async scheduler and timers ==================================== */

    /* Async scheduler run queue. */
    sched_entry_t  *async_run_head;
    sched_entry_t  *async_run_tail;

    /* Async timer queue. */
    timer_entry_t  *async_timers;

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
static inline mino_thread_ctx_t *mino_current_ctx(mino_state_t *S)
{
    return mino_tls_ctx != NULL ? mino_tls_ctx : &S->main_ctx;
}

#include "runtime/host_future.h"

#include "runtime/coordination.h"

/* Inline fast paths for the coordination primitives. Live here, not in
 * coordination.h, because they reach into struct mino_state and
 * mino_thread_ctx_t. Once Cycle 4 decomposes mino_state into per-
 * subsystem sub-structs these can move to coordination.h alongside
 * their declarations. */

/* Safepoint fast path: the branch inlines into every alloc /
 * eval-impl-entry / loop-recur site. */
static inline void mino_safepoint_poll(mino_state_t *S)
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
 * on outermost acquire, install this ctx's snapshot into S->bc_regs/
 * cap/top so the worker resumes with its own frame slots, isolated
 * from any other worker that ran during the yield window. On
 * outermost release, snapshot back so the next acquirer can install
 * theirs. Without this, concurrent (fn [x] (thread-sleep) x) calls
 * share one bc_regs stack and clobber each other's args. */
static inline void mino_lock(mino_state_t *S)
{
    mino_thread_ctx_t *ctx;
    mino_state_lock_acquire(S);
    ctx = mino_current_ctx(S);
    if (ctx->lock_depth == 0) {
        if (ctx->bc_snapshot_valid) {
            S->bc_regs     = ctx->bc_regs_storage;
            S->bc_regs_cap = ctx->bc_regs_storage_cap;
            S->bc_top      = ctx->bc_top_snapshot;
        } else {
            S->bc_regs     = NULL;
            S->bc_regs_cap = 0;
            S->bc_top      = 0;
        }
    }
    ctx->lock_depth++;
}

static inline void mino_unlock(mino_state_t *S)
{
    mino_thread_ctx_t *ctx = mino_current_ctx(S);
    ctx->lock_depth--;
    if (ctx->lock_depth == 0) {
        ctx->bc_regs_storage     = S->bc_regs;
        ctx->bc_regs_storage_cap = S->bc_regs_cap;
        ctx->bc_top_snapshot     = S->bc_top;
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
mino_val_t *mino_uuid_from_bytes(mino_state_t *S, const unsigned char *b);
int         mino_uuid_parse(const char *s, size_t len, unsigned char out[16]);

/* Regex constructor (defined in src/prim/regex.c) -- declared here so
 * the reader can build a MINO_REGEX for the `#"..."` literal. */
mino_val_t *mino_regex_from_source(mino_state_t *S, mino_val_t *source);

/* Inline truthiness for hot branch-dispatch paths (eval_if, eval_when,
 * eval_and, eval_or). The exported `mino_is_truthy` in src/mino.h
 * stays available for embedders; this internal sibling sidesteps the
 * function-call cost on the eval-side hot loop. The two must stay in
 * lockstep — adversarial tests catch divergence. */
static inline int mino_is_truthy_inline(const mino_val_t *v)
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
