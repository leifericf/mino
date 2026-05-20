/*
 * coordination.h -- safepoint poll, STW driver, per-state lock primitives.
 *
 * Non-inline declarations only; the inline fast paths for
 * mino_safepoint_poll / mino_lock / mino_unlock live in
 * runtime/internal.h because they reach into struct mino_state and
 * mino_thread_ctx_t fields that haven't been narrowed yet.
 *
 * Internal to the runtime; embedders should only use mino.h.
 */

#ifndef RUNTIME_COORDINATION_H
#define RUNTIME_COORDINATION_H

#include "mino_internal.h"

/* ------------------------------------------------------------------------- */
/* Safepoint poll                                                            */
/*                                                                           */
/* Mutators poll `should_yield` at canonical safepoints so a stop-the-world  */
/* major collection can run with a stable view of the heap. The fast path   */
/* is one predictably-not-taken branch; the slow path (mino_safepoint_park) */
/* blocks until the collector signals release.                              */
/*                                                                           */
/* Single-threaded today: gc_request_stw / gc_release_stw flip the flag on */
/* the main ctx around major collections. The mutator is also the         */
/* collector, so the flag is set on a ctx that is already parked by       */
/* definition (the request runs from within the same call stack); the     */
/* flag write is a formal record and the park slow path is reachable      */
/* only when a future multi-worker variant wires the flag onto a peer     */
/* worker ctx.                                                              */
/* ------------------------------------------------------------------------- */

void mino_safepoint_park(mino_state *S);

/* BC dispatch safepoint poll: cooperative cancel + state_lock
 * auto-yield. Called from every backward jump in the BC VM. Returns
 * 1 to continue, 0 to abort (caller propagates NULL via bc_done).
 * Fast path is one branch on cancel_ptr (NULL on the embedder)
 * plus an unsigned counter increment. */
int mino_bc_safepoint(mino_state *S);

/* GC-side STW driver: request all worker ctxs park before a major sweep,
 * then release them after. Single-threaded today these are O(1) on
 * S->main_ctx; multi-threaded variants iterate the worker set. */
void gc_request_stw(mino_state *S);
void gc_release_stw(mino_state *S);

/* ------------------------------------------------------------------------- */
/* Per-state lock helpers                                                    */
/*                                                                           */
/* mino_lock(S) / mino_unlock(S) acquire/release S->threading.state_lock when         */
/* multi_threaded is set; otherwise they're a no-op. Held by worker threads */
/* across the entire eval call so single-state futures execute serialized.  */
/* Cross-state work runs fully concurrent (each state has its own lock).    */
/* ------------------------------------------------------------------------- */

void mino_state_lock_init(mino_state *S);
void mino_state_lock_destroy(mino_state *S);
void mino_state_lock_acquire(mino_state *S);
void mino_state_lock_release(mino_state *S);

/* worker_list_lock: brief lock for worker_ctxs_head + thread_count.
 * Inner to state_lock; never wraps an eval. Workers at entry/exit
 * acquire alone; spawn + GC root scan acquire from inside state_lock. */
void mino_worker_list_lock_init(mino_state *S);
void mino_worker_list_lock_destroy(mino_state *S);
void mino_worker_list_lock_acquire(mino_state *S);
void mino_worker_list_lock_release(mino_state *S);

/* Yield: drop down to lock_depth==0, returning the previous depth so
 * the caller can resume to the same level after a blocking wait. */
int  mino_yield_lock(mino_state *S);
void mino_resume_lock(mino_state *S, int saved_depth);

/* Debug-only invariant: assert that the caller is in a state-safe
 * window -- either single-threaded (no other writer can interpose,
 * including the init path before any future has been spawned) or
 * holding state_lock recursively on this thread. Used to guard
 * shared-table mutators (intern table, record-type registry) whose
 * documented contract is "caller must hold state_lock"; the assert
 * surfaces a missing lock at the offending call site under a debug
 * build instead of letting a torn table escape into production. */
#define MINO_ASSERT_STATE_SAFE(S) \
    assert(!(S)->threading.multi_threaded \
           || mino_current_ctx(S)->lock_depth > 0)

#endif /* RUNTIME_COORDINATION_H */
