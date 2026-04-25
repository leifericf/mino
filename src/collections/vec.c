/*
 * vec.c -- persistent 32-way trie vector.
 */

#include "runtime/internal.h"

/* ------------------------------------------------------------------------- */
/* Persistent vector: 32-way trie with tail                                  */
/* ------------------------------------------------------------------------- */
/*
 * Layout (Bagwell persistent vector):
 *   - tail holds the trailing 1..32 elements; appends that fit in the tail
 *     are O(1) amortized (one 32-slot copy, no trie walk).
 *   - root is a trie whose leaves each hold exactly 32 values; the rightmost
 *     spine may be partially filled (branches store a `count`).
 *   - shift encodes height: 0 means the root is itself a leaf; otherwise a
 *     branch whose children live at shift - 5.
 *   - All node mutations are path-copies: conj/assoc return fresh nodes along
 *     the walked path, leaving structural sharing with the source vector.
 *
 * Nodes are untyped unions of child pointers; the active variant is dictated
 * by the node's level during traversal rather than a per-node tag.
 */

/*/* Types and constants in collections_internal.h */

static mino_vec_node_t *vnode_new(mino_state_t *S, unsigned count, int is_leaf)
{
    mino_vec_node_t *n = (mino_vec_node_t *)gc_alloc_typed(
        S, GC_T_VEC_NODE, sizeof(*n));
    n->is_leaf = (unsigned char)(is_leaf ? 1 : 0);
    n->count   = count;
    return n;
}

static mino_vec_node_t *vnode_clone(mino_state_t *S,
                                    const mino_vec_node_t *src)
{
    mino_vec_node_t *n = (mino_vec_node_t *)gc_alloc_typed(
        S, GC_T_VEC_NODE, sizeof(*n));
    memcpy(n, src, sizeof(*n));
    return n;
}

/*
 * new_path: build a spine from a branch at level `shift` down to `leaf`,
 * placing the leaf in slot 0 at every level along the way.
 */
static mino_vec_node_t *new_path(mino_state_t *S, unsigned shift,
                                 mino_vec_node_t *leaf)
{
    mino_vec_node_t *n;
    if (shift == 0) {
        return leaf;
    }
    n = vnode_new(S, 1, 0);
    n->slots[0] = new_path(S, shift - MINO_VEC_B, leaf);
    return n;
}

/*
 * push_tail: insert `leaf` into the subtree rooted at `node` (level `shift`),
 * at the position implied by `subindex` (the leaf's first element's flat
 * index into the trie). Path-copies the walked spine; returns the new root.
 * Caller ensures `subindex` fits within the current tree (no root overflow).
 */
static mino_vec_node_t *push_tail(mino_state_t *S,
                                  const mino_vec_node_t *node, unsigned shift,
                                  size_t subindex, mino_vec_node_t *leaf)
{
    unsigned         digit = (unsigned)((subindex >> shift) & MINO_VEC_MASK);
    mino_vec_node_t *clone = vnode_clone(S, node);
    if (shift == MINO_VEC_B) {
        /* Children are leaves: place the tail directly. */
        clone->slots[digit] = leaf;
    } else {
        mino_vec_node_t *child = (mino_vec_node_t *)node->slots[digit];
        mino_vec_node_t *new_child = (child == NULL)
            ? new_path(S, shift - MINO_VEC_B, leaf)
            : push_tail(S, child, shift - MINO_VEC_B, subindex, leaf);
        clone->slots[digit] = new_child;
    }
    if (digit + 1u > clone->count) {
        clone->count = digit + 1u;
    }
    return clone;
}

/*
 * trie_assoc: path-copy update of the element at flat index `i`. Requires
 * i to refer to the trie (not the tail).
 */
static mino_vec_node_t *trie_assoc(mino_state_t *S,
                                    const mino_vec_node_t *node, unsigned shift,
                                    size_t i, mino_val_t *item)
{
    mino_vec_node_t *clone = vnode_clone(S, node);
    if (shift == 0) {
        clone->slots[i & MINO_VEC_MASK] = item;
    } else {
        unsigned digit = (unsigned)((i >> shift) & MINO_VEC_MASK);
        clone->slots[digit] = trie_assoc(S,
                                          (mino_vec_node_t *)node->slots[digit],
                                          shift - MINO_VEC_B, i, item);
    }
    return clone;
}

/* Construct a vector value from an already-built trie and tail.
 * If `src` is non-NULL, its metadata is carried over to the new vector. */
static mino_val_t *vec_assemble(mino_state_t *S, const mino_val_t *src,
                                 mino_vec_node_t *root,
                                 mino_vec_node_t *tail,
                                 unsigned tail_len, unsigned shift, size_t len)
{
    mino_val_t *v = alloc_val(S, MINO_VECTOR);
    v->as.vec.root     = root;
    v->as.vec.tail     = tail;
    v->as.vec.tail_len = tail_len;
    v->as.vec.shift    = shift;
    v->as.vec.len      = len;
    v->as.vec.offset   = 0;
    v->as.vec.blen     = len;
    if (src != NULL) {
        v->meta = src->meta;
    }
    return v;
}

/* Read one element by flat index; undefined if i >= visible len.
 * For subvecs (offset > 0), translates the visible index to the backing
 * trie's absolute index before lookup. */
mino_val_t *vec_nth(const mino_val_t *v, size_t i)
{
    size_t                  trie_count = v->as.vec.blen - v->as.vec.tail_len;
    size_t                  abs_i      = i + v->as.vec.offset;
    const mino_vec_node_t  *node;
    unsigned                shift;
    if (abs_i >= trie_count) {
        return (mino_val_t *)v->as.vec.tail->slots[abs_i - trie_count];
    }
    node  = v->as.vec.root;
    shift = v->as.vec.shift;
    while (shift > 0) {
        node = (const mino_vec_node_t *)node->slots[(abs_i >> shift) & MINO_VEC_MASK];
        shift -= MINO_VEC_B;
    }
    return (mino_val_t *)node->slots[abs_i & MINO_VEC_MASK];
}

/* Materialize a subvec (offset > 0) into a fresh vector with offset 0.
 * Required before structural mutations (conj, pop) that assume offset == 0. */
static mino_val_t *vec_materialize(mino_state_t *S, const mino_val_t *v)
{
    size_t       len = v->as.vec.len;
    mino_val_t **buf;
    mino_val_t  *result;
    size_t       i;
    if (len == 0) {
        result = vec_from_array(S, NULL, 0);
        result->meta = v->meta;
        return result;
    }
    buf = (mino_val_t **)gc_alloc_typed(S, GC_T_VALARR, len * sizeof(*buf));
    for (i = 0; i < len; i++)
        buf[i] = vec_nth(v, i);
    result = vec_from_array(S, buf, len);
    result->meta = v->meta;
    return result;
}

/* Append one element. O(log32 n) worst case, O(1) amortized for tail appends. */
mino_val_t *vec_conj1(mino_state_t *S, const mino_val_t *v, mino_val_t *item)
{
    mino_vec_node_t *new_tail;
    mino_vec_node_t *new_root;
    unsigned         new_shift;
    size_t           trie_count;
    if (v->as.vec.offset > 0)
        return vec_conj1(S, vec_materialize(S, v), item);
    if (v->as.vec.tail_len < MINO_VEC_WIDTH) {
        /* Tail has room: copy it and append. */
        if (v->as.vec.tail == NULL) {
            new_tail = vnode_new(S, 1, 1);
            new_tail->slots[0] = item;
        } else {
            new_tail = vnode_clone(S, v->as.vec.tail);
            new_tail->slots[v->as.vec.tail_len] = item;
            new_tail->count = v->as.vec.tail_len + 1u;
        }
        return vec_assemble(S, v, v->as.vec.root, new_tail,
                            v->as.vec.tail_len + 1u,
                            v->as.vec.shift, v->as.vec.len + 1u);
    }
    /* Tail is full: push it into the trie, start a fresh tail with the new item. */
    new_tail = vnode_new(S, 1, 1);
    new_tail->slots[0] = item;
    trie_count = v->as.vec.len - v->as.vec.tail_len; /* before incorporation */
    new_shift  = v->as.vec.shift;
    if (v->as.vec.root == NULL) {
        /* Trie was empty: old tail becomes the leaf root. */
        new_root  = v->as.vec.tail;
        new_shift = 0;
    } else if (trie_count == ((size_t)1u << (v->as.vec.shift + MINO_VEC_B))) {
        /* Root is full at the current height: add a level. */
        mino_vec_node_t *grown = vnode_new(S, 2, 0);
        grown->slots[0] = v->as.vec.root;
        grown->slots[1] = new_path(S, v->as.vec.shift, v->as.vec.tail);
        new_root  = grown;
        new_shift = v->as.vec.shift + MINO_VEC_B;
    } else {
        new_root = push_tail(S, v->as.vec.root, v->as.vec.shift, trie_count,
                             v->as.vec.tail);
    }
    return vec_assemble(S, v, new_root, new_tail, 1u, new_shift,
                        v->as.vec.len + 1u);
}

/* Update index i. Index equal to len appends; any other out-of-range call is
 * the caller's responsibility to guard against. */
mino_val_t *vec_assoc1(mino_state_t *S, const mino_val_t *v, size_t i,
                       mino_val_t *item)
{
    size_t           trie_count;
    mino_vec_node_t *new_tail;
    mino_vec_node_t *new_root;
    if (v->as.vec.offset > 0)
        return vec_assoc1(S, vec_materialize(S, v), i, item);
    if (i == v->as.vec.len) {
        return vec_conj1(S, v, item);
    }
    trie_count = v->as.vec.len - v->as.vec.tail_len;
    if (i >= trie_count) {
        /* In the tail: copy and overwrite one slot. */
        new_tail = vnode_clone(S, v->as.vec.tail);
        new_tail->slots[i - trie_count] = item;
        return vec_assemble(S, v, v->as.vec.root, new_tail, v->as.vec.tail_len,
                            v->as.vec.shift, v->as.vec.len);
    }
    /* In the trie: path-copy the spine. */
    new_root = trie_assoc(S, v->as.vec.root, v->as.vec.shift, i, item);
    return vec_assemble(S, v, new_root, v->as.vec.tail, v->as.vec.tail_len,
                        v->as.vec.shift, v->as.vec.len);
}

/*
 * Bulk build from a flat source array — the internal "transient" path.
 *
 * Per-element vec_conj1 is O(log32 n) per append, so a naïve build walks
 * O(n log32 n) work and churns a path-copy on every boundary. vec_from_array
 * skips that: it freezes the last 1..32 elements as the tail, splits the rest
 * into full 32-slot leaves, and folds leaves into the spine layer by layer
 * (32 → 1 each pass). Every node is mutated in place during construction and
 * only becomes part of a persistent vector when vec_assemble returns, so no
 * transient state is observable outside this call.
 *
 * Total work is O(n): one pass writing leaves, then n/32 + n/1024 + ... ≤ n/31
 * more writes up the spine. Caller retains ownership of `items` and elements.
 */
mino_val_t *vec_from_array(mino_state_t *S, mino_val_t **items, size_t len)
{
    mino_vec_node_t  *tail;
    unsigned          tail_len;
    size_t            trie_count;
    size_t            num_leaves;
    mino_vec_node_t **layer;
    size_t            layer_n;
    unsigned          shift;
    size_t            i;
    if (len == 0) {
        return vec_assemble(S, NULL, NULL, NULL, 0u, 0u, 0);
    }
    tail_len = (unsigned)(len % MINO_VEC_WIDTH);
    if (tail_len == 0) {
        tail_len = MINO_VEC_WIDTH;
    }
    /* Internal construction: suppress collection across the whole build so
     * intermediate layer arrays (plain malloc'd; not visible to the GC) stay
     * consistent. No user code runs inside; the next allocation after return
     * will re-enable periodic collection. */
    S->gc_depth++;
    trie_count = len - tail_len;
    tail = vnode_new(S, tail_len, 1);
    memcpy(tail->slots, items + trie_count, tail_len * sizeof(*items));
    if (trie_count == 0) {
        S->gc_depth--;
        return vec_assemble(S, NULL, NULL, tail, tail_len, 0u, len);
    }
    num_leaves = trie_count / MINO_VEC_WIDTH;
    layer = (mino_vec_node_t **)malloc(num_leaves * sizeof(*layer));
    if (layer == NULL) {
        S->gc_depth--;
        set_eval_diag(S, S->eval_current_form, "internal", "MIN001", "out of memory");
        return NULL;
    }
    for (i = 0; i < num_leaves; i++) {
        mino_vec_node_t *leaf = vnode_new(S, MINO_VEC_WIDTH, 1);
        memcpy(leaf->slots, items + i * MINO_VEC_WIDTH,
               MINO_VEC_WIDTH * sizeof(*items));
        layer[i] = leaf;
    }
    layer_n = num_leaves;
    shift   = 0;
    while (layer_n > 1) {
        size_t            next_n = (layer_n + MINO_VEC_WIDTH - 1) / MINO_VEC_WIDTH;
        mino_vec_node_t **next   = (mino_vec_node_t **)malloc(
                                       next_n * sizeof(*next));
        if (next == NULL) {
            free(layer);
            S->gc_depth--;
            set_eval_diag(S, S->eval_current_form, "internal", "MIN001", "out of memory");
            return NULL;
        }
        for (i = 0; i < next_n; i++) {
            size_t           base = i * MINO_VEC_WIDTH;
            size_t           take = layer_n - base;
            mino_vec_node_t *node;
            if (take > MINO_VEC_WIDTH) {
                take = MINO_VEC_WIDTH;
            }
            node = vnode_new(S, (unsigned)take, 0);
            memcpy(node->slots, layer + base, take * sizeof(*layer));
            next[i] = node;
        }
        free(layer);
        layer   = next;
        layer_n = next_n;
        shift  += MINO_VEC_B;
    }
    {
        mino_vec_node_t *root = layer[0];
        free(layer);
        S->gc_depth--;
        return vec_assemble(S, NULL, root, tail, tail_len, shift, len);
    }
}

/*
 * pop_tail: remove the rightmost leaf from the subtree at `node` (level
 * `shift`). Returns the new subtree root, or NULL if the subtree became
 * empty. The removed leaf is returned through *out_leaf.
 */
static mino_vec_node_t *pop_tail(mino_state_t *S,
                                 const mino_vec_node_t *node, unsigned shift,
                                 size_t trie_count,
                                 mino_vec_node_t **out_leaf)
{
    unsigned digit = (unsigned)(((trie_count - 1) >> shift) & MINO_VEC_MASK);
    if (shift == MINO_VEC_B) {
        *out_leaf = (mino_vec_node_t *)node->slots[digit];
        if (digit == 0) return NULL;
        {
            mino_vec_node_t *clone = vnode_clone(S, node);
            clone->slots[digit] = NULL;
            clone->count = digit;
            return clone;
        }
    }
    {
        mino_vec_node_t *child = (mino_vec_node_t *)node->slots[digit];
        mino_vec_node_t *new_child = pop_tail(S, child, shift - MINO_VEC_B,
                                              trie_count, out_leaf);
        if (new_child == NULL && digit == 0) return NULL;
        {
            mino_vec_node_t *clone = vnode_clone(S, node);
            clone->slots[digit] = new_child;
            if (new_child == NULL) clone->count = digit;
            return clone;
        }
    }
}

/* Remove the last element. Returns an empty vector when len == 1.
 * Caller must ensure len > 0. */
mino_val_t *vec_pop(mino_state_t *S, const mino_val_t *v)
{
    size_t new_len;
    if (v->as.vec.offset > 0)
        return vec_pop(S, vec_materialize(S, v));
    new_len = v->as.vec.len - 1;
    if (new_len == 0) {
        return vec_assemble(S, v, NULL, NULL, 0u, 0u, 0);
    }
    if (v->as.vec.tail_len > 1) {
        mino_vec_node_t *new_tail = vnode_clone(S, v->as.vec.tail);
        new_tail->slots[v->as.vec.tail_len - 1] = NULL;
        new_tail->count = v->as.vec.tail_len - 1u;
        return vec_assemble(S, v, v->as.vec.root, new_tail,
                            v->as.vec.tail_len - 1u,
                            v->as.vec.shift, new_len);
    }
    /* tail_len == 1: pull the rightmost trie leaf as the new tail. */
    if (v->as.vec.root == NULL) {
        /* Should not happen (len >= 2 and tail_len == 1 implies trie
         * has at least one leaf), but guard defensively. */
        return vec_assemble(S, v, NULL, NULL, 0u, 0u, 0);
    }
    {
        size_t           trie_count = v->as.vec.len - v->as.vec.tail_len;
        mino_vec_node_t *new_leaf   = NULL;
        mino_vec_node_t *new_root;
        unsigned         new_shift  = v->as.vec.shift;
        if (v->as.vec.shift == 0) {
            /* Root is the only leaf; it becomes the new tail. */
            new_leaf  = v->as.vec.root;
            new_root  = NULL;
            new_shift = 0;
        } else {
            new_root = pop_tail(S, v->as.vec.root, v->as.vec.shift,
                                trie_count, &new_leaf);
            /* Shrink height if root has only one child. */
            if (new_root != NULL && new_root->count == 1
                && new_shift > 0) {
                new_root  = (mino_vec_node_t *)new_root->slots[0];
                new_shift -= MINO_VEC_B;
            }
        }
        return vec_assemble(S, v, new_root, new_leaf,
                            new_leaf != NULL ? new_leaf->count : 0u,
                            new_shift, new_len);
    }
}

/* O(1) subvec: share the backing trie with an offset and reduced len. */
mino_val_t *vec_subvec(mino_state_t *S, const mino_val_t *v,
                       size_t start, size_t end)
{
    mino_val_t *sv;
    if (start == 0 && end == v->as.vec.len)
        return (mino_val_t *)v;
    if (start == end)
        return vec_from_array(S, NULL, 0);
    sv = alloc_val(S, MINO_VECTOR);
    sv->as.vec.root     = v->as.vec.root;
    sv->as.vec.tail     = v->as.vec.tail;
    sv->as.vec.tail_len = v->as.vec.tail_len;
    sv->as.vec.shift    = v->as.vec.shift;
    sv->as.vec.len      = end - start;
    sv->as.vec.offset   = v->as.vec.offset + start;
    sv->as.vec.blen     = v->as.vec.blen;
    sv->meta            = v->meta;
    return sv;
}

mino_val_t *mino_vector(mino_state_t *S, mino_val_t **items, size_t len)
{
    return vec_from_array(S, items, len);
}
