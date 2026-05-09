/*
 * stm.c -- Software Transactional Memory: refs and dosync.
 *
 * mino's STM uses single-version optimistic locking, NOT MVCC with
 * history. ref-min-history, ref-max-history, ref-history-count are
 * stubs (return 0 / 10 / 0). Long readers under high writer
 * contention may exhaust the 10000-retry cap rather than serve an
 * older snapshot from history. For mino's typical workload (single-
 * digit refs, a handful of worker threads, mostly single-threaded
 * embedders), that trade-off is the right one.
 *
 * Concurrency model: a single global commit lock (S->stm_commit_lock)
 * lazy-initialized in mino_install_stm. Held only across the commit
 * phase of a transaction (read-set validation, writes, version
 * bumps); watch dispatch runs outside it. Per-thread transaction
 * state lives on mino_thread_ctx_t::current_tx; the GC walks it from
 * gc_mark_roots so tentative values stay reachable.
 *
 * Embedder optionality: the public surface (ref, ref?, dosync*, ...)
 * is gated behind mino_install_stm. mino_install_all calls it, so
 * the standalone binary has STM out of the box; embedders calling
 * only mino_new stay opt-out.
 */

#include "prim/internal.h"

/* --- helpers -------------------------------------------------------------- */

/* Fetch or create the per-tx state node for a given ref within the
 * active transaction. Returns NULL and throws when called outside a
 * transaction. */
static tx_ref_state_t *tx_get_or_create_ref_state(mino_state_t *S,
                                                   mino_val_t *ref)
{
    mino_thread_ctx_t *ctx = mino_current_ctx(S);
    tx_state_t        *tx  = ctx->current_tx;
    tx_ref_state_t    *rs;
    if (tx == NULL) return NULL;
    for (rs = tx->refs_head; rs != NULL; rs = rs->next) {
        if (rs->ref == ref) return rs;
    }
    rs = (tx_ref_state_t *)calloc(1, sizeof(*rs));
    if (rs == NULL) {
        prim_throw_classified(S, "eval/state", "MST005",
            "out of memory allocating tx state");
        return NULL;
    }
    rs->ref              = ref;
    rs->snapshot_version = 0;
    rs->read             = 0;
    rs->kind             = TX_STATE_ALTER;
    rs->tentative        = NULL;
    rs->commute_log      = NULL;
    rs->next             = tx->refs_head;
    tx->refs_head        = rs;
    return rs;
}

/* Release every per-ref state node attached to tx. Called on every
 * transaction exit, including on error and on retry. */
static void tx_clear_ref_states(tx_state_t *tx)
{
    tx_ref_state_t *rs, *next;
    for (rs = tx->refs_head; rs != NULL; rs = next) {
        next = rs->next;
        free(rs);
    }
    tx->refs_head = NULL;
}

/* --- ref construction + identity predicate -------------------------------- */

mino_val_t *prim_ref(mino_state_t *S, mino_val_t *args, mino_env_t *env)
{
    mino_val_t *initial;
    (void)env;
    if (!mino_is_cons(args) || mino_is_cons(args->as.cons.cdr)) {
        return prim_throw_classified(S, "eval/arity", "MAR001",
            "ref requires one argument");
    }
    initial = args->as.cons.car;
    return mino_tx_ref(S, initial);
}

mino_val_t *prim_ref_p(mino_state_t *S, mino_val_t *args, mino_env_t *env)
{
    mino_val_t *v;
    (void)env;
    if (!mino_is_cons(args) || mino_is_cons(args->as.cons.cdr)) {
        return prim_throw_classified(S, "eval/arity", "MAR001",
            "ref? requires one argument");
    }
    v = args->as.cons.car;
    return (v != NULL && v->type == MINO_TX_REF) ? mino_true(S)
                                                  : mino_false(S);
}

/* --- deref dispatch for refs --------------------------------------------- *
 *
 * The main `deref` primitive lives in prim/stateful.c; refs hook in
 * via mino_ref_deref which prim_deref calls when it sees MINO_TX_REF.
 *
 * Inside a transaction: if a tentative value is set, return it; else
 * record the read (snapshot version captured) and return the committed
 * value. Outside a transaction: return the committed value.
 */
mino_val_t *mino_ref_deref(mino_state_t *S, mino_val_t *ref)
{
    mino_thread_ctx_t *ctx = mino_current_ctx(S);
    tx_state_t        *tx  = ctx->current_tx;
    tx_ref_state_t    *rs;
    if (tx == NULL) {
        return ref->as.tx_ref.val;
    }
    rs = tx_get_or_create_ref_state(S, ref);
    if (rs == NULL) return NULL;
    if (rs->tentative != NULL) {
        return rs->tentative;
    }
    if (!rs->read) {
        rs->read             = 1;
        rs->snapshot_version = ref->as.tx_ref.version;
    }
    return ref->as.tx_ref.val;
}

/* --- dosync entry point --------------------------------------------------- *
 *
 * `dosync*` takes a thunk (zero-arg fn) and runs it inside a
 * transaction. Nested dosync is a no-op around the outer one: the
 * inner thunk runs against the same tx state, and only the
 * outermost frame commits.
 *
 * The retry mechanism is fully wired in subsequent commits when
 * ref-set / alter / commute land. For now the body runs once, the
 * commit phase is empty (no writes are recorded yet), and the result
 * propagates out as if it were a plain `(thunk)`.
 */
mino_val_t *prim_dosync_star(mino_state_t *S, mino_val_t *args,
                              mino_env_t *env)
{
    mino_val_t        *thunk;
    mino_val_t        *result;
    mino_thread_ctx_t *ctx = mino_current_ctx(S);
    if (!mino_is_cons(args) || mino_is_cons(args->as.cons.cdr)) {
        return prim_throw_classified(S, "eval/arity", "MAR001",
            "dosync* requires one argument (a thunk)");
    }
    thunk = args->as.cons.car;
    if (thunk == NULL || (thunk->type != MINO_FN
                          && thunk->type != MINO_PRIM
                          && thunk->type != MINO_MACRO)) {
        return prim_throw_classified(S, "eval/type", "MTY001",
            "dosync*: expected a thunk");
    }
    /* Nested dosync: bump depth, reuse the outer tx, run the thunk. */
    if (ctx->current_tx != NULL) {
        ctx->current_tx->depth++;
        result = mino_call(S, thunk, mino_nil(S), env);
        ctx->current_tx->depth--;
        return result;
    }
    /* Outermost dosync: allocate tx state on the C stack, attach. */
    {
        tx_state_t tx;
        tx.depth              = 1;
        tx.refs_head          = NULL;
        tx.retry_count        = 0;
        tx.try_depth_at_start = ctx->try_depth;
        tx.retry_signal       = 0;
        ctx->current_tx       = &tx;
        result = mino_call(S, thunk, mino_nil(S), env);
        /* Commit phase will land in commit #5 (read-set validation,
         * write barrier, version bump). For now there are no writes
         * to commit, so detaching and returning is the whole story. */
        tx_clear_ref_states(&tx);
        ctx->current_tx = NULL;
    }
    return result;
}

/* --- primitive table ----------------------------------------------------- */

const mino_prim_def k_prims_stm[] = {
    {"ref",       prim_ref,
     "Creates an STM ref holding the given initial value. Mutate via "
     "ref-set / alter / commute inside dosync."},
    {"ref?",      prim_ref_p,
     "Returns true if x is an STM ref."},
    {"dosync*",   prim_dosync_star,
     "Runs a zero-arg thunk inside an STM transaction. The `dosync` "
     "macro expands to (dosync* (fn [] body...))."},
};

const size_t k_prims_stm_count = sizeof(k_prims_stm) / sizeof(k_prims_stm[0]);

/* Lazy-init the global commit lock on the first mino_install_stm
 * call. State_init does not pay this cost so embedders that never
 * opt into STM keep the bool flag and nothing else. */
static void stm_lazy_init_commit_lock(mino_state_t *S)
{
    if (S->stm_lock_inited) return;
#if !(defined(_WIN32) && defined(_MSC_VER))
    if (pthread_mutex_init(&S->stm_commit_lock, NULL) != 0) {
        /* Mutex init failure is unrecoverable for STM; if the host's
         * pthread layer is broken we cannot run dosync safely. */
        abort();
    }
#endif
    S->stm_lock_inited = 1;
}

void mino_install_stm(mino_state_t *S, mino_env_t *env)
{
    stm_lazy_init_commit_lock(S);
    prim_install_table(S, env, "clojure.core",
                       k_prims_stm, k_prims_stm_count);
}
