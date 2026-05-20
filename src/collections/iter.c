/*
 * collections/iter.c -- unified C-side iterator over mino collections.
 *
 * One iterator type walks vectors, maps (hashed and sorted), sets
 * (hashed and sorted), cons lists, the empty-list singleton, lazy
 * seqs, and chunked seqs. Dispatch happens internally on the cell's
 * effective type at each step; embedders just call _next until it
 * returns 0.
 *
 * Lifecycle: the host owns the storage (allocates `mino_iter_sizeof()`
 * bytes, typically on the C stack), then calls
 *     mino_iter_init -> repeated mino_iter_next -> mino_iter_done.
 * The iterator roots its collection so a GC during iteration cannot
 * reclaim the cells the walker holds borrowed pointers into. Storage
 * does not move: the GC root is keyed by the iter's `ref` slot, which
 * `mino_iter_done` releases.
 */

#include "mino.h"
#include "mino_internal.h"
#include "collections/internal.h"
#include "eval/internal.h"          /* lazy_force */
#include "runtime/value_assert.h"   /* mino_type_of */

/* Sorted maps and sets walk an in-order red-black tree via a small
 * fixed-size stack instead of materialising the keys vector. The RB
 * tree's worst-case height for n entries is 2*log2(n+1); 64 covers
 * trees with > 4 billion entries comfortably. */
#define MINO_ITER_RB_STACK_DEPTH 64

struct mino_iter {
    mino_state *S;
    mino_ref   *ref;        /* roots the collection across GC */
    mino_val   *cursor;     /* the cell currently being walked */
    size_t        idx;        /* index into vector / chunk */
    int           rb_top;     /* depth of rb_stack (0 = empty/uninit) */
    int           rb_started; /* 1 once rb walk has been seeded */
    const mino_rb_node_t *rb_stack[MINO_ITER_RB_STACK_DEPTH];
};

size_t mino_iter_sizeof(void)
{
    return sizeof(struct mino_iter);
}

void mino_iter_init(mino_state *S, mino_iter *it, mino_val *coll)
{
    if (it == NULL) return;
    it->S          = S;
    it->ref        = (S != NULL) ? mino_ref_new(S, coll) : NULL;
    it->cursor     = coll;
    it->idx        = 0;
    it->rb_top     = 0;
    it->rb_started = 0;
}

void mino_iter_done(mino_iter *it)
{
    if (it == NULL) return;
    if (it->ref != NULL) {
        mino_unref(it->S, it->ref);
        it->ref = NULL;
    }
    it->cursor = NULL;
}

/* Per-collection-kind step. Each handler returns 1 and writes
 * *out_k / *out_v on a successful step; returns 0 (and typically
 * clears it->cursor) when the walk is exhausted. The dispatcher
 * resolves lazies and looks up the handler via k_iter_dispatch[]. */
typedef int (*iter_step_fn)(mino_iter *it, mino_val *cur,
                            mino_type kind,
                            mino_val **out_k, mino_val **out_v);

static int iter_step_finish(mino_iter *it, mino_val *cur,
                            mino_type kind,
                            mino_val **out_k, mino_val **out_v)
{
    (void)cur; (void)kind; (void)out_k; (void)out_v;
    it->cursor = NULL;
    return 0;
}

static int iter_step_cons(mino_iter *it, mino_val *cur,
                          mino_type kind,
                          mino_val **out_k, mino_val **out_v)
{
    (void)kind; (void)out_v;
    if (out_k != NULL) *out_k = cur->as.cons.car;
    it->cursor = cur->as.cons.cdr;
    return 1;
}

static int iter_step_vector(mino_iter *it, mino_val *cur,
                            mino_type kind,
                            mino_val **out_k, mino_val **out_v)
{
    (void)kind; (void)out_v;
    if (it->idx >= cur->as.vec.len) { it->cursor = NULL; return 0; }
    if (out_k != NULL) *out_k = vec_nth(cur, it->idx);
    it->idx++;
    return 1;
}

static int iter_step_map(mino_iter *it, mino_val *cur,
                         mino_type kind,
                         mino_val **out_k, mino_val **out_v)
{
    mino_val *ko = cur->as.map.key_order;
    mino_val *k;
    (void)kind;
    if (ko == NULL || it->idx >= cur->as.map.len) {
        it->cursor = NULL; return 0;
    }
    k = vec_nth(ko, it->idx);
    if (out_k != NULL) *out_k = k;
    if (out_v != NULL) *out_v = map_get_val(cur, k);
    it->idx++;
    return 1;
}

static int iter_step_set(mino_iter *it, mino_val *cur,
                         mino_type kind,
                         mino_val **out_k, mino_val **out_v)
{
    mino_val *ko = cur->as.set.key_order;
    (void)kind; (void)out_v;
    if (ko == NULL || it->idx >= cur->as.set.len) {
        it->cursor = NULL; return 0;
    }
    if (out_k != NULL) *out_k = vec_nth(ko, it->idx);
    it->idx++;
    return 1;
}

static int iter_step_sorted(mino_iter *it, mino_val *cur,
                            mino_type kind,
                            mino_val **out_k, mino_val **out_v)
{
    const mino_rb_node_t *node;
    /* Seed the in-order walk on the first call: push the leftmost
     * spine starting from the root. */
    if (!it->rb_started) {
        const mino_rb_node_t *n = cur->as.sorted.root;
        it->rb_started = 1;
        while (n != NULL && it->rb_top < MINO_ITER_RB_STACK_DEPTH) {
            it->rb_stack[it->rb_top++] = n;
            n = n->left;
        }
    }
    if (it->rb_top == 0) { it->cursor = NULL; return 0; }
    node = it->rb_stack[--it->rb_top];
    if (out_k != NULL) *out_k = node->key;
    if (out_v != NULL && kind == MINO_SORTED_MAP) *out_v = node->val;
    /* Push the leftmost spine of the right subtree so the next call
     * yields the next in-order node. */
    {
        const mino_rb_node_t *n = node->right;
        while (n != NULL && it->rb_top < MINO_ITER_RB_STACK_DEPTH) {
            it->rb_stack[it->rb_top++] = n;
            n = n->left;
        }
    }
    return 1;
}

static int iter_step_chunk(mino_iter *it, mino_val *cur,
                           mino_type kind,
                           mino_val **out_k, mino_val **out_v)
{
    (void)kind; (void)out_v;
    if (it->idx >= cur->as.chunk.len) { it->cursor = NULL; return 0; }
    if (out_k != NULL) *out_k = cur->as.chunk.vals[it->idx];
    it->idx++;
    return 1;
}

static int iter_step_chunked_cons(mino_iter *it, mino_val *cur,
                                  mino_type kind,
                                  mino_val **out_k, mino_val **out_v)
{
    mino_val *chunk = cur->as.chunked_cons.chunk;
    unsigned    off   = cur->as.chunked_cons.off + (unsigned)it->idx;
    (void)kind;
    if (chunk == NULL || off >= chunk->as.chunk.len) {
        /* Walk into the `more` cell, resetting idx for the new
         * cursor's shape. */
        it->cursor = cur->as.chunked_cons.more;
        it->idx    = 0;
        return mino_iter_next(it, out_k, out_v);
    }
    if (out_k != NULL) *out_k = chunk->as.chunk.vals[off];
    it->idx++;
    return 1;
}

static const struct {
    mino_type  kind;
    iter_step_fn fn;
} k_iter_dispatch[] = {
    {MINO_NIL,          iter_step_finish},
    {MINO_EMPTY_LIST,   iter_step_finish},
    {MINO_CONS,         iter_step_cons},
    {MINO_VECTOR,       iter_step_vector},
    {MINO_MAP,          iter_step_map},
    {MINO_SET,          iter_step_set},
    {MINO_SORTED_MAP,   iter_step_sorted},
    {MINO_SORTED_SET,   iter_step_sorted},
    {MINO_CHUNK,        iter_step_chunk},
    {MINO_CHUNKED_CONS, iter_step_chunked_cons},
};

int mino_iter_next(mino_iter *it, mino_val **out_k, mino_val **out_v)
{
    mino_val *cur;
    mino_type kind;
    size_t      i;

    if (out_k != NULL) *out_k = NULL;
    if (out_v != NULL) *out_v = NULL;
    if (it == NULL || it->cursor == NULL) return 0;

    /* Resolve lazy seqs lazily: force on each next call so a lazy seq
     * yielded by an outer map / filter pipeline walks transparently. */
    cur = it->cursor;
    while (mino_type_of(cur) == MINO_LAZY) {
        cur = lazy_force(it->S, cur);
        if (cur == NULL) { it->cursor = NULL; return 0; }
        it->cursor = cur;
    }
    kind = mino_type_of(cur);

    for (i = 0; i < sizeof(k_iter_dispatch) / sizeof(k_iter_dispatch[0]); i++) {
        if (k_iter_dispatch[i].kind == kind) {
            return k_iter_dispatch[i].fn(it, cur, kind, out_k, out_v);
        }
    }
    /* Not a sequential value -- finish. */
    it->cursor = NULL;
    return 0;
}
