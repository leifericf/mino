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
static int seq_head_rest(mino_state *S, mino_val *coll,
                         mino_val **out_head, mino_val **out_rest)
{
    if (coll == NULL || mino_type_of(coll) == MINO_NIL) return 0;
    if (mino_type_of(coll) == MINO_CONS) {
        *out_head = coll->as.cons.car;
        *out_rest = coll->as.cons.cdr;
        return 1;
    }
    if (mino_type_of(coll) == MINO_CHUNKED_CONS) {
        const mino_val *ch = coll->as.chunked_cons.chunk;
        *out_head = ch->as.chunk.vals[coll->as.chunked_cons.off];
        *out_rest = mino_chunked_cons_advance(S, coll);
        if (*out_rest == NULL) return 0;
        return 1;
    }
    return 0;
}

/* Force any leading lazy seqs and coerce the result to a CONS-shaped
 * or CHUNKED_CONS-shaped seq. Returns nil/NULL when the seq is empty. */
static mino_val *normalize_seq(mino_state *S, mino_val *coll)
{
    if (coll != NULL && mino_type_of(coll) == MINO_LAZY) {
        coll = lazy_force(S, coll);
        if (coll == NULL) return NULL;
    }
    if (coll == NULL || mino_type_of(coll) == MINO_NIL
        || mino_type_of(coll) == MINO_EMPTY_LIST) {
        return mino_nil(S);
    }
    if (mino_type_of(coll) == MINO_CONS || mino_type_of(coll) == MINO_CHUNKED_CONS) {
        return coll;
    }
    /* Fall back to prim_seq for vector/map/set/string/etc. */
    coll = prim_seq(S, mino_cons(S, coll, mino_nil(S)), NULL);
    if (coll == NULL) return NULL;
    if (mino_type_of(coll) == MINO_NIL) return mino_nil(S);
    if (mino_type_of(coll) == MINO_CONS || mino_type_of(coll) == MINO_CHUNKED_CONS) {
        return coll;
    }
    return mino_nil(S);
}

static mino_val *lazy_map1_thunk(mino_state *S, mino_val *ctx)
{
    mino_val *fn   = ctx->as.cons.car;
    mino_val *coll = ctx->as.cons.cdr->as.cons.car;
    mino_val *head;
    mino_val *rest;
    mino_val *mapped;
    mino_val *call_args;
    mino_val *next_ctx;
    mino_val *next_lz;
    coll = normalize_seq(S, coll);
    if (coll == NULL) return NULL;
    if (mino_type_of(coll) == MINO_NIL) return mino_nil(S);
    if (mino_type_of(coll) == MINO_CHUNKED_CONS) {
        /* Chunked fast path: pull the whole head chunk in one go,
         * apply f to each slot into a fresh chunk, and emit a
         * chunk-cons whose tail wraps the next chunk-rest in a fresh
         * lazy of this same thunk. Preserves chunkedness through the
         * pipeline so a downstream filter/keep/etc. can also pull
         * chunks at a time. */
        const mino_val *src = coll->as.chunked_cons.chunk;
        unsigned          off = coll->as.chunked_cons.off;
        unsigned          n   = src->as.chunk.len - off;
        mino_val       *buf;
        mino_val       *more;
        unsigned          k;
        buf = mino_chunk_buffer(S, n);
        if (buf == NULL) return NULL;
        gc_pin(buf);
        for (k = 0; k < n; k++) {
            mino_val *elem = src->as.chunk.vals[off + k];
            mino_val *m;
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
        if (more != NULL && mino_type_of(more) != MINO_NIL
            && mino_type_of(more) != MINO_EMPTY_LIST) {
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
mino_val *prim_lazy_map_1(mino_state *S, mino_val *args, mino_env *env)
{
    mino_val *fn;
    mino_val *coll;
    mino_val *ctx;
    mino_val *lz;
    size_t n;
    (void)env;
    arg_count(S, args, &n);
    if (n != 2) {
        return prim_throw_classified(S, "eval/arity", "MAR001",
            "lazy-map-1 requires 2 arguments");
    }
    fn   = args->as.cons.car;
    coll = args->as.cons.cdr->as.cons.car;
    if (coll == NULL || mino_type_of(coll) == MINO_NIL
        || mino_type_of(coll) == MINO_EMPTY_LIST) {
        return mino_empty_list(S);
    }
    /* Normalize initial coll to cons-or-lazy so the thunk only needs to
     * handle those cases going forward. */
    if (mino_type_of(coll) != MINO_CONS && mino_type_of(coll) != MINO_LAZY
        && mino_type_of(coll) != MINO_CHUNKED_CONS) {
        coll = prim_seq(S, mino_cons(S, coll, mino_nil(S)), NULL);
        if (coll == NULL) return NULL;
        if (mino_type_of(coll) == MINO_NIL) return mino_empty_list(S);
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
static mino_val *lazy_filter_thunk(mino_state *S, mino_val *ctx)
{
    mino_val *pred = ctx->as.cons.car;
    mino_val *coll = ctx->as.cons.cdr->as.cons.car;
    for (;;) {
        mino_val *head;
        mino_val *rest;
        mino_val *call_args;
        mino_val *ok;
        coll = normalize_seq(S, coll);
        if (coll == NULL) return NULL;
        if (mino_type_of(coll) == MINO_NIL) return mino_nil(S);
        if (mino_type_of(coll) == MINO_CHUNKED_CONS) {
            /* Chunked path: scan the head chunk into a fresh chunk
             * holding only the elements where pred is truthy, then
             * emit chunk-cons plus a lazy continuation over the
             * chunk-rest. The fresh buffer can be smaller than the
             * source chunk; we size it pessimistically at the source
             * chunk's remaining length. */
            const mino_val *src = coll->as.chunked_cons.chunk;
            unsigned          off = coll->as.chunked_cons.off;
            unsigned          n   = src->as.chunk.len - off;
            mino_val       *buf;
            mino_val       *more;
            mino_val       *next_ctx;
            mino_val       *next_lz;
            unsigned          k;
            buf = mino_chunk_buffer(S, n);
            if (buf == NULL) return NULL;
            gc_pin(buf);
            for (k = 0; k < n; k++) {
                mino_val *elem = src->as.chunk.vals[off + k];
                mino_val *r;
                call_args = mino_cons(S, elem, mino_nil(S));
                gc_pin(call_args);
                r = apply_callable(S, pred, call_args, NULL);
                gc_unpin(1);
                if (r == NULL) { gc_unpin(1); return NULL; }
                if (mino_is_truthy_inline(r)) {
                    gc_write_barrier(S, buf, NULL, elem);
                    buf->as.chunk.vals[buf->as.chunk.len++] = elem;
                }
            }
            mino_chunk_seal(buf);
            more = coll->as.chunked_cons.more;
            if (more != NULL && mino_type_of(more) != MINO_NIL
                && mino_type_of(more) != MINO_EMPTY_LIST) {
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
                if (next_lz != NULL && mino_type_of(next_lz) == MINO_LAZY) {
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
        if (mino_is_truthy_inline(ok)) {
            mino_val *next_ctx;
            mino_val *next_lz;
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
mino_val *prim_lazy_filter(mino_state *S, mino_val *args, mino_env *env)
{
    mino_val *pred;
    mino_val *coll;
    mino_val *ctx;
    mino_val *lz;
    size_t n;
    (void)env;
    arg_count(S, args, &n);
    if (n != 2) {
        return prim_throw_classified(S, "eval/arity", "MAR001",
            "lazy-filter requires 2 arguments");
    }
    pred = args->as.cons.car;
    coll = args->as.cons.cdr->as.cons.car;
    if (coll == NULL || mino_type_of(coll) == MINO_NIL
        || mino_type_of(coll) == MINO_EMPTY_LIST) {
        return mino_empty_list(S);
    }
    if (mino_type_of(coll) != MINO_CONS && mino_type_of(coll) != MINO_LAZY
        && mino_type_of(coll) != MINO_CHUNKED_CONS) {
        coll = prim_seq(S, mino_cons(S, coll, mino_nil(S)), NULL);
        if (coll == NULL) return NULL;
        if (mino_type_of(coll) == MINO_NIL) return mino_empty_list(S);
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
static mino_val *range_thunk(mino_state *S, mino_val *ctx);
static mino_val *lazy_take_thunk(mino_state *S, mino_val *ctx);

/* Recognise a lazy seq created by prim_range. Used by prim_reduce's
 * int-range fast path to lift the iteration into a tight C loop
 * instead of forcing every chunk and rebuilding a 2-arg cons spine
 * per call. Returns 1 and fills the params on a hit, 0 on a miss. */
int lazy_is_int_range(const mino_val *coll, long long *start_out,
                      long long *end_out, long long *step_out,
                      int *infinite_out)
{
    const mino_val *ctx;
    if (coll == NULL || mino_type_of(coll) != MINO_LAZY) return 0;
    if (coll->as.lazy.c_thunk != range_thunk) return 0;
    if (coll->as.lazy.realized != LAZY_UNREALIZED) return 0;
    ctx = coll->as.lazy.body;
    if (ctx == NULL || !mino_is_cons(ctx)) return 0;
    *start_out    = mino_val_int_get(ctx->as.cons.car);
    *end_out      = mino_val_int_get(ctx->as.cons.cdr->as.cons.car);
    *step_out     = mino_val_int_get(ctx->as.cons.cdr->as.cons.cdr->as.cons.car);
    *infinite_out = (mino_type_of(ctx->as.cons.cdr->as.cons.cdr->as.cons.cdr
                                  ->as.cons.car) == MINO_BOOL
                     && mino_val_bool_get(ctx->as.cons.cdr->as.cons.cdr
                                          ->as.cons.cdr->as.cons.car) == 1);
    return 1;
}

/* Pipeline-stage thunk identification. Returns 1 iff `coll` is an
 * unrealized LAZY whose thunk is exactly the named stage. Used by
 * prim_reduce to fuse a (->> src (map ...) (filter ...) (take ...))
 * chain into a single element-by-element walk over `src`, eliminating
 * the per-stage lazy-cell allocations. A realized LAZY returns 0 — its
 * body has been cleared and the cached value is just a regular cons /
 * chunked-cons / nil. */
int lazy_thunk_is_map1(const mino_val *coll)
{
    return coll != NULL
        && mino_type_of(coll) == MINO_LAZY
        && coll->as.lazy.realized == LAZY_UNREALIZED
        && coll->as.lazy.c_thunk == lazy_map1_thunk;
}

int lazy_thunk_is_filter(const mino_val *coll)
{
    return coll != NULL
        && mino_type_of(coll) == MINO_LAZY
        && coll->as.lazy.realized == LAZY_UNREALIZED
        && coll->as.lazy.c_thunk == lazy_filter_thunk;
}

int lazy_thunk_is_take(const mino_val *coll)
{
    return coll != NULL
        && mino_type_of(coll) == MINO_LAZY
        && coll->as.lazy.realized == LAZY_UNREALIZED
        && coll->as.lazy.c_thunk == lazy_take_thunk;
}

static mino_val *range_make_lazy(mino_state *S, long long start,
                                   long long end, long long step,
                                   int infinite)
{
    mino_val *lz;
    mino_val *ctx;
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

/* Generic (non-long) range: floats, ratios, bigints, bigdecs, and any
 * mix of them. Each element is the previous plus `step` through the
 * auto-promoting add, with the bound checked through the numeric
 * tower's `<` -- the same repeated-addition contract the canonical
 * generic range keeps (so `(count (range 0 1 0.1))` is 10, float
 * drift included). ctx = cons(cur, cons(end, cons(step, cons(asc?,
 * nil)))). */
static mino_val *range_thunk_g(mino_state *S, mino_val *ctx);

static mino_val *range_make_lazy_g(mino_state *S, mino_val *cur,
                                     mino_val *end, mino_val *step,
                                     int ascending)
{
    mino_val *ctx;
    mino_val *lz;
    ctx = mino_cons(S, cur,
            mino_cons(S, end,
                mino_cons(S, step,
                    mino_cons(S, ascending ? mino_true(S) : mino_false(S),
                              mino_nil(S)))));
    lz = alloc_val(S, MINO_LAZY);
    if (lz == NULL) return NULL;
    lz->as.lazy.body    = ctx;
    lz->as.lazy.c_thunk = range_thunk_g;
    return lz;
}

/* `cur` still inside the range? Ascending checks cur < end, descending
 * end < cur. Sets *ok to 0 when the comparison itself threw. */
static int range_g_in_bounds(mino_state *S, mino_val *cur, mino_val *end,
                             int ascending, int *ok)
{
    mino_val *argv[2];
    mino_val *r;
    if (ascending) { argv[0] = cur; argv[1] = end; }
    else           { argv[0] = end; argv[1] = cur; }
    r = prim_lt_argv(S, argv, 2, NULL);
    if (r == NULL) { *ok = 0; return 0; }
    *ok = 1;
    return mino_type_of(r) == MINO_BOOL && mino_val_bool_get(r) != 0;
}

static mino_val *range_thunk_g(mino_state *S, mino_val *ctx)
{
    enum { CHUNK_N = 32 };
    mino_val *cur  = ctx->as.cons.car;
    mino_val *end  = ctx->as.cons.cdr->as.cons.car;
    mino_val *step = ctx->as.cons.cdr->as.cons.cdr->as.cons.car;
    int ascending  = ctx->as.cons.cdr->as.cons.cdr->as.cons.cdr->as.cons.car
                         == mino_true(S);
    mino_val *buf;
    unsigned    n    = 0;
    int         more;
    int         ok;
    more = range_g_in_bounds(S, cur, end, ascending, &ok);
    if (!ok) return NULL;
    if (!more) return mino_nil(S);
    buf = mino_chunk_buffer(S, CHUNK_N);
    if (buf == NULL) return NULL;
    while (n < CHUNK_N && more) {
        mino_val *argv[2];
        if (!mino_chunk_append(buf, cur)) return NULL;
        n++;
        argv[0] = cur;
        argv[1] = step;
        cur = prim_addp_argv(S, argv, 2, NULL);
        if (cur == NULL) return NULL;
        more = range_g_in_bounds(S, cur, end, ascending, &ok);
        if (!ok) return NULL;
    }
    mino_chunk_seal(buf);
    if (more) {
        mino_val *tail = range_make_lazy_g(S, cur, end, step, ascending);
        if (tail == NULL) return NULL;
        return mino_chunked_cons(S, buf, tail);
    }
    return mino_chunked_cons(S, buf, mino_nil(S));
}

static mino_val *range_thunk(mino_state *S, mino_val *ctx)
{
    long long start = mino_val_int_get(ctx->as.cons.car);
    long long end   = mino_val_int_get(ctx->as.cons.cdr->as.cons.car);
    long long step  = mino_val_int_get(ctx->as.cons.cdr->as.cons.cdr->as.cons.car);
    int infinite    = ctx->as.cons.cdr->as.cons.cdr->as.cons.cdr->as.cons.car
                          == mino_true(S);
    /* Emit a 32-element chunk (or whatever remains) on each force.
     * Downstream consumers that handle MINO_CHUNKED_CONS skip the
     * per-element lazy thunk and the per-element cons cell. */
    enum { CHUNK_N = 32 };
    long long count = CHUNK_N;
    long long cur   = start;
    long long i;
    mino_val *buf;
    if (!infinite) {
        if (step > 0) {
            long long remaining = (end - start + step - 1) / step;
            if (remaining < count) count = remaining;
        } else {
            long long remaining = (start - end - step - 1) / -step;
            if (remaining < count) count = remaining;
        }
        if (count <= 0) return mino_nil(S);
    }
    buf = mino_chunk_buffer(S, (unsigned)count);
    if (buf == NULL) return NULL;
    for (i = 0; i < count; i++) {
        if (!mino_chunk_append(buf, mino_int(S, cur))) return NULL;
        cur += step;
    }
    mino_chunk_seal(buf);
    return mino_chunked_cons(S, buf,
        range_make_lazy(S, cur, end, step, infinite));
}

/* Lazy take: ctx = cons(n, coll_state). Each force yields up to one
 * chunk (for chunked sources) or one element (for cons / lazy sources)
 * and continues with a fresh lazy bound to the remaining count.
 *
 * Chunked fast path: when the underlying coll is MINO_CHUNKED_CONS,
 * forward the whole chunk in a single emit (sub-chunk if `n` falls
 * inside the chunk). One allocation per chunk instead of one per
 * element. Realising `(take 10000 (range))` produces ~313 chunk-cons
 * cells instead of 10000. */
static mino_val *lazy_take_thunk(mino_state *S, mino_val *ctx)
{
    long long n   = mino_val_int_get(ctx->as.cons.car);
    mino_val *coll = ctx->as.cons.cdr->as.cons.car;
    mino_val *head;
    mino_val *rest;
    mino_val *next_ctx;
    mino_val *next_lz;
    if (n <= 0) return mino_nil(S);
    coll = normalize_seq(S, coll);
    if (coll == NULL) return NULL;
    if (mino_type_of(coll) == MINO_NIL) return mino_nil(S);
    if (mino_type_of(coll) == MINO_CHUNKED_CONS) {
        const mino_val *src = coll->as.chunked_cons.chunk;
        unsigned          off = coll->as.chunked_cons.off;
        unsigned          avail = src->as.chunk.len - off;
        if ((long long)avail <= n) {
            /* Use the whole remaining chunk; emit a fresh chunked-cons
             * that preserves the source offset and either points at the
             * rest (recursing through a fresh lazy) or terminates when
             * we've taken all we need. */
            mino_val *more = coll->as.chunked_cons.more;
            long long   left = n - (long long)avail;
            mino_val *cell = alloc_val(S, MINO_CHUNKED_CONS);
            if (cell == NULL) return NULL;
            cell->as.chunked_cons.chunk = (mino_val *)src;
            cell->as.chunked_cons.off   = off;
            if (left <= 0 || more == NULL || mino_type_of(more) == MINO_NIL
                || mino_type_of(more) == MINO_EMPTY_LIST) {
                cell->as.chunked_cons.more = mino_nil(S);
            } else {
                next_ctx = mino_cons(S, mino_int(S, left),
                            mino_cons(S, more, mino_nil(S)));
                next_lz = alloc_val(S, MINO_LAZY);
                if (next_lz == NULL) return NULL;
                next_lz->as.lazy.body    = next_ctx;
                next_lz->as.lazy.c_thunk = lazy_take_thunk;
                cell->as.chunked_cons.more = next_lz;
            }
            return cell;
        } else {
            /* `n < avail`: materialise a fresh chunk of exactly `n`
             * elements so the resulting seq stops at the boundary. */
            mino_val *buf;
            long long   i;
            buf = mino_chunk_buffer(S, (unsigned)n);
            if (buf == NULL) return NULL;
            for (i = 0; i < n; i++) {
                if (!mino_chunk_append(buf, src->as.chunk.vals[off + (unsigned)i])) {
                    return NULL;
                }
            }
            mino_chunk_seal(buf);
            return mino_chunked_cons(S, buf, mino_nil(S));
        }
    }
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
mino_val *prim_lazy_take(mino_state *S, mino_val *args, mino_env *env)
{
    long long n;
    mino_val *coll;
    mino_val *ctx;
    mino_val *lz;
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
    if (n <= 0 || coll == NULL || mino_type_of(coll) == MINO_NIL
        || mino_type_of(coll) == MINO_EMPTY_LIST) {
        return mino_empty_list(S);
    }
    if (mino_type_of(coll) != MINO_CONS && mino_type_of(coll) != MINO_LAZY
        && mino_type_of(coll) != MINO_CHUNKED_CONS) {
        coll = prim_seq(S, mino_cons(S, coll, mino_nil(S)), NULL);
        if (coll == NULL) return NULL;
        if (mino_type_of(coll) == MINO_NIL) return mino_empty_list(S);
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
mino_val *prim_drop_seq(mino_state *S, mino_val *args, mino_env *env)
{
    long long n;
    mino_val *coll;
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
        mino_val *seq_args;
        mino_val *seqd;
        if (coll == NULL || mino_type_of(coll) == MINO_NIL) return mino_empty_list(S);
        /* `drop` always returns a seq, never the source collection. A
         * vector / map / set / chunked source becomes its seq view so
         * downstream `pr-str`, `vector?`, etc. see the seq type. */
        seq_args = mino_cons(S, coll, mino_nil(S));
        seqd     = prim_seq(S, seq_args, env);
        if (seqd == NULL) return NULL;
        if (mino_type_of(seqd) == MINO_NIL) return mino_empty_list(S);
        return seqd;
    }
    while (n > 0) {
        if (coll != NULL && mino_type_of(coll) == MINO_LAZY) {
            coll = lazy_force(S, coll);
            if (coll == NULL) return NULL;
        }
        if (coll == NULL || mino_type_of(coll) == MINO_NIL
            || mino_type_of(coll) == MINO_EMPTY_LIST) {
            return mino_empty_list(S);
        }
        if (mino_type_of(coll) == MINO_CHUNKED_CONS) {
            /* Bulk-skip the head chunk when `n` exceeds its remaining
             * elements. One advance walks past the whole chunk instead
             * of `len - off` per-element advances. */
            const mino_val *src = coll->as.chunked_cons.chunk;
            unsigned          off = coll->as.chunked_cons.off;
            unsigned          avail = src->as.chunk.len - off;
            if ((long long)avail <= n) {
                coll = coll->as.chunked_cons.more;
                n   -= (long long)avail;
                if (coll == NULL || mino_type_of(coll) == MINO_NIL
                    || mino_type_of(coll) == MINO_EMPTY_LIST) {
                    return mino_empty_list(S);
                }
                continue;
            }
            /* `n < avail`: rebase the head chunk's offset. */
            {
                mino_val *cell = alloc_val(S, MINO_CHUNKED_CONS);
                if (cell == NULL) return NULL;
                cell->as.chunked_cons.chunk = (mino_val *)src;
                cell->as.chunked_cons.off   = off + (unsigned)n;
                cell->as.chunked_cons.more  = coll->as.chunked_cons.more;
                return cell;
            }
        }
        if (mino_type_of(coll) != MINO_CONS) {
            coll = prim_seq(S, mino_cons(S, coll, mino_nil(S)), NULL);
            if (coll == NULL) return NULL;
            if (mino_type_of(coll) == MINO_NIL) return mino_empty_list(S);
            if (mino_type_of(coll) == MINO_CHUNKED_CONS) continue;
            if (mino_type_of(coll) != MINO_CONS) return mino_empty_list(S);
        }
        coll = coll->as.cons.cdr;
        n--;
    }
    if (coll == NULL || mino_type_of(coll) == MINO_NIL) return mino_empty_list(S);
    return coll;
}

/* (range), (range end), (range start end), (range start end step). */
/* Infinite repetition of one value, element-wise. Zero-step ranges
 * degenerate to this, mirroring the canonical contract where the seq
 * never advances. Element-wise (not chunked) so a take/first consumer
 * realizes exactly what it asks for. ctx is the repeated value. */
static mino_val *repeat_forever_thunk(mino_state *S, mino_val *ctx)
{
    mino_val *next = alloc_val(S, MINO_LAZY);
    if (next == NULL) return NULL;
    next->as.lazy.body    = ctx;
    next->as.lazy.c_thunk = repeat_forever_thunk;
    return mino_cons(S, ctx, next);
}

static mino_val *repeat_forever_lazy(mino_state *S, mino_val *x)
{
    mino_val *lz = alloc_val(S, MINO_LAZY);
    if (lz == NULL) return NULL;
    lz->as.lazy.body    = x;
    lz->as.lazy.c_thunk = repeat_forever_thunk;
    return lz;
}

/* Numeric-tower membership for range bounds. */
static int range_val_is_number(const mino_val *v)
{
    return v != NULL
        && (mino_val_int_p(v)
            || mino_type_of(v) == MINO_FLOAT
            || mino_type_of(v) == MINO_FLOAT32
            || mino_type_of(v) == MINO_BIGINT
            || mino_type_of(v) == MINO_RATIO
            || mino_type_of(v) == MINO_BIGDEC);
}

mino_val *prim_range(mino_state *S, mino_val *args, mino_env *env)
{
    long long start = 0, end = 0, step = 1;
    size_t n;
    int infinite = 0;
    int generic  = 0;
    (void)env;
    arg_count(S, args, &n);
    if (n == 0) {
        infinite = 1;
    } else if (n == 1) {
        generic = !mino_to_int(args->as.cons.car, &end);
    } else if (n == 2) {
        generic = !mino_to_int(args->as.cons.car, &start)
               || !mino_to_int(args->as.cons.cdr->as.cons.car, &end);
    } else if (n == 3) {
        generic = !mino_to_int(args->as.cons.car, &start)
               || !mino_to_int(args->as.cons.cdr->as.cons.car, &end)
               || !mino_to_int(args->as.cons.cdr->as.cons.cdr->as.cons.car,
                               &step);
        if (!generic && step == 0) {
            /* Zero step never advances: empty when the bounds already
             * meet, otherwise start repeats forever. */
            if (end == start) return mino_empty_list(S);
            return repeat_forever_lazy(S, mino_int(S, start));
        }
    } else {
        return prim_throw_classified(S, "eval/arity", "MAR001",
            "range takes 0, 1, 2, or 3 arguments");
    }
    if (generic) {
        /* At least one bound falls outside long long: dispatch through
         * the numeric tower (floats, ratios, bigints, bigdecs). */
        mino_val *g_start = mino_int(S, 0);
        mino_val *g_end   = args->as.cons.car;
        mino_val *g_step  = mino_int(S, 1);
        mino_val *zero    = mino_int(S, 0);
        int         ascending;
        int         ok;
        if (n >= 2) {
            g_start = args->as.cons.car;
            g_end   = args->as.cons.cdr->as.cons.car;
        }
        if (n == 3) {
            g_step = args->as.cons.cdr->as.cons.cdr->as.cons.car;
        }
        if (!range_val_is_number(g_start) || !range_val_is_number(g_end)
            || !range_val_is_number(g_step)) {
            return prim_throw_classified(S, "eval/type", "MTY001",
                "range arguments must be numbers");
        }
        ascending = range_g_in_bounds(S, zero, g_step, 1, &ok);
        if (!ok) return NULL;
        if (!ascending) {
            int descending = range_g_in_bounds(S, g_step, zero, 1, &ok);
            if (!ok) return NULL;
            if (!descending) {
                /* Zero step (or unordered, e.g. NaN): empty when the
                 * bounds are numerically equal, otherwise start
                 * repeats forever. */
                int apart = range_g_in_bounds(S, g_start, g_end, 1, &ok);
                if (!ok) return NULL;
                if (!apart) {
                    apart = range_g_in_bounds(S, g_end, g_start, 1, &ok);
                    if (!ok) return NULL;
                }
                if (!apart) return mino_empty_list(S);
                return repeat_forever_lazy(S, g_start);
            }
        }
        {
            mino_val *r = range_make_lazy_g(S, g_start, g_end, g_step,
                                              ascending);
            if (r == NULL) return NULL;
            return r;
        }
    }
    {
        mino_val *r = range_make_lazy(S, start, end, step, infinite);
        if (r == NULL) return NULL;
        if (mino_type_of(r) == MINO_NIL) return mino_empty_list(S);
        return r;
    }
}

/* ------------------------------------------------------------------------- */
/* Chunked-seq family                                                        */
/* ------------------------------------------------------------------------- */

static mino_val *prim_chunk_buffer(mino_state *S, mino_val *args,
                              mino_env *env)
{
    long long  cap;
    size_t     n;
    mino_val *buf;
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

static mino_val *prim_chunk_append(mino_state *S, mino_val *args,
                              mino_env *env)
{
    mino_val *buf, *elem;
    size_t      n;
    (void)env;
    arg_count(S, args, &n);
    if (n != 2) {
        return prim_throw_classified(S, "eval/arity", "MAR001",
            "chunk-append requires 2 arguments");
    }
    buf  = args->as.cons.car;
    elem = args->as.cons.cdr->as.cons.car;
    if (buf == NULL || mino_type_of(buf) != MINO_CHUNK) {
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

static mino_val *prim_chunk(mino_state *S, mino_val *args, mino_env *env)
{
    mino_val *buf;
    size_t      n;
    (void)env;
    arg_count(S, args, &n);
    if (n != 1) {
        return prim_throw_classified(S, "eval/arity", "MAR001",
            "chunk requires 1 argument");
    }
    buf = args->as.cons.car;
    if (buf == NULL || mino_type_of(buf) != MINO_CHUNK) {
        return prim_throw_classified(S, "eval/type", "MTY001",
            "chunk: argument must be a chunk-buffer");
    }
    return mino_chunk_seal(buf);
}

static mino_val *prim_chunk_cons(mino_state *S, mino_val *args,
                            mino_env *env)
{
    mino_val *chunk, *more;
    size_t      n;
    (void)env;
    arg_count(S, args, &n);
    if (n != 2) {
        return prim_throw_classified(S, "eval/arity", "MAR001",
            "chunk-cons requires 2 arguments");
    }
    chunk = args->as.cons.car;
    more  = args->as.cons.cdr->as.cons.car;
    if (chunk == NULL || mino_type_of(chunk) != MINO_CHUNK) {
        return prim_throw_classified(S, "eval/type", "MTY001",
            "chunk-cons: first argument must be a chunk");
    }
    if (chunk->as.chunk.len == 0) {
        if (more == NULL || mino_type_of(more) == MINO_NIL) return mino_empty_list(S);
        return more;
    }
    return mino_chunked_cons(S, chunk, more);
}

static mino_val *prim_chunk_first(mino_state *S, mino_val *args,
                             mino_env *env)
{
    mino_val *cs;
    size_t      n;
    (void)env;
    arg_count(S, args, &n);
    if (n != 1) {
        return prim_throw_classified(S, "eval/arity", "MAR001",
            "chunk-first requires 1 argument");
    }
    cs = args->as.cons.car;
    if (cs == NULL || mino_type_of(cs) != MINO_CHUNKED_CONS) {
        return prim_throw_classified(S, "eval/type", "MTY001",
            "chunk-first: argument must be a chunked-seq");
    }
    /* Consumers index the result with count/nth from zero, so a head
     * chunk whose cons carries a non-zero offset (drop / drop-while
     * rebase `off` instead of copying) must be materialized as the
     * remaining-elements view. The common off == 0 case stays
     * alloc-free. */
    if (cs->as.chunked_cons.off == 0) {
        return cs->as.chunked_cons.chunk;
    }
    {
        const mino_val *src = cs->as.chunked_cons.chunk;
        unsigned          off = cs->as.chunked_cons.off;
        unsigned          len = src->as.chunk.len - off;
        unsigned          k;
        mino_val       *buf = mino_chunk_buffer(S, len);
        if (buf == NULL) return NULL;
        for (k = 0; k < len; k++) {
            buf->as.chunk.vals[buf->as.chunk.len++] =
                src->as.chunk.vals[off + k];
        }
        mino_chunk_seal(buf);
        return buf;
    }
}

static mino_val *prim_chunk_rest(mino_state *S, mino_val *args,
                            mino_env *env)
{
    mino_val *cs;
    mino_val *more;
    size_t      n;
    (void)env;
    arg_count(S, args, &n);
    if (n != 1) {
        return prim_throw_classified(S, "eval/arity", "MAR001",
            "chunk-rest requires 1 argument");
    }
    cs = args->as.cons.car;
    if (cs == NULL || mino_type_of(cs) != MINO_CHUNKED_CONS) {
        return prim_throw_classified(S, "eval/type", "MTY001",
            "chunk-rest: argument must be a chunked-seq");
    }
    more = cs->as.chunked_cons.more;
    if (more == NULL || mino_type_of(more) == MINO_NIL) return mino_empty_list(S);
    return more;
}

static mino_val *prim_chunk_next(mino_state *S, mino_val *args,
                            mino_env *env)
{
    mino_val *cs;
    mino_val *more;
    size_t      n;
    (void)env;
    arg_count(S, args, &n);
    if (n != 1) {
        return prim_throw_classified(S, "eval/arity", "MAR001",
            "chunk-next requires 1 argument");
    }
    cs = args->as.cons.car;
    if (cs == NULL || mino_type_of(cs) != MINO_CHUNKED_CONS) {
        return prim_throw_classified(S, "eval/type", "MTY001",
            "chunk-next: argument must be a chunked-seq");
    }
    more = cs->as.chunked_cons.more;
    if (more != NULL && mino_type_of(more) == MINO_LAZY) {
        more = lazy_force(S, more);
        if (more == NULL) return NULL;
    }
    if (more == NULL || mino_type_of(more) == MINO_NIL
        || mino_type_of(more) == MINO_EMPTY_LIST) {
        return mino_nil(S);
    }
    return more;
}

static mino_val *prim_chunked_seq_p(mino_state *S, mino_val *args,
                               mino_env *env)
{
    size_t n;
    (void)env;
    arg_count(S, args, &n);
    if (n != 1) {
        return prim_throw_classified(S, "eval/arity", "MAR001",
            "chunked-seq? requires 1 argument");
    }
    return (args->as.cons.car != NULL
            && mino_type_of(args->as.cons.car) == MINO_CHUNKED_CONS)
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
