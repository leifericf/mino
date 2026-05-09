/*
 * stm.c -- Software Transactional Memory: refs and dosync.
 *
 * Documented deviations from JVM Clojure (clojure.lang.LockingTransaction).
 * The Clojure-level surface (ref, dosync, alter, commute, ensure,
 * ref-set, io!, watches, validators, in-transaction?) matches canon
 * for any program that does not depend on the items below.
 *
 *  1. Single-version optimistic locking. JVM uses MVCC with a
 *     ref-history ring buffer; we hold one committed value per ref.
 *     A long-running reader competing with sustained writer pressure
 *     may exhaust the retry cap (STM_RETRY_CAP = 10000) where JVM
 *     would serve an older snapshot. Trade-off chosen for mino's
 *     typical workload (small ref sets, handful of worker threads,
 *     mostly single-threaded embedders).
 *
 *  2. Global commit lock. JVM uses per-ref read/write locks plus
 *     write-skew detection; mino serializes all commits behind one
 *     mutex (S->stm_commit_lock) lazy-initialized in
 *     mino_install_stm. Coarser, simpler, and on a single thread
 *     completely free (the lock is skipped when thread_limit <= 1).
 *
 *  3. No barging. JVM's older-tx-bumps-younger-tx mechanism is
 *     intentionally absent. Every retry restarts the body from
 *     scratch.
 *
 *  4. No mid-body retry. JVM detects conflicts as soon as a read
 *     observes a stale version; mino only checks the read set at
 *     commit time. Wasted work bounded by the retry cap.
 *
 *  5. ref-min-history, ref-max-history, ref-history-count are stubs
 *     returning 0 / 10 / 0 (no MVCC history to introspect).
 *
 *  6. Print form is `#ref[ID VAL]`. JVM prints
 *     `#object[clojure.lang.Ref 0x... {:status :ready, :val ...}]`,
 *     a JVM-specific shape. mino's form is deliberately simpler and
 *     not pretending to be a JVM class.
 *
 *  7. set-validator! does not validate the current value at install
 *     time. JVM matches this; mino's earlier behavior of throwing
 *     MCT001 on install was the deviation, removed in v0.101.0.
 *
 *  8. alter-after-commute and ref-set-after-commute throw
 *     "Can't set after commute" (JVM canon). The commute-after-alter
 *     direction folds the commute into the alter's tentative; commit
 *     writes the alter+commute tentative, matching JVM's behavior of
 *     skipping commute-log replay for refs already in the write set.
 *
 * Concurrency model: per-thread transaction state lives on
 * mino_thread_ctx_t::current_tx; the GC walks it from gc_mark_roots
 * so tentative values stay reachable. The commit lock is held only
 * across read-set validation, write application, and version bumps;
 * watch dispatch runs outside it.
 *
 * Embedder optionality: the public surface (ref, ref?, dosync*, ...)
 * is gated behind mino_install_stm. mino_install_all calls it, so
 * the standalone binary has STM out of the box; embedders calling
 * only mino_new stay opt-out.
 */

#include "prim/internal.h"
#include "eval/internal.h"
#include <setjmp.h>

#define STM_RETRY_CAP 10000

/* --- helpers -------------------------------------------------------------- */

/* Acquire / release the global STM commit lock. No-op when STM has
 * never been installed (the bool stays 0); otherwise a plain pthread
 * mutex acquire. The single-threaded fast path skips the lock since
 * no other thread can interfere with the commit phase. */
static void stm_lock(mino_state_t *S)
{
    if (!S->stm_lock_inited) return;
    if (S->thread_limit <= 1) return;
#if !(defined(_WIN32) && defined(_MSC_VER))
    pthread_mutex_lock(&S->stm_commit_lock);
#endif
}

static void stm_unlock(mino_state_t *S)
{
    if (!S->stm_lock_inited) return;
    if (S->thread_limit <= 1) return;
#if !(defined(_WIN32) && defined(_MSC_VER))
    pthread_mutex_unlock(&S->stm_commit_lock);
#endif
}

/* Fetch or create the per-tx state node for a given ref within the
 * active transaction. Returns NULL outside a transaction. */
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
 * transaction exit, including on retry, error, and successful commit. */
static void tx_clear_ref_states(tx_state_t *tx)
{
    tx_ref_state_t *rs, *next;
    for (rs = tx->refs_head; rs != NULL; rs = next) {
        next = rs->next;
        free(rs);
    }
    tx->refs_head = NULL;
}

/* --- io! support, history stubs, in-tx predicate ------------------------- */

/* (io!-check) -- throws if a transaction is active on the current
 * thread. The `io!` macro in core.clj expands to
 * (do (io!-check) body...) so the throw fires before body evaluates. */
mino_val_t *prim_io_bang_check(mino_state_t *S, mino_val_t *args,
                                mino_env_t *env)
{
    mino_thread_ctx_t *ctx;
    (void)env;
    if (mino_is_cons(args)) {
        return prim_throw_classified(S, "eval/arity", "MAR001",
            "io!-check takes no arguments");
    }
    ctx = mino_current_ctx(S);
    if (ctx->current_tx != NULL) {
        return prim_throw_classified(S, "eval/state", "MST003",
            "I/O in transaction");
    }
    return mino_nil(S);
}

mino_val_t *prim_in_tx_p(mino_state_t *S, mino_val_t *args, mino_env_t *env)
{
    mino_thread_ctx_t *ctx;
    (void)env;
    if (mino_is_cons(args)) {
        return prim_throw_classified(S, "eval/arity", "MAR001",
            "in-transaction? takes no arguments");
    }
    ctx = mino_current_ctx(S);
    return ctx->current_tx != NULL ? mino_true(S) : mino_false(S);
}

/* History stubs. mino's STM uses single-version optimistic locking,
 * NOT MVCC with history. The min/max/count surface exists for
 * Clojure-script compatibility but the value is fixed. */
mino_val_t *prim_ref_min_history(mino_state_t *S, mino_val_t *args,
                                  mino_env_t *env)
{
    (void)env;
    if (!mino_is_cons(args)) {
        return prim_throw_classified(S, "eval/arity", "MAR001",
            "ref-min-history requires a ref");
    }
    return mino_int(S, 0);
}

mino_val_t *prim_ref_max_history(mino_state_t *S, mino_val_t *args,
                                  mino_env_t *env)
{
    (void)env;
    if (!mino_is_cons(args)) {
        return prim_throw_classified(S, "eval/arity", "MAR001",
            "ref-max-history requires a ref");
    }
    return mino_int(S, 10);
}

mino_val_t *prim_ref_history_count(mino_state_t *S, mino_val_t *args,
                                    mino_env_t *env)
{
    (void)env;
    if (!mino_is_cons(args)) {
        return prim_throw_classified(S, "eval/arity", "MAR001",
            "ref-history-count requires a ref");
    }
    return mino_int(S, 0);
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

/* Forward declaration -- alter_build_args is defined below the deref
 * dispatch but used by commute_log_replay above. */
static mino_val_t *alter_build_args(mino_state_t *S, mino_val_t *cur,
                                    mino_val_t *extra);

/* C-side commute closure: pairs a host transformer with its user
 * pointer. Stored on the heap and wrapped in a MINO_HANDLE so the
 * GC sweep finalizer frees the struct when the handle becomes
 * unreachable. The handle's tag is a static string -- pointer
 * identity is the discriminator. */
struct tx_c_closure {
    mino_tx_xform_fn fn;
    void            *user;
};

static const char *const TX_C_CLOSURE_TAG = "mino/tx-c-closure";

static void tx_c_closure_finalize(void *ptr, const char *tag)
{
    (void)tag;
    free(ptr);
}

/* --- commute log replay -------------------------------------------------- *
 *
 * The commute log is a list of entries stored in REVERSE CHRONOLOGICAL
 * ORDER (cons-prepended so each commute is O(1) to record). Each entry
 * is either:
 *   - a cons (fn arg1 arg2 ...) for a Clojure-side commute, OR
 *   - a MINO_HANDLE with TX_C_CLOSURE_TAG for a C-side commute_c.
 * Replay reverses the list onto a fresh stack and walks forward,
 * dispatching per entry shape against the running accumulator.
 */
static mino_val_t *commute_log_replay(mino_state_t *S, mino_val_t *log,
                                      mino_val_t *base, mino_env_t *env)
{
    mino_val_t *reversed = mino_empty_list(S);
    mino_val_t *p = log;
    mino_val_t *cur = base;
    while (mino_is_cons(p)) {
        reversed = mino_cons(S, p->as.cons.car, reversed);
        p = p->as.cons.cdr;
    }
    for (p = reversed; mino_is_cons(p); p = p->as.cons.cdr) {
        mino_val_t *entry = p->as.cons.car;
        mino_val_t *result = NULL;
        if (entry != NULL && entry->type == MINO_HANDLE
            && entry->as.handle.tag == TX_C_CLOSURE_TAG) {
            struct tx_c_closure *c = (struct tx_c_closure *)
                                      entry->as.handle.ptr;
            result = c->fn(S, cur, c->user, env);
        } else if (mino_is_cons(entry)) {
            mino_val_t *fn        = entry->as.cons.car;
            mino_val_t *extra     = entry->as.cons.cdr;
            mino_val_t *call_args = alter_build_args(S, cur, extra);
            result = mino_call(S, fn, call_args, env);
        } else {
            continue;
        }
        if (result == NULL) return NULL;
        cur = result;
    }
    return cur;
}

/* Compute the in-transaction value of a ref WITHOUT marking the read.
 * Used by deref / alter / ref-set (which then mark the read) and by
 * commute (which deliberately does not). */
static mino_val_t *tx_effective_value(mino_state_t *S, tx_ref_state_t *rs,
                                      mino_env_t *env)
{
    if (rs->kind == TX_STATE_ALTER && rs->tentative != NULL) {
        return rs->tentative;
    }
    if (rs->kind == TX_STATE_COMMUTE_LOG && rs->commute_log != NULL) {
        return commute_log_replay(S, rs->commute_log,
                                   rs->ref->as.tx_ref.val, env);
    }
    return rs->ref->as.tx_ref.val;
}

/* --- deref dispatch for refs --------------------------------------------- *
 *
 * The main `deref` primitive lives in prim/stateful.c; refs hook in
 * via mino_tx_ref_deref which prim_deref calls when it sees MINO_TX_REF.
 *
 * Inside a transaction: return the in-tx effective value (tentative
 * for alter, log-replayed for commute, committed otherwise) and
 * record a read for read-set validation. Outside a transaction:
 * return the committed value.
 *
 * Public-API entry point: also called by C hosts as the C-level @ref.
 * Tolerates NULL or non-ref input (returns NULL) so a host can
 * defensively pass any mino_val_t * without pre-checking the type.
 */
mino_val_t *mino_tx_ref_deref(mino_state_t *S, mino_val_t *ref)
{
    mino_thread_ctx_t *ctx;
    tx_state_t        *tx;
    tx_ref_state_t    *rs;
    mino_val_t        *val;
    if (ref == NULL || ref->type != MINO_TX_REF) return NULL;
    ctx = mino_current_ctx(S);
    tx  = ctx->current_tx;
    if (tx == NULL) {
        return ref->as.tx_ref.val;
    }
    rs = tx_get_or_create_ref_state(S, ref);
    if (rs == NULL) return NULL;
    val = tx_effective_value(S, rs, NULL);
    if (val == NULL) return NULL;
    if (!rs->read) {
        rs->read             = 1;
        rs->snapshot_version = ref->as.tx_ref.version;
    }
    return val;
}

int mino_is_tx_ref(const mino_val_t *v)
{
    return v != NULL && v->type == MINO_TX_REF;
}

/* --- ref-set + alter ----------------------------------------------------- */

/* Shared core for prim_ref_set / mino_tx_ref_set. Caller has already
 * type-checked ref. Returns val on success; NULL on throw (the throw
 * has already been raised via prim_throw_classified). */
static mino_val_t *tx_ref_set_core(mino_state_t *S, mino_val_t *ref,
                                    mino_val_t *val)
{
    tx_ref_state_t    *rs;
    mino_thread_ctx_t *ctx = mino_current_ctx(S);
    if (ctx->current_tx == NULL) {
        return prim_throw_classified(S, "eval/state", "MST002",
            "No transaction running");
    }
    rs = tx_get_or_create_ref_state(S, ref);
    if (rs == NULL) return NULL;
    /* JVM canon: ref-set after commute on the same ref throws
     * "Can't set after commute". The commute log captures values
     * that should survive replay; an explicit set would silently
     * discard them, which JVM forbids. */
    if (rs->kind == TX_STATE_COMMUTE_LOG && rs->commute_log != NULL) {
        return prim_throw_classified(S, "eval/state", "MST002",
            "Can't set after commute");
    }
    if (!rs->read) {
        rs->read             = 1;
        rs->snapshot_version = ref->as.tx_ref.version;
    }
    rs->kind        = TX_STATE_ALTER;
    rs->tentative   = val;
    rs->commute_log = NULL;
    return val;
}

mino_val_t *prim_ref_set(mino_state_t *S, mino_val_t *args, mino_env_t *env)
{
    mino_val_t *ref;
    mino_val_t *val;
    (void)env;
    if (!mino_is_cons(args) || !mino_is_cons(args->as.cons.cdr)
        || mino_is_cons(args->as.cons.cdr->as.cons.cdr)) {
        return prim_throw_classified(S, "eval/arity", "MAR001",
            "ref-set requires two arguments: ref and value");
    }
    ref = args->as.cons.car;
    val = args->as.cons.cdr->as.cons.car;
    if (ref == NULL || ref->type != MINO_TX_REF) {
        return prim_throw_classified(S, "eval/type", "MTY001",
            "ref-set: first argument must be a ref");
    }
    return tx_ref_set_core(S, ref, val);
}

mino_val_t *mino_tx_ref_set(mino_state_t *S, mino_val_t *ref, mino_val_t *val)
{
    if (ref == NULL || ref->type != MINO_TX_REF) {
        return prim_throw_classified(S, "eval/type", "MTY001",
            "mino_tx_ref_set: argument must be a ref");
    }
    return tx_ref_set_core(S, ref, val);
}

/* Build the argument list (cur-val arg1 arg2 ...) for invoking f
 * during alter / commute. The cur-val is the tentative or committed
 * value as of this call site. */
static mino_val_t *alter_build_args(mino_state_t *S, mino_val_t *cur,
                                    mino_val_t *extra)
{
    mino_val_t *cell, *head;
    head = mino_cons(S, cur, mino_nil(S));
    cell = head;
    while (mino_is_cons(extra)) {
        mino_val_t *next = mino_cons(S, extra->as.cons.car, mino_nil(S));
        cell->as.cons.cdr = next;
        cell = next;
        extra = extra->as.cons.cdr;
    }
    return head;
}

/* Compute-new callback: produces the post-transformer value given
 * the current in-tx value. Implementations bind to either a Clojure
 * fn invocation (mino_call) or a direct C transformer call. */
typedef mino_val_t *(*tx_compute_fn)(mino_state_t *S, mino_val_t *cur,
                                      void *ctx, mino_env_t *env);

struct compute_clj_ctx {
    mino_val_t *fn;
    mino_val_t *extra;
};

static mino_val_t *compute_clj(mino_state_t *S, mino_val_t *cur,
                                void *ctx, mino_env_t *env)
{
    struct compute_clj_ctx *c = (struct compute_clj_ctx *)ctx;
    mino_val_t *call_args = alter_build_args(S, cur, c->extra);
    return mino_call(S, c->fn, call_args, env);
}

struct compute_c_ctx {
    mino_tx_xform_fn fn;
    void            *user;
};

static mino_val_t *compute_c(mino_state_t *S, mino_val_t *cur,
                              void *ctx, mino_env_t *env)
{
    struct compute_c_ctx *c = (struct compute_c_ctx *)ctx;
    return c->fn(S, cur, c->user, env);
}

/* Shared core for prim_alter / mino_tx_alter_c. Caller has already
 * type-checked ref. The compute callback runs under the in-tx
 * effective value AFTER the read mark is recorded, since alter
 * implicitly reads the ref. Returns the new value, or NULL on throw. */
static mino_val_t *tx_alter_core(mino_state_t *S, mino_val_t *ref,
                                  tx_compute_fn compute, void *ctx,
                                  mino_env_t *env)
{
    tx_ref_state_t    *rs;
    mino_val_t        *cur, *result;
    mino_thread_ctx_t *t_ctx = mino_current_ctx(S);
    if (t_ctx->current_tx == NULL) {
        return prim_throw_classified(S, "eval/state", "MST002",
            "No transaction running");
    }
    rs = tx_get_or_create_ref_state(S, ref);
    if (rs == NULL) return NULL;
    /* JVM canon: alter after commute on the same ref throws
     * "Can't set after commute". See tx_ref_set_core above. */
    if (rs->kind == TX_STATE_COMMUTE_LOG && rs->commute_log != NULL) {
        return prim_throw_classified(S, "eval/state", "MST002",
            "Can't set after commute");
    }
    cur = tx_effective_value(S, rs, env);
    if (cur == NULL) return NULL;
    if (!rs->read) {
        rs->read             = 1;
        rs->snapshot_version = ref->as.tx_ref.version;
    }
    result = compute(S, cur, ctx, env);
    if (result == NULL) return NULL;
    rs->kind        = TX_STATE_ALTER;
    rs->tentative   = result;
    rs->commute_log = NULL;
    return result;
}

mino_val_t *prim_alter(mino_state_t *S, mino_val_t *args, mino_env_t *env)
{
    mino_val_t *ref;
    struct compute_clj_ctx clj_ctx;
    if (!mino_is_cons(args) || !mino_is_cons(args->as.cons.cdr)) {
        return prim_throw_classified(S, "eval/arity", "MAR001",
            "alter requires at least two arguments: ref and function");
    }
    ref           = args->as.cons.car;
    clj_ctx.fn    = args->as.cons.cdr->as.cons.car;
    clj_ctx.extra = args->as.cons.cdr->as.cons.cdr;
    if (ref == NULL || ref->type != MINO_TX_REF) {
        return prim_throw_classified(S, "eval/type", "MTY001",
            "alter: first argument must be a ref");
    }
    return tx_alter_core(S, ref, compute_clj, &clj_ctx, env);
}

mino_val_t *mino_tx_alter_c(mino_state_t *S, mino_val_t *ref,
                            mino_tx_xform_fn fn, void *user,
                            mino_env_t *env)
{
    struct compute_c_ctx c_ctx;
    if (ref == NULL || ref->type != MINO_TX_REF) {
        return prim_throw_classified(S, "eval/type", "MTY001",
            "mino_tx_alter_c: argument must be a ref");
    }
    if (fn == NULL) {
        return prim_throw_classified(S, "eval/type", "MTY001",
            "mino_tx_alter_c: transformer fn must not be NULL");
    }
    c_ctx.fn   = fn;
    c_ctx.user = user;
    return tx_alter_core(S, ref, compute_c, &c_ctx, env);
}

/* --- commute + ensure ---------------------------------------------------- */

/* Build a log entry for the given commute. For Clojure-side commutes
 * the entry is a `(fn . extra)` cons; for C-side it is a HANDLE
 * wrapping a malloc'd struct freed by tx_c_closure_finalize on GC. */
static mino_val_t *make_clj_log_entry(mino_state_t *S, void *ctx)
{
    struct compute_clj_ctx *c = (struct compute_clj_ctx *)ctx;
    return mino_cons(S, c->fn, c->extra);
}

static mino_val_t *make_c_log_entry(mino_state_t *S, void *ctx)
{
    struct compute_c_ctx *c = (struct compute_c_ctx *)ctx;
    struct tx_c_closure  *closure = (struct tx_c_closure *)
                                     malloc(sizeof(*closure));
    if (closure == NULL) {
        prim_throw_classified(S, "eval/state", "MST005",
            "out of memory allocating commute closure");
        return NULL;
    }
    closure->fn   = c->fn;
    closure->user = c->user;
    return mino_handle_ex(S, closure, TX_C_CLOSURE_TAG,
                           tx_c_closure_finalize);
}

typedef mino_val_t *(*tx_log_entry_fn)(mino_state_t *S, void *ctx);

/* Shared core for prim_commute / mino_tx_commute_c. Caller has
 * already type-checked ref. Returns the call-site value, or NULL on
 * throw. The compute callback runs eagerly to produce the call-site
 * return; if a fresh-or-chained commute, make_log_entry produces the
 * value pushed onto the log for replay at commit. */
static mino_val_t *tx_commute_core(mino_state_t *S, mino_val_t *ref,
                                    tx_compute_fn compute,
                                    tx_log_entry_fn make_log_entry,
                                    void *ctx, mino_env_t *env)
{
    tx_ref_state_t    *rs;
    mino_val_t        *cur, *result, *entry;
    mino_thread_ctx_t *t_ctx = mino_current_ctx(S);
    if (t_ctx->current_tx == NULL) {
        return prim_throw_classified(S, "eval/state", "MST002",
            "No transaction running");
    }
    rs = tx_get_or_create_ref_state(S, ref);
    if (rs == NULL) return NULL;
    cur = tx_effective_value(S, rs, env);
    if (cur == NULL) return NULL;
    /* commute does NOT mark rs->read -- two transactions commuting on
     * the same ref shouldn't conflict. */
    result = compute(S, cur, ctx, env);
    if (result == NULL) return NULL;
    if (rs->kind == TX_STATE_ALTER && (rs->tentative != NULL || rs->read)) {
        /* alter-then-commute: alter has already pinned a value (or at
         * least committed to read-set validation). Fold the commute
         * into the alter -- it's just another arbitrary recompute.
         * Matches JVM, which skips commute-log replay at commit for
         * refs already in the write set. */
        rs->tentative = result;
    } else {
        /* Fresh commute or commute-then-commute: prepend a log entry
         * and switch to COMMUTE_LOG. */
        entry = make_log_entry(S, ctx);
        if (entry == NULL) return NULL;
        rs->commute_log = mino_cons(S, entry, rs->commute_log != NULL
                                              ? rs->commute_log
                                              : mino_empty_list(S));
        rs->kind        = TX_STATE_COMMUTE_LOG;
        rs->tentative   = NULL;
    }
    return result;
}

mino_val_t *prim_commute(mino_state_t *S, mino_val_t *args, mino_env_t *env)
{
    mino_val_t *ref;
    struct compute_clj_ctx clj_ctx;
    if (!mino_is_cons(args) || !mino_is_cons(args->as.cons.cdr)) {
        return prim_throw_classified(S, "eval/arity", "MAR001",
            "commute requires at least two arguments: ref and function");
    }
    ref           = args->as.cons.car;
    clj_ctx.fn    = args->as.cons.cdr->as.cons.car;
    clj_ctx.extra = args->as.cons.cdr->as.cons.cdr;
    if (ref == NULL || ref->type != MINO_TX_REF) {
        return prim_throw_classified(S, "eval/type", "MTY001",
            "commute: first argument must be a ref");
    }
    return tx_commute_core(S, ref, compute_clj, make_clj_log_entry,
                            &clj_ctx, env);
}

mino_val_t *mino_tx_commute_c(mino_state_t *S, mino_val_t *ref,
                              mino_tx_xform_fn fn, void *user,
                              mino_env_t *env)
{
    struct compute_c_ctx c_ctx;
    if (ref == NULL || ref->type != MINO_TX_REF) {
        return prim_throw_classified(S, "eval/type", "MTY001",
            "mino_tx_commute_c: argument must be a ref");
    }
    if (fn == NULL) {
        return prim_throw_classified(S, "eval/type", "MTY001",
            "mino_tx_commute_c: transformer fn must not be NULL");
    }
    c_ctx.fn   = fn;
    c_ctx.user = user;
    return tx_commute_core(S, ref, compute_c, make_c_log_entry,
                            &c_ctx, env);
}

/* Shared core for prim_ensure / mino_tx_ensure. Caller has already
 * type-checked ref. Returns the in-tx effective value, or NULL on
 * throw. */
static mino_val_t *tx_ensure_core(mino_state_t *S, mino_val_t *ref,
                                   mino_env_t *env)
{
    tx_ref_state_t    *rs;
    mino_val_t        *val;
    mino_thread_ctx_t *ctx = mino_current_ctx(S);
    if (ctx->current_tx == NULL) {
        return prim_throw_classified(S, "eval/state", "MST002",
            "No transaction running");
    }
    rs = tx_get_or_create_ref_state(S, ref);
    if (rs == NULL) return NULL;
    val = tx_effective_value(S, rs, env);
    if (val == NULL) return NULL;
    /* In our single-version optimistic model, ensure is structurally
     * the same as a deref -- it captures the snapshot version so any
     * other tx that mutates the ref will fail this tx's read-set
     * validation. The Clojure semantic of "block any other write" is
     * automatically enforced via the version-bump-on-commit rule. */
    if (!rs->read) {
        rs->read             = 1;
        rs->snapshot_version = ref->as.tx_ref.version;
    }
    return val;
}

mino_val_t *prim_ensure(mino_state_t *S, mino_val_t *args, mino_env_t *env)
{
    mino_val_t *ref;
    if (!mino_is_cons(args) || mino_is_cons(args->as.cons.cdr)) {
        return prim_throw_classified(S, "eval/arity", "MAR001",
            "ensure requires one argument");
    }
    ref = args->as.cons.car;
    if (ref == NULL || ref->type != MINO_TX_REF) {
        return prim_throw_classified(S, "eval/type", "MTY001",
            "ensure: argument must be a ref");
    }
    return tx_ensure_core(S, ref, env);
}

mino_val_t *mino_tx_ensure(mino_state_t *S, mino_val_t *ref,
                           mino_env_t *env)
{
    if (ref == NULL || ref->type != MINO_TX_REF) {
        return prim_throw_classified(S, "eval/type", "MTY001",
            "mino_tx_ensure: argument must be a ref");
    }
    return tx_ensure_core(S, ref, env);
}

/* --- commit phase --------------------------------------------------------- *
 *
 * Validate the read set against current ref versions; if any
 * mismatched, return 0 to signal retry. Otherwise apply every
 * recorded write under the global commit lock with a write barrier
 * + version bump and return 1 (committed). Commute logs replay
 * against the latest committed value so concurrent commutes against
 * the same ref do not conflict.
 */
/* Run a ref's validator (if any) against the proposed new value via
 * mino_pcall so a thrown validator does not longjmp out while we still
 * hold the commit lock. Returns 1 on success, 0 on validator throw,
 * -1 on validator returning falsy (which is also a rejection but
 * surfaces a different error). */
static int run_ref_validator(mino_state_t *S, mino_val_t *ref,
                              mino_val_t *new_val, mino_env_t *env)
{
    mino_val_t *vfn = ref->as.tx_ref.validator;
    mino_val_t *vargs;
    mino_val_t *result = NULL;
    int         pc;
    if (vfn == NULL) return 1;
    vargs = mino_cons(S, new_val, mino_nil(S));
    pc = mino_pcall(S, vfn, vargs, env, &result);
    if (pc != 0) return 0;
    if (result == NULL) return 0;
    if (!mino_is_truthy(result)) return -1;
    return 1;
}

static int tx_commit(mino_state_t *S, tx_state_t *tx, mino_env_t *env,
                     int *out_validator_rejected)
{
    tx_ref_state_t *rs;
    if (out_validator_rejected != NULL) *out_validator_rejected = 0;
    stm_lock(S);
    /* Validate read set: every ref that the tx read must still be at
     * its snapshot version. */
    for (rs = tx->refs_head; rs != NULL; rs = rs->next) {
        if (rs->read
            && rs->ref->as.tx_ref.version != rs->snapshot_version) {
            stm_unlock(S);
            return 0;
        }
    }
    /* Compute new values + run validators + apply writes. */
    for (rs = tx->refs_head; rs != NULL; rs = rs->next) {
        mino_val_t *new_val = NULL;
        rs->committed_old = NULL;
        rs->committed_new = NULL;
        if (rs->kind == TX_STATE_ALTER && rs->tentative != NULL) {
            new_val = rs->tentative;
        } else if (rs->kind == TX_STATE_COMMUTE_LOG
                   && rs->commute_log != NULL) {
            new_val = commute_log_replay(S, rs->commute_log,
                                          rs->ref->as.tx_ref.val, env);
            if (new_val == NULL) {
                stm_unlock(S);
                return 0;
            }
        }
        if (new_val != NULL) {
            int vc = run_ref_validator(S, rs->ref, new_val, env);
            if (vc != 1) {
                stm_unlock(S);
                if (out_validator_rejected != NULL && vc == -1) {
                    *out_validator_rejected = 1;
                }
                return 0;
            }
            rs->committed_old = rs->ref->as.tx_ref.val;
            rs->committed_new = new_val;
            gc_write_barrier(S, rs->ref, rs->committed_old, new_val);
            rs->ref->as.tx_ref.val = new_val;
            rs->ref->as.tx_ref.version++;
        }
    }
    stm_unlock(S);
    return 1;
}

/* Walk the per-ref state list and dispatch watch callbacks for every
 * ref that committed a write. Runs OUTSIDE the commit lock with
 * ctx->current_tx already cleared so a watch fn that itself calls
 * dosync allocates fresh transaction state. A watch that throws
 * propagates out of the commit; later watches do not fire. */
static int dispatch_watches(mino_state_t *S, tx_state_t *tx,
                             mino_env_t *env)
{
    tx_ref_state_t *rs;
    for (rs = tx->refs_head; rs != NULL; rs = rs->next) {
        mino_val_t *watches;
        size_t      i, n;
        if (rs->committed_new == NULL) continue;
        watches = rs->ref->as.tx_ref.watches;
        if (watches == NULL || watches->type != MINO_MAP
            || watches->as.map.len == 0) {
            continue;
        }
        n = watches->as.map.len;
        for (i = 0; i < n; i++) {
            mino_val_t *key = vec_nth(watches->as.map.key_order, i);
            mino_val_t *fn  = map_get_val(watches, key);
            mino_val_t *wargs;
            if (fn == NULL) continue;
            wargs = mino_cons(S, key,
                      mino_cons(S, rs->ref,
                        mino_cons(S, rs->committed_old,
                          mino_cons(S, rs->committed_new, mino_nil(S)))));
            if (mino_call(S, fn, wargs, env) == NULL) return -1;
        }
    }
    return 0;
}

/* --- dosync entry point --------------------------------------------------- *
 *
 * The retry loop: push our own try frame so a throw inside the body
 * lands here for cleanup before propagating outward (otherwise
 * ctx->current_tx would dangle past the now-unwound C stack). On
 * commit-time conflict, clear tentatives, free per-ref state nodes,
 * and re-run the body up to STM_RETRY_CAP times.
 */
/* Body of dosync*: extracted so the setjmp-bearing function does
 * minimal work. Splitting the retry-loop into its own non-setjmp
 * frame sidesteps -Wclobbered for every local that mutates across
 * the loop. */
static mino_val_t *dosync_run(mino_state_t *S, mino_val_t *thunk,
                              mino_env_t *env, tx_state_t *tx)
{
    for (;;) {
        mino_val_t *r;
        int         validator_rejected = 0;
        tx->refs_head    = NULL;
        tx->retry_signal = 0;

        if (tx->retry_count > STM_RETRY_CAP) {
            tx->retry_count = 0;
            prim_throw_classified(S, "eval/state", "MST004",
                "transaction retry limit exceeded");
            return NULL; /* unreachable -- prim_throw_classified longjmps */
        }
        r = mino_call(S, thunk, mino_nil(S), env);
        if (r == NULL) {
            return NULL;
        }
        if (tx_commit(S, tx, env, &validator_rejected)) {
            return r;
        }
        if (validator_rejected) {
            tx_clear_ref_states(tx);
            prim_throw_classified(S, "eval/contract", "MCT001",
                "Invalid reference state");
            return NULL; /* unreachable */
        }
        /* Conflict: free per-ref state and retry. */
        tx_clear_ref_states(tx);
        tx->retry_count++;
    }
}

mino_val_t *prim_dosync_star(mino_state_t *S, mino_val_t *args,
                              mino_env_t *env)
{
    mino_val_t                *thunk;
    /* Volatile-qualified so the setjmp-clobber analysis stays clean:
     * any local needed in both the success and longjmp arms must be
     * either declared volatile or reloaded after setjmp. */
    mino_thread_ctx_t * volatile ctx_v;
    mino_val_t        * volatile result_v = NULL;
    int                          saved_try;
    tx_state_t                   tx;
    try_frame_t                 *frame;

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
    ctx_v = mino_current_ctx(S);
    /* Nested dosync: bump depth, reuse the outer tx, run the thunk. */
    if (ctx_v->current_tx != NULL) {
        mino_val_t        *r;
        mino_thread_ctx_t *ctx = ctx_v;
        ctx->current_tx->depth++;
        r = mino_call(S, thunk, mino_nil(S), env);
        if (ctx->current_tx != NULL) ctx->current_tx->depth--;
        return r;
    }
    /* Outermost dosync: tx state on the C stack. Push a try frame so
     * an in-body throw is intercepted long enough to clear
     * ctx->current_tx and free per-ref state before propagating. */
    saved_try             = ctx_v->try_depth;
    tx.depth              = 1;
    tx.refs_head          = NULL;
    tx.retry_count        = 0;
    tx.try_depth_at_start = saved_try;
    tx.retry_signal       = 0;

    if (ctx_v->try_depth >= MAX_TRY_DEPTH) {
        return prim_throw_classified(S, "eval/state", "MST006",
            "dosync*: try-stack overflow");
    }
    frame                 = &ctx_v->try_stack[ctx_v->try_depth];
    frame->exception      = NULL;
    frame->saved_ns       = S->current_ns;
    frame->saved_ambient  = S->fn_ambient_ns;
    frame->saved_load_len = S->load_stack_len;
    if (setjmp(frame->buf) != 0) {
        /* Body threw. Use the volatile ctx_v captured before setjmp so
         * we don't re-enter the inlined mino_current_ctx (whose locals
         * trigger -Wclobbered). */
        mino_thread_ctx_t *c  = ctx_v;
        mino_val_t        *ex = c->try_stack[saved_try].exception;
        tx_clear_ref_states(&tx);
        c->current_tx    = NULL;
        c->try_depth     = saved_try;
        S->current_ns    = c->try_stack[saved_try].saved_ns;
        S->fn_ambient_ns = c->try_stack[saved_try].saved_ambient;
        if (c->try_depth > 0) {
            c->try_stack[c->try_depth - 1].exception = ex;
            longjmp(c->try_stack[c->try_depth - 1].buf, 1);
        }
        if (ex != NULL && ex->type == MINO_STRING) {
            set_eval_diag(S, c->eval_current_form,
                          "user", "MUS001", ex->as.s.data);
        } else {
            set_eval_diag(S, c->eval_current_form,
                          "internal", "MIN001",
                          "unhandled exception in dosync");
        }
        return NULL;
    }
    ctx_v->try_depth++;
    ctx_v->current_tx = &tx;

    result_v = dosync_run(S, thunk, env, &tx);

    {
        mino_thread_ctx_t *c = ctx_v;
        /* Clear current_tx BEFORE watch dispatch so a watch fn that
         * itself calls dosync allocates fresh transaction state. The
         * try frame is still active so a thrown watch still cleans up
         * via the catch arm (refs_head freed there). */
        c->current_tx = NULL;
        if (result_v != NULL) {
            (void)dispatch_watches(S, &tx, env);
            /* If a watch threw, the longjmp landed at our setjmp and
             * cleanup ran there; this branch only sees clean returns. */
        }
        tx_clear_ref_states(&tx);
        c->try_depth  = saved_try;
    }
    return result_v;
}

/* --- primitive table ----------------------------------------------------- */

const mino_prim_def k_prims_stm[] = {
    {"ref",       prim_ref,
     "Creates an STM ref holding the given initial value. Mutate via "
     "ref-set / alter / commute inside dosync."},
    {"ref?",      prim_ref_p,
     "Returns true if x is an STM ref."},
    {"ref-set",   prim_ref_set,
     "Sets the value of ref. Must be in dosync. Returns the new value."},
    {"alter",     prim_alter,
     "Sets ref to (apply f current-value args). Must be in dosync. "
     "Returns the new value."},
    {"commute",   prim_commute,
     "Sets ref to (apply f current-value args). Like alter but does "
     "not participate in read-set validation -- two transactions "
     "commuting on the same ref do not conflict. The fn is replayed "
     "against the latest committed value at commit time. Must be in "
     "dosync."},
    {"ensure",    prim_ensure,
     "Reads ref and prevents any other transaction from changing it "
     "before this transaction commits. Must be in dosync. Returns "
     "the current in-tx value."},
    {"dosync*",   prim_dosync_star,
     "Runs a zero-arg thunk inside an STM transaction. The `dosync` "
     "macro expands to (dosync* (fn [] body...))."},
    {"io!-check", prim_io_bang_check,
     "Internal: throws when called inside a transaction. The io! "
     "macro expands to (do (io!-check) body...) so the check runs "
     "before the body evaluates."},
    {"in-transaction?", prim_in_tx_p,
     "Returns true when called from inside a `dosync` body."},
    {"ref-min-history", prim_ref_min_history,
     "Returns the ref's min-history. mino uses single-version "
     "optimistic locking; this stub always returns 0."},
    {"ref-max-history", prim_ref_max_history,
     "Returns the ref's max-history. mino uses single-version "
     "optimistic locking; this stub always returns 10."},
    {"ref-history-count", prim_ref_history_count,
     "Returns the ref's current history-count. mino uses single-"
     "version optimistic locking; this stub always returns 0."},
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
