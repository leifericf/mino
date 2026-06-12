/*
 * map.c -- persistent hash array mapped trie (HAMT) map and set.
 *
 * Hash utilities (fnv_mix, fnv_bytes, hash_val) live in map_hash.c.
 * Owned-edit (transient) HAMT walks live in map_owned.c.
 * Both are compiled as sibling TUs in src/collections/.
 */

#include "runtime/internal.h"

/* ------------------------------------------------------------------------- */
/* Persistent map: 32-wide HAMT + insertion-order companion vector           */
/* ------------------------------------------------------------------------- */
/*
 * Each map value carries a HAMT root (for O(log32 n) lookup) and a companion
 * MINO_VECTOR of keys in insertion order (for iteration). assoc consults the
 * HAMT to decide whether the key is new (append to key_order) or a rebinding
 * (leave key_order alone; only the HAMT value changes).
 *
 * HAMT nodes come in two shapes, distinguished by collision_count:
 *   - Bitmap node (collision_count == 0):
 *       bitmap — which of 32 hash-digit slots are populated.
 *       subnode_mask — of populated slots, which hold a child node (the rest
 *         hold a direct (key, value) entry).
 *       slots — length popcount(bitmap), packed by slot index.
 *   - Collision bucket (collision_count > 0):
 *       bucket of full-hash-equal entries; slots[] holds hamt_entry_t*.
 *
 * Splitting and merging handle the depth-bounded hash: descent through five
 * bits per level admits up to seven levels before the 32-bit hash is
 * exhausted; past that, same-hash keys coexist in a collision bucket.
 *
 * The public map struct is immutable once assembled — every assoc returns a
 * fresh root that shares unmodified subtrees with its predecessor.
 */

/* Types and constants in collections/internal.h */

unsigned popcount32(uint32_t x)
{
    unsigned c = 0;
    while (x != 0) {
        x &= x - 1u;
        c++;
    }
    return c;
}

hamt_entry_t *hamt_entry_new(mino_state *S, mino_val *key, mino_val *val)
{
    hamt_entry_t *e = (hamt_entry_t *)gc_alloc_typed(
        S, GC_T_HAMT_ENTRY, sizeof(*e));
    e->key = key;
    e->val = val;
    return e;
}

static mino_hamt_node_t *hamt_bitmap_node(mino_state *S, uint32_t bitmap,
                                           uint32_t subnode_mask, void **slots)
{
    mino_hamt_node_t *n = (mino_hamt_node_t *)gc_alloc_typed(
        S, GC_T_HAMT_NODE, sizeof(*n));
    n->bitmap       = bitmap;
    n->subnode_mask = subnode_mask;
    n->slots        = slots;
    return n;
}

static mino_hamt_node_t *hamt_collision_node(mino_state *S, uint32_t hash,
                                              void **slots, unsigned count)
{
    mino_hamt_node_t *n = (mino_hamt_node_t *)gc_alloc_typed(
        S, GC_T_HAMT_NODE, sizeof(*n));
    n->collision_hash  = hash;
    n->collision_count = count;
    n->slots           = slots;
    return n;
}

/*
 * merge_entries: build the smallest subtree that separates two leaf entries
 * whose hashes collide at `shift - HAMT_B` (the parent level). The returned
 * subtree lives at level `shift`.
 */
static mino_hamt_node_t *merge_entries(mino_state *S, hamt_entry_t *e1, uint32_t h1,
                                        hamt_entry_t *e2, uint32_t h2,
                                        unsigned shift)
{
    if (h1 == h2 || shift >= 32u) {
        /* Can't separate further: collision bucket. */
        void **slots = (void **)gc_alloc_typed(S, GC_T_PTRARR, 2 * sizeof(*slots));
        if (slots == NULL) { return NULL; }
        slots[0] = e1;
        slots[1] = e2;
        return hamt_collision_node(S, h1, slots, 2);
    }
    {
        unsigned i1 = (unsigned)((h1 >> shift) & HAMT_MASK);
        unsigned i2 = (unsigned)((h2 >> shift) & HAMT_MASK);
        if (i1 == i2) {
            mino_hamt_node_t *child = merge_entries(S, e1, h1, e2, h2,
                                                     shift + HAMT_B);
            void **slots = (void **)gc_alloc_typed(S, GC_T_PTRARR, sizeof(*slots));
            if (slots == NULL) { return NULL; }
            slots[0] = child;
            return hamt_bitmap_node(S, 1u << i1, 1u << i1, slots);
        } else {
            void **slots = (void **)gc_alloc_typed(S, GC_T_PTRARR, 2 * sizeof(*slots));
            if (slots == NULL) { return NULL; }
            if (i1 < i2) {
                slots[0] = e1; slots[1] = e2;
            } else {
                slots[0] = e2; slots[1] = e1;
            }
            return hamt_bitmap_node(S, (1u << i1) | (1u << i2), 0u, slots);
        }
    }
}

/*
 * hamt_assoc: insert or rebind `new_entry` in the subtree rooted at `n`.
 * Sets *replaced = 1 when the key was already present (so the map's len
 * and key_order don't grow).
 */
mino_hamt_node_t *hamt_assoc(mino_state *S, const mino_hamt_node_t *n,
                                     hamt_entry_t *new_entry, uint32_t h,
                                     unsigned shift, int *replaced)
{
    if (n == NULL) {
        unsigned  i     = (unsigned)((h >> shift) & HAMT_MASK);
        void    **slots = (void **)gc_alloc_typed(S, GC_T_PTRARR, sizeof(*slots));
        if (slots == NULL) { return NULL; }
        slots[0] = new_entry;
        return hamt_bitmap_node(S, 1u << i, 0u, slots);
    }
    if (n->collision_count > 0) {
        /* Either hash matches the bucket's (update or append) or it doesn't
         * (promote to a bitmap node that routes them separately). */
        if (h == n->collision_hash) {
            unsigned j;
            for (j = 0; j < n->collision_count; j++) {
                hamt_entry_t *e = (hamt_entry_t *)n->slots[j];
                if (mino_eq(e->key, new_entry->key)) {
                    void **slots = (void **)gc_alloc_typed(S, GC_T_PTRARR, n->collision_count * sizeof(*slots));
                    unsigned k;
                    if (slots == NULL) { return NULL; }
                    for (k = 0; k < n->collision_count; k++) { slots[k] = n->slots[k]; }
                    slots[j] = new_entry;
                    *replaced = 1;
                    return hamt_collision_node(S, h, slots, n->collision_count);
                }
            }
            {
                void **slots = (void **)gc_alloc_typed(S, GC_T_PTRARR, (n->collision_count + 1u) * sizeof(*slots));
                unsigned k;
                if (slots == NULL) { return NULL; }
                for (k = 0; k < n->collision_count; k++) { slots[k] = n->slots[k]; }
                slots[n->collision_count] = new_entry;
                return hamt_collision_node(S, h, slots, n->collision_count + 1u);
            }
        }
        {
            /* Promote: wrap the collision bucket so it lives in one slot of
             * a bitmap node at this level, then insert the new entry. */
            unsigned  ib     = (unsigned)((n->collision_hash >> shift) & HAMT_MASK);
            unsigned  in     = (unsigned)((h               >> shift) & HAMT_MASK);
            if (ib == in) {
                /* Deeper shared prefix: descend. */
                mino_hamt_node_t *sub   = hamt_assoc(S, n, new_entry, h,
                                                      shift + HAMT_B, replaced);
                void            **slots = (void **)gc_alloc_typed(S, GC_T_PTRARR, sizeof(*slots));
                if (slots == NULL) { return NULL; }
                slots[0] = sub;
                return hamt_bitmap_node(S, 1u << ib, 1u << ib, slots);
            } else {
                void **slots = (void **)gc_alloc_typed(S, GC_T_PTRARR, 2 * sizeof(*slots));
                uint32_t bitmap       = (1u << ib) | (1u << in);
                uint32_t subnode_mask = 1u << ib;
                if (slots == NULL) { return NULL; }
                if (ib < in) {
                    slots[0] = (void *)n;
                    slots[1] = new_entry;
                } else {
                    slots[0] = new_entry;
                    slots[1] = (void *)n;
                }
                return hamt_bitmap_node(S, bitmap, subnode_mask, slots);
            }
        }
    }
    {
        unsigned  i    = (unsigned)((h >> shift) & HAMT_MASK);
        uint32_t  bit  = 1u << i;
        unsigned  phys = popcount32(n->bitmap & (bit - 1u));
        unsigned  pop  = popcount32(n->bitmap);
        if ((n->bitmap & bit) == 0) {
            /* Empty slot: insert directly. */
            void **slots = (void **)gc_alloc_typed(S, GC_T_PTRARR, (pop + 1u) * sizeof(*slots));
            unsigned k;
            if (slots == NULL) { return NULL; }
            for (k = 0; k < phys; k++)        { slots[k]        = n->slots[k];       }
            slots[phys] = new_entry;
            for (k = phys; k < pop; k++)      { slots[k + 1]    = n->slots[k];       }
            return hamt_bitmap_node(S, n->bitmap | bit, n->subnode_mask, slots);
        }
        if (n->subnode_mask & bit) {
            /* Child subtree: recurse, then rewrap. */
            mino_hamt_node_t *new_child = hamt_assoc(S,
                (const mino_hamt_node_t *)n->slots[phys], new_entry, h,
                shift + HAMT_B, replaced);
            void **slots = (void **)gc_alloc_typed(S, GC_T_PTRARR, pop * sizeof(*slots));
            unsigned k;
            if (slots == NULL) { return NULL; }
            for (k = 0; k < pop; k++) { slots[k] = n->slots[k]; }
            slots[phys] = new_child;
            return hamt_bitmap_node(S, n->bitmap, n->subnode_mask, slots);
        }
        {
            /* Leaf entry in slot. Same key → replace. Different key → split. */
            hamt_entry_t *existing = (hamt_entry_t *)n->slots[phys];
            if (mino_eq(existing->key, new_entry->key)) {
                void **slots = (void **)gc_alloc_typed(S, GC_T_PTRARR, pop * sizeof(*slots));
                unsigned k;
                if (slots == NULL) { return NULL; }
                for (k = 0; k < pop; k++) { slots[k] = n->slots[k]; }
                slots[phys] = new_entry;
                *replaced = 1;
                return hamt_bitmap_node(S, n->bitmap, n->subnode_mask, slots);
            }
            {
                uint32_t          eh   = hash_val(existing->key);
                mino_hamt_node_t *sub  = merge_entries(S, existing, eh,
                                                        new_entry, h,
                                                        shift + HAMT_B);
                void            **slots = (void **)gc_alloc_typed(S, GC_T_PTRARR, pop * sizeof(*slots));
                unsigned k;
                if (slots == NULL) { return NULL; }
                for (k = 0; k < pop; k++) { slots[k] = n->slots[k]; }
                slots[phys] = sub;
                return hamt_bitmap_node(S, n->bitmap,
                                         n->subnode_mask | bit, slots);
            }
        }
    }
}

/* Look up a key; returns NULL if absent. */
mino_val *hamt_get(const mino_hamt_node_t *n, const mino_val *key,
                             uint32_t h, unsigned shift)
{
    while (n != NULL) {
        if (n->collision_count > 0) {
            unsigned i;
            if (h != n->collision_hash) {
                return NULL;
            }
            for (i = 0; i < n->collision_count; i++) {
                hamt_entry_t *e = (hamt_entry_t *)n->slots[i];
                if (mino_eq(e->key, key)) {
                    return e->val;
                }
            }
            return NULL;
        }
        {
            uint32_t bit = 1u << ((h >> shift) & HAMT_MASK);
            unsigned phys;
            if ((n->bitmap & bit) == 0) {
                return NULL;
            }
            phys = popcount32(n->bitmap & (bit - 1u));
            if (n->subnode_mask & bit) {
                n = (const mino_hamt_node_t *)n->slots[phys];
                shift += HAMT_B;
            } else {
                hamt_entry_t *e = (hamt_entry_t *)n->slots[phys];
                return mino_eq(e->key, key) ? e->val : NULL;
            }
        }
    }
    return NULL;
}

/* ------------------------------------------------------------------------- */
/* Flatmap: small-persistent-map fast path                                   */
/* ------------------------------------------------------------------------- */
/*
 * For len <= MINO_FLATMAP_THRESHOLD, we skip the HAMT entirely and keep
 * keys + values in two parallel insertion-order vectors. Lookup is a
 * linear scan via mino_eq -- which short-circuits on pointer-eq for
 * the typical keyword key case, so the inner loop is N pointer
 * compares + at most one structural compare. Construction skips the
 * per-entry hash + hamt_entry_t alloc + bitmap-node alloc.
 *
 * Promotion (flat -> HAMT) happens lazily at the assoc that pushes len
 * past the threshold. Demotion is intentionally never done: a map that
 * was once HAMT stays HAMT even after dissoc shrinks it, so callers
 * that thrash around the boundary don't pay re-build cost on every
 * write.
 */

/* Linear scan for `key` in a flat key_order vector. Returns the
 * matching index, or len when absent. */
static size_t flat_find_index(const mino_val *key_order,
                              const mino_val *key, size_t len)
{
    size_t i;
    for (i = 0; i < len; i++) {
        if (mino_eq(vec_nth(key_order, i), key)) return i;
    }
    return len;
}

/* Build a HAMT from a parallel (key_order, val_order) pair of vectors.
 * Used by the flat -> HAMT promotion path. Caller is responsible for
 * gc_depth suppression around the call. */
static mino_hamt_node_t *hamt_from_flat(mino_state *S,
                                         const mino_val *key_order,
                                         const mino_val *val_order,
                                         size_t len)
{
    mino_hamt_node_t *root = NULL;
    size_t i;
    for (i = 0; i < len; i++) {
        mino_val   *k = vec_nth(key_order, i);
        mino_val   *vv = vec_nth(val_order, i);
        hamt_entry_t *e  = hamt_entry_new(S, k, vv);
        uint32_t      h  = hash_val(k);
        int           replaced = 0;
        if (e == NULL) return NULL;
        root = hamt_assoc(S, root, e, h, 0u, &replaced);
        if (root == NULL) return NULL;
    }
    return root;
}

/* Convenience: look up a key in a map value. */
mino_val *map_get_val(const mino_val *m, const mino_val *key)
{
    return mino_map_lookup(m, key);
}

mino_val *mino_map_lookup(const mino_val *m, const mino_val *key)
{
    if (m == NULL || m->as.map.len == 0) return NULL;
    if (m->as.map.val_order != NULL) {
        /* Flatmap: linear scan. */
        size_t i = flat_find_index(m->as.map.key_order, key, m->as.map.len);
        if (i == m->as.map.len) return NULL;
        return vec_nth(m->as.map.val_order, i);
    }
    {
        uint32_t h = hash_val(key);
        return hamt_get(m->as.map.root, key, h, 0u);
    }
}

mino_val *mino_map_assoc1(mino_state *S, mino_val *m,
                            mino_val *key, mino_val *val)
{
    mino_val *out;
    /* Identity short-circuit: if the key already maps to a value that
     * compares `mino_eq`-equal to `val`, return m unchanged. The
     * cached-hash on collection values keeps this O(1) in the miss
     * case for sequable vals; for cheap scalar vals the structural
     * compare is constant-time anyway. Lets `(identical? m (assoc m
     * k (get m k)))` hold and saves the rebuild traffic on the
     * surprisingly common "replace with current value" idiom. */
    if (m != NULL && m->as.map.len > 0) {
        mino_val *existing = map_get_val(m, key);
        if (existing != NULL && mino_eq(existing, val)) return m;
    }
    mino_current_ctx(S)->gc_depth++;
    out = alloc_val(S, MINO_MAP);
    if (m == NULL || m->as.map.len == 0) {
        mino_val *ko = mino_vector(S, NULL, 0);
        mino_val *vo = mino_vector(S, NULL, 0);
        ko = vec_conj1(S, ko, key);
        vo = vec_conj1(S, vo, val);
        out->as.map.root      = NULL;
        out->as.map.key_order = ko;
        out->as.map.val_order = vo;
        out->as.map.len       = 1;
        if (m != NULL) out->meta = m->meta;
        mino_current_ctx(S)->gc_depth--;
        return out;
    }
    if (m->as.map.val_order != NULL) {
        /* Flatmap source. */
        size_t      old_len = m->as.map.len;
        size_t      i       = flat_find_index(m->as.map.key_order, key, old_len);
        mino_val *ko, *vo;
        if (i < old_len) {
            /* Replace existing value in place. */
            ko = m->as.map.key_order;
            vo = vec_assoc1(S, m->as.map.val_order, i, val);
            out->as.map.root      = NULL;
            out->as.map.key_order = ko;
            out->as.map.val_order = vo;
            out->as.map.len       = old_len;
            out->meta             = m->meta;
            mino_current_ctx(S)->gc_depth--;
            return out;
        }
        /* New key: append. Promote to HAMT if we'd exceed threshold. */
        if (old_len + 1 > MINO_FLATMAP_THRESHOLD) {
            mino_hamt_node_t *root;
            int               replaced = 0;
            hamt_entry_t     *e;
            ko = vec_conj1(S, m->as.map.key_order, key);
            root = hamt_from_flat(S, m->as.map.key_order,
                                   m->as.map.val_order, old_len);
            if (root == NULL) { mino_current_ctx(S)->gc_depth--; return NULL; }
            e = hamt_entry_new(S, key, val);
            if (e == NULL) { mino_current_ctx(S)->gc_depth--; return NULL; }
            root = hamt_assoc(S, root, e, hash_val(key), 0u, &replaced);
            if (root == NULL) { mino_current_ctx(S)->gc_depth--; return NULL; }
            out->as.map.root      = root;
            out->as.map.key_order = ko;
            out->as.map.val_order = NULL;
            out->as.map.len       = old_len + 1;
            out->meta             = m->meta;
            mino_current_ctx(S)->gc_depth--;
            return out;
        }
        ko = vec_conj1(S, m->as.map.key_order, key);
        vo = vec_conj1(S, m->as.map.val_order, val);
        out->as.map.root      = NULL;
        out->as.map.key_order = ko;
        out->as.map.val_order = vo;
        out->as.map.len       = old_len + 1;
        out->meta             = m->meta;
        mino_current_ctx(S)->gc_depth--;
        return out;
    }
    /* HAMT source. */
    {
        hamt_entry_t     *e        = hamt_entry_new(S, key, val);
        uint32_t          h        = hash_val(key);
        int               replaced = 0;
        mino_hamt_node_t *root;
        mino_val       *ko       = m->as.map.key_order;
        if (e == NULL) { mino_current_ctx(S)->gc_depth--; return NULL; }
        root = hamt_assoc(S, m->as.map.root, e, h, 0u, &replaced);
        if (root == NULL) { mino_current_ctx(S)->gc_depth--; return NULL; }
        if (!replaced) ko = vec_conj1(S, ko, key);
        out->as.map.root      = root;
        out->as.map.key_order = ko;
        out->as.map.val_order = NULL;
        out->as.map.len       = m->as.map.len + (replaced ? 0 : 1);
        out->meta             = m->meta;
        mino_current_ctx(S)->gc_depth--;
        return out;
    }
}

mino_val *mino_map_dissoc1(mino_state *S, mino_val *m,
                             mino_val *key)
{
    mino_val *out;
    if (m == NULL || m->as.map.len == 0) return m;
    mino_current_ctx(S)->gc_depth++;
    if (m->as.map.val_order != NULL) {
        /* Flatmap: rebuild without the matching slot. */
        size_t old_len = m->as.map.len;
        size_t idx     = flat_find_index(m->as.map.key_order, key, old_len);
        size_t i;
        mino_val *ko, *vo;
        if (idx == old_len) { mino_current_ctx(S)->gc_depth--; return m; }
        ko = mino_vector(S, NULL, 0);
        vo = mino_vector(S, NULL, 0);
        for (i = 0; i < old_len; i++) {
            if (i == idx) continue;
            ko = vec_conj1(S, ko, vec_nth(m->as.map.key_order, i));
            vo = vec_conj1(S, vo, vec_nth(m->as.map.val_order, i));
        }
        out = alloc_val(S, MINO_MAP);
        out->as.map.root      = NULL;
        out->as.map.key_order = ko;
        out->as.map.val_order = vo;
        out->as.map.len       = old_len - 1;
        out->meta             = m->meta;
        mino_current_ctx(S)->gc_depth--;
        return out;
    }
    /* HAMT source. There's no in-place hamt_dissoc here; the existing
     * pattern (mirrored from prim_dissoc) rebuilds the HAMT from
     * key_order minus the dissoc'd key. Bail out early when the key is
     * absent so unchanged maps stay pointer-equal. */
    {
        size_t            old_len = m->as.map.len;
        mino_val       *ko;
        mino_hamt_node_t *root    = NULL;
        size_t            i;
        if (mino_map_lookup(m, key) == NULL) {
            mino_current_ctx(S)->gc_depth--;
            return m;
        }
        ko = mino_vector(S, NULL, 0);
        for (i = 0; i < old_len; i++) {
            mino_val *k = vec_nth(m->as.map.key_order, i);
            if (mino_eq(k, key)) continue;
            ko = vec_conj1(S, ko, k);
        }
        for (i = 0; i < old_len; i++) {
            mino_val   *k = vec_nth(m->as.map.key_order, i);
            mino_val   *vv;
            hamt_entry_t *e;
            int           replaced = 0;
            if (mino_eq(k, key)) continue;
            vv = hamt_get(m->as.map.root, k, hash_val(k), 0u);
            e  = hamt_entry_new(S, k, vv);
            if (e == NULL) { mino_current_ctx(S)->gc_depth--; return NULL; }
            root = hamt_assoc(S, root, e, hash_val(k), 0u, &replaced);
            if (root == NULL) { mino_current_ctx(S)->gc_depth--; return NULL; }
        }
        out = alloc_val(S, MINO_MAP);
        out->as.map.root      = root;
        out->as.map.key_order = ko;
        out->as.map.val_order = NULL;
        out->as.map.len       = old_len - 1;
        out->meta             = m->meta;
        mino_current_ctx(S)->gc_depth--;
        return out;
    }
}

/* ------------------------------------------------------------------------- */
/* Owned-edit map mutators                                                   */
/* ------------------------------------------------------------------------- */
/*
 * mino_map_assoc1_owned / mino_map_dissoc1_owned mirror the persistent
 * mutators but route trie + companion-vector edits through the owner
 * discipline so a long transient batch reuses nodes in place. The
 * MINO_MAP value-head is still freshly allocated per call (matches the
 * vec_assemble pattern); the saving comes from the trie and the
 * key_order / val_order vectors.
 */

mino_val *mino_map_assoc1_owned(mino_state *S, mino_val *m,
                                   mino_val *key, mino_val *val,
                                   uintptr_t owner)
{
    mino_val *out;
    if (m != NULL && m->as.map.len > 0) {
        mino_val *existing = map_get_val(m, key);
        if (existing != NULL && mino_eq(existing, val)) return m;
    }
    mino_current_ctx(S)->gc_depth++;
    out = alloc_val(S, MINO_MAP);
    if (m == NULL || m->as.map.len == 0) {
        mino_val *ko = mino_vector(S, NULL, 0);
        mino_val *vo = mino_vector(S, NULL, 0);
        ko = vec_conj1_owned(S, ko, key, owner);
        vo = vec_conj1_owned(S, vo, val, owner);
        out->as.map.root      = NULL;
        out->as.map.key_order = ko;
        out->as.map.val_order = vo;
        out->as.map.len       = 1;
        if (m != NULL) out->meta = m->meta;
        mino_current_ctx(S)->gc_depth--;
        return out;
    }
    if (m->as.map.val_order != NULL) {
        size_t      old_len = m->as.map.len;
        size_t      i       = flat_find_index(m->as.map.key_order, key, old_len);
        mino_val *ko, *vo;
        if (i < old_len) {
            ko = m->as.map.key_order;
            vo = vec_assoc1_owned(S, m->as.map.val_order, i, val, owner);
            out->as.map.root      = NULL;
            out->as.map.key_order = ko;
            out->as.map.val_order = vo;
            out->as.map.len       = old_len;
            out->meta             = m->meta;
            mino_current_ctx(S)->gc_depth--;
            return out;
        }
        if (old_len + 1 > MINO_FLATMAP_THRESHOLD) {
            mino_hamt_node_t *root;
            int               replaced = 0;
            hamt_entry_t     *e;
            ko = vec_conj1_owned(S, m->as.map.key_order, key, owner);
            root = hamt_from_flat(S, m->as.map.key_order,
                                   m->as.map.val_order, old_len);
            if (root == NULL) { mino_current_ctx(S)->gc_depth--; return NULL; }
            e = hamt_entry_new(S, key, val);
            if (e == NULL) { mino_current_ctx(S)->gc_depth--; return NULL; }
            root = hamt_assoc_owned(S, root, e, hash_val(key), 0u, &replaced, owner);
            if (root == NULL) { mino_current_ctx(S)->gc_depth--; return NULL; }
            out->as.map.root      = root;
            out->as.map.key_order = ko;
            out->as.map.val_order = NULL;
            out->as.map.len       = old_len + 1;
            out->meta             = m->meta;
            mino_current_ctx(S)->gc_depth--;
            return out;
        }
        ko = vec_conj1_owned(S, m->as.map.key_order, key, owner);
        vo = vec_conj1_owned(S, m->as.map.val_order, val, owner);
        out->as.map.root      = NULL;
        out->as.map.key_order = ko;
        out->as.map.val_order = vo;
        out->as.map.len       = old_len + 1;
        out->meta             = m->meta;
        mino_current_ctx(S)->gc_depth--;
        return out;
    }
    {
        hamt_entry_t     *e        = hamt_entry_new(S, key, val);
        uint32_t          h        = hash_val(key);
        int               replaced = 0;
        mino_hamt_node_t *root;
        mino_val       *ko       = m->as.map.key_order;
        if (e == NULL) { mino_current_ctx(S)->gc_depth--; return NULL; }
        root = hamt_assoc_owned(S, m->as.map.root, e, h, 0u, &replaced, owner);
        if (root == NULL) { mino_current_ctx(S)->gc_depth--; return NULL; }
        if (!replaced) ko = vec_conj1_owned(S, ko, key, owner);
        out->as.map.root      = root;
        out->as.map.key_order = ko;
        out->as.map.val_order = NULL;
        out->as.map.len       = m->as.map.len + (replaced ? 0 : 1);
        out->meta             = m->meta;
        mino_current_ctx(S)->gc_depth--;
        return out;
    }
}

mino_val *mino_map_dissoc1_owned(mino_state *S, mino_val *m,
                                    mino_val *key, uintptr_t owner)
{
    mino_val *out;
    if (m == NULL || m->as.map.len == 0) return m;
    mino_current_ctx(S)->gc_depth++;
    if (m->as.map.val_order != NULL) {
        /* Flatmap dissoc: no vec_dissoc_at_owned exists, so rebuild
         * the order vectors via vec_conj1_owned. Trie nodes still get
         * owner-stamped on first touch. */
        size_t old_len = m->as.map.len;
        size_t idx     = flat_find_index(m->as.map.key_order, key, old_len);
        size_t i;
        mino_val *ko, *vo;
        if (idx == old_len) { mino_current_ctx(S)->gc_depth--; return m; }
        ko = mino_vector(S, NULL, 0);
        vo = mino_vector(S, NULL, 0);
        for (i = 0; i < old_len; i++) {
            if (i == idx) continue;
            ko = vec_conj1_owned(S, ko, vec_nth(m->as.map.key_order, i), owner);
            vo = vec_conj1_owned(S, vo, vec_nth(m->as.map.val_order, i), owner);
        }
        out = alloc_val(S, MINO_MAP);
        out->as.map.root      = NULL;
        out->as.map.key_order = ko;
        out->as.map.val_order = vo;
        out->as.map.len       = old_len - 1;
        out->meta             = m->meta;
        mino_current_ctx(S)->gc_depth--;
        return out;
    }
    {
        size_t            old_len = m->as.map.len;
        mino_val       *ko;
        mino_hamt_node_t *root;
        size_t            i;
        int               removed = 0;
        root = hamt_dissoc_owned(S, m->as.map.root, key, hash_val(key), 0u,
                                  &removed, owner);
        if (!removed) {
            mino_current_ctx(S)->gc_depth--;
            return m;
        }
        ko = mino_vector(S, NULL, 0);
        for (i = 0; i < old_len; i++) {
            mino_val *k = vec_nth(m->as.map.key_order, i);
            if (mino_eq(k, key)) continue;
            ko = vec_conj1_owned(S, ko, k, owner);
        }
        out = alloc_val(S, MINO_MAP);
        out->as.map.root      = root;
        out->as.map.key_order = ko;
        out->as.map.val_order = NULL;
        out->as.map.len       = old_len - 1;
        out->meta             = m->meta;
        mino_current_ctx(S)->gc_depth--;
        return out;
    }
}

/*
 * Map construction. The semantics remain: duplicate keys resolve
 * last-write-wins, and the resulting map iterates keys in the order they
 * first appeared in the source sequence. Caller retains ownership of the
 * source arrays.
 *
 * When the source has at most MINO_FLATMAP_THRESHOLD distinct keys we
 * land in flatmap form (root = NULL, val_order non-NULL); otherwise we
 * build the HAMT directly with no flat intermediary.
 */
mino_val *mino_map(mino_state *S, mino_val **keys, mino_val **vals, size_t len)
{
    mino_val       *v;
    /* Suppress GC during construction so caller-owned key/value arrays
     * stay valid. Without this, intermediate allocations (hamt_entry_new,
     * vec_conj1) could trigger collection that reclaims values the caller
     * holds only on the C stack. */
    mino_current_ctx(S)->gc_depth++;
    v = alloc_val(S, MINO_MAP);
    if (len <= MINO_FLATMAP_THRESHOLD) {
        /* Flat path: keep keys/vals in parallel order vectors, no HAMT. */
        mino_val *order = mino_vector(S, NULL, 0);
        mino_val *vals_ord = (len == 0) ? NULL : mino_vector(S, NULL, 0);
        size_t      len_out = 0;
        size_t      i;
        for (i = 0; i < len; i++) {
            size_t idx = flat_find_index(order, keys[i], len_out);
            if (idx < len_out) {
                vals_ord = vec_assoc1(S, vals_ord, idx, vals[i]);
            } else {
                order    = vec_conj1(S, order, keys[i]);
                vals_ord = vec_conj1(S, vals_ord, vals[i]);
                len_out++;
            }
        }
        if (len_out == 0) {
            v->as.map.root      = NULL;
            v->as.map.key_order = order;
            v->as.map.val_order = NULL;
            v->as.map.len       = 0;
        } else if (len_out <= MINO_FLATMAP_THRESHOLD) {
            v->as.map.root      = NULL;
            v->as.map.key_order = order;
            v->as.map.val_order = vals_ord;
            v->as.map.len       = len_out;
        } else {
            /* Cannot happen — len_out <= len <= threshold. */
            v->as.map.root      = NULL;
            v->as.map.key_order = order;
            v->as.map.val_order = vals_ord;
            v->as.map.len       = len_out;
        }
    } else {
        mino_hamt_node_t *root    = NULL;
        mino_val       *order   = mino_vector(S, NULL, 0);
        size_t            len_out = 0;
        size_t            i;
        for (i = 0; i < len; i++) {
            hamt_entry_t *e        = hamt_entry_new(S, keys[i], vals[i]);
            uint32_t      h        = hash_val(keys[i]);
            int           replaced = 0;
            root = hamt_assoc(S, root, e, h, 0u, &replaced);
            if (!replaced) {
                order = vec_conj1(S, order, keys[i]);
                len_out++;
            }
        }
        v->as.map.root      = root;
        v->as.map.key_order = order;
        v->as.map.val_order = NULL;
        v->as.map.len       = len_out;
    }
    mino_current_ctx(S)->gc_depth--;
    return v;
}

mino_val *mino_set(mino_state *S, mino_val **items, size_t len)
{
    mino_val       *v;
    mino_hamt_node_t *root     = NULL;
    mino_val       *order;
    size_t            len_out  = 0;
    size_t            i;
    mino_val       *sentinel;
    mino_current_ctx(S)->gc_depth++;
    v        = alloc_val(S, MINO_SET);
    order    = mino_vector(S, NULL, 0);
    sentinel = mino_true(S);
    for (i = 0; i < len; i++) {
        hamt_entry_t *e        = hamt_entry_new(S, items[i], sentinel);
        uint32_t      h        = hash_val(items[i]);
        int           replaced = 0;
        root = hamt_assoc(S, root, e, h, 0u, &replaced);
        if (!replaced) {
            order = vec_conj1(S, order, items[i]);
            len_out++;
        }
    }
    v->as.set.root      = root;
    v->as.set.key_order = order;
    v->as.set.len       = len_out;
    mino_current_ctx(S)->gc_depth--;
    return v;
}

mino_val *mino_prim(mino_state *S, const char *name, mino_prim_fn fn)
{

    mino_val *v = alloc_val(S, MINO_PRIM);
    v->as.prim.name = name;
    v->as.prim.fn   = fn;
    v->as.prim.fn2  = NULL;
    return v;
}

mino_val *mino_prim_argv(mino_state *S, const char *name, mino_prim_fn2 fn)
{
    mino_val *v = alloc_val(S, MINO_PRIM);
    v->as.prim.name = name;
    v->as.prim.fn   = NULL;
    v->as.prim.fn2  = fn;
    return v;
}

mino_val *mino_handle(mino_state *S, void *ptr, const char *tag)
{

    mino_val *v = alloc_val(S, MINO_HANDLE);
    v->as.handle.ptr       = ptr;
    v->as.handle.tag       = tag;
    v->as.handle.finalizer = NULL;
    return v;
}

mino_val *mino_handle_ex(mino_state *S, void *ptr, const char *tag,
                           mino_finalizer_fn finalizer)
{

    mino_val *v = alloc_val(S, MINO_HANDLE);
    v->as.handle.ptr       = ptr;
    v->as.handle.tag       = tag;
    v->as.handle.finalizer = finalizer;
    return v;
}

int mino_is_handle(const mino_val *v)
{
    return v != NULL && mino_type_of(v) == MINO_HANDLE;
}

void *mino_handle_ptr(const mino_val *v)
{
    if (v == NULL || mino_type_of(v) != MINO_HANDLE) {
        return NULL;
    }
    return v->as.handle.ptr;
}

const char *mino_handle_tag(const mino_val *v)
{
    if (v == NULL || mino_type_of(v) != MINO_HANDLE) {
        return NULL;
    }
    return v->as.handle.tag;
}

mino_val *mino_atom(mino_state *S, mino_val *val)
{
    mino_val *v = alloc_val(S, MINO_ATOM);
    v->as.atom.val       = val;
    v->as.atom.watches   = NULL;
    v->as.atom.validator = NULL;
    return v;
}

int mino_is_atom(const mino_val *v)
{
    return v != NULL && mino_type_of(v) == MINO_ATOM;
}

mino_val *mino_atom_deref(const mino_val *a)
{
    if (a == NULL || mino_type_of(a) != MINO_ATOM) return NULL;
    return a->as.atom.val;
}

mino_val *mino_agent_deref(const mino_val *a)
{
    if (a == NULL || mino_type_of(a) != MINO_AGENT) return NULL;
    return a->as.agent.val;
}

mino_val *mino_volatile(mino_state *S, mino_val *val)
{
    mino_val *v = alloc_val(S, MINO_VOLATILE);
    v->as.volatile_.val = val;
    return v;
}

int mino_is_volatile(const mino_val *v)
{
    return v != NULL && mino_type_of(v) == MINO_VOLATILE;
}

mino_val *mino_volatile_deref(const mino_val *v)
{
    if (v == NULL || mino_type_of(v) != MINO_VOLATILE) return NULL;
    return v->as.volatile_.val;
}

void mino_atom_reset(mino_state *S, mino_val *a, mino_val *val)
{
    if (a != NULL && mino_type_of(a) == MINO_ATOM) {
        gc_write_barrier(S, a, a->as.atom.val, val);
        a->as.atom.val = val;
    }
}

mino_val *make_fn(mino_state *S, mino_val *params, mino_val *body,
                    mino_env *env)
{
    mino_val *v = alloc_val(S, MINO_FN);
    v->as.fn.params      = params;
    v->as.fn.body        = body;
    v->as.fn.env         = env;
    v->as.fn.shape       = 0;
    v->as.fn.wraps_prim  = NULL;
    v->as.fn.template_fn = NULL;
    /* Inside a macro body, current_ns is still the caller's ns (only
     * fn_ambient_ns is the macro's defining ns). Closures created here
     * are artifacts of the macro expansion -- they should resolve free
     * vars and qualify syntax-quoted symbols against the macro's ns,
     * not the caller's. Without this, `(fn [...] `(sym ...))` inside
     * a macro body emits bare `sym` instead of `defining-ns/sym` once
     * the closure runs, since invoking the closure overwrites
     * fn_ambient_ns with its (caller-derived) defining_ns. */
    if (S->ns_vars.fn_ambient_ns != NULL
        && S->ns_vars.fn_ambient_ns != S->ns_vars.current_ns
        && (S->ns_vars.current_ns == NULL
            || strcmp(S->ns_vars.fn_ambient_ns, S->ns_vars.current_ns) != 0)) {
        v->as.fn.defining_ns = S->ns_vars.fn_ambient_ns;
    } else {
        v->as.fn.defining_ns = S->ns_vars.current_ns;
    }
    return v;
}

