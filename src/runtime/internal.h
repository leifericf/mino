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

/* ------------------------------------------------------------------------- */
/* Tagged-value debug invariants                                             */
/* ------------------------------------------------------------------------- */

/*
 * Internal assertion helpers for the pointer-tagged representation
 * (see mino.h "Pointer-tagged value representation"). Active in
 * builds with assertions enabled; compile to no-ops under -DNDEBUG.
 * Runtime-internal; embedders use the public MINO_IS_* / MINO_*_VAL
 * macros directly.
 */
#define MINO_ASSERT_INT(v)            assert(MINO_IS_INT(v))
#define MINO_ASSERT_PTR(v)            assert(MINO_IS_PTR(v))
#define MINO_ASSERT_TAGGED_NONNULL(v) assert((v) != NULL)

/* Alignment guard for newly-allocated heap objects: every alloc site
 * must return a pointer with the low three bits clear, otherwise the
 * tag scheme silently corrupts. */
#define MINO_ASSERT_ALIGNED(p) \
    assert(((uintptr_t)(p) & MINO_TAG_MASK) == 0)

/* Effective type discriminator: returns the inline-tagged type for
 * tagged scalars, MINO_NIL for NULL, otherwise the boxed header type.
 * Use this in switch / type comparisons so the dispatch is
 * form-agnostic and NULL-safe. */
static inline mino_type_t mino_type_of(const mino_val_t *v)
{
    uintptr_t tag;
    if (v == NULL) return MINO_NIL;
    tag = (uintptr_t)v & MINO_TAG_MASK;
    if (tag == MINO_TAG_PTR) return v->type;
    if (tag == MINO_TAG_INT) return MINO_INT;
    if (tag == MINO_TAG_BOOL) return MINO_BOOL;
    if (tag == MINO_TAG_NIL) return MINO_NIL;
    if (tag == MINO_TAG_CHAR) return MINO_CHAR;
    return v->type; /* unreachable: reserved tags */
}

/* Unified accessors that handle both inline-tagged scalars and boxed
 * cells. The constructors return tagged for in-range / supported
 * cases; readers go through these helpers to stay form-agnostic. The
 * boxed form is still reachable for out-of-tagged-range ints, so each
 * helper handles both forms. */
static inline int mino_val_int_p(const mino_val_t *v)
{
    return mino_type_of(v) == MINO_INT;
}

static inline long long mino_val_int_get(const mino_val_t *v)
{
    return MINO_IS_INT(v) ? MINO_INT_VAL(v) : v->as.i;
}

static inline int mino_val_bool_p(const mino_val_t *v)
{
    return mino_type_of(v) == MINO_BOOL;
}

static inline int mino_val_bool_get(const mino_val_t *v)
{
    return MINO_IS_BOOL(v) ? MINO_BOOL_VAL(v) : v->as.b;
}

static inline int mino_val_char_p(const mino_val_t *v)
{
    return mino_type_of(v) == MINO_CHAR;
}

static inline int mino_val_char_get(const mino_val_t *v)
{
    return MINO_IS_CHAR(v) ? MINO_CHAR_VAL(v) : v->as.ch;
}

/* Checked size-arithmetic helpers used by dynamic-growth code paths.
 * Each returns 1 on success (storing the result through `out`) and 0 on
 * overflow (leaving `*out` untouched). Callers route the overflow case
 * into the same OOM/diag path they already use for `realloc` failure.
 * Inline so the compiler can fold them through the surrounding cap and
 * length comparisons; static so each TU emits its own copy if needed. */
static inline int checked_add_sz(size_t a, size_t b, size_t *out)
{
    if (a > SIZE_MAX - b) return 0;
    *out = a + b;
    return 1;
}

static inline int checked_mul_sz(size_t a, size_t b, size_t *out)
{
    if (b != 0 && a > SIZE_MAX / b) return 0;
    *out = a * b;
    return 1;
}

static inline int checked_double_sz(size_t cap, size_t *out)
{
    if (cap > SIZE_MAX / 2) return 0;
    *out = cap * 2;
    return 1;
}

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

/* Open-addressing hash slot for the var registry. Keyed on the
 * (ns*, name*) pointer pair: both are interned so equality is pointer
 * equality. ns == NULL marks an empty slot. */
typedef struct {
    const char *ns;
    const char *name;
    mino_val_t *var;
} var_hash_slot_t;

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
/* STM transaction state                                                     */
/*                                                                           */
/* Per-thread state for a running `dosync` invocation. tx_ref_state_t is the */
/* node tracking what a transaction has done with a single ref since it     */
/* opened: snapshot version captured on first read, the kind of pending     */
/* mutation (alter vs. logged commute), the tentative value or commute log, */
/* and the next-pointer for the linked list owned by tx_state_t. The list  */
/* itself is GC-traced via gc_mark_roots so tentative values stay reachable */
/* during a transaction's lifetime.                                          */
/* ------------------------------------------------------------------------- */

typedef enum {
    TX_STATE_ALTER       = 0,
    TX_STATE_COMMUTE_LOG = 1
} tx_ref_state_kind_t;

typedef struct tx_ref_state {
    mino_val_t          *ref;              /* MINO_TX_REF this state tracks */
    uint64_t             snapshot_version; /* version at first read */
    int                  read;             /* 1 if this tx read the ref */
    tx_ref_state_kind_t  kind;
    mino_val_t          *tentative;        /* in-tx value; NULL if no write */
    mino_val_t          *commute_log;      /* MINO_CONS of (fn args...); or NULL */
    /* Populated by tx_commit when a write is applied: the ref's value
     * before the commit (for watch dispatch's `old` arg) and the new
     * committed value (`new` arg). Both NULL if no write happened on
     * this ref. */
    mino_val_t          *committed_old;
    mino_val_t          *committed_new;
    struct tx_ref_state *next;
} tx_ref_state_t;

typedef struct tx_state {
    int                  depth;            /* outermost dosync = 1 */
    tx_ref_state_t      *refs_head;        /* per-ref state chain */
    int                  retry_count;
    int                  try_depth_at_start; /* try-stack snapshot for retry */
    /* Non-zero while tx_commit is walking the write set and invoking
     * user code (commute log replay, validators). Mutating ops
     * (alter / ref-set / commute) re-entered through that user code
     * cannot be honoured -- the iterator has already moved past the
     * affected ref node, so the new tentative would be silently lost.
     * Throw MST002 instead. */
    int                  in_commit;
    /* Set by tx_commit to the validator's thrown exception (if any)
     * so dosync_run can re-throw the original payload instead of a
     * generic MCT001 message. NULL when the validator returned falsy
     * without throwing or when no validator ran. */
    mino_val_t          *validator_thrown_ex;
    /* (send / send-off) called from inside the transaction body
     * appends (agent fn extra) triples to this cons list (LIFO --
     * head holds the most recent send) instead of dispatching
     * synchronously. JVM canon: pending sends fire only on
     * successful commit, are silently discarded on retry or abort
     * so a body that's run multiple times sends the action once. */
    mino_val_t          *pending_sends;
} tx_state_t;

/* ------------------------------------------------------------------------- */
/* Per-thread runtime context                                                */
/*                                                                           */
/* Every field that mutates with eval progress lives here, separately from   */
/* the shared mino_state_t. Each OS thread that enters eval has its own      */
/* ctx; the state pointer is shared. The embedder thread reads main_ctx via */
/* the mino_current_ctx() TLS-fallback accessor; spawned host worker threads */
/* install their own ctx via TLS at thread entry.                            */
/* ------------------------------------------------------------------------- */

typedef struct mino_thread_ctx {
    /* Eval progress + step limit + interrupt poll. */
    size_t          eval_steps;
    int             limit_exceeded;
    const mino_val_t *eval_current_form;
    volatile int    interrupted;

    /* Safepoint-cooperative-yield flag.
     *
     * Set by the stop-the-world propagation path when a major
     * collection wants every worker for this state to park at its
     * next safepoint so the collector can run with a stable view of
     * the heap. The mutator polls `should_yield` at canonical
     * safepoints (eval_impl entry, gc_alloc_typed prologue,
     * loop/recur backward branches); when non-zero the mutator calls
     * into the parking slow path.
     *
     * The flag is set by `mino_safepoint_propagate_stw`
     * (runtime/state.c) when STW is requested, and cleared by
     * `mino_safepoint_park` and `mino_safepoint_resume_world` once
     * the collector releases the world. The fast-path poll is a
     * single predictably-not-taken branch when STW isn't active. */
    volatile int    should_yield;

    /* Exception handling: longjmp targets for try/catch. */
    try_frame_t     try_stack[MAX_TRY_DEPTH];
    int             try_depth;

    /* Last raw user-thrown payload caught by an inner eval try frame
     * (mino_eval_inner / mino_eval_string_inner). The inner publishes
     * a diagnostic (which the surrounding pcall-style try frame catches
     * as a string), but stashes the original payload here so the outer
     * pcall can surface it through `*out_ex` per the documented contract.
     * Cleared at the entry of every pcall-style frame and after consume. */
    mino_val_t     *pending_user_ex;

    /* Bytecode-VM catch frames. Parallel to try_stack, recording the
     * BC-side state (handler pc, register window base, env at entry,
     * exception register) that a longjmp landing at OP_PUSHCATCH's
     * setjmp needs to resume execution at the matching handler.
     * bc_catch_depth indexes the topmost active BC catch entry. When
     * the longjmp originates from a tree-walker `try` that interleaves
     * BC frames, the BC entry's try_depth_at_push is compared against
     * the active try_depth so a stray longjmp doesn't pick up the wrong
     * handler. Plain int + size_t members -- no GC-reachable refs (the
     * thrown value lives in try_stack[..].exception, already a GC root
     * via gc_mark_runtime_globals). */
    struct {
        size_t   handler_pc;
        size_t   reg_window_base;
        int      try_depth_at_push;
        unsigned ex_reg;
        struct mino_env *env_at_push;
        /* Anchor of the dyn stack at push. A throw across a
         * `(binding [...] ...)` form inside the try body would
         * otherwise leak the dyn frame -- the longjmp bypasses the
         * matching OP_POPDYN, and the per-fn bc_done unwind loop only
         * fires on fn exit, not on a catch landing inside the same fn.
         * Mirrors `saved_dyn` in `eval_try`. */
        dyn_frame_t *dyn_stack_at_push;
    } bc_catch_stack[MAX_TRY_DEPTH];
    int             bc_catch_depth;

    /* Error reporting: text buffer + structured diagnostic + frame stack. */
    char            error_buf[2048];
    mino_diag_t    *last_diag;
    call_frame_t    call_stack[MAX_CALL_DEPTH];
    int             call_depth;
    int             trace_added;

    /* BC "where are we right now" cursor. mino_bc_run keeps these
     * fields in sync with the dispatch loop's local bc / pc so error
     * paths that fire without a useful S->eval_current_form (e.g., a
     * stencil-internal fault from a future native tier) can resolve a
     * precise source span via mino_bc_source_lookup. Stack discipline:
     * each mino_bc_run frame snapshots its caller's values at entry
     * and restores them on exit; the result is a one-deep cursor that
     * always points at the innermost active BC frame (or NULL when no
     * BC is on the C stack). */
    const struct mino_bc_fn *bc_current_bc;
    size_t                   bc_current_pc;

    /* GC save stack: transient roots pinned across allocations. */
    mino_val_t     *gc_save[64];
    int             gc_save_len;

    /* Conservative stack scan anchor + GC re-entrancy depth. */
    void           *gc_stack_bottom;
    int             gc_depth;

    /* Dynamic binding stack head. */
    dyn_frame_t    *dyn_stack;

    /* Active JIT invoke env. Set by mino_jit_invoke from the
     * mino_bc_run frame's `env` parameter so slow helpers (e.g.,
     * mino_jit_getglobal_cached_slow) can resolve captured-local
     * symbols through the same env-then-cache cascade the interpreter
     * uses. NULL when no JIT region is active. Saved / restored by
     * mino_jit_invoke around its call so nested JIT regions
     * (`call_cached` chains in later releases) keep the correct env
     * across re-entry. */
    struct mino_env *jit_invoke_env;

    /* This thread's recursive depth on S->state_lock.
     * mino_lock increments, mino_unlock decrements; mino_yield_lock
     * saves the depth and unlocks down to zero, mino_resume_lock
     * re-locks up to the saved depth. Used by mino_future_deref so
     * the waiter can park on the future's cv without holding the
     * state_lock and starving the worker. */
    int             lock_depth;

    /* Active STM transaction on this thread, or NULL outside `dosync`.
     * Allocated on the C stack at the dosync* entry; nested dosyncs reuse
     * the outermost frame and only bump the depth counter. The collector
     * walks ->refs_head from gc_mark_roots so tentative values stay
     * reachable during a transaction. */
    struct tx_state        *current_tx;

    /* Linked list of live worker ctxs. Walked during gc_mark_roots so
     * values pinned by gc_save / dyn_stack / etc. on a blocked worker
     * stay reachable across GCs initiated from another thread.
     * main_ctx is not on this list; the GC walker processes it
     * separately. NULL on main_ctx and on the head sentinel. */
    struct mino_thread_ctx *next_worker;
} mino_thread_ctx_t;

/* TLS pointer to the per-thread ctx for the current worker.
 *
 * NULL on the embedder's main thread (the one that called
 * mino_state_new). Spawned host threads set this to their
 * freshly-allocated worker ctx at thread entry and clear it before
 * exit. Accessed only via `mino_current_ctx(S)` (defined below
 * struct mino_state). */
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
/* Agent run-queue node                                                      */
/* ------------------------------------------------------------------------- */

/* One queued action targeting a specific agent. Lives in the malloc
 * heap (not GC). The mino_val_t pointers below are GC roots reached
 * via gc_mark_agent_runq during root marking. */
typedef struct agent_action_node {
    mino_val_t                *agent;     /* MINO_AGENT target */
    mino_val_t                *fn;        /* action fn */
    mino_val_t                *extra;     /* extra arg list (nil/cons) */
    mino_val_t                *dyn_snap;  /* dyn-binding snapshot at send */
    mino_env_t                *env;       /* env captured at send time */
    struct agent_action_node  *next;
} agent_action_node_t;

/* Two agent pools live alongside each other: POOLED is the target of
 * `send` (canonical CPU-bound shape), SOLO is the target of `send-off`
 * (canonical IO-bound shape). Each pool has its own FIFO and its own
 * worker thread, but all pools share agent_mu/agent_cv so an await
 * waiter sleeps once and wakes for either pool's progress.
 *
 * mino's per-state eval lock still serializes one action at a time,
 * so the split is architectural -- it preserves enqueue-order within
 * each shape, and gives us a clean seam if we later let SOLO actions
 * yield the eval lock during blocking IO. */
typedef enum {
    AGENT_POOL_POOLED = 0, /* send */
    AGENT_POOL_SOLO   = 1, /* send-off */
    AGENT_POOL_COUNT  = 2
} agent_pool_kind_t;

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

/* ------------------------------------------------------------------------- */
/* Host-thread future                                                        */
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
    /* Map of dyn-var bindings at spawn time; the worker unpacks this
     * into a dyn_frame before invoking the thunk so the body sees the
     * same binding context as the caller. */
    mino_val_t         *dyn_snapshot;

    mino_future_t      *next_in_state;   /* S->future_list_head chain */
};

/* ------------------------------------------------------------------------- */
/* Safepoint poll                                                            */
/*                                                                           */
/* Mutators poll `should_yield` at canonical safepoints so a stop-the-world  */
/* major collection can run with a stable view of the heap. The fast path   */
/* is one predictably-not-taken branch; the slow path (mino_safepoint_park) */
/* is in state.c and blocks until the collector signals release.            */
/*                                                                           */
/* Single-threaded today: gc_request_stw / gc_release_stw flip the flag on */
/* the main ctx around major collections. The mutator is also the         */
/* collector, so the flag is set on a ctx that is already parked by       */
/* definition (the request runs from within the same call stack); the     */
/* flag write is a formal record and the park slow path is reachable      */
/* only when a future multi-worker variant wires the flag onto a peer     */
/* worker ctx. The inline is in the header so the branch inlines into    */
/* every alloc / eval-impl-entry / loop-recur site.                       */
/* ------------------------------------------------------------------------- */

void mino_safepoint_park(mino_state_t *S);

static inline void mino_safepoint_poll(mino_state_t *S)
{
    if (mino_current_ctx(S)->should_yield) {
        mino_safepoint_park(S);
    }
}

/* GC-side STW driver: request all worker ctxs park before a major sweep,
 * then release them after. Single-threaded today these are O(1) on
 * S->main_ctx; multi-threaded variants iterate the worker set. */
void gc_request_stw(mino_state_t *S);
void gc_release_stw(mino_state_t *S);

/* ------------------------------------------------------------------------- */
/* Per-state lock helpers                                                    */
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

/* worker_list_lock: brief lock for worker_ctxs_head + thread_count.
 * Inner to state_lock; never wraps an eval. Workers at entry/exit
 * acquire alone; spawn + GC root scan acquire from inside state_lock. */
void mino_worker_list_lock_init(mino_state_t *S);
void mino_worker_list_lock_destroy(mino_state_t *S);
void mino_worker_list_lock_acquire(mino_state_t *S);
void mino_worker_list_lock_release(mino_state_t *S);

/* mino_lock / mino_unlock take the recursive state_lock unconditionally.
 * This ensures correctness when multi_threaded flips mid-eval (a
 * conditional lock could race because the eval that called future-spawn
 * entered without taking the lock when multi_threaded was still 0).
 * Single-threaded states pay one uncontested mutex lock per eval entry,
 * which is on the order of tens of nanoseconds and dominated by other
 * eval costs. A fast-path skip is reintroducable later if safe-flip
 * sequencing can be proven. */
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
/* Extended variant of set_eval_diag that also attaches a `:mino/data`
 * payload (GC-owned; the runtime keeps it alive while the diag is
 * live) and an optional note. Pass NULL for either to skip. */
void        set_eval_diag_with_data(mino_state_t *S, const mino_val_t *form,
                                    const char *kind, const char *code,
                                    const char *msg, mino_val_t *data,
                                    const char *note);
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
env_binding_t *env_find_here_n(mino_env_t *env, const char *name, size_t nlen);
/* Symbol-aware lookup. Caller already has sym->as.s.{data,len}; we
 * skip strlen and walk the parent chain with the cached length. */
mino_val_t    *mino_env_get_sym(mino_env_t *env, const mino_val_t *sym);
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

/* Snapshot the calling thread's dyn_stack into a map (symbol -> value).
 * Returns mino_nil(S) when the stack is empty. Used by future spawn to
 * convey caller bindings to the worker, and by get-thread-bindings. */
mino_val_t    *mino_snapshot_thread_bindings(mino_state_t *S);

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
int  runtime_module_add_alias(mino_state_t *S,
                              const char *alias, const char *full);

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
