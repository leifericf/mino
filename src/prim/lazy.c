/*
 * prim_lazy.c -- C-level lazy sequence primitives: range, lazy-map-1,
 *                lazy-filter. Each keeps a cons-shaped context in the
 *                lazy's body field and walks it via a static c_thunk,
 *                avoiding the per-element fn-call frame that a pure
 *                mino implementation pays.
 */

#include "prim/internal.h"

/* Lazy c_thunk for (map f coll): ctx = cons(f, coll_state). Each force
 * walks one element of coll, applies f, and wires the rest as another
 * lazy whose thunk is this same function. Saves one mino-level fn-call
 * frame per element compared to the pure-core implementation. */
static mino_val_t *lazy_map1_thunk(mino_state_t *S, mino_val_t *ctx)
{
    mino_val_t *fn   = ctx->as.cons.car;
    mino_val_t *coll = ctx->as.cons.cdr->as.cons.car;
    mino_val_t *head;
    mino_val_t *rest;
    mino_val_t *mapped;
    mino_val_t *call_args;
    mino_val_t *next_ctx;
    mino_val_t *next_lz;
    if (coll != NULL && coll->type == MINO_LAZY) {
        coll = lazy_force(S, coll);
        if (coll == NULL) return NULL;
    }
    if (coll == NULL || coll->type == MINO_NIL) {
        return mino_nil(S);
    }
    if (coll->type != MINO_CONS) {
        /* Coerce non-cons collections (vector, map, set, string) into a
         * cons seq; the initial prim call already did this for the first
         * element, but seq'ed lazy rests that fall through here need the
         * same treatment. */
        coll = prim_seq(S, mino_cons(S, coll, mino_nil(S)), NULL);
        if (coll == NULL || coll->type == MINO_NIL) return mino_nil(S);
        if (coll->type != MINO_CONS) return mino_nil(S);
    }
    head = coll->as.cons.car;
    rest = coll->as.cons.cdr;
    call_args = mino_cons(S, head, mino_nil(S));
    gc_pin(call_args);
    mapped = apply_callable(S, fn, call_args, NULL);
    gc_unpin(1);
    if (mapped == NULL) return NULL;
    gc_pin(mapped);
    next_ctx = mino_cons(S, fn, mino_cons(S, rest, mino_nil(S)));
    next_lz = alloc_val(S, MINO_LAZY);
    next_lz->as.lazy.body    = next_ctx;
    next_lz->as.lazy.c_thunk = lazy_map1_thunk;
    gc_unpin(1);
    return mino_cons(S, mapped, next_lz);
}

/* (lazy-map-1 f coll) -- lazy map for a single collection. Named with a
 * leading "lazy-" prefix to signal it is the single-coll fast path; the
 * public `map` in core.mino dispatches to it for the 1-collection case
 * and keeps the multi-coll implementation for the uncommon wide form. */
mino_val_t *prim_lazy_map_1(mino_state_t *S, mino_val_t *args, mino_env_t *env)
{
    mino_val_t *fn;
    mino_val_t *coll;
    mino_val_t *ctx;
    mino_val_t *lz;
    size_t n;
    (void)env;
    arg_count(S, args, &n);
    if (n != 2) {
        return prim_throw_classified(S, "eval/arity", "MAR001",
            "lazy-map-1 requires 2 arguments");
    }
    fn   = args->as.cons.car;
    coll = args->as.cons.cdr->as.cons.car;
    if (coll == NULL || coll->type == MINO_NIL) {
        return mino_nil(S);
    }
    /* Normalize initial coll to cons-or-lazy so the thunk only needs to
     * handle those cases going forward. */
    if (coll->type != MINO_CONS && coll->type != MINO_LAZY) {
        coll = prim_seq(S, mino_cons(S, coll, mino_nil(S)), NULL);
        if (coll == NULL) return NULL;
        if (coll->type == MINO_NIL) return mino_nil(S);
    }
    ctx = mino_cons(S, fn, mino_cons(S, coll, mino_nil(S)));
    lz = alloc_val(S, MINO_LAZY);
    lz->as.lazy.body    = ctx;
    lz->as.lazy.c_thunk = lazy_map1_thunk;
    return lz;
}

/* Lazy c_thunk for (filter pred coll): ctx = cons(pred, coll_state). On
 * force, advances through coll until pred(elem) is truthy; returns
 * cons(elem, lazy-filter(pred, rest)), or nil at end. */
static mino_val_t *lazy_filter_thunk(mino_state_t *S, mino_val_t *ctx)
{
    mino_val_t *pred = ctx->as.cons.car;
    mino_val_t *coll = ctx->as.cons.cdr->as.cons.car;
    for (;;) {
        mino_val_t *head;
        mino_val_t *rest;
        mino_val_t *call_args;
        mino_val_t *ok;
        if (coll != NULL && coll->type == MINO_LAZY) {
            coll = lazy_force(S, coll);
            if (coll == NULL) return NULL;
        }
        if (coll == NULL || coll->type == MINO_NIL) {
            return mino_nil(S);
        }
        if (coll->type != MINO_CONS) {
            coll = prim_seq(S, mino_cons(S, coll, mino_nil(S)), NULL);
            if (coll == NULL || coll->type == MINO_NIL) return mino_nil(S);
            if (coll->type != MINO_CONS) return mino_nil(S);
        }
        head = coll->as.cons.car;
        rest = coll->as.cons.cdr;
        call_args = mino_cons(S, head, mino_nil(S));
        gc_pin(call_args);
        ok = apply_callable(S, pred, call_args, NULL);
        gc_unpin(1);
        if (ok == NULL) return NULL;
        if (mino_is_truthy(ok)) {
            mino_val_t *next_ctx;
            mino_val_t *next_lz;
            gc_pin(head);
            next_ctx = mino_cons(S, pred,
                        mino_cons(S, rest, mino_nil(S)));
            next_lz = alloc_val(S, MINO_LAZY);
            next_lz->as.lazy.body    = next_ctx;
            next_lz->as.lazy.c_thunk = lazy_filter_thunk;
            gc_unpin(1);
            return mino_cons(S, head, next_lz);
        }
        coll = rest;
    }
}

/* (lazy-filter pred coll) -- lazy filter. Pairs with lazy-map-1; see
 * its comment for dispatch rationale. */
mino_val_t *prim_lazy_filter(mino_state_t *S, mino_val_t *args, mino_env_t *env)
{
    mino_val_t *pred;
    mino_val_t *coll;
    mino_val_t *ctx;
    mino_val_t *lz;
    size_t n;
    (void)env;
    arg_count(S, args, &n);
    if (n != 2) {
        return prim_throw_classified(S, "eval/arity", "MAR001",
            "lazy-filter requires 2 arguments");
    }
    pred = args->as.cons.car;
    coll = args->as.cons.cdr->as.cons.car;
    if (coll == NULL || coll->type == MINO_NIL) {
        return mino_nil(S);
    }
    if (coll->type != MINO_CONS && coll->type != MINO_LAZY) {
        coll = prim_seq(S, mino_cons(S, coll, mino_nil(S)), NULL);
        if (coll == NULL) return NULL;
        if (coll->type == MINO_NIL) return mino_nil(S);
    }
    ctx = mino_cons(S, pred, mino_cons(S, coll, mino_nil(S)));
    lz = alloc_val(S, MINO_LAZY);
    lz->as.lazy.body    = ctx;
    lz->as.lazy.c_thunk = lazy_filter_thunk;
    return lz;
}

/* Lazy range via C thunks, so each step skips the fn-call + lazy-seq body
 * overhead that a mino-level implementation pays per element.
 *
 * The context is a cons(start, cons(end, step)) triple of interned ints; a
 * vector would work too but this keeps alloc count low for the hot path.
 */
static mino_val_t *range_thunk(mino_state_t *S, mino_val_t *ctx);

static mino_val_t *range_make_lazy(mino_state_t *S, long long start,
                                   long long end, long long step,
                                   int infinite)
{
    mino_val_t *lz;
    mino_val_t *ctx;
    if (!infinite) {
        if (step > 0 ? start >= end : start <= end) {
            return mino_nil(S);
        }
    }
    ctx = mino_cons(S, mino_int(S, start),
            mino_cons(S, mino_int(S, end),
                mino_cons(S, mino_int(S, step),
                    mino_cons(S, infinite ? mino_true(S) : mino_false(S),
                              mino_nil(S)))));
    lz = alloc_val(S, MINO_LAZY);
    lz->as.lazy.body    = ctx;
    lz->as.lazy.c_thunk = range_thunk;
    return lz;
}

static mino_val_t *range_thunk(mino_state_t *S, mino_val_t *ctx)
{
    long long start = ctx->as.cons.car->as.i;
    long long end   = ctx->as.cons.cdr->as.cons.car->as.i;
    long long step  = ctx->as.cons.cdr->as.cons.cdr->as.cons.car->as.i;
    int infinite    = ctx->as.cons.cdr->as.cons.cdr->as.cons.cdr->as.cons.car
                          == mino_true(S);
    mino_val_t *head = ctx->as.cons.car;
    return mino_cons(S, head,
        range_make_lazy(S, start + step, end, step, infinite));
}

/* Lazy take: ctx = cons(n, coll_state). Each force decrements n and
 * yields (first coll) until n reaches zero or coll is exhausted. */
static mino_val_t *lazy_take_thunk(mino_state_t *S, mino_val_t *ctx)
{
    long long n   = ctx->as.cons.car->as.i;
    mino_val_t *coll = ctx->as.cons.cdr->as.cons.car;
    mino_val_t *head;
    mino_val_t *rest;
    mino_val_t *next_ctx;
    mino_val_t *next_lz;
    if (n <= 0) return mino_nil(S);
    if (coll != NULL && coll->type == MINO_LAZY) {
        coll = lazy_force(S, coll);
        if (coll == NULL) return NULL;
    }
    if (coll == NULL || coll->type == MINO_NIL) {
        return mino_nil(S);
    }
    if (coll->type != MINO_CONS) {
        coll = prim_seq(S, mino_cons(S, coll, mino_nil(S)), NULL);
        if (coll == NULL || coll->type == MINO_NIL) return mino_nil(S);
        if (coll->type != MINO_CONS) return mino_nil(S);
    }
    head = coll->as.cons.car;
    rest = coll->as.cons.cdr;
    if (n == 1) {
        return mino_cons(S, head, mino_nil(S));
    }
    next_ctx = mino_cons(S, mino_int(S, n - 1),
                mino_cons(S, rest, mino_nil(S)));
    next_lz = alloc_val(S, MINO_LAZY);
    next_lz->as.lazy.body    = next_ctx;
    next_lz->as.lazy.c_thunk = lazy_take_thunk;
    return mino_cons(S, head, next_lz);
}

/* (lazy-take n coll) -- lazy take for n items; see lazy-map-1 comment. */
mino_val_t *prim_lazy_take(mino_state_t *S, mino_val_t *args, mino_env_t *env)
{
    long long n;
    mino_val_t *coll;
    mino_val_t *ctx;
    mino_val_t *lz;
    size_t na;
    (void)env;
    arg_count(S, args, &na);
    if (na != 2) {
        return prim_throw_classified(S, "eval/arity", "MAR001",
            "lazy-take requires 2 arguments");
    }
    if (!mino_to_int(args->as.cons.car, &n)) {
        return prim_throw_classified(S, "eval/type", "MTY001",
            "lazy-take: n must be an integer");
    }
    coll = args->as.cons.cdr->as.cons.car;
    if (n <= 0 || coll == NULL || coll->type == MINO_NIL) {
        return mino_nil(S);
    }
    if (coll->type != MINO_CONS && coll->type != MINO_LAZY) {
        coll = prim_seq(S, mino_cons(S, coll, mino_nil(S)), NULL);
        if (coll == NULL) return NULL;
        if (coll->type == MINO_NIL) return mino_nil(S);
    }
    ctx = mino_cons(S, mino_int(S, n), mino_cons(S, coll, mino_nil(S)));
    lz = alloc_val(S, MINO_LAZY);
    lz->as.lazy.body    = ctx;
    lz->as.lazy.c_thunk = lazy_take_thunk;
    return lz;
}

/* (drop-seq n coll) -- eagerly walk past n items, returning the tail
 * seq. Mirrors Clojure's eager-drop; the public `drop` dispatches here
 * for the 2-arg form and keeps the transducer path in core.mino. */
mino_val_t *prim_drop_seq(mino_state_t *S, mino_val_t *args, mino_env_t *env)
{
    long long n;
    mino_val_t *coll;
    size_t na;
    (void)env;
    arg_count(S, args, &na);
    if (na != 2) {
        return prim_throw_classified(S, "eval/arity", "MAR001",
            "drop-seq requires 2 arguments");
    }
    if (!mino_to_int(args->as.cons.car, &n)) {
        return prim_throw_classified(S, "eval/type", "MTY001",
            "drop-seq: n must be an integer");
    }
    coll = args->as.cons.cdr->as.cons.car;
    if (n <= 0) return coll;
    while (n > 0) {
        if (coll != NULL && coll->type == MINO_LAZY) {
            coll = lazy_force(S, coll);
            if (coll == NULL) return NULL;
        }
        if (coll == NULL || coll->type == MINO_NIL) return mino_nil(S);
        if (coll->type != MINO_CONS) {
            coll = prim_seq(S, mino_cons(S, coll, mino_nil(S)), NULL);
            if (coll == NULL) return NULL;
            if (coll->type == MINO_NIL) return mino_nil(S);
            if (coll->type != MINO_CONS) return mino_nil(S);
        }
        coll = coll->as.cons.cdr;
        n--;
    }
    return coll;
}

/* (range), (range end), (range start end), (range start end step). */
mino_val_t *prim_range(mino_state_t *S, mino_val_t *args, mino_env_t *env)
{
    long long start = 0, end = 0, step = 1;
    size_t n;
    int infinite = 0;
    (void)env;
    arg_count(S, args, &n);
    if (n == 0) {
        infinite = 1;
    } else if (n == 1) {
        if (!mino_to_int(args->as.cons.car, &end)) {
            return prim_throw_classified(S, "eval/type", "MTY001",
                "range argument must be an integer");
        }
    } else if (n == 2) {
        if (!mino_to_int(args->as.cons.car, &start) ||
            !mino_to_int(args->as.cons.cdr->as.cons.car, &end)) {
            return prim_throw_classified(S, "eval/type", "MTY001",
                "range arguments must be integers");
        }
    } else if (n == 3) {
        if (!mino_to_int(args->as.cons.car, &start) ||
            !mino_to_int(args->as.cons.cdr->as.cons.car, &end) ||
            !mino_to_int(args->as.cons.cdr->as.cons.cdr->as.cons.car, &step)) {
            return prim_throw_classified(S, "eval/type", "MTY001",
                "range arguments must be integers");
        }
        if (step == 0) {
            return prim_throw_classified(S, "eval/bounds", "MBD001",
                "range step must not be zero");
        }
    } else {
        return prim_throw_classified(S, "eval/arity", "MAR001",
            "range takes 0, 1, 2, or 3 arguments");
    }
    return range_make_lazy(S, start, end, step, infinite);
}
