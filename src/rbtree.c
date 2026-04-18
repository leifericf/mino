/*
 * rbtree.c -- persistent red-black tree for sorted maps and sets.
 *
 * Left-leaning red-black tree (LLRB) with path-copying for structural
 * sharing. All mutations return fresh nodes along the walked spine,
 * leaving the source tree intact.
 */

#include "mino_internal.h"

/* --- Comparison ---------------------------------------------------------- */

/* Natural ordering for mino values. Shared with sort in prim_sequences.c. */
int val_compare(const mino_val_t *a, const mino_val_t *b)
{
    if (a == b) return 0;
    if (a == NULL || a->type == MINO_NIL) return -1;
    if (b == NULL || b->type == MINO_NIL) return 1;
    if (a->type == MINO_INT && b->type == MINO_INT) {
        return a->as.i < b->as.i ? -1 : a->as.i > b->as.i ? 1 : 0;
    }
    if (a->type == MINO_FLOAT && b->type == MINO_FLOAT) {
        return a->as.f < b->as.f ? -1 : a->as.f > b->as.f ? 1 : 0;
    }
    if (a->type == MINO_INT && b->type == MINO_FLOAT) {
        double da = (double)a->as.i;
        return da < b->as.f ? -1 : da > b->as.f ? 1 : 0;
    }
    if (a->type == MINO_FLOAT && b->type == MINO_INT) {
        double db = (double)b->as.i;
        return a->as.f < db ? -1 : a->as.f > db ? 1 : 0;
    }
    if ((a->type == MINO_STRING || a->type == MINO_SYMBOL
         || a->type == MINO_KEYWORD) && a->type == b->type) {
        size_t min_len = a->as.s.len < b->as.s.len ? a->as.s.len : b->as.s.len;
        int c = memcmp(a->as.s.data, b->as.s.data, min_len);
        if (c != 0) return c;
        return a->as.s.len < b->as.s.len ? -1
             : a->as.s.len > b->as.s.len ? 1 : 0;
    }
    if (a->type == MINO_VECTOR && b->type == MINO_VECTOR) {
        size_t i;
        size_t min_len = a->as.vec.len < b->as.vec.len
                       ? a->as.vec.len : b->as.vec.len;
        for (i = 0; i < min_len; i++) {
            int c = val_compare(vec_nth(a, i), vec_nth(b, i));
            if (c != 0) return c;
        }
        return a->as.vec.len < b->as.vec.len ? -1
             : a->as.vec.len > b->as.vec.len ? 1 : 0;
    }
    return a->type < b->type ? -1 : a->type > b->type ? 1 : 0;
}

/* Compare with optional custom comparator function. */
int rb_compare(mino_state_t *S, const mino_val_t *a, const mino_val_t *b,
               mino_val_t *comparator)
{
    if (comparator == NULL) return val_compare(a, b);
    {
        mino_val_t *args = mino_cons(S, (mino_val_t *)a,
                               mino_cons(S, (mino_val_t *)b, mino_nil(S)));
        mino_val_t *result = mino_call(S, comparator, args, NULL);
        if (result == NULL) return 0;
        if (result->type == MINO_INT)
            return result->as.i < 0 ? -1 : result->as.i > 0 ? 1 : 0;
        if (result->type == MINO_FLOAT)
            return result->as.f < 0 ? -1 : result->as.f > 0 ? 1 : 0;
        return mino_is_truthy(result) ? -1 : 1;
    }
}

/* --- Node helpers -------------------------------------------------------- */

static mino_rb_node_t *rb_new(mino_state_t *S, mino_val_t *key,
                               mino_val_t *val, int red)
{
    mino_rb_node_t *n = (mino_rb_node_t *)gc_alloc_typed(
        S, GC_T_RB_NODE, sizeof(*n));
    n->key   = key;
    n->val   = val;
    n->left  = NULL;
    n->right = NULL;
    n->red   = (unsigned char)red;
    return n;
}

static mino_rb_node_t *rb_clone(mino_state_t *S, const mino_rb_node_t *src)
{
    mino_rb_node_t *n = (mino_rb_node_t *)gc_alloc_typed(
        S, GC_T_RB_NODE, sizeof(*n));
    memcpy(n, src, sizeof(*n));
    return n;
}

static int is_red(const mino_rb_node_t *n)
{
    return n != NULL && n->red;
}

/* LLRB rotations and color flip -- all path-copy. */

static mino_rb_node_t *rotate_left(mino_state_t *S, const mino_rb_node_t *h)
{
    mino_rb_node_t *x  = rb_clone(S, h->right);
    mino_rb_node_t *hc = rb_clone(S, h);
    hc->right = x->left;
    x->left   = hc;
    x->red    = hc->red;
    hc->red   = 1;
    return x;
}

static mino_rb_node_t *rotate_right(mino_state_t *S, const mino_rb_node_t *h)
{
    mino_rb_node_t *x  = rb_clone(S, h->left);
    mino_rb_node_t *hc = rb_clone(S, h);
    hc->left  = x->right;
    x->right  = hc;
    x->red    = hc->red;
    hc->red   = 1;
    return x;
}

static mino_rb_node_t *flip_colors(mino_state_t *S, const mino_rb_node_t *h)
{
    mino_rb_node_t *hc = rb_clone(S, h);
    mino_rb_node_t *lc = rb_clone(S, h->left);
    mino_rb_node_t *rc = rb_clone(S, h->right);
    hc->red = !hc->red;
    lc->red = !lc->red;
    rc->red = !rc->red;
    hc->left  = lc;
    hc->right = rc;
    return hc;
}

static mino_rb_node_t *fixup(mino_state_t *S, mino_rb_node_t *h)
{
    if (is_red(h->right) && !is_red(h->left))
        h = rotate_left(S, h);
    if (is_red(h->left) && is_red(h->left->left))
        h = rotate_right(S, h);
    if (is_red(h->left) && is_red(h->right))
        h = flip_colors(S, h);
    return h;
}

/* --- Lookup -------------------------------------------------------------- */

mino_val_t *rb_get(mino_state_t *S, const mino_rb_node_t *n,
                   const mino_val_t *key, mino_val_t *comparator)
{
    while (n != NULL) {
        int cmp = rb_compare(S, key, n->key, comparator);
        if (cmp < 0)      n = n->left;
        else if (cmp > 0)  n = n->right;
        else               return n->val;
    }
    return NULL;
}

int rb_contains(mino_state_t *S, const mino_rb_node_t *n,
                const mino_val_t *key, mino_val_t *comparator)
{
    while (n != NULL) {
        int cmp = rb_compare(S, key, n->key, comparator);
        if (cmp < 0)      n = n->left;
        else if (cmp > 0)  n = n->right;
        else               return 1;
    }
    return 0;
}

/* --- Insertion ----------------------------------------------------------- */

static mino_rb_node_t *rb_insert(mino_state_t *S, const mino_rb_node_t *n,
                                  mino_val_t *key, mino_val_t *val,
                                  mino_val_t *comparator, int *replaced)
{
    mino_rb_node_t *h;
    int cmp;
    if (n == NULL) return rb_new(S, key, val, 1);
    cmp = rb_compare(S, key, n->key, comparator);
    h = rb_clone(S, n);
    if (cmp < 0)       h->left  = rb_insert(S, n->left, key, val, comparator, replaced);
    else if (cmp > 0)  h->right = rb_insert(S, n->right, key, val, comparator, replaced);
    else {
        h->val = val;
        *replaced = 1;
    }
    return fixup(S, h);
}

mino_rb_node_t *rb_assoc(mino_state_t *S, const mino_rb_node_t *n,
                          mino_val_t *key, mino_val_t *val,
                          mino_val_t *comparator, int *replaced)
{
    mino_rb_node_t *root;
    *replaced = 0;
    root = rb_insert(S, n, key, val, comparator, replaced);
    if (root->red) {
        mino_rb_node_t *rc = rb_clone(S, root);
        rc->red = 0;
        return rc;
    }
    return root;
}

/* --- Deletion ------------------------------------------------------------ */

static mino_rb_node_t *move_red_left(mino_state_t *S, const mino_rb_node_t *h)
{
    mino_rb_node_t *hc = flip_colors(S, h);
    if (is_red(hc->right->left)) {
        mino_rb_node_t *rc = rotate_right(S, hc->right);
        mino_rb_node_t *hc2 = rb_clone(S, hc);
        hc2->right = rc;
        hc2 = rotate_left(S, hc2);
        return flip_colors(S, hc2);
    }
    return hc;
}

static mino_rb_node_t *move_red_right(mino_state_t *S, const mino_rb_node_t *h)
{
    mino_rb_node_t *hc = flip_colors(S, h);
    if (is_red(hc->left->left)) {
        hc = rotate_right(S, hc);
        return flip_colors(S, hc);
    }
    return hc;
}

static const mino_rb_node_t *rb_min(const mino_rb_node_t *n)
{
    while (n->left != NULL) n = n->left;
    return n;
}

static mino_rb_node_t *rb_delete_min(mino_state_t *S, const mino_rb_node_t *h)
{
    mino_rb_node_t *hc;
    if (h->left == NULL) return NULL;
    hc = rb_clone(S, h);
    if (!is_red(h->left) && !is_red(h->left->left))
        hc = move_red_left(S, hc);
    hc->left = rb_delete_min(S, hc->left);
    return fixup(S, hc);
}

static mino_rb_node_t *rb_delete(mino_state_t *S, const mino_rb_node_t *h,
                                  const mino_val_t *key,
                                  mino_val_t *comparator)
{
    mino_rb_node_t *hc;
    int cmp;
    if (h == NULL) return NULL;
    cmp = rb_compare(S, key, h->key, comparator);
    hc = rb_clone(S, h);
    if (cmp < 0) {
        if (hc->left == NULL) return hc; /* key not found */
        if (!is_red(h->left) && !is_red(h->left->left))
            hc = move_red_left(S, hc);
        hc->left = rb_delete(S, hc->left, key, comparator);
    } else {
        if (is_red(h->left))
            hc = rotate_right(S, hc);
        if (rb_compare(S, key, hc->key, comparator) == 0 && hc->right == NULL)
            return NULL;
        if (hc->right != NULL) {
            if (!is_red(hc->right) && !is_red(hc->right->left))
                hc = move_red_right(S, hc);
            if (rb_compare(S, key, hc->key, comparator) == 0) {
                const mino_rb_node_t *m = rb_min(hc->right);
                hc->key   = m->key;
                hc->val   = m->val;
                hc->right = rb_delete_min(S, hc->right);
            } else {
                hc->right = rb_delete(S, hc->right, key, comparator);
            }
        }
    }
    return fixup(S, hc);
}

mino_rb_node_t *rb_dissoc(mino_state_t *S, const mino_rb_node_t *n,
                           const mino_val_t *key, mino_val_t *comparator)
{
    mino_rb_node_t *root;
    if (n == NULL) return NULL;
    root = rb_delete(S, n, key, comparator);
    if (root != NULL && root->red) {
        mino_rb_node_t *rc = rb_clone(S, root);
        rc->red = 0;
        return rc;
    }
    return root;
}

/* --- In-order traversal to cons list ------------------------------------- */

void rb_to_list(mino_state_t *S, const mino_rb_node_t *n,
                mino_val_t **head, mino_val_t **tail)
{
    if (n == NULL) return;
    rb_to_list(S, n->left, head, tail);
    {
        mino_val_t *cell = mino_cons(S, n->key, mino_nil(S));
        if (*tail == NULL) *head = cell;
        else (*tail)->as.cons.cdr = cell;
        *tail = cell;
    }
    rb_to_list(S, n->right, head, tail);
}

/* --- Equality (allocation-free, for use by mino_eq) ---------------------- */

int rb_trees_equal(const mino_rb_node_t *a, const mino_rb_node_t *b,
                   int compare_vals)
{
    if (a == b) return 1;
    if (a == NULL || b == NULL) return 0;
    if (!mino_eq(a->key, b->key)) return 0;
    if (compare_vals && !mino_eq(a->val, b->val)) return 0;
    return rb_trees_equal(a->left, b->left, compare_vals)
        && rb_trees_equal(a->right, b->right, compare_vals);
}

/* --- Constructors -------------------------------------------------------- */

mino_val_t *mino_sorted_map(mino_state_t *S, mino_val_t **keys,
                             mino_val_t **vals, size_t len)
{
    mino_val_t     *v;
    mino_rb_node_t *root = NULL;
    size_t i;
    S->gc_depth++;
    v = alloc_val(S, MINO_SORTED_MAP);
    v->as.sorted.comparator = NULL;
    v->as.sorted.len = 0;
    for (i = 0; i < len; i++) {
        int replaced = 0;
        root = rb_assoc(S, root, keys[i], vals[i], NULL, &replaced);
        if (!replaced) v->as.sorted.len++;
    }
    v->as.sorted.root = root;
    S->gc_depth--;
    return v;
}

mino_val_t *mino_sorted_set(mino_state_t *S, mino_val_t **items, size_t len)
{
    mino_val_t     *v;
    mino_rb_node_t *root = NULL;
    size_t i;
    S->gc_depth++;
    v = alloc_val(S, MINO_SORTED_SET);
    v->as.sorted.comparator = NULL;
    v->as.sorted.len = 0;
    for (i = 0; i < len; i++) {
        int replaced = 0;
        root = rb_assoc(S, root, items[i], NULL, NULL, &replaced);
        if (!replaced) v->as.sorted.len++;
    }
    v->as.sorted.root = root;
    S->gc_depth--;
    return v;
}

/* --- High-level operations for prim integration -------------------------- */

mino_val_t *sorted_map_assoc1(mino_state_t *S, const mino_val_t *m,
                               mino_val_t *key, mino_val_t *val)
{
    int replaced = 0;
    mino_val_t *nv = alloc_val(S, MINO_SORTED_MAP);
    nv->as.sorted.comparator = m->as.sorted.comparator;
    nv->as.sorted.root = rb_assoc(S, m->as.sorted.root, key, val,
                                   m->as.sorted.comparator, &replaced);
    nv->as.sorted.len = m->as.sorted.len + (replaced ? 0 : 1);
    return nv;
}

mino_val_t *sorted_map_dissoc1(mino_state_t *S, const mino_val_t *m,
                                const mino_val_t *key)
{
    mino_rb_node_t *nr = rb_dissoc(S, m->as.sorted.root, key,
                                    m->as.sorted.comparator);
    if (nr == m->as.sorted.root) return (mino_val_t *)m;
    {
        mino_val_t *nv = alloc_val(S, MINO_SORTED_MAP);
        nv->as.sorted.comparator = m->as.sorted.comparator;
        nv->as.sorted.root = nr;
        nv->as.sorted.len = m->as.sorted.len - 1;
        return nv;
    }
}

mino_val_t *sorted_set_conj1(mino_state_t *S, const mino_val_t *s,
                              mino_val_t *elem)
{
    int replaced = 0;
    mino_val_t *nv = alloc_val(S, MINO_SORTED_SET);
    nv->as.sorted.comparator = s->as.sorted.comparator;
    nv->as.sorted.root = rb_assoc(S, s->as.sorted.root, elem, NULL,
                                   s->as.sorted.comparator, &replaced);
    nv->as.sorted.len = s->as.sorted.len + (replaced ? 0 : 1);
    return nv;
}

mino_val_t *sorted_set_disj1(mino_state_t *S, const mino_val_t *s,
                              const mino_val_t *elem)
{
    mino_rb_node_t *nr = rb_dissoc(S, s->as.sorted.root, elem,
                                    s->as.sorted.comparator);
    if (nr == s->as.sorted.root) return (mino_val_t *)s;
    {
        mino_val_t *nv = alloc_val(S, MINO_SORTED_SET);
        nv->as.sorted.comparator = s->as.sorted.comparator;
        nv->as.sorted.root = nr;
        nv->as.sorted.len = s->as.sorted.len - 1;
        return nv;
    }
}

mino_val_t *sorted_seq(mino_state_t *S, const mino_val_t *coll)
{
    mino_val_t *head = mino_nil(S), *tail = NULL;
    if (coll->as.sorted.len == 0) return mino_nil(S);
    if (coll->type == MINO_SORTED_MAP) {
        mino_val_t *keys = mino_nil(S), *kt = NULL;
        rb_to_list(S, coll->as.sorted.root, &keys, &kt);
        while (mino_is_cons(keys)) {
            mino_val_t *kv[2], *cell;
            kv[0] = keys->as.cons.car;
            kv[1] = rb_get(S, coll->as.sorted.root, kv[0],
                           coll->as.sorted.comparator);
            cell = mino_cons(S, mino_vector(S, kv, 2), mino_nil(S));
            if (tail == NULL) head = cell; else tail->as.cons.cdr = cell;
            tail = cell;
            keys = keys->as.cons.cdr;
        }
    } else {
        rb_to_list(S, coll->as.sorted.root, &head, &tail);
    }
    return head;
}

mino_val_t *sorted_rest(mino_state_t *S, const mino_val_t *coll)
{
    mino_val_t *s;
    if (coll->as.sorted.len <= 1) return mino_nil(S);
    s = sorted_seq(S, coll);
    return mino_is_cons(s) ? s->as.cons.cdr : mino_nil(S);
}
