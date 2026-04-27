/*
 * map.c -- persistent hash array mapped trie (HAMT) map and set.
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

/*/* Types and constants in collections_internal.h */

unsigned popcount32(uint32_t x)
{
    unsigned c = 0;
    while (x != 0) {
        x &= x - 1u;
        c++;
    }
    return c;
}

hamt_entry_t *hamt_entry_new(mino_state_t *S, mino_val_t *key, mino_val_t *val)
{
    hamt_entry_t *e = (hamt_entry_t *)gc_alloc_typed(
        S, GC_T_HAMT_ENTRY, sizeof(*e));
    e->key = key;
    e->val = val;
    return e;
}

uint32_t fnv_mix(uint32_t h, unsigned char b)
{
    h ^= (uint32_t)b;
    h *= 16777619u;
    return h;
}

uint32_t fnv_bytes(uint32_t h, const unsigned char *p, size_t n)
{
    size_t i;
    for (i = 0; i < n; i++) {
        h = fnv_mix(h, p[i]);
    }
    return h;
}

/*
 * Equal-implies-equal-hash helpers. Every hash_val branch that is
 * reachable from a `mino_eq`-equal pair must funnel through the same
 * little-endian byte loop, so callers that share a tag (numeric tier
 * collapse, identity-by-pointer, accumulated subhashes) emit identical
 * byte sequences for equal inputs.
 */

/* Mix the eight little-endian bytes of a long long. Used by the numeric
 * tier collapse (MINO_INT, integral MINO_FLOAT, fits-in-ll MINO_BIGINT),
 * which all share tag byte 0x03 so (= 1 1.0 1N) ⇒ all hash the same. */
static uint32_t hash_long_long_bytes(uint32_t h, long long n)
{
    unsigned i;
    for (i = 0; i < 8; i++) {
        h = fnv_mix(h, (unsigned char)(n & 0xFFu));
        n >>= 8;
    }
    return h;
}

/* Mix the bytes of a uintptr_t. Used by identity-hashed types (HANDLE,
 * ATOM, and the default PRIM/FN/RECUR fallback); each caller mixes a
 * distinct tag byte first so different identity-kinds don't collide. */
static uint32_t hash_pointer_bytes(uint32_t h, uintptr_t p)
{
    unsigned i;
    for (i = 0; i < sizeof(uintptr_t); i++) {
        h = fnv_mix(h, (unsigned char)(p & 0xFFu));
        p >>= 8;
    }
    return h;
}

/* Mix the four little-endian bytes of a uint32_t. Used wherever a
 * subhash gets folded into the parent (CONS car/cdr, VECTOR elements,
 * MAP/SET XOR accumulator, the upper-magnitude BIGINT/RATIO/BIGDEC
 * paths). Equal sub-values → equal sub-hashes → equal byte sequence
 * here. */
static uint32_t hash_uint32_bytes(uint32_t h, uint32_t x)
{
    unsigned i;
    for (i = 0; i < 4; i++) {
        h = fnv_mix(h, (unsigned char)(x & 0xFFu));
        x >>= 8;
    }
    return h;
}

/*
 * Hash function compatible with mino_eq:
 *   - Integral floats hash the same as the equivalent int so (= 1 1.0) ⇒ 1.
 *   - Strings, symbols, and keywords each carry a distinct type tag so byte-
 *     equal values of different types hash differently (and compare unequal).
 *   - Collections hash their contents; maps XOR-fold entry hashes for
 *     order-insensitivity.
 *   - Non-hashable types (PRIM, FN) fall back to pointer identity.
 */
uint32_t hash_val(const mino_val_t *v)
{
    uint32_t h = 2166136261u;   /* FNV-1a offset basis */
    if (v == NULL || v->type == MINO_NIL) {
        return fnv_mix(h, 0x01);
    }
    switch (v->type) {
    case MINO_NIL:
        return fnv_mix(h, 0x01);
    case MINO_BOOL:
        h = fnv_mix(h, 0x02);
        return fnv_mix(h, (unsigned char)(v->as.b ? 1 : 0));
    case MINO_INT:
        h = fnv_mix(h, 0x03);
        return hash_long_long_bytes(h, v->as.i);
    case MINO_FLOAT: {
        double    d  = v->as.f;
        long long ll = (long long)d;
        if ((double)ll == d) {
            /* Same tag as MINO_INT so (= 1 1.0) matches in hash too. */
            h = fnv_mix(h, 0x03);
            return hash_long_long_bytes(h, ll);
        }
        h = fnv_mix(h, 0x04);
        {
            unsigned char buf[sizeof(double)];
            memcpy(buf, &d, sizeof(d));
            return fnv_bytes(h, buf, sizeof(d));
        }
    }
    case MINO_CHAR:
        h = fnv_mix(h, 0x0f);
        return hash_uint32_bytes(h, (uint32_t)v->as.ch);
    case MINO_STRING:
        h = fnv_mix(h, 0x05);
        return fnv_bytes(h, (const unsigned char *)v->as.s.data, v->as.s.len);
    case MINO_SYMBOL:
        h = fnv_mix(h, 0x06);
        return fnv_bytes(h, (const unsigned char *)v->as.s.data, v->as.s.len);
    case MINO_KEYWORD:
        h = fnv_mix(h, 0x07);
        return fnv_bytes(h, (const unsigned char *)v->as.s.data, v->as.s.len);
    case MINO_CONS:
        h = fnv_mix(h, 0x08);
        h = hash_uint32_bytes(h, hash_val(v->as.cons.car));
        return hash_uint32_bytes(h, hash_val(v->as.cons.cdr));
    case MINO_VECTOR: {
        size_t n = v->as.vec.len;
        size_t i;
        h = fnv_mix(h, 0x09);
        for (i = 0; i < n; i++) {
            h = hash_uint32_bytes(h, hash_val(vec_nth(v, i)));
        }
        return h;
    }
    case MINO_MAP: {
        /* XOR-fold of per-entry hashes for order independence. Each entry's
         * hash mixes key and value hashes with a prime to avoid (k ^ v)
         * self-cancellation when k == v. */
        uint32_t acc = 0;
        size_t   n   = v->as.map.len;
        size_t   i;
        for (i = 0; i < n; i++) {
            mino_val_t *key = vec_nth(v->as.map.key_order, i);
            uint32_t    hk  = hash_val(key);
            uint32_t    hv  = hash_val(map_get_val(v, key));
            hk ^= hv * 2654435761u;
            acc ^= hk;
        }
        h = fnv_mix(h, 0x0a);
        return hash_uint32_bytes(h, acc);
    }
    case MINO_SET: {
        /* XOR-fold of element hashes for order independence. */
        uint32_t acc = 0;
        size_t   n   = v->as.set.len;
        size_t   i;
        for (i = 0; i < n; i++) {
            mino_val_t *elem = vec_nth(v->as.set.key_order, i);
            acc ^= hash_val(elem);
        }
        h = fnv_mix(h, 0x0d);
        return hash_uint32_bytes(h, acc);
    }
    case MINO_HANDLE:
        h = fnv_mix(h, 0x0c);
        return hash_pointer_bytes(h, (uintptr_t)v->as.handle.ptr);
    case MINO_ATOM:
        h = fnv_mix(h, 0x0e);
        return hash_pointer_bytes(h, (uintptr_t)v);
    case MINO_BIGINT: {
        /* Bigints that fit in a long long hash under the MINO_INT tag
         * so (= 1 1N) and (hash-of 1 1N) agree. Larger magnitudes use a
         * bigint-dedicated tag seeded from the imath-provided hash. */
        long long ll;
        if (mino_as_ll(v, &ll)) {
            h = fnv_mix(h, 0x03);
            return hash_long_long_bytes(h, ll);
        }
        h = fnv_mix(h, 0x10);
        return hash_uint32_bytes(h, mino_bigint_hash(v));
    }
    case MINO_RATIO:
        h = fnv_mix(h, 0x11);
        return hash_uint32_bytes(h, mino_ratio_hash(v));
    case MINO_BIGDEC:
        h = fnv_mix(h, 0x12);
        return hash_uint32_bytes(h, mino_bigdec_hash(v));
    case MINO_TYPE:
        h = fnv_mix(h, 0x13);
        return hash_pointer_bytes(h, (uintptr_t)v);
    case MINO_RECORD: {
        /* Records hash by combining the type-pointer hash with each
         * declared field's hash and the ext map's hash. Equal
         * records hash the same; records and plain maps with the
         * same content do not. */
        size_t i, n;
        h = fnv_mix(h, 0x14);
        h = hash_pointer_bytes(h, (uintptr_t)v->as.record.type);
        n = (v->as.record.type->as.record_type.fields != NULL)
            ? v->as.record.type->as.record_type.fields->as.vec.len : 0;
        for (i = 0; i < n; i++) {
            h = hash_uint32_bytes(h, hash_val(v->as.record.vals[i]));
        }
        if (v->as.record.ext != NULL) {
            h = hash_uint32_bytes(h, hash_val(v->as.record.ext));
        }
        return h;
    }
    default:
        /* PRIM, FN, RECUR: identity-based. */
        h = fnv_mix(h, 0x0b);
        return hash_pointer_bytes(h, (uintptr_t)v);
    }
}

static mino_hamt_node_t *hamt_bitmap_node(mino_state_t *S, uint32_t bitmap,
                                           uint32_t subnode_mask, void **slots)
{
    mino_hamt_node_t *n = (mino_hamt_node_t *)gc_alloc_typed(
        S, GC_T_HAMT_NODE, sizeof(*n));
    n->bitmap       = bitmap;
    n->subnode_mask = subnode_mask;
    n->slots        = slots;
    return n;
}

static mino_hamt_node_t *hamt_collision_node(mino_state_t *S, uint32_t hash,
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
static mino_hamt_node_t *merge_entries(mino_state_t *S, hamt_entry_t *e1, uint32_t h1,
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
mino_hamt_node_t *hamt_assoc(mino_state_t *S, const mino_hamt_node_t *n,
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
mino_val_t *hamt_get(const mino_hamt_node_t *n, const mino_val_t *key,
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

/* Convenience: look up a key in a map value. */
mino_val_t *map_get_val(const mino_val_t *m, const mino_val_t *key)
{
    uint32_t h = hash_val(key);
    return hamt_get(m->as.map.root, key, h, 0u);
}

/*
 * Map construction. The semantics remain: duplicate keys resolve
 * last-write-wins, and the resulting map iterates keys in the order they
 * first appeared in the source sequence. Caller retains ownership of the
 * source arrays.
 */
mino_val_t *mino_map(mino_state_t *S, mino_val_t **keys, mino_val_t **vals, size_t len)
{
    mino_val_t       *v;
    mino_hamt_node_t *root     = NULL;
    mino_val_t       *order;
    size_t            len_out  = 0;
    size_t            i;
    /* Suppress GC during construction so caller-owned key/value arrays
     * stay valid. Without this, intermediate allocations (hamt_entry_new,
     * vec_conj1) could trigger collection that reclaims values the caller
     * holds only on the C stack. */
    S->gc_depth++;
    v     = alloc_val(S, MINO_MAP);
    order = mino_vector(S, NULL, 0);
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
    v->as.map.len       = len_out;
    S->gc_depth--;
    return v;
}

mino_val_t *mino_set(mino_state_t *S, mino_val_t **items, size_t len)
{
    mino_val_t       *v;
    mino_hamt_node_t *root     = NULL;
    mino_val_t       *order;
    size_t            len_out  = 0;
    size_t            i;
    mino_val_t       *sentinel;
    S->gc_depth++;
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
    S->gc_depth--;
    return v;
}

mino_val_t *mino_prim(mino_state_t *S, const char *name, mino_prim_fn fn)
{

    mino_val_t *v = alloc_val(S, MINO_PRIM);
    v->as.prim.name = name;
    v->as.prim.fn   = fn;
    return v;
}

mino_val_t *mino_handle(mino_state_t *S, void *ptr, const char *tag)
{

    mino_val_t *v = alloc_val(S, MINO_HANDLE);
    v->as.handle.ptr       = ptr;
    v->as.handle.tag       = tag;
    v->as.handle.finalizer = NULL;
    return v;
}

mino_val_t *mino_handle_ex(mino_state_t *S, void *ptr, const char *tag,
                           mino_finalizer_fn finalizer)
{

    mino_val_t *v = alloc_val(S, MINO_HANDLE);
    v->as.handle.ptr       = ptr;
    v->as.handle.tag       = tag;
    v->as.handle.finalizer = finalizer;
    return v;
}

int mino_is_handle(const mino_val_t *v)
{
    return v != NULL && v->type == MINO_HANDLE;
}

void *mino_handle_ptr(const mino_val_t *v)
{
    if (v == NULL || v->type != MINO_HANDLE) {
        return NULL;
    }
    return v->as.handle.ptr;
}

const char *mino_handle_tag(const mino_val_t *v)
{
    if (v == NULL || v->type != MINO_HANDLE) {
        return NULL;
    }
    return v->as.handle.tag;
}

mino_val_t *mino_atom(mino_state_t *S, mino_val_t *val)
{
    mino_val_t *v = alloc_val(S, MINO_ATOM);
    v->as.atom.val       = val;
    v->as.atom.watches   = NULL;
    v->as.atom.validator = NULL;
    return v;
}

int mino_is_atom(const mino_val_t *v)
{
    return v != NULL && v->type == MINO_ATOM;
}

mino_val_t *mino_atom_deref(const mino_val_t *a)
{
    if (a == NULL || a->type != MINO_ATOM) return NULL;
    return a->as.atom.val;
}

void mino_atom_reset(mino_val_t *a, mino_val_t *val)
{
    if (a != NULL && a->type == MINO_ATOM) {
        a->as.atom.val = val;
    }
}

mino_val_t *make_fn(mino_state_t *S, mino_val_t *params, mino_val_t *body,
                    mino_env_t *env)
{
    mino_val_t *v = alloc_val(S, MINO_FN);
    v->as.fn.params      = params;
    v->as.fn.body        = body;
    v->as.fn.env         = env;
    v->as.fn.defining_ns = S->current_ns;
    return v;
}

