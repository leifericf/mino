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

#include "runtime/internal.h"
#include "eval/internal.h"
#include "collections/internal.h"

/* Sorted maps and sets walk an in-order red-black tree via a small
 * fixed-size stack instead of materialising the keys vector. The RB
 * tree's worst-case height for n entries is 2*log2(n+1); 64 covers
 * trees with > 4 billion entries comfortably. */
#define MINO_ITER_RB_STACK_DEPTH 64

struct mino_iter {
    mino_state_t *S;
    mino_ref_t   *ref;        /* roots the collection across GC */
    mino_val_t   *cursor;     /* the cell currently being walked */
    size_t        idx;        /* index into vector / chunk */
    int           rb_top;     /* depth of rb_stack (0 = empty/uninit) */
    int           rb_started; /* 1 once rb walk has been seeded */
    const mino_rb_node_t *rb_stack[MINO_ITER_RB_STACK_DEPTH];
};

size_t mino_iter_sizeof(void)
{
    return sizeof(struct mino_iter);
}

void mino_iter_init(mino_state_t *S, mino_iter_t *it, mino_val_t *coll)
{
    if (it == NULL) return;
    it->S          = S;
    it->ref        = (S != NULL) ? mino_ref(S, coll) : NULL;
    it->cursor     = coll;
    it->idx        = 0;
    it->rb_top     = 0;
    it->rb_started = 0;
}

void mino_iter_done(mino_iter_t *it)
{
    if (it == NULL) return;
    if (it->ref != NULL) {
        mino_unref(it->S, it->ref);
        it->ref = NULL;
    }
    it->cursor = NULL;
}

int mino_iter_next(mino_iter_t *it, mino_val_t **out_k, mino_val_t **out_v)
{
    mino_val_t *cur;
    mino_type_t kind;

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

    switch (kind) {
    case MINO_NIL:
    case MINO_EMPTY_LIST:
        it->cursor = NULL;
        return 0;

    case MINO_CONS: {
        if (out_k != NULL) *out_k = cur->as.cons.car;
        it->cursor = cur->as.cons.cdr;
        return 1;
    }

    case MINO_VECTOR: {
        if (it->idx >= cur->as.vec.len) { it->cursor = NULL; return 0; }
        if (out_k != NULL) *out_k = vec_nth(cur, it->idx);
        it->idx++;
        return 1;
    }

    case MINO_MAP: {
        mino_val_t *ko = cur->as.map.key_order;
        if (ko == NULL || it->idx >= cur->as.map.len) {
            it->cursor = NULL; return 0;
        }
        {
            mino_val_t *k = vec_nth(ko, it->idx);
            if (out_k != NULL) *out_k = k;
            if (out_v != NULL) *out_v = map_get_val(cur, k);
        }
        it->idx++;
        return 1;
    }

    case MINO_SET: {
        mino_val_t *ko = cur->as.set.key_order;
        if (ko == NULL || it->idx >= cur->as.set.len) {
            it->cursor = NULL; return 0;
        }
        if (out_k != NULL) *out_k = vec_nth(ko, it->idx);
        it->idx++;
        return 1;
    }

    case MINO_SORTED_MAP:
    case MINO_SORTED_SET: {
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
        /* Push the leftmost spine of the right subtree so the next
         * call yields the next in-order node. */
        {
            const mino_rb_node_t *n = node->right;
            while (n != NULL && it->rb_top < MINO_ITER_RB_STACK_DEPTH) {
                it->rb_stack[it->rb_top++] = n;
                n = n->left;
            }
        }
        return 1;
    }

    case MINO_CHUNK: {
        if (it->idx >= cur->as.chunk.len) { it->cursor = NULL; return 0; }
        if (out_k != NULL) *out_k = cur->as.chunk.vals[it->idx];
        it->idx++;
        return 1;
    }

    case MINO_CHUNKED_CONS: {
        mino_val_t *chunk = cur->as.chunked_cons.chunk;
        unsigned    off   = cur->as.chunked_cons.off + (unsigned)it->idx;
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

    default:
        /* Not a sequential value -- finish. */
        it->cursor = NULL;
        return 0;
    }
}
