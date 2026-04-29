/*
 * lazy.c -- C-level lazy sequence primitives: range, lazy-map-1,
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
/* Pull the head element and the rest seq from a coll that has already
 * been normalized to one of {CONS, CHUNKED_CONS}. Returns 1 on
 * success (sets *out_head and *out_rest), 0 on end-of-seq. */
static int seq_head_rest(mino_state_t *S, mino_val_t *coll,
                         mino_val_t **out_head, mino_val_t **out_rest)
{
    if (coll == NULL || coll->type == MINO_NIL) return 0;
    if (coll->type == MINO_CONS) {
        *out_head = coll->as.cons.car;
        *out_rest = coll->as.cons.cdr;
        return 1;
    }
    if (coll->type == MINO_CHUNKED_CONS) {
        const mino_val_t *ch = coll->as.chunked_cons.chunk;
        *out_head = ch->as.chunk.vals[coll->as.chunked_cons.off];
        *out_rest = mino_chunked_cons_advance(S, coll);
        if (*out_rest == NULL) return 0;
        return 1;
    }
    return 0;
}

/* Force any leading lazy seqs and coerce the result to a CONS-shaped
 * or CHUNKED_CONS-shaped seq. Returns nil/NULL when the seq is empty. */
static mino_val_t *normalize_seq(mino_state_t *S, mino_val_t *coll)
{
    if (coll != NULL && coll->type == MINO_LAZY) {
        coll = lazy_force(S, coll);
        if (coll == NULL) return NULL;
    }
    if (coll == NULL || coll->type == MINO_NIL
        || coll->type == MINO_EMPTY_LIST) {
        return mino_nil(S);
    }
    if (coll->type == MINO_CONS || coll->type == MINO_CHUNKED_CONS) {
        return coll;
    }
    /* Fall back to prim_seq for vector/map/set/string/etc. */
    coll = prim_seq(S, mino_cons(S, coll, mino_nil(S)), NULL);
    if (coll == NULL) return NULL;
    if (coll->type == MINO_NIL) return mino_nil(S);
    if (coll->type == MINO_CONS || coll->type == MINO_CHUNKED_CONS) {
        return coll;
    }
    return mino_nil(S);
}

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
    coll = normalize_seq(S, coll);
    if (coll == NULL) return NULL;
    if (coll->type == MINO_NIL) return mino_nil(S);
    if (coll->type == MINO_CHUNKED_CONS) {
        /* Chunked fast path: pull the whole head chunk in one go,
         * apply f to each slot into a fresh chunk, and emit a
         * chunk-cons whose tail wraps the next chunk-rest in a fresh
         * lazy of this same thunk. Preserves chunkedness through the
         * pipeline so a downstream filter/keep/etc. can also pull
         * chunks at a time. */
        const mino_val_t *src = coll->as.chunked_cons.chunk;
        unsigned          off = coll->as.chunked_cons.off;
        unsigned          n   = src->as.chunk.len - off;
        mino_val_t       *buf;
        mino_val_t       *more;
        unsigned          k;
        buf = mino_chunk_buffer(S, n);
        if (buf == NULL) return NULL;
        gc_pin(buf);
        for (k = 0; k < n; k++) {
            mino_val_t *elem = src->as.chunk.vals[off + k];
            mino_val_t *m;
            call_args = mino_cons(S, elem, mino_nil(S));
            gc_pin(call_args);
            m = apply_callable(S, fn, call_args, NULL);
            gc_unpin(1);
            if (m == NULL) { gc_unpin(1); return NULL; }
            gc_write_barrier(S, buf, NULL, m);
            buf->as.chunk.vals[buf->as.chunk.len++] = m;
        }
        mino_chunk_seal(buf);
        more = coll->as.chunked_cons.more;
        if (more != NULL && more->type != MINO_NIL
            && more->type != MINO_EMPTY_LIST) {
            next_ctx = mino_cons(S, fn, mino_cons(S, more, mino_nil(S)));
            next_lz = alloc_val(S, MINO_LAZY);
            next_lz->as.lazy.body    = next_ctx;
            next_lz->as.lazy.c_thunk = lazy_map1_thunk;
        } else {
            next_lz = mino_nil(S);
        }
        gc_unpin(1);
        return mino_chunked_cons(S, buf, next_lz);
    }
    if (!seq_head_rest(S, coll, &head, &rest)) return mino_nil(S);
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
 * public `map` in core.clj dispatches to it for the 1-collection case
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
    if (coll == NULL || coll->type == MINO_NIL
        || coll->type == MINO_EMPTY_LIST) {
        return mino_empty_list(S);
    }
    /* Normalize initial coll to cons-or-lazy so the thunk only needs to
     * handle those cases going forward. */
    if (coll->type != MINO_CONS && coll->type != MINO_LAZY
        && coll->type != MINO_CHUNKED_CONS) {
        coll = prim_seq(S, mino_cons(S, coll, mino_nil(S)), NULL);
        if (coll == NULL) return NULL;
        if (coll->type == MINO_NIL) return mino_empty_list(S);
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
        coll = normalize_seq(S, coll);
        if (coll == NULL) return NULL;
        if (coll->type == MINO_NIL) return mino_nil(S);
        if (coll->type == MINO_CHUNKED_CONS) {
            /* Chunked path: scan the head chunk into a fresh chunk
             * holding only the elements where pred is truthy, then
             * emit chunk-cons plus a lazy continuation over the
             * chunk-rest. The fresh buffer can be smaller than the
             * source chunk; we size it pessimistically at the source
             * chunk's remaining length. */
            const mino_val_t *src = coll->as.chunked_cons.chunk;
            unsigned          off = coll->as.chunked_cons.off;
            unsigned          n   = src->as.chunk.len - off;
            mino_val_t       *buf;
            mino_val_t       *more;
            mino_val_t       *next_ctx;
            mino_val_t       *next_lz;
            unsigned          k;
            buf = mino_chunk_buffer(S, n);
            if (buf == NULL) return NULL;
            gc_pin(buf);
            for (k = 0; k < n; k++) {
                mino_val_t *elem = src->as.chunk.vals[off + k];
                mino_val_t *r;
                call_args = mino_cons(S, elem, mino_nil(S));
                gc_pin(call_args);
                r = apply_callable(S, pred, call_args, NULL);
                gc_unpin(1);
                if (r == NULL) { gc_unpin(1); return NULL; }
                if (mino_is_truthy(r)) {
                    gc_write_barrier(S, buf, NULL, elem);
                    buf->as.chunk.vals[buf->as.chunk.len++] = elem;
                }
            }
            mino_chunk_seal(buf);
            more = coll->as.chunked_cons.more;
            if (more != NULL && more->type != MINO_NIL
                && more->type != MINO_EMPTY_LIST) {
                next_ctx = mino_cons(S, pred,
                            mino_cons(S, more, mino_nil(S)));
                next_lz = alloc_val(S, MINO_LAZY);
                next_lz->as.lazy.body    = next_ctx;
                next_lz->as.lazy.c_thunk = lazy_filter_thunk;
            } else {
                next_lz = mino_nil(S);
            }
            gc_unpin(1);
            if (buf->as.chunk.len == 0) {
                /* No elements survived in this chunk — keep walking
                 * the next one without emitting an empty chunk-cons
                 * cell (chunk-cons returns more directly when the
                 * chunk is empty, but a downstream `chunked-seq?`
                 * test would see no chunk at the head). Instead,
                 * recur into the lazy continuation to find the next
                 * non-empty chunk. */
                if (next_lz != NULL && next_lz->type == MINO_LAZY) {
                    return lazy_force(S, next_lz);
                }
                return mino_nil(S);
            }
            return mino_chunked_cons(S, buf, next_lz);
        }
        if (!seq_head_rest(S, coll, &head, &rest)) return mino_nil(S);
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
    if (coll == NULL || coll->type == MINO_NIL
        || coll->type == MINO_EMPTY_LIST) {
        return mino_empty_list(S);
    }
    if (coll->type != MINO_CONS && coll->type != MINO_LAZY
        && coll->type != MINO_CHUNKED_CONS) {
        coll = prim_seq(S, mino_cons(S, coll, mino_nil(S)), NULL);
        if (coll == NULL) return NULL;
        if (coll->type == MINO_NIL) return mino_empty_list(S);
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
    coll = normalize_seq(S, coll);
    if (coll == NULL) return NULL;
    if (coll->type == MINO_NIL) return mino_nil(S);
    if (!seq_head_rest(S, coll, &head, &rest)) return mino_nil(S);
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
    if (n <= 0 || coll == NULL || coll->type == MINO_NIL
        || coll->type == MINO_EMPTY_LIST) {
        return mino_empty_list(S);
    }
    if (coll->type != MINO_CONS && coll->type != MINO_LAZY
        && coll->type != MINO_CHUNKED_CONS) {
        coll = prim_seq(S, mino_cons(S, coll, mino_nil(S)), NULL);
        if (coll == NULL) return NULL;
        if (coll->type == MINO_NIL) return mino_empty_list(S);
    }
    ctx = mino_cons(S, mino_int(S, n), mino_cons(S, coll, mino_nil(S)));
    lz = alloc_val(S, MINO_LAZY);
    lz->as.lazy.body    = ctx;
    lz->as.lazy.c_thunk = lazy_take_thunk;
    return lz;
}

/* (drop-seq n coll) -- eagerly walk past n items, returning the tail
 * seq. Mirrors Clojure's eager-drop; the public `drop` dispatches here
 * for the 2-arg form and keeps the transducer path in core.clj. */
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
    if (n <= 0) {
        if (coll == NULL || coll->type == MINO_NIL) return mino_empty_list(S);
        return coll;
    }
    while (n > 0) {
        if (coll != NULL && coll->type == MINO_LAZY) {
            coll = lazy_force(S, coll);
            if (coll == NULL) return NULL;
        }
        if (coll == NULL || coll->type == MINO_NIL
            || coll->type == MINO_EMPTY_LIST) {
            return mino_empty_list(S);
        }
        if (coll->type == MINO_CHUNKED_CONS) {
            coll = mino_chunked_cons_advance(S, coll);
            if (coll == NULL) return mino_empty_list(S);
            n--;
            continue;
        }
        if (coll->type != MINO_CONS) {
            coll = prim_seq(S, mino_cons(S, coll, mino_nil(S)), NULL);
            if (coll == NULL) return NULL;
            if (coll->type == MINO_NIL) return mino_empty_list(S);
            if (coll->type == MINO_CHUNKED_CONS) continue;
            if (coll->type != MINO_CONS) return mino_empty_list(S);
        }
        coll = coll->as.cons.cdr;
        n--;
    }
    if (coll == NULL || coll->type == MINO_NIL) return mino_empty_list(S);
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
    {
        mino_val_t *r = range_make_lazy(S, start, end, step, infinite);
        if (r == NULL) return NULL;
        if (r->type == MINO_NIL) return mino_empty_list(S);
        return r;
    }
}

/* ------------------------------------------------------------------------- */
/* Chunked-seq family                                                        */
/* ------------------------------------------------------------------------- */

mino_val_t *prim_chunk_buffer(mino_state_t *S, mino_val_t *args,
                              mino_env_t *env)
{
    long long  cap;
    size_t     n;
    mino_val_t *buf;
    (void)env;
    arg_count(S, args, &n);
    if (n != 1) {
        return prim_throw_classified(S, "eval/arity", "MAR001",
            "chunk-buffer requires 1 argument");
    }
    if (!mino_to_int(args->as.cons.car, &cap)) {
        return prim_throw_classified(S, "eval/type", "MTY001",
            "chunk-buffer: capacity must be an integer");
    }
    if (cap <= 0 || cap > 65535) {
        return prim_throw_classified(S, "eval/bounds", "MBD001",
            "chunk-buffer: capacity out of range (1..65535)");
    }
    buf = mino_chunk_buffer(S, (unsigned)cap);
    if (buf == NULL) {
        return prim_throw_classified(S, "internal", "MIN001",
            "chunk-buffer: allocation failed");
    }
    return buf;
}

mino_val_t *prim_chunk_append(mino_state_t *S, mino_val_t *args,
                              mino_env_t *env)
{
    mino_val_t *buf, *elem;
    size_t      n;
    (void)env;
    arg_count(S, args, &n);
    if (n != 2) {
        return prim_throw_classified(S, "eval/arity", "MAR001",
            "chunk-append requires 2 arguments");
    }
    buf  = args->as.cons.car;
    elem = args->as.cons.cdr->as.cons.car;
    if (buf == NULL || buf->type != MINO_CHUNK) {
        return prim_throw_classified(S, "eval/type", "MTY001",
            "chunk-append: first argument must be a chunk-buffer");
    }
    if (buf->as.chunk.sealed) {
        return prim_throw_classified(S, "eval/contract", "MCO001",
            "chunk-append: buffer is sealed");
    }
    if (buf->as.chunk.len >= buf->as.chunk.cap) {
        return prim_throw_classified(S, "eval/bounds", "MBD001",
            "chunk-append: buffer is full");
    }
    /* The append is a slot store inside an already-allocated chunk
     * cell. Old slot value is NULL (chunk-buffer zeroed it), so the
     * write barrier sees a transition from NULL to elem; that's the
     * correct shape for the SATB+Dijkstra pair. */
    gc_write_barrier(S, buf, NULL, elem);
    buf->as.chunk.vals[buf->as.chunk.len++] = elem;
    return buf;
}

mino_val_t *prim_chunk(mino_state_t *S, mino_val_t *args, mino_env_t *env)
{
    mino_val_t *buf;
    size_t      n;
    (void)env;
    arg_count(S, args, &n);
    if (n != 1) {
        return prim_throw_classified(S, "eval/arity", "MAR001",
            "chunk requires 1 argument");
    }
    buf = args->as.cons.car;
    if (buf == NULL || buf->type != MINO_CHUNK) {
        return prim_throw_classified(S, "eval/type", "MTY001",
            "chunk: argument must be a chunk-buffer");
    }
    return mino_chunk_seal(buf);
}

mino_val_t *prim_chunk_cons(mino_state_t *S, mino_val_t *args,
                            mino_env_t *env)
{
    mino_val_t *chunk, *more;
    size_t      n;
    (void)env;
    arg_count(S, args, &n);
    if (n != 2) {
        return prim_throw_classified(S, "eval/arity", "MAR001",
            "chunk-cons requires 2 arguments");
    }
    chunk = args->as.cons.car;
    more  = args->as.cons.cdr->as.cons.car;
    if (chunk == NULL || chunk->type != MINO_CHUNK) {
        return prim_throw_classified(S, "eval/type", "MTY001",
            "chunk-cons: first argument must be a chunk");
    }
    if (chunk->as.chunk.len == 0) {
        if (more == NULL || more->type == MINO_NIL) return mino_empty_list(S);
        return more;
    }
    return mino_chunked_cons(S, chunk, more);
}

mino_val_t *prim_chunk_first(mino_state_t *S, mino_val_t *args,
                             mino_env_t *env)
{
    mino_val_t *cs;
    size_t      n;
    (void)env;
    arg_count(S, args, &n);
    if (n != 1) {
        return prim_throw_classified(S, "eval/arity", "MAR001",
            "chunk-first requires 1 argument");
    }
    cs = args->as.cons.car;
    if (cs == NULL || cs->type != MINO_CHUNKED_CONS) {
        return prim_throw_classified(S, "eval/type", "MTY001",
            "chunk-first: argument must be a chunked-seq");
    }
    /* Return the chunk as-is; consumers use count/nth. The off-truncated
     * sub-view costs an extra alloc that callers rarely need (chunk-first
     * is normally followed by a fresh chunk-buffer of `count` slots). */
    return cs->as.chunked_cons.chunk;
}

mino_val_t *prim_chunk_rest(mino_state_t *S, mino_val_t *args,
                            mino_env_t *env)
{
    mino_val_t *cs;
    mino_val_t *more;
    size_t      n;
    (void)env;
    arg_count(S, args, &n);
    if (n != 1) {
        return prim_throw_classified(S, "eval/arity", "MAR001",
            "chunk-rest requires 1 argument");
    }
    cs = args->as.cons.car;
    if (cs == NULL || cs->type != MINO_CHUNKED_CONS) {
        return prim_throw_classified(S, "eval/type", "MTY001",
            "chunk-rest: argument must be a chunked-seq");
    }
    more = cs->as.chunked_cons.more;
    if (more == NULL || more->type == MINO_NIL) return mino_empty_list(S);
    return more;
}

mino_val_t *prim_chunk_next(mino_state_t *S, mino_val_t *args,
                            mino_env_t *env)
{
    mino_val_t *cs;
    mino_val_t *more;
    size_t      n;
    (void)env;
    arg_count(S, args, &n);
    if (n != 1) {
        return prim_throw_classified(S, "eval/arity", "MAR001",
            "chunk-next requires 1 argument");
    }
    cs = args->as.cons.car;
    if (cs == NULL || cs->type != MINO_CHUNKED_CONS) {
        return prim_throw_classified(S, "eval/type", "MTY001",
            "chunk-next: argument must be a chunked-seq");
    }
    more = cs->as.chunked_cons.more;
    if (more != NULL && more->type == MINO_LAZY) {
        more = lazy_force(S, more);
        if (more == NULL) return NULL;
    }
    if (more == NULL || more->type == MINO_NIL
        || more->type == MINO_EMPTY_LIST) {
        return mino_nil(S);
    }
    return more;
}

mino_val_t *prim_chunked_seq_p(mino_state_t *S, mino_val_t *args,
                               mino_env_t *env)
{
    size_t n;
    (void)env;
    arg_count(S, args, &n);
    if (n != 1) {
        return prim_throw_classified(S, "eval/arity", "MAR001",
            "chunked-seq? requires 1 argument");
    }
    return (args->as.cons.car != NULL
            && args->as.cons.car->type == MINO_CHUNKED_CONS)
        ? mino_true(S) : mino_false(S);
}

const mino_prim_def k_prims_lazy[] = {
    {"range",       prim_range,
     "Returns a lazy sequence of nums from start (inclusive) to end (exclusive), by step. With no args, returns an infinite sequence from 0."},
    {"lazy-map-1",  prim_lazy_map_1,
     "Internal fast path for single-collection lazy map."},
    {"lazy-filter", prim_lazy_filter,
     "Internal fast path for lazy filter."},
    {"lazy-take",   prim_lazy_take,
     "Internal fast path for lazy take."},
    {"drop-seq",    prim_drop_seq,
     "Internal fast path for eager drop."},
    {"chunk-buffer", prim_chunk_buffer,
     "Returns a fresh chunk-buffer of the given capacity. Append values with chunk-append, then seal with chunk."},
    {"chunk-append", prim_chunk_append,
     "Appends elem to chunk-buffer buf and returns buf. Throws if buf is full or already sealed."},
    {"chunk", prim_chunk,
     "Seals chunk-buffer buf so no further appends are accepted, and returns the chunk."},
    {"chunk-cons", prim_chunk_cons,
     "Returns a chunked seq prepending the given chunk to the seq more."},
    {"chunk-first", prim_chunk_first,
     "Returns the chunk at the head of a chunked seq."},
    {"chunk-rest", prim_chunk_rest,
     "Returns the rest of a chunked seq after the head chunk, or () if none."},
    {"chunk-next", prim_chunk_next,
     "Returns the rest of a chunked seq as a seq, or nil if empty."},
    {"chunked-seq?", prim_chunked_seq_p,
     "Returns true if x is a chunked seq."},
};

const size_t k_prims_lazy_count =
    sizeof(k_prims_lazy) / sizeof(k_prims_lazy[0]);
