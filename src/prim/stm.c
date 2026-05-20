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
 *     mutex (S->stm.commit_lock) lazy-initialized in
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
 *     time. JVM matches this; throwing MCT001 at install time would
 *     be the deviation.
 *
 *  8. alter-after-commute and ref-set-after-commute throw
 *     "Can't set after commute" (JVM canon). The commute-after-alter
 *     direction folds the commute into the alter's tentative; commit
 *     writes the alter+commute tentative, matching JVM's behavior of
 *     skipping commute-log replay for refs already in the write set.
 *
 *  9. mino-only addition: cross-state ref defense. JVM has one global
 *     transactional surface; mino has many `mino_state` per host
 *     process. Every public C entry checks `tx_ref.owning_state == S`
 *     and throws `eval/state` MST007 on mismatch.
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
#include "mino.h"
#include "eval/internal.h"
#include <setjmp.h>

#define STM_RETRY_CAP 10000

/* --- helpers -------------------------------------------------------------- */

/* Acquire / release the global STM commit lock. No-op when STM has
 * never been installed (the bool stays 0); otherwise a plain pthread
 * mutex acquire. The single-threaded fast path skips the lock since
 * no other thread can interfere with the commit phase. */
static void stm_lock(mino_state *S)
{
    if (!S->stm.lock_inited) return;
    if (S->threading.thread_limit <= 1) return;
#if !(defined(_WIN32) && defined(_MSC_VER))
    pthread_mutex_lock(&S->stm.commit_lock);
#endif
}

static void stm_unlock(mino_state *S)
{
    if (!S->stm.lock_inited) return;
    if (S->threading.thread_limit <= 1) return;
#if !(defined(_WIN32) && defined(_MSC_VER))
    pthread_mutex_unlock(&S->stm.commit_lock);
#endif
}

/* Fetch or create the per-tx state node for a given ref within the
 * active transaction. Returns NULL outside a transaction. */
static tx_ref_state_t *tx_get_or_create_ref_state(mino_state *S,
                                                   mino_val *ref)
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
mino_val *prim_io_bang_check(mino_state *S, mino_val *args,
                                mino_env *env)
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

mino_val *prim_in_tx_p(mino_state *S, mino_val *args, mino_env *env)
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
mino_val *prim_ref_min_history(mino_state *S, mino_val *args,
                                  mino_env *env)
{
    (void)env;
    if (!mino_is_cons(args)) {
        return prim_throw_classified(S, "eval/arity", "MAR001",
            "ref-min-history requires a ref");
    }
    return mino_int(S, 0);
}

mino_val *prim_ref_max_history(mino_state *S, mino_val *args,
                                  mino_env *env)
{
    (void)env;
    if (!mino_is_cons(args)) {
        return prim_throw_classified(S, "eval/arity", "MAR001",
            "ref-max-history requires a ref");
    }
    return mino_int(S, 10);
}

mino_val *prim_ref_history_count(mino_state *S, mino_val *args,
                                    mino_env *env)
{
    (void)env;
    if (!mino_is_cons(args)) {
        return prim_throw_classified(S, "eval/arity", "MAR001",
            "ref-history-count requires a ref");
    }
    return mino_int(S, 0);
}

/* --- ref construction + identity predicate -------------------------------- */

mino_val *prim_ref(mino_state *S, mino_val *args, mino_env *env)
{
    mino_val *initial;
    mino_val *opts;
    mino_val *validator = NULL;
    mino_val *meta      = NULL;
    mino_val *ref;
    (void)env;
    if (!mino_is_cons(args)) {
        return prim_throw_classified(S, "eval/arity", "MAR001",
            "ref requires an initial value");
    }
    initial = args->as.cons.car;
    /* Parse trailing :validator / :meta / :min-history / :max-history
     * keyword pairs. Mirrors JVM Clojure's `ref` signature; mino's STM
     * doesn't track history so the history options are accepted as
     * no-ops for source compatibility. Unknown options are rejected
     * (canon behaviour) so a typo doesn't silently install no
     * validator. */
    opts = args->as.cons.cdr;
    while (mino_is_cons(opts)) {
        mino_val *k = opts->as.cons.car;
        mino_val *v;
        if (k == NULL || mino_type_of(k) != MINO_KEYWORD) {
            return prim_throw_classified(S, "eval/arity", "MAR001",
                "ref: option keys must be keywords");
        }
        if (!mino_is_cons(opts->as.cons.cdr)) {
            return prim_throw_classified(S, "eval/arity", "MAR001",
                "ref: option key without value");
        }
        v = opts->as.cons.cdr->as.cons.car;
        if (k->as.s.len == 9 && memcmp(k->as.s.data, "validator", 9) == 0) {
            if (v != NULL && mino_type_of(v) != MINO_NIL
                && mino_type_of(v) != MINO_FN
                && mino_type_of(v) != MINO_PRIM
                && mino_type_of(v) != MINO_MACRO) {
                return prim_throw_classified(S, "eval/type", "MTY001",
                    "ref: :validator must be a fn or nil");
            }
            validator = (v != NULL && mino_type_of(v) != MINO_NIL) ? v : NULL;
        } else if (k->as.s.len == 4 && memcmp(k->as.s.data, "meta", 4) == 0) {
            if (v != NULL && mino_type_of(v) != MINO_NIL
                && mino_type_of(v) != MINO_MAP) {
                return prim_throw_classified(S, "eval/type", "MTY001",
                    "ref: :meta must be a map or nil");
            }
            meta = (v != NULL && mino_type_of(v) != MINO_NIL) ? v : NULL;
        } else if ((k->as.s.len == 11
                    && memcmp(k->as.s.data, "min-history", 11) == 0)
                   || (k->as.s.len == 11
                       && memcmp(k->as.s.data, "max-history", 11) == 0)) {
            /* Accepted, no-op. mino's STM has no history. */
        } else {
            char msg[200];
            snprintf(msg, sizeof(msg),
                "ref: unknown option :%.*s",
                (int)k->as.s.len, k->as.s.data);
            return prim_throw_classified(S, "eval/arity", "MAR001", msg);
        }
        opts = opts->as.cons.cdr->as.cons.cdr;
    }
    ref = mino_tx_ref(S, initial);
    if (ref == NULL) return NULL;
    if (validator != NULL) ref->as.tx_ref.validator = validator;
    if (meta != NULL)      ref->meta = meta;
    return ref;
}

mino_val *prim_ref_p(mino_state *S, mino_val *args, mino_env *env)
{
    mino_val *v;
    (void)env;
    if (!mino_is_cons(args) || mino_is_cons(args->as.cons.cdr)) {
        return prim_throw_classified(S, "eval/arity", "MAR001",
            "ref? requires one argument");
    }
    v = args->as.cons.car;
    return (v != NULL && mino_type_of(v) == MINO_TX_REF) ? mino_true(S)
                                                  : mino_false(S);
}

/* Forward declaration -- alter_build_args is defined below the deref
 * dispatch but used by commute_log_replay above. */
static mino_val *alter_build_args(mino_state *S, mino_val *cur,
                                    mino_val *extra);

/* Forward declaration -- tx_check_ref_owned is defined just below
 * mino_is_tx_ref but called from mino_tx_ref_deref above it. */
static int tx_check_ref_owned(mino_state *S, mino_val *ref);

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
 *
 * Clojure-side entries are invoked via mino_pcall so a throw cannot
 * longjmp past the caller. Commit-phase callers hold the global STM
 * lock; an unprotected longjmp there would leak it. When out_ex is
 * non-NULL and a Clojure entry throws, *out_ex receives the user's
 * raw exception value and the function returns NULL. C-side closures
 * (TX_C_CLOSURE_TAG) are invoked directly: per the public C API
 * contract, host transformers must surface failure via NULL, not
 * longjmp.
 */
static mino_val *commute_log_replay(mino_state *S, mino_val *log,
                                      mino_val *base, mino_env *env,
                                      mino_val **out_ex)
{
    mino_val *reversed = mino_empty_list(S);
    mino_val *p = log;
    mino_val *cur = base;
    if (out_ex != NULL) *out_ex = NULL;
    while (mino_is_cons(p)) {
        reversed = mino_cons(S, p->as.cons.car, reversed);
        p = p->as.cons.cdr;
    }
    for (p = reversed; mino_is_cons(p); p = p->as.cons.cdr) {
        mino_val *entry  = p->as.cons.car;
        mino_val *result = NULL;
        if (entry != NULL && mino_type_of(entry) == MINO_HANDLE
            && entry->as.handle.tag == TX_C_CLOSURE_TAG) {
            struct tx_c_closure *c = (struct tx_c_closure *)
                                      entry->as.handle.ptr;
            result = c->fn(S, cur, c->user, env);
        } else if (mino_is_cons(entry)) {
            mino_val *fn        = entry->as.cons.car;
            mino_val *extra     = entry->as.cons.cdr;
            mino_val *call_args = alter_build_args(S, cur, extra);
            mino_val *thrown    = NULL;
            int         pc        = mino_pcall(S, fn, call_args, env,
                                                &result, &thrown);
            if (pc != 0) {
                if (out_ex != NULL) *out_ex = thrown;
                return NULL;
            }
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
static mino_val *tx_effective_value(mino_state *S, tx_ref_state_t *rs,
                                      mino_env *env)
{
    if (rs->kind == TX_STATE_ALTER && rs->tentative != NULL) {
        return rs->tentative;
    }
    if (rs->kind == TX_STATE_COMMUTE_LOG && rs->commute_log != NULL) {
        return commute_log_replay(S, rs->commute_log,
                                   rs->ref->as.tx_ref.val, env, NULL);
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
 * defensively pass any mino_val * without pre-checking the type.
 */
mino_val *mino_tx_ref_deref(mino_state *S, mino_val *ref)
{
    mino_thread_ctx_t *ctx;
    tx_state_t        *tx;
    tx_ref_state_t    *rs;
    mino_val        *val;
    if (ref == NULL || mino_type_of(ref) != MINO_TX_REF) return NULL;
    if (tx_check_ref_owned(S, ref)) return NULL;
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

int mino_is_tx_ref(const mino_val *v)
{
    return v != NULL && mino_type_of(v) == MINO_TX_REF;
}

/* Cross-state defense: throw eval/state MST007 if the ref was
 * allocated in a different state than S. mino_tx_ref records its
 * allocating state in tx_ref.owning_state at construction time, so
 * the check is one pointer comparison. Catches a host that passes a
 * ref between states (typically by smuggling one through
 * mino_env_set or mino_call) without going through serialization.
 * Returns 0 on success, 1 if a throw was raised (caller should
 * propagate NULL).
 *
 * Called from the shared cores below so both the public C API and
 * the Clojure-side prims are covered -- a foreign ref injected
 * into S's env via mino_env_set was always reachable from a
 * Clojure program; the earlier "C entries only" gate left that
 * gap. */
static int tx_check_ref_owned(mino_state *S, mino_val *ref)
{
    if (ref->as.tx_ref.owning_state != S) {
        prim_throw_classified(S, "eval/state", "MST007",
            "ref from foreign state");
        return 1;
    }
    return 0;
}

/* --- ref-set + alter ----------------------------------------------------- */

/* Shared core for prim_ref_set / mino_tx_ref_set. Caller has already
 * type-checked ref. Returns val on success; NULL on throw (the throw
 * has already been raised via prim_throw_classified). */
static mino_val *tx_ref_set_core(mino_state *S, mino_val *ref,
                                    mino_val *val)
{
    tx_ref_state_t    *rs;
    mino_thread_ctx_t *ctx = mino_current_ctx(S);
    if (tx_check_ref_owned(S, ref)) return NULL;
    if (ctx->current_tx == NULL) {
        return prim_throw_classified(S, "eval/state", "MST002",
            "No transaction running");
    }
    if (ctx->current_tx->in_commit) {
        return prim_throw_classified(S, "eval/state", "MST002",
            "ref-set not allowed during commit-phase replay");
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

mino_val *prim_ref_set(mino_state *S, mino_val *args, mino_env *env)
{
    mino_val *ref;
    mino_val *val;
    (void)env;
    if (!mino_is_cons(args) || !mino_is_cons(args->as.cons.cdr)
        || mino_is_cons(args->as.cons.cdr->as.cons.cdr)) {
        return prim_throw_classified(S, "eval/arity", "MAR001",
            "ref-set requires two arguments: ref and value");
    }
    ref = args->as.cons.car;
    val = args->as.cons.cdr->as.cons.car;
    if (ref == NULL || mino_type_of(ref) != MINO_TX_REF) {
        return prim_throw_classified(S, "eval/type", "MTY001",
            "ref-set: first argument must be a ref");
    }
    return tx_ref_set_core(S, ref, val);
}

mino_val *mino_tx_ref_set(mino_state *S, mino_val *ref, mino_val *val)
{
    if (ref == NULL || mino_type_of(ref) != MINO_TX_REF) {
        return prim_throw_classified(S, "eval/type", "MTY001",
            "mino_tx_ref_set: argument must be a ref");
    }
    return tx_ref_set_core(S, ref, val);
}

/* Build the argument list (cur-val arg1 arg2 ...) for invoking f
 * during alter / commute. The cur-val is the tentative or committed
 * value as of this call site. */
static mino_val *alter_build_args(mino_state *S, mino_val *cur,
                                    mino_val *extra)
{
    mino_val *cell, *head;
    head = mino_cons(S, cur, mino_nil(S));
    cell = head;
    while (mino_is_cons(extra)) {
        mino_val *next = mino_cons(S, extra->as.cons.car, mino_nil(S));
        cell->as.cons.cdr = next;
        cell = next;
        extra = extra->as.cons.cdr;
    }
    return head;
}

/* Compute-new callback: produces the post-transformer value given
 * the current in-tx value. Implementations bind to either a Clojure
 * fn invocation (mino_call) or a direct C transformer call. */
typedef mino_val *(*tx_compute_fn)(mino_state *S, mino_val *cur,
                                      void *ctx, mino_env *env);

struct compute_clj_ctx {
    mino_val *fn;
    mino_val *extra;
};

static mino_val *compute_clj(mino_state *S, mino_val *cur,
                                void *ctx, mino_env *env)
{
    struct compute_clj_ctx *c = (struct compute_clj_ctx *)ctx;
    mino_val *call_args = alter_build_args(S, cur, c->extra);
    return mino_call(S, c->fn, call_args, env);
}

struct compute_c_ctx {
    mino_tx_xform_fn fn;
    void            *user;
};

static mino_val *compute_c(mino_state *S, mino_val *cur,
                              void *ctx, mino_env *env)
{
    struct compute_c_ctx *c = (struct compute_c_ctx *)ctx;
    return c->fn(S, cur, c->user, env);
}

/* Shared core for prim_alter / mino_tx_alter_c. Caller has already
 * type-checked ref. The compute callback runs under the in-tx
 * effective value AFTER the read mark is recorded, since alter
 * implicitly reads the ref. Returns the new value, or NULL on throw. */
static mino_val *tx_alter_core(mino_state *S, mino_val *ref,
                                  tx_compute_fn compute, void *ctx,
                                  mino_env *env)
{
    tx_ref_state_t    *rs;
    mino_val        *cur, *result;
    mino_thread_ctx_t *t_ctx = mino_current_ctx(S);
    if (tx_check_ref_owned(S, ref)) return NULL;
    if (t_ctx->current_tx == NULL) {
        return prim_throw_classified(S, "eval/state", "MST002",
            "No transaction running");
    }
    if (t_ctx->current_tx->in_commit) {
        return prim_throw_classified(S, "eval/state", "MST002",
            "alter not allowed during commit-phase replay");
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

mino_val *prim_alter(mino_state *S, mino_val *args, mino_env *env)
{
    mino_val *ref;
    struct compute_clj_ctx clj_ctx;
    if (!mino_is_cons(args) || !mino_is_cons(args->as.cons.cdr)) {
        return prim_throw_classified(S, "eval/arity", "MAR001",
            "alter requires at least two arguments: ref and function");
    }
    ref           = args->as.cons.car;
    clj_ctx.fn    = args->as.cons.cdr->as.cons.car;
    clj_ctx.extra = args->as.cons.cdr->as.cons.cdr;
    if (ref == NULL || mino_type_of(ref) != MINO_TX_REF) {
        return prim_throw_classified(S, "eval/type", "MTY001",
            "alter: first argument must be a ref");
    }
    return tx_alter_core(S, ref, compute_clj, &clj_ctx, env);
}

mino_val *mino_tx_alter_c(mino_state *S, mino_val *ref,
                            mino_tx_xform_fn fn, void *user,
                            mino_env *env)
{
    struct compute_c_ctx c_ctx;
    if (ref == NULL || mino_type_of(ref) != MINO_TX_REF) {
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
static mino_val *make_clj_log_entry(mino_state *S, void *ctx)
{
    struct compute_clj_ctx *c = (struct compute_clj_ctx *)ctx;
    return mino_cons(S, c->fn, c->extra);
}

static mino_val *make_c_log_entry(mino_state *S, void *ctx)
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

typedef mino_val *(*tx_log_entry_fn)(mino_state *S, void *ctx);

/* Shared core for prim_commute / mino_tx_commute_c. Caller has
 * already type-checked ref. Returns the call-site value, or NULL on
 * throw. The compute callback runs eagerly to produce the call-site
 * return; if a fresh-or-chained commute, make_log_entry produces the
 * value pushed onto the log for replay at commit. */
static mino_val *tx_commute_core(mino_state *S, mino_val *ref,
                                    tx_compute_fn compute,
                                    tx_log_entry_fn make_log_entry,
                                    void *ctx, mino_env *env)
{
    tx_ref_state_t    *rs;
    mino_val        *cur, *result, *entry;
    mino_thread_ctx_t *t_ctx = mino_current_ctx(S);
    if (tx_check_ref_owned(S, ref)) return NULL;
    if (t_ctx->current_tx == NULL) {
        return prim_throw_classified(S, "eval/state", "MST002",
            "No transaction running");
    }
    if (t_ctx->current_tx->in_commit) {
        return prim_throw_classified(S, "eval/state", "MST002",
            "commute not allowed during commit-phase replay");
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

mino_val *prim_commute(mino_state *S, mino_val *args, mino_env *env)
{
    mino_val *ref;
    struct compute_clj_ctx clj_ctx;
    if (!mino_is_cons(args) || !mino_is_cons(args->as.cons.cdr)) {
        return prim_throw_classified(S, "eval/arity", "MAR001",
            "commute requires at least two arguments: ref and function");
    }
    ref           = args->as.cons.car;
    clj_ctx.fn    = args->as.cons.cdr->as.cons.car;
    clj_ctx.extra = args->as.cons.cdr->as.cons.cdr;
    if (ref == NULL || mino_type_of(ref) != MINO_TX_REF) {
        return prim_throw_classified(S, "eval/type", "MTY001",
            "commute: first argument must be a ref");
    }
    return tx_commute_core(S, ref, compute_clj, make_clj_log_entry,
                            &clj_ctx, env);
}

mino_val *mino_tx_commute_c(mino_state *S, mino_val *ref,
                              mino_tx_xform_fn fn, void *user,
                              mino_env *env)
{
    struct compute_c_ctx c_ctx;
    if (ref == NULL || mino_type_of(ref) != MINO_TX_REF) {
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
static mino_val *tx_ensure_core(mino_state *S, mino_val *ref,
                                   mino_env *env)
{
    tx_ref_state_t    *rs;
    mino_val        *val;
    mino_thread_ctx_t *ctx = mino_current_ctx(S);
    if (tx_check_ref_owned(S, ref)) return NULL;
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

mino_val *prim_ensure(mino_state *S, mino_val *args, mino_env *env)
{
    mino_val *ref;
    if (!mino_is_cons(args) || mino_is_cons(args->as.cons.cdr)) {
        return prim_throw_classified(S, "eval/arity", "MAR001",
            "ensure requires one argument");
    }
    ref = args->as.cons.car;
    if (ref == NULL || mino_type_of(ref) != MINO_TX_REF) {
        return prim_throw_classified(S, "eval/type", "MTY001",
            "ensure: argument must be a ref");
    }
    return tx_ensure_core(S, ref, env);
}

mino_val *mino_tx_ensure(mino_state *S, mino_val *ref,
                           mino_env *env)
{
    if (ref == NULL || mino_type_of(ref) != MINO_TX_REF) {
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
 * mino_pcall so a thrown validator does not longjmp out while we
 * still hold the commit lock. Returns:
 *   1  -- validator returned truthy
 *   0  -- validator threw; *out_ex set to the thrown value
 *  -1  -- validator returned falsy (no throw); *out_ex unchanged.
 * vfn==NULL is treated as "no validator" and returns 1. */
static int run_ref_validator(mino_state *S, mino_val *ref,
                              mino_val *new_val, mino_env *env,
                              mino_val **out_ex)
{
    mino_val *vfn = ref->as.tx_ref.validator;
    mino_val *vargs;
    mino_val *result = NULL;
    mino_val *thrown = NULL;
    int         pc;
    if (vfn == NULL) return 1;
    vargs = mino_cons(S, new_val, mino_nil(S));
    pc = mino_pcall(S, vfn, vargs, env, &result, &thrown);
    if (pc != 0) {
        if (out_ex != NULL) *out_ex = thrown;
        return 0;
    }
    if (result == NULL) return 0;  /* defensive; shouldn't happen */
    if (!mino_is_truthy(result)) return -1;
    return 1;
}

static int tx_commit(mino_state *S, tx_state_t *tx, mino_env *env,
                     int *out_validator_rejected)
{
    tx_ref_state_t *rs;
    if (out_validator_rejected != NULL) *out_validator_rejected = 0;
    tx->validator_thrown_ex = NULL;
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
    /* Mark in_commit so user code re-entered through commute log
     * replay or validator pcall sees a clean error if it tries to
     * mutate refs (alter / ref-set / commute). Without this flag
     * the new tentative would be appended to refs_head past the
     * iterator and silently lost, or invalidate the active replay
     * walk. Cleared on every exit path below. */
    tx->in_commit = 1;
    /* Two-pass commit for atomicity. Pass 1 computes every new
     * value and runs every validator; any throw or rejection
     * aborts the whole commit before a single ref->val is touched.
     * Pass 2 applies the staged writes -- no user code there, so
     * it cannot fail mid-flight. The earlier single-pass walk wrote
     * committed refs in iteration order, so a late-iteration
     * validator rejection or commute throw left earlier refs
     * already committed (atomicity violation). */
    /* Pass 1: stage. */
    for (rs = tx->refs_head; rs != NULL; rs = rs->next) {
        mino_val *new_val = NULL;
        rs->committed_old = NULL;
        rs->committed_new = NULL;
        if (rs->kind == TX_STATE_ALTER && rs->tentative != NULL) {
            new_val = rs->tentative;
        } else if (rs->kind == TX_STATE_COMMUTE_LOG
                   && rs->commute_log != NULL) {
            mino_val *replay_ex = NULL;
            new_val = commute_log_replay(S, rs->commute_log,
                                          rs->ref->as.tx_ref.val, env,
                                          &replay_ex);
            if (new_val == NULL) {
                tx->in_commit = 0;
                stm_unlock(S);
                /* A commute fn throw during replay is a hard failure,
                 * not a retry-able read-set conflict. Funnel through
                 * the same path as a validator throw so dosync_run
                 * surfaces the user's original exception payload
                 * instead of looping until MST004. */
                if (replay_ex != NULL && out_validator_rejected != NULL) {
                    *out_validator_rejected = 1;
                    tx->validator_thrown_ex = replay_ex;
                }
                return 0;
            }
        }
        if (new_val != NULL) {
            mino_val *vex = NULL;
            int vc = run_ref_validator(S, rs->ref, new_val, env, &vex);
            if (vc != 1) {
                tx->in_commit = 0;
                stm_unlock(S);
                /* Both throws and falsy-rejects are validator
                 * rejections (hard failures), distinct from read-set
                 * conflicts that should retry. dosync_run inspects
                 * tx->validator_thrown_ex to decide whether to
                 * propagate the user's exception or throw the generic
                 * "Invalid reference state" message. */
                if (out_validator_rejected != NULL) {
                    *out_validator_rejected = 1;
                }
                tx->validator_thrown_ex = vex;
                return 0;
            }
            rs->committed_new = new_val;
        }
    }
    /* Pass 2: apply. Pure memory writes; cannot abort. */
    for (rs = tx->refs_head; rs != NULL; rs = rs->next) {
        if (rs->committed_new != NULL) {
            rs->committed_old = rs->ref->as.tx_ref.val;
            gc_write_barrier(S, rs->ref, rs->committed_old,
                              rs->committed_new);
            rs->ref->as.tx_ref.val = rs->committed_new;
            rs->ref->as.tx_ref.version++;
        }
    }
    tx->in_commit = 0;
    stm_unlock(S);
    return 1;
}

/* Walk the per-ref state list and dispatch watch callbacks for every
 * ref that committed a write. Runs OUTSIDE the commit lock with
 * ctx->current_tx already cleared so a watch fn that itself calls
 * dosync allocates fresh transaction state.
 *
 * Each watch is invoked through mino_pcall so a throw doesn't
 * abort dispatch -- earlier behavior was inconsistent with agent
 * watch dispatch (which already pcall'd) and could swallow watches
 * registered against later refs. The first thrown exception is
 * captured and re-thrown after every watch has been given a chance
 * to fire, so the dosync caller still surfaces a watch error but
 * never silently loses unrelated watches.
 */
static int dispatch_watches(mino_state *S, tx_state_t *tx,
                             mino_env *env)
{
    tx_ref_state_t *rs;
    mino_val     *first_thrown = NULL;
    for (rs = tx->refs_head; rs != NULL; rs = rs->next) {
        mino_val *watches;
        size_t      i, n;
        if (rs->committed_new == NULL) continue;
        watches = rs->ref->as.tx_ref.watches;
        if (watches == NULL || mino_type_of(watches) != MINO_MAP
            || watches->as.map.len == 0) {
            continue;
        }
        n = watches->as.map.len;
        for (i = 0; i < n; i++) {
            mino_val *key = vec_nth(watches->as.map.key_order, i);
            mino_val *fn  = map_get_val(watches, key);
            mino_val *wargs;
            mino_val *result = NULL;
            mino_val *thrown = NULL;
            int         pc;
            if (fn == NULL) continue;
            wargs = mino_cons(S, key,
                      mino_cons(S, rs->ref,
                        mino_cons(S, rs->committed_old,
                          mino_cons(S, rs->committed_new, mino_nil(S)))));
            pc = mino_pcall(S, fn, wargs, env, &result, &thrown);
            if (pc != 0 && first_thrown == NULL) {
                first_thrown = thrown;
            }
        }
    }
    if (first_thrown != NULL) {
        mino_throw(S, first_thrown);
        return -1; /* unreachable -- mino_throw longjmps */
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
/* Body invoker: runs the user-supplied transaction body once and
 * returns its result. Implementations bind to either a Clojure thunk
 * call (mino_call) or a direct C body invocation. Re-invoked on
 * retry, so the body must be idempotent w.r.t. user state outside
 * the tx. */
typedef mino_val *(*tx_invoke_body_fn)(mino_state *S, void *body_user,
                                          mino_env *env);

struct invoke_clj_thunk { mino_val *thunk; };

static mino_val *invoke_clj_thunk(mino_state *S, void *body_user,
                                     mino_env *env)
{
    struct invoke_clj_thunk *c = (struct invoke_clj_thunk *)body_user;
    return mino_call(S, c->thunk, mino_nil(S), env);
}

struct invoke_c_body { mino_tx_body_fn fn; void *user; };

static mino_val *invoke_c_body(mino_state *S, void *body_user,
                                  mino_env *env)
{
    struct invoke_c_body *c = (struct invoke_c_body *)body_user;
    return c->fn(S, c->user, env);
}

/* Retry loop body: split off the setjmp-bearing tx_outer_run so it
 * does minimal work, sidestepping -Wclobbered for locals that mutate
 * across iterations. */
static mino_val *tx_run_loop(mino_state *S,
                                tx_invoke_body_fn invoke,
                                void *body_user, mino_env *env,
                                tx_state_t *tx)
{
    for (;;) {
        mino_val *r;
        int         validator_rejected = 0;
        tx->refs_head     = NULL;
        tx->pending_sends = NULL;

        if (tx->retry_count > STM_RETRY_CAP) {
            tx->retry_count = 0;
            prim_throw_classified(S, "eval/state", "MST004",
                "transaction retry limit exceeded");
            return NULL; /* unreachable -- prim_throw_classified longjmps */
        }
        r = invoke(S, body_user, env);
        if (r == NULL) {
            return NULL;
        }
        if (tx_commit(S, tx, env, &validator_rejected)) {
            return r;
        }
        if (validator_rejected) {
            mino_val *vex = tx->validator_thrown_ex;
            tx_clear_ref_states(tx);
            tx->validator_thrown_ex = NULL;
            tx->pending_sends       = NULL;
            if (vex != NULL) {
                /* Validator threw -- propagate the user's original
                 * exception value. JVM Clojure surfaces a validator
                 * throw to the dosync caller as that exception, not
                 * as a generic IllegalStateException. */
                mino_throw(S, vex);
            } else {
                prim_throw_classified(S, "eval/contract", "MCT001",
                    "Invalid reference state");
            }
            return NULL; /* unreachable */
        }
        /* Conflict: free per-ref state and retry. JVM canon also
         * clears pending sends -- the failed attempt's queued
         * dispatches must not fire on a later successful run. */
        tx_clear_ref_states(tx);
        tx->pending_sends = NULL;
        tx->retry_count++;
    }
}

/* Outer runner shared by prim_dosync_star and mino_tx_run. Owns the
 * setjmp / try-frame, retry loop, commit, and watch dispatch. The
 * body invoker is parameterised so the Clojure side dispatches to
 * mino_call(thunk) and the C side calls the host fn directly.
 *
 * The caller must guarantee no enclosing transaction is active --
 * that case (nested dosync / mino_tx_run) is handled at the public
 * entry by absorbing the inner into the outer's tx without touching
 * the setjmp frame. */
static mino_val *tx_outer_run(mino_state *S,
                                 tx_invoke_body_fn invoke,
                                 void *body_user, mino_env *env,
                                 const char *origin_label)
{
    /* Volatile-qualified so the setjmp-clobber analysis stays clean:
     * any local needed in both the success and longjmp arms must be
     * either declared volatile or reloaded after setjmp. */
    mino_thread_ctx_t * volatile ctx_v;
    mino_val        * volatile result_v = NULL;
    int                          saved_try;
    tx_state_t                   tx;
    try_frame_t                 *frame;

    ctx_v                 = mino_current_ctx(S);
    saved_try             = ctx_v->try_depth;
    tx.depth                = 1;
    tx.refs_head            = NULL;
    tx.retry_count          = 0;
    tx.try_depth_at_start   = saved_try;
    tx.in_commit            = 0;
    tx.validator_thrown_ex  = NULL;
    tx.pending_sends        = NULL;

    if (ctx_v->try_depth >= MAX_TRY_DEPTH) {
        return prim_throw_classified(S, "eval/state", "MST006",
            origin_label);
    }
    frame                 = &ctx_v->try_stack[ctx_v->try_depth];
    frame->exception      = NULL;
    frame->saved_ns       = S->ns_vars.current_ns;
    frame->saved_ambient  = S->ns_vars.fn_ambient_ns;
    frame->saved_load_len = S->module.load_stack_len;
    if (setjmp(frame->buf) != 0) {
        /* Body threw. Use the volatile ctx_v captured before setjmp so
         * we don't re-enter the inlined mino_current_ctx (whose locals
         * trigger -Wclobbered). */
        mino_thread_ctx_t *c  = ctx_v;
        mino_val        *ex = c->try_stack[saved_try].exception;
        tx_clear_ref_states(&tx);
        c->current_tx    = NULL;
        c->try_depth     = saved_try;
        S->ns_vars.current_ns    = c->try_stack[saved_try].saved_ns;
        S->ns_vars.fn_ambient_ns = c->try_stack[saved_try].saved_ambient;
        if (c->try_depth > 0) {
            c->try_stack[c->try_depth - 1].exception = ex;
            longjmp(c->try_stack[c->try_depth - 1].buf, 1);
        }
        if (ex != NULL && mino_type_of(ex) == MINO_STRING) {
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

    result_v = tx_run_loop(S, invoke, body_user, env, &tx);

    {
        mino_thread_ctx_t *c = ctx_v;
        /* Clear current_tx BEFORE watch dispatch so a watch fn that
         * itself calls dosync allocates fresh transaction state. The
         * try frame is still active so a thrown watch still cleans up
         * via the catch arm (refs_head freed there). */
        c->current_tx = NULL;
        if (result_v != NULL) {
            mino_val *pending = tx.pending_sends;
            tx.pending_sends = NULL;
            /* Drain agent pending sends BEFORE watch dispatch. A
             * thrown ref watch longjmps past everything that
             * follows, and we don't want a misbehaving watch fn to
             * silently swallow queued agent dispatches. JVM canon
             * doesn't strictly order the two notifications, but
             * agent dispatches FIRST means a body that completed
             * cleanly always reaches its agents. */
            mino_agent_drain_pending(S, pending, env);
            (void)dispatch_watches(S, &tx, env);
            /* If a watch threw, the longjmp landed at our setjmp
             * and cleanup ran there; this branch only sees clean
             * returns. */
        }
        tx_clear_ref_states(&tx);
        c->try_depth  = saved_try;
    }
    return result_v;
}

mino_val *prim_dosync_star(mino_state *S, mino_val *args,
                              mino_env *env)
{
    mino_val              *thunk;
    mino_thread_ctx_t       *ctx;
    struct invoke_clj_thunk  body_ctx;

    if (!mino_is_cons(args) || mino_is_cons(args->as.cons.cdr)) {
        return prim_throw_classified(S, "eval/arity", "MAR001",
            "dosync* requires one argument (a thunk)");
    }
    thunk = args->as.cons.car;
    if (thunk == NULL || (mino_type_of(thunk) != MINO_FN
                          && mino_type_of(thunk) != MINO_PRIM
                          && mino_type_of(thunk) != MINO_MACRO)) {
        return prim_throw_classified(S, "eval/type", "MTY001",
            "dosync*: expected a thunk");
    }
    ctx = mino_current_ctx(S);
    /* Nested dosync: bump depth, reuse the outer tx, run the thunk. */
    if (ctx->current_tx != NULL) {
        mino_val *r;
        ctx->current_tx->depth++;
        r = mino_call(S, thunk, mino_nil(S), env);
        if (ctx->current_tx != NULL) ctx->current_tx->depth--;
        return r;
    }
    body_ctx.thunk = thunk;
    return tx_outer_run(S, invoke_clj_thunk, &body_ctx, env,
                         "dosync*: try-stack overflow");
}

mino_val *mino_tx_run(mino_state *S, mino_tx_body_fn body, void *user,
                        mino_env *env)
{
    mino_thread_ctx_t    *ctx;
    struct invoke_c_body  body_ctx;

    if (body == NULL) {
        return prim_throw_classified(S, "eval/type", "MTY001",
            "mino_tx_run: body fn must not be NULL");
    }
    ctx = mino_current_ctx(S);
    /* Nested tx: absorb into the outer just like a Clojure dosync
     * inside another dosync. The body runs once, no retry frame. */
    if (ctx->current_tx != NULL) {
        mino_val *r;
        ctx->current_tx->depth++;
        r = body(S, user, env);
        if (ctx->current_tx != NULL) ctx->current_tx->depth--;
        return r;
    }
    body_ctx.fn   = body;
    body_ctx.user = user;
    return tx_outer_run(S, invoke_c_body, &body_ctx, env,
                         "mino_tx_run: try-stack overflow");
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
static void stm_lazy_init_commit_lock(mino_state *S)
{
    if (S->stm.lock_inited) return;
#if !(defined(_WIN32) && defined(_MSC_VER))
    if (pthread_mutex_init(&S->stm.commit_lock, NULL) != 0) {
        abort(); /* Class I: pthread_mutex_init failure means dosync cannot run safely */
    }
#endif
    S->stm.lock_inited = 1;
}

void mino_install_stm(mino_state *S, mino_env *env)
{
    mino_env *core_env = ns_env_ensure(S, "clojure.core");
    (void)env;
    stm_lazy_init_commit_lock(S);
    prim_install_table_with_capability(S, core_env, "clojure.core",
                                       k_prims_stm, k_prims_stm_count,
                                       "stm");
    S->caps_installed |= MINO_CAP_STM;
}
