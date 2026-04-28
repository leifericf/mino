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

#include "mino.h"
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

/* Cycle G4.3: pthread for state_lock + worker threads. Win32 path uses
 * a CRITICAL_SECTION wrapped in state_lock as void* (defined in state.c). */
#if !(defined(_WIN32) && defined(_MSC_VER))
#  include <pthread.h>
#endif
#include <time.h>

/* ------------------------------------------------------------------------- */
/* Runtime support types                                                     */
/* ------------------------------------------------------------------------- */

/* Module cache entry. */
typedef struct {
    char       *name;
    mino_val_t *value;
} module_entry_t;

/* Bundled-stdlib registry entry. Source pointer is a static C-string
 * literal in a generated header (e.g. lib_clojure_string.h); never
 * freed. Name is malloc-owned (copied in mino_register_bundled_lib). */
typedef struct {
    char       *name;
    const char *source;
} bundled_lib_entry_t;

/* Metadata table entry. The capability is the install-group label
 * (e.g. "fs", "proc", "io", "host", "async") used by the doc and
 * mino-capability primitives. NULL means the binding is part of the
 * always-installed core and carries no capability gate. */
typedef struct {
    char       *name;
    char       *docstring;
    char       *capability;
    mino_val_t *source;
} meta_entry_t;

/* Call-stack frame for stack traces. */
#define MAX_CALL_DEPTH 256

typedef struct {
    const char *name;
    const char *file;
    int         line;
    int         column;
} call_frame_t;

/* GC root-environment registry node (malloc-owned). */
typedef struct root_env {
    mino_env_t      *env;
    struct root_env *next;
} root_env_t;

/* Host-retained value ref (malloc-owned). */
struct mino_ref {
    mino_val_t      *val;
    struct mino_ref *next;
    struct mino_ref *prev;
};

/* Dynamic binding frame. */
typedef struct dyn_binding {
    const char          *name;
    mino_val_t          *val;
    struct dyn_binding  *next;
} dyn_binding_t;

typedef struct dyn_frame {
    dyn_binding_t       *bindings;
    struct dyn_frame    *prev;
} dyn_frame_t;

/* Environment binding. */
typedef struct {
    char       *name;
    mino_val_t *val;
} env_binding_t;

/* Namespace alias entry. Each alias is owned by the namespace that
 * declared it via require/use/alias; the same alias name can mean
 * different targets in different namespaces. */
typedef struct {
    char *owning_ns;
    char *alias;
    char *full_name;
} ns_alias_t;

/* Per-namespace root env entry. */
typedef struct {
    const char *name;     /* interned ns name */
    mino_env_t *env;      /* root env for this ns; parent → clojure.core (or NULL for clojure.core itself) */
    mino_val_t *meta;     /* nil or a map of ns-level metadata */
} ns_env_entry_t;

/* Var registry entry. */
typedef struct {
    const char *ns;      /* interned namespace */
    const char *name;    /* interned name */
    mino_val_t *var;     /* the MINO_VAR value */
} var_entry_t;

/* Record-type registry entry. Pinned for the life of the state so
 * MINO_TYPE values keep stable pointer identity across re-evaluation
 * of the same defrecord form. The fields vector is GC-owned and
 * traced via the registry walk in gc_mark_roots. */
typedef struct record_type_entry {
    const char               *ns;    /* interned ns */
    const char               *name;  /* interned name */
    mino_val_t               *type;  /* the MINO_TYPE value */
    struct record_type_entry *next;
} record_type_entry_t;

/* Full environment definition.
 * Large frames (>= ENV_HASH_THRESHOLD bindings) get a hash index for O(1)
 * lookup; small frames use linear scan (faster for typical let/fn sizes). */
#define ENV_HASH_THRESHOLD 32

/* Small-integer cache range. Must fit in the small_ints[] array (256 slots). */
#define MINO_SMALL_INT_LO (-128)
#define MINO_SMALL_INT_HI  127

struct mino_env {
    env_binding_t *bindings;
    size_t         len;
    size_t         cap;
    mino_env_t    *parent;
    size_t        *ht_buckets;  /* hash index: maps hash -> binding slot */
    size_t         ht_cap;      /* power of 2; SIZE_MAX = empty slot */
};

/* ------------------------------------------------------------------------- */
/* Per-thread runtime context (G4.1).                                        */
/*                                                                           */
/* Every field that mutates with eval progress lives here, separately from   */
/* the shared mino_state_t. Each OS thread that enters eval has its own      */
/* ctx; the state pointer is shared. The embedder thread reads main_ctx via */
/* the mino_current_ctx() TLS-fallback accessor; spawned host worker threads */
/* (Cycle G4.3+) install their own ctx via TLS at thread entry.              */
/* ------------------------------------------------------------------------- */

typedef struct mino_thread_ctx {
    /* Eval progress + step limit + interrupt poll. */
    size_t          eval_steps;
    int             limit_exceeded;
    const mino_val_t *eval_current_form;
    volatile int    interrupted;

    /* Safepoint-cooperative-yield flag (Cycle G4.2).
     *
     * Set by the GC driver when a major collection wants every worker
     * for this state to park at its next safepoint, so the collector
     * can run with a stable view of the heap. The mutator polls
     * `should_yield` at canonical safepoints (eval_impl entry,
     * gc_alloc_typed prologue, loop/recur backward branches); when
     * non-zero the mutator calls into the parking slow path.
     *
     * Single-threaded today: nothing sets the flag, so the poll is
     * a single predictably-not-taken branch on the fast path. Cycle
     * G4 later sub-cycles flip it from the GC driver once real host
     * threads exist, and `mino_safepoint_park` blocks the mutator
     * until the collector signals release. */
    volatile int    should_yield;

    /* Exception handling: longjmp targets for try/catch. */
    try_frame_t     try_stack[MAX_TRY_DEPTH];
    int             try_depth;

    /* Error reporting: text buffer + structured diagnostic + frame stack. */
    char            error_buf[2048];
    mino_diag_t    *last_diag;
    call_frame_t    call_stack[MAX_CALL_DEPTH];
    int             call_depth;
    int             trace_added;

    /* GC save stack: transient roots pinned across allocations. */
    mino_val_t     *gc_save[64];
    int             gc_save_len;

    /* Conservative stack scan anchor + GC re-entrancy depth. */
    void           *gc_stack_bottom;
    int             gc_depth;

    /* Dynamic binding stack head. */
    dyn_frame_t    *dyn_stack;

    /* Cycle G4.3: this thread's recursive depth on S->state_lock.
     * mino_lock increments, mino_unlock decrements; mino_yield_lock
     * saves the depth and unlocks down to zero, mino_resume_lock
     * re-locks up to the saved depth. Used by mino_future_deref so
     * the waiter can park on the future's cv without holding the
     * state_lock and starving the worker. */
    int             lock_depth;

    /* Linked list of live worker ctxs (Cycle G4.3). Walked during
     * gc_mark_roots so values pinned by gc_save / dyn_stack / etc.
     * on a blocked worker stay reachable across GCs initiated from
     * another thread. main_ctx is not on this list; the GC walker
     * processes it separately. NULL on main_ctx and on the head
     * sentinel. */
    struct mino_thread_ctx *next_worker;
} mino_thread_ctx_t;

/* TLS pointer to the per-thread ctx for the current worker.
 *
 * NULL on the embedder's main thread (the one that called
 * mino_state_new). Spawned host threads (Cycle G4.3+) set this to
 * their freshly-allocated worker ctx at thread entry and clear it
 * before exit. Accessed only via `mino_current_ctx(S)` (defined
 * below struct mino_state). */
extern
#if defined(_WIN32) && defined(_MSC_VER)
__declspec(thread)
#else
__thread
#endif
mino_thread_ctx_t *mino_tls_ctx;

/* Forward decl for the embed thread pool surface (mino.h provides
 * the struct definition; struct mino_state stores a pointer). */
struct mino_thread_pool;

/* ------------------------------------------------------------------------- */
/* Runtime state                                                             */
/* ------------------------------------------------------------------------- */

struct mino_state {
    /* Per-thread context.
     *
     * `main_ctx` is the embedded ctx for the OS thread that owns S
     * (calls mino_state_new and runs the bulk of work). Spawned host
     * threads (Cycle G4.3+) allocate their own ctx and install it via
     * `mino_tls_ctx` for the duration of the worker run. Code that
     * needs the active ctx calls `mino_current_ctx(S)` which returns
     * the TLS ctx if set, else &main_ctx for the embedder thread. */
    mino_thread_ctx_t  main_ctx;

    /* Garbage collection.
     *
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

    /* Singletons */
    mino_val_t      nil_singleton;
    mino_val_t      true_singleton;
    mino_val_t      false_singleton;
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

    /* Metadata */
    meta_entry_t   *meta_table;
    size_t          meta_table_len;
    size_t          meta_table_cap;

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
     *   0 = allow (default; match S->reader_dialect / clj / default)
     *   1 = preserve (return a reader-conditional record)
     *   2 = disallow (error on any #? or #?@) */
    int             reader_cond_mode;

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

    /* Source cache for diagnostic rendering. */
    #define MINO_SOURCE_CACHE_SIZE 4
    struct {
        const char *file;   /* interned filename */
        char       *text;   /* malloc-owned full source text */
        size_t      len;    /* length of text */
    } source_cache[4];

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

    /* Host interop */
    int             interop_enabled;
    host_type_t    *host_types;
    size_t          host_types_len;
    size_t          host_types_cap;

    /* Eval current_form moved to mino_thread_ctx_t. */

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

    /* Host-thread grant (Cycle G4 foundation).
     *
     * thread_limit is the host-granted ceiling on concurrent host
     * threads. Default 1 (single-threaded; future/promise/etc. throw
     * :mino/unsupported with a message naming the grant API). Set via
     * mino_set_thread_limit. Standalone `./mino` grants cpu_count right
     * after mino_install_all so REPL users get the canonical surface
     * by default; embedders opt in per state.
     *
     * thread_count is the live worker count, incremented at spawn,
     * decremented at join. multi_threaded flips to 1 the first time a
     * spawn actually runs; single-threaded states pay none of the
     * inter-thread coordination cost. The full implementation
     * (per-thread context refactor, GC STW machinery, atom CAS upgrade)
     * lands across upcoming versions; v0.84.x is the API surface plus
     * thrown stubs that distinguish "host has not granted threads"
     * from "host granted but runtime impl is in flight." */
    int             thread_limit;
    int             thread_count;
    int             multi_threaded;

    /* Embed-distinctive thread knobs (Cycle G4.5). NULL/0 leaves the
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

    /* Per-state mutex held across worker-thread eval (Cycle G4.3).
     *
     * Worker threads acquire `state_lock` before running eval and
     * release after, serializing all evals (workers + embedder
     * thread) within a single state. This is a coarse v0.89 model
     * that gives correct semantics for futures/promises/threads
     * without a per-subsystem lock matrix. Cross-state work runs
     * fully concurrent (each state has its own lock); a host pool
     * sharing N workers across M states gets per-state mutual
     * exclusion but pool-wide parallelism.
     *
     * Initialized in state_init unconditionally (mutex_init is
     * cheap). `mino_lock(S)` / `mino_unlock(S)` no-op when
     * !multi_threaded so single-threaded states pay nothing.
     *
     * Cycle G4.4+ relaxes serialization with per-thread allocator
     * arenas + finer-grained registry locks for parallel eval. */
#if defined(_WIN32) && defined(_MSC_VER)
    void           *state_lock;        /* CRITICAL_SECTION; see state.c */
#else
    pthread_mutex_t state_lock;
#endif

    /* Outstanding futures (Cycle G4.3). Singly-linked; quiesce walks
     * this to join worker threads before state teardown. The struct
     * mino_future definition is below (after struct mino_state). */
    mino_future_t *future_list_head;

    /* Linked list of live worker ctxs (Cycle G4.3). Walked during
     * GC root scanning so blocked workers' pinned values stay live.
     * Mutated under state_lock. */
    mino_thread_ctx_t *worker_ctxs_head;

    /* Stop-the-world request for major GC (Cycle G4.2).
     *
     * Set by `gc_request_stw` before running a major collection;
     * mino_safepoint_propagate_stw walks the live thread set and
     * sets `should_yield` on each ctx. Cleared after GC by
     * `gc_release_stw`. In single-threaded mode there is exactly
     * one ctx (S->main_ctx) and the GC is itself the mutator, so
     * the propagation is a single-store no-op. The flag is
     * declared volatile so future multi-threaded sub-cycles can
     * safely read it from worker threads without explicit fences
     * (the ordering invariants are enforced via the same
     * __atomic_* primitives used by atom CAS). */
    volatile int    stw_request;

    /* Async scheduler run queue. */
    sched_entry_t  *async_run_head;
    sched_entry_t  *async_run_tail;

    /* Async timer queue. */
    timer_entry_t  *async_timers;
};

/* Resolve the active per-thread ctx for state S.
 *
 * Worker threads set TLS at spawn; the embedder's thread leaves it
 * NULL and falls through to &S->main_ctx. Inline so the fast path
 * is a TLS load + predictable branch. */
static inline mino_thread_ctx_t *mino_current_ctx(mino_state_t *S)
{
    mino_thread_ctx_t *t = mino_tls_ctx;
    return t != NULL ? t : &S->main_ctx;
}

/* ------------------------------------------------------------------------- */
/* Host-thread future (Cycle G4.3).                                          */
/*                                                                            */
/* Defined here (rather than runtime/host_threads.h) so GC trace + sweep,    */
/* print, and other dispatch sites can see field offsets without including   */
/* host_threads.h transitively. Pthread/Win32 mu+cv wrappers are the only    */
/* platform-conditional fields.                                              */
/* ------------------------------------------------------------------------- */

typedef enum {
    MINO_FUTURE_PENDING   = 0,
    MINO_FUTURE_RESOLVED  = 1,
    MINO_FUTURE_FAILED    = 2,
    MINO_FUTURE_CANCELLED = 3
} mino_future_state_t;

struct mino_future {
    mino_state_t       *state;

#if defined(_WIN32) && defined(_MSC_VER)
    CRITICAL_SECTION    mu;
    CONDITION_VARIABLE  cv;
#else
    pthread_mutex_t     mu;
    pthread_cond_t      cv;
#endif
    int                 state_tag;       /* mino_future_state_t */
    mino_val_t         *result;          /* RESOLVED: worker's return */
    mino_val_t         *exception;       /* FAILED: thrown value */

    volatile int        cancel_flag;     /* set by future-cancel */

#if defined(_WIN32) && defined(_MSC_VER)
    HANDLE              thread;
#else
    pthread_t           thread;
#endif
    int                 thread_started;  /* 1 once spawn succeeded */
    int                 thread_joined;   /* 1 once join completed */

    mino_val_t         *thunk;           /* zero-arg fn for the body */
    mino_val_t         *body_env;        /* env captured at spawn */
    mino_val_t         *dyn_snapshot;    /* TODO: dyn-var conveyance */

    mino_future_t      *next_in_state;   /* S->future_list_head chain */
};

/* ------------------------------------------------------------------------- */
/* Safepoint poll (Cycle G4.2).                                              */
/*                                                                           */
/* Mutators poll `should_yield` at canonical safepoints so a stop-the-world  */
/* major collection can run with a stable view of the heap. The fast path   */
/* is one predictably-not-taken branch; the slow path (mino_safepoint_park) */
/* is in state.c and blocks until the collector signals release.            */
/*                                                                           */
/* Single-threaded today: nothing sets the flag, so park is unreachable on  */
/* the live execution path. The macro is in the header so the branch       */
/* inlines into every alloc / eval-impl-entry / loop-recur site.            */
/* ------------------------------------------------------------------------- */

void mino_safepoint_park(mino_state_t *S);

#define mino_safepoint_poll(S) do {                                       \
    if (mino_current_ctx(S)->should_yield) {                               \
        mino_safepoint_park(S);                                            \
    }                                                                      \
} while (0)

/* GC-side STW driver: request all worker ctxs park before a major sweep,
 * then release them after. Single-threaded today these are O(1) on
 * S->main_ctx; multi-threaded variants iterate the worker set. */
void gc_request_stw(mino_state_t *S);
void gc_release_stw(mino_state_t *S);

/* ------------------------------------------------------------------------- */
/* Per-state lock helpers (Cycle G4.3).                                      */
/*                                                                           */
/* mino_lock(S) / mino_unlock(S) acquire/release S->state_lock when         */
/* multi_threaded is set; otherwise they're a no-op. Held by worker threads */
/* across the entire eval call so single-state futures execute serialized.  */
/* Cross-state work runs fully concurrent (each state has its own lock).    */
/* ------------------------------------------------------------------------- */

void mino_state_lock_init(mino_state_t *S);
void mino_state_lock_destroy(mino_state_t *S);
void mino_state_lock_acquire(mino_state_t *S);
void mino_state_lock_release(mino_state_t *S);

/* mino_lock / mino_unlock take the recursive state_lock unconditionally.
 * This ensures correctness when multi_threaded flips mid-eval (a
 * conditional lock could race because the eval that called future-spawn
 * entered without taking the lock when multi_threaded was still 0).
 * Single-threaded states pay one uncontested mutex lock per eval entry,
 * which is on the order of tens of nanoseconds and dominated by other
 * eval costs. Cycle G4.4+ may reintroduce a fast-path skip after
 * proving safe-flip sequencing. */
#define mino_lock(S) do {                                                 \
    mino_state_lock_acquire(S);                                            \
    mino_current_ctx(S)->lock_depth++;                                     \
} while (0)

#define mino_unlock(S) do {                                               \
    mino_current_ctx(S)->lock_depth--;                                     \
    mino_state_lock_release(S);                                            \
} while (0)

/* Yield: drop down to lock_depth==0, returning the previous depth so
 * the caller can resume to the same level after a blocking wait. */
int  mino_yield_lock(mino_state_t *S);
void mino_resume_lock(mino_state_t *S, int saved_depth);

/* ------------------------------------------------------------------------- */
/* error.c                                                                   */
/* ------------------------------------------------------------------------- */

/* set_error/set_error_at copy msg into the current ctx's error_buf; msg is borrowed. */
void        set_error(mino_state_t *S, const char *msg);          /* msg: borrowed */
void        set_error_at(mino_state_t *S, const mino_val_t *form, /* form: borrowed */
                         const char *msg);                         /* msg: borrowed */
void        clear_error(mino_state_t *S);
void        set_diag(mino_state_t *S, mino_diag_t *d);           /* d: consumed */
void        source_cache_store(mino_state_t *S, const char *file,
                               const char *text, size_t len);
const char *source_cache_get_line(mino_state_t *S, const char *file,
                                  int line, size_t *out_len);
void        set_eval_diag(mino_state_t *S, const mino_val_t *form,
                          const char *kind, const char *code,
                          const char *msg);
const char *type_tag_str(const mino_val_t *v);                    /* static string */
void        push_frame(mino_state_t *S, const char *name,     /* name: borrowed */
                       const char *file, int line,            /* file: borrowed */
                       int column);
void        pop_frame(mino_state_t *S);
void        append_trace(mino_state_t *S);
meta_entry_t *meta_find(mino_state_t *S, const char *name);   /* borrowed into meta_table */
void meta_set(mino_state_t *S, const char *name,              /* name: borrowed (copied) */
              const char *doc, size_t doc_len,                 /* doc: borrowed (copied) */
              mino_val_t *source);                             /* source: GC-owned, retained */
/* meta_set_capability tags a registered binding with its install-group
 * label. Borrows; copies. NULL clears. */
void meta_set_capability(mino_state_t *S, const char *name,
                         const char *capability);

/* ------------------------------------------------------------------------- */
/* env.c: environment and dynamic bindings                                   */
/*                                                                           */
/* Environments are GC-owned. Bindings within are borrowed views.            */
/* ------------------------------------------------------------------------- */

mino_env_t    *env_alloc(mino_state_t *S, mino_env_t *parent); /* GC-owned */
env_binding_t *env_find_here(mino_env_t *env, const char *name); /* borrowed */
void           env_bind(mino_state_t *S, mino_env_t *env,
                        const char *name,                      /* borrowed (copied) */
                        mino_val_t *val);                      /* GC-owned, retained */
void           env_bind_sym(mino_state_t *S, mino_env_t *env,
                        mino_val_t *sym,                       /* interned symbol */
                        mino_val_t *val);                      /* GC-owned, retained */
int            env_unbind(mino_state_t *S, mino_env_t *env,
                        const char *name);                     /* 1 if removed */
mino_env_t    *env_child(mino_state_t *S, mino_env_t *parent); /* GC-owned */
mino_env_t    *env_root(mino_state_t *S, mino_env_t *env);     /* borrowed (walks up) */
mino_val_t    *dyn_lookup(mino_state_t *S, const char *name);  /* borrowed */
void           dyn_binding_list_free(dyn_binding_t *head);     /* frees malloc chain */

/* ------------------------------------------------------------------------- */
/* ns_env.c: per-namespace root env table.                                   */
/*                                                                           */
/* Each ns has a root env owning that ns's def/refer bindings. Every ns env  */
/* except clojure.core has parent → clojure.core, so unqualified lookup     */
/* walks lexical → current-ns env → clojure.core env.                        */
/* ------------------------------------------------------------------------- */

void load_stack_truncate(mino_state_t *S, size_t len);
mino_env_t *ns_env_lookup(mino_state_t *S, const char *name);   /* borrowed */
mino_env_t *ns_env_ensure(mino_state_t *S, const char *name);   /* GC-owned, rooted */
mino_val_t *ns_symbol_with_meta(mino_state_t *S, const char *name);
void        mino_publish_current_ns(mino_state_t *S);
mino_val_t *ns_env_get_meta(mino_state_t *S, const char *name);
void        ns_env_set_meta(mino_state_t *S, const char *name, mino_val_t *meta);
mino_env_t *current_ns_env(mino_state_t *S);                    /* GC-owned, rooted */

/* ------------------------------------------------------------------------- */
/* var.c: var registry helpers                                               */
/* ------------------------------------------------------------------------- */

mino_val_t    *var_intern(mino_state_t *S, const char *ns, const char *name);
void           var_set_root(mino_state_t *S, mino_val_t *var, mino_val_t *val);
mino_val_t    *var_find(mino_state_t *S, const char *ns, const char *name);
void           var_unintern(mino_state_t *S, const char *ns, const char *name);

/* ------------------------------------------------------------------------- */
/* state.c: per-state PRNG. Seeds lazily on first call.                      */
/* ------------------------------------------------------------------------- */

uint64_t state_rand64(mino_state_t *S);

/* ------------------------------------------------------------------------- */
/* module.c: shared module-resolution helpers used by the                    */
/* ns special form (eval/defs.c) and the require primitive                   */
/* (prim/module.c).                                                          */
/* ------------------------------------------------------------------------- */

int  runtime_module_dotted_to_path(const char *name, size_t nlen,
                                   char *buf, size_t bufsize);
void runtime_module_add_alias(mino_state_t *S,
                              const char *alias, const char *full);

/* ------------------------------------------------------------------------- */
/* Monotonic wall-clock nanoseconds. Uses CLOCK_MONOTONIC on POSIX,          */
/* QueryPerformanceCounter on Windows, clock() as coarse fallback.           */
/* Shared between prim_nano_time and gc_major_collect timing.                */
/* ------------------------------------------------------------------------- */

long long mino_monotonic_ns(void);

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
