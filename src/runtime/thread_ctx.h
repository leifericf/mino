/*
 * thread_ctx.h -- per-thread runtime context and TLS pointers.
 *
 * Every field that mutates with eval progress lives in mino_thread_ctx_t,
 * separately from the shared mino_state_t. Each OS thread that enters
 * eval has its own ctx; the state pointer is shared. The embedder
 * thread reads main_ctx via mino_current_ctx() (defined in
 * runtime/internal.h alongside struct mino_state); spawned host worker
 * threads install their own ctx via TLS at thread entry.
 *
 * try_frame_t and MAX_TRY_DEPTH live here -- they're the per-ctx
 * try-stack record types, not eval-internal. eval/internal.h includes
 * this header to keep its existing consumers compiling.
 *
 * Internal to the runtime; embedders should only use mino.h.
 */

#ifndef RUNTIME_THREAD_CTX_H
#define RUNTIME_THREAD_CTX_H

#include "mino_internal.h"
#include "diag.h"
#include "runtime/runtime_types.h"
#include "runtime/stm_state.h"

#include <setjmp.h>
#include <stddef.h>

/* ------------------------------------------------------------------------- */
/* Exception handling: try-stack frame                                       */
/* ------------------------------------------------------------------------- */

#define MAX_TRY_DEPTH 64

typedef struct {
    jmp_buf     buf;
    mino_val_t *exception;
    const char *saved_ns;       /* current_ns at try-frame entry; restored on catch */
    const char *saved_ambient;  /* fn_ambient_ns at try-frame entry */
    size_t      saved_load_len; /* require load-stack depth at frame entry */
} try_frame_t;

/* ------------------------------------------------------------------------- */
/* Per-thread runtime context                                                */
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

    /* This thread's recursive depth on S->threading.state_lock.
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

    /* Per-ctx BC register stack snapshot.
     *
     * The state-level S->bc.bc_regs / S->bc.bc_top / S->bc.bc_regs_cap fields
     * track the BC stack of whichever worker currently holds
     * state_lock. When that worker yields (mino_yield_lock), the
     * full snapshot is copied here so the bc_top cursor + the array
     * pointer survive across the lock-release window; the actual
     * slot data lives in the ctx's owned bc_regs_storage array,
     * which the GC traces via the worker-ctx walk. When the worker
     * resumes (mino_resume_lock), the snapshot is restored.
     *
     * This makes concurrent `(fn [x] (thread-sleep 5) x)` calls from
     * sibling workers safe: each worker's `x` arg lives in its own
     * private bc_regs_storage; a peer's bc_push/pop on the shared
     * state during the yield window can't reach it.
     *
     * Placed at struct tail so existing JIT-pinned offsets
     * (dyn_stack, jit_invoke_env) stay byte-stable. */
    mino_val_t    **bc_regs_storage;     /* this ctx's own array */
    size_t          bc_regs_storage_cap; /* capacity of bc_regs_storage */
    size_t          bc_top_snapshot;     /* this ctx's bc_top at yield */
    int             bc_snapshot_valid;   /* 1 once first run installs */
    /* Reentrant depth of mino_jit_invoke on this thread. Bumped on
     * entry, decremented on exit. invoke_bc_fn_argv reads it through
     * the adaptive-tier path: when > 0, the callee being invoked sits
     * inside a JIT-active call chain and gets a threshold of 1
     * regardless of the state's jit_hot_threshold setting. This
     * accelerates the warm-up of frequently-called callees on
     * short-running scripts where the default threshold would
     * otherwise gate compile attempts past the script's wall time.
     * Placed at the tail to keep JIT-pinned offsets above stable. */
    int             jit_invoke_depth;
} mino_thread_ctx_t;

/* TLS pointer to the per-thread ctx for the current worker.
 *
 * NULL on the embedder's main thread (the one that called
 * mino_state_new). Spawned host threads set this to their
 * freshly-allocated worker ctx at thread entry and clear it before
 * exit. Accessed only via `mino_current_ctx(S)` (defined alongside
 * struct mino_state in runtime/internal.h). */
extern
#if defined(_WIN32) && defined(_MSC_VER)
__declspec(thread)
#else
__thread
#endif
mino_thread_ctx_t *mino_tls_ctx;

/* TLS pointer to the owning future_impl's cancel_flag for worker
 * threads. NULL on the embedder's main thread. The BC safepoint
 * poll reads through this pointer to check whether future-cancel
 * has been called on the worker's future. Kept out of
 * mino_thread_ctx_t so the JIT-pinned offsets in main_ctx don't
 * shift; TLS is a clean place since lookup is per-thread anyway. */
extern
#if defined(_WIN32) && defined(_MSC_VER)
__declspec(thread)
#else
__thread
#endif
volatile int *mino_tls_cancel_ptr;

/* TLS counter for the BC safepoint poll's auto-yield slot. Rolls
 * over naturally on unsigned overflow; the auto-yield slot bit is
 * checked once every ~64K iterations so the safepoint cost is
 * amortised. */
extern
#if defined(_WIN32) && defined(_MSC_VER)
__declspec(thread)
#else
__thread
#endif
unsigned int mino_tls_safepoint_count;

/* Forward decl for the embed thread pool surface (mino.h provides
 * the struct definition; struct mino_state stores a pointer). */
struct mino_thread_pool;

#endif /* RUNTIME_THREAD_CTX_H */
