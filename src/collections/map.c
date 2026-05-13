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

/* Hash any sequential value (vector, cons-chain, chunked-cons, empty
 * list, realized lazy seq) under a unified scheme so equal content
 * across representations always hashes equal. Tag byte 0x09 is shared
 * with MINO_VECTOR (so existing vector hashes are preserved and
 * MINO_MAP_ENTRY -- which already uses 0x09 -- stays compatible with a
 * 2-element vector of the same content).
 *
 * Unrealized lazy seq tails are not forced (hash_val has no state to
 * allocate with). When the walk meets one it folds in the lazy's
 * pointer-identity hash and stops -- so two distinct unrealized lazies
 * holding the same would-be content still hash differently. Callers
 * that need lazy-content hash equality must force first via
 * `(seq ...)`, `(doall ...)`, or any operation that drives iteration. */
static uint32_t hash_sequential(const mino_val_t *original)
{
    uint32_t h;
    const mino_val_t *v;
    if (original != NULL && mino_type_of(original) == MINO_VECTOR
        && original->as.vec.cached_hash != 0) {
        return original->as.vec.cached_hash;
    }
    h = 2166136261u;
    h = fnv_mix(h, 0x09);
    v = original;
    for (;;) {
        while (v != NULL && mino_type_of(v) == MINO_LAZY
               && v->as.lazy.realized) {
            v = v->as.lazy.cached;
        }
        if (v == NULL) break;
        {
            mino_type_t t = mino_type_of(v);
            if (t == MINO_NIL || t == MINO_EMPTY_LIST) break;
            if (t == MINO_VECTOR) {
                size_t i;
                size_t n = v->as.vec.len;
                for (i = 0; i < n; i++) {
                    h = hash_uint32_bytes(h, hash_val(vec_nth(v, i)));
                }
                break;
            }
            if (t == MINO_CONS) {
                h = hash_uint32_bytes(h, hash_val(v->as.cons.car));
                v = v->as.cons.cdr;
                continue;
            }
            if (t == MINO_CHUNKED_CONS) {
                size_t idx = v->as.chunked_cons.off;
                while (v != NULL && mino_type_of(v) == MINO_CHUNKED_CONS) {
                    const mino_val_t *ch = v->as.chunked_cons.chunk;
                    for (; idx < ch->as.chunk.len; idx++) {
                        h = hash_uint32_bytes(h, hash_val(ch->as.chunk.vals[idx]));
                    }
                    v = v->as.chunked_cons.more;
                    idx = (v != NULL && mino_type_of(v) == MINO_CHUNKED_CONS)
                              ? v->as.chunked_cons.off : 0;
                }
                continue;
            }
            if (t == MINO_LAZY) {
                /* Unrealized tail: can't force without state. Fold the
                 * lazy's identity-pointer hash so the walk produces a
                 * stable, distinguishable value, then stop. */
                h = hash_uint32_bytes(h,
                        hash_pointer_bytes(2166136261u, (uintptr_t)v));
                break;
            }
            /* Defensive: anything else dispatched here is a bug. */
            break;
        }
    }
    if (original != NULL && mino_type_of(original) == MINO_VECTOR) {
        ((mino_val_t *)original)->as.vec.cached_hash = h;
    }
    return h;
}

/* XOR-fold per-entry hashes across a red-black tree. The same mixing
 * scheme as the MINO_MAP / MINO_SET branches in hash_val, just walked
 * via tree recursion instead of the insertion-order vector. Used so
 * sorted-map / sorted-set hashes match their hash-map / hash-set
 * cross-type-equal counterparts. */
static uint32_t rb_xor_fold_entries(const mino_rb_node_t *n, int include_val)
{
    uint32_t acc;
    uint32_t hk;
    if (n == NULL) return 0;
    acc = rb_xor_fold_entries(n->left, include_val);
    hk  = hash_val(n->key);
    if (include_val) {
        uint32_t hv = hash_val(n->val);
        hk ^= hv * 2654435761u;
    }
    acc ^= hk;
    acc ^= rb_xor_fold_entries(n->right, include_val);
    return acc;
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
    if (v == NULL || mino_type_of(v) == MINO_NIL) {
        return fnv_mix(h, 0x01);
    }
    switch (mino_type_of(v)) {
    case MINO_NIL:
        return fnv_mix(h, 0x01);
    case MINO_EMPTY_LIST:
        return hash_sequential(v);
    case MINO_BOOL:
        h = fnv_mix(h, 0x02);
        return fnv_mix(h, (unsigned char)(mino_val_bool_get(v) ? 1 : 0));
    case MINO_INT:
        h = fnv_mix(h, 0x03);
        return hash_long_long_bytes(h, mino_val_int_get(v));
    case MINO_FLOAT:
    case MINO_FLOAT32: {
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
        return hash_uint32_bytes(h, (uint32_t)mino_val_char_get(v));
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
        return hash_sequential(v);
    case MINO_VECTOR:
        return hash_sequential(v);
    case MINO_MAP_ENTRY: {
        /* Hashes as a 2-vector of (k, v) so cross-type equality with
         * MINO_VECTOR also gives identical hashes (so a hash-map keyed
         * by entries vs. by 2-vectors still compares equal). */
        h = fnv_mix(h, 0x09);
        h = hash_uint32_bytes(h, hash_val(v->as.map_entry.k));
        h = hash_uint32_bytes(h, hash_val(v->as.map_entry.v));
        return h;
    }
    case MINO_MAP: {
        /* XOR-fold of per-entry hashes for order independence. Each entry's
         * hash mixes key and value hashes with a prime to avoid (k ^ v)
         * self-cancellation when k == v. */
        uint32_t acc = 0;
        size_t   n   = v->as.map.len;
        size_t   i;
        if (v->as.map.cached_hash != 0) return v->as.map.cached_hash;
        for (i = 0; i < n; i++) {
            mino_val_t *key = vec_nth(v->as.map.key_order, i);
            uint32_t    hk  = hash_val(key);
            uint32_t    hv  = hash_val(map_get_val(v, key));
            hk ^= hv * 2654435761u;
            acc ^= hk;
        }
        h = fnv_mix(h, 0x0a);
        h = hash_uint32_bytes(h, acc);
        ((mino_val_t *)v)->as.map.cached_hash = h;
        return h;
    }
    case MINO_SET: {
        /* XOR-fold of element hashes for order independence. */
        uint32_t acc = 0;
        size_t   n   = v->as.set.len;
        size_t   i;
        if (v->as.set.cached_hash != 0) return v->as.set.cached_hash;
        for (i = 0; i < n; i++) {
            mino_val_t *elem = vec_nth(v->as.set.key_order, i);
            acc ^= hash_val(elem);
        }
        h = fnv_mix(h, 0x0d);
        h = hash_uint32_bytes(h, acc);
        ((mino_val_t *)v)->as.set.cached_hash = h;
        return h;
    }
    case MINO_SORTED_MAP: {
        /* Same tag and mixing as MINO_MAP so cross-type-equal maps
         * hash equal. No cached_hash slot on the sorted variant, so
         * recompute on demand -- sorted collections rarely appear as
         * hash-map keys, which is the only path that calls this. */
        uint32_t acc = rb_xor_fold_entries(v->as.sorted.root, 1);
        h = fnv_mix(h, 0x0a);
        h = hash_uint32_bytes(h, acc);
        return h;
    }
    case MINO_SORTED_SET: {
        /* Same tag and mixing as MINO_SET; see MINO_SORTED_MAP above. */
        uint32_t acc = rb_xor_fold_entries(v->as.sorted.root, 0);
        h = fnv_mix(h, 0x0d);
        h = hash_uint32_bytes(h, acc);
        return h;
    }
    case MINO_HANDLE:
        h = fnv_mix(h, 0x0c);
        return hash_pointer_bytes(h, (uintptr_t)v->as.handle.ptr);
    case MINO_ATOM:
    case MINO_VOLATILE:
        h = fnv_mix(h, 0x0e);
        return hash_pointer_bytes(h, (uintptr_t)v);
    case MINO_CHUNK:
        /* Internal seq leaf; identity-hashed (matches identity equality
         * in mino_eq above). */
        h = fnv_mix(h, 0x14);
        return hash_pointer_bytes(h, (uintptr_t)v);
    case MINO_CHUNKED_CONS:
        return hash_sequential(v);
    case MINO_LAZY:
        /* Hashes equal to its sequential content when realized; when
         * unrealized, falls back to identity-pointer inside
         * hash_sequential. */
        return hash_sequential(v);
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
    case MINO_UUID: {
        size_t i;
        h = fnv_mix(h, 0x15);
        for (i = 0; i < 16; i++) h = fnv_mix(h, v->as.uuid.bytes[i]);
        return h;
    }
    case MINO_REGEX:
        /* Identity equality means identity hash too. */
        h = fnv_mix(h, 0x16);
        return hash_pointer_bytes(h, (uintptr_t)v);
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
static size_t flat_find_index(const mino_val_t *key_order,
                              const mino_val_t *key, size_t len)
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
static mino_hamt_node_t *hamt_from_flat(mino_state_t *S,
                                         const mino_val_t *key_order,
                                         const mino_val_t *val_order,
                                         size_t len)
{
    mino_hamt_node_t *root = NULL;
    size_t i;
    for (i = 0; i < len; i++) {
        mino_val_t   *k = vec_nth(key_order, i);
        mino_val_t   *vv = vec_nth(val_order, i);
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
mino_val_t *map_get_val(const mino_val_t *m, const mino_val_t *key)
{
    return mino_map_lookup(m, key);
}

mino_val_t *mino_map_lookup(const mino_val_t *m, const mino_val_t *key)
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

mino_val_t *mino_map_assoc1(mino_state_t *S, mino_val_t *m,
                            mino_val_t *key, mino_val_t *val)
{
    mino_val_t *out;
    /* Identity short-circuit: if the key already maps to a value that
     * compares `mino_eq`-equal to `val`, return m unchanged. The
     * cached-hash on collection values keeps this O(1) in the miss
     * case for sequable vals; for cheap scalar vals the structural
     * compare is constant-time anyway. Lets `(identical? m (assoc m
     * k (get m k)))` hold and saves the rebuild traffic on the
     * surprisingly common "replace with current value" idiom. */
    if (m != NULL && m->as.map.len > 0) {
        mino_val_t *existing = map_get_val(m, key);
        if (existing != NULL && mino_eq(existing, val)) return m;
    }
    mino_current_ctx(S)->gc_depth++;
    out = alloc_val(S, MINO_MAP);
    if (m == NULL || m->as.map.len == 0) {
        mino_val_t *ko = mino_vector(S, NULL, 0);
        mino_val_t *vo = mino_vector(S, NULL, 0);
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
        mino_val_t *ko, *vo;
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
        mino_val_t       *ko       = m->as.map.key_order;
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

mino_val_t *mino_map_dissoc1(mino_state_t *S, mino_val_t *m,
                             mino_val_t *key)
{
    mino_val_t *out;
    if (m == NULL || m->as.map.len == 0) return m;
    mino_current_ctx(S)->gc_depth++;
    if (m->as.map.val_order != NULL) {
        /* Flatmap: rebuild without the matching slot. */
        size_t old_len = m->as.map.len;
        size_t idx     = flat_find_index(m->as.map.key_order, key, old_len);
        size_t i;
        mino_val_t *ko, *vo;
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
        mino_val_t       *ko;
        mino_hamt_node_t *root    = NULL;
        size_t            i;
        if (mino_map_lookup(m, key) == NULL) {
            mino_current_ctx(S)->gc_depth--;
            return m;
        }
        ko = mino_vector(S, NULL, 0);
        for (i = 0; i < old_len; i++) {
            mino_val_t *k = vec_nth(m->as.map.key_order, i);
            if (mino_eq(k, key)) continue;
            ko = vec_conj1(S, ko, k);
        }
        for (i = 0; i < old_len; i++) {
            mino_val_t   *k = vec_nth(m->as.map.key_order, i);
            mino_val_t   *vv;
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
mino_val_t *mino_map(mino_state_t *S, mino_val_t **keys, mino_val_t **vals, size_t len)
{
    mino_val_t       *v;
    /* Suppress GC during construction so caller-owned key/value arrays
     * stay valid. Without this, intermediate allocations (hamt_entry_new,
     * vec_conj1) could trigger collection that reclaims values the caller
     * holds only on the C stack. */
    mino_current_ctx(S)->gc_depth++;
    v = alloc_val(S, MINO_MAP);
    if (len <= MINO_FLATMAP_THRESHOLD) {
        /* Flat path: keep keys/vals in parallel order vectors, no HAMT. */
        mino_val_t *order = mino_vector(S, NULL, 0);
        mino_val_t *vals_ord = (len == 0) ? NULL : mino_vector(S, NULL, 0);
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
        mino_val_t       *order   = mino_vector(S, NULL, 0);
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

mino_val_t *mino_set(mino_state_t *S, mino_val_t **items, size_t len)
{
    mino_val_t       *v;
    mino_hamt_node_t *root     = NULL;
    mino_val_t       *order;
    size_t            len_out  = 0;
    size_t            i;
    mino_val_t       *sentinel;
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

mino_val_t *mino_prim(mino_state_t *S, const char *name, mino_prim_fn fn)
{

    mino_val_t *v = alloc_val(S, MINO_PRIM);
    v->as.prim.name = name;
    v->as.prim.fn   = fn;
    v->as.prim.fn2  = NULL;
    return v;
}

mino_val_t *mino_prim_argv(mino_state_t *S, const char *name, mino_prim_fn2 fn)
{
    mino_val_t *v = alloc_val(S, MINO_PRIM);
    v->as.prim.name = name;
    v->as.prim.fn   = NULL;
    v->as.prim.fn2  = fn;
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
    return v != NULL && mino_type_of(v) == MINO_HANDLE;
}

void *mino_handle_ptr(const mino_val_t *v)
{
    if (v == NULL || mino_type_of(v) != MINO_HANDLE) {
        return NULL;
    }
    return v->as.handle.ptr;
}

const char *mino_handle_tag(const mino_val_t *v)
{
    if (v == NULL || mino_type_of(v) != MINO_HANDLE) {
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
    return v != NULL && mino_type_of(v) == MINO_ATOM;
}

mino_val_t *mino_atom_deref(const mino_val_t *a)
{
    if (a == NULL || mino_type_of(a) != MINO_ATOM) return NULL;
    return a->as.atom.val;
}

mino_val_t *mino_volatile(mino_state_t *S, mino_val_t *val)
{
    mino_val_t *v = alloc_val(S, MINO_VOLATILE);
    v->as.volatile_.val = val;
    return v;
}

int mino_is_volatile(const mino_val_t *v)
{
    return v != NULL && mino_type_of(v) == MINO_VOLATILE;
}

mino_val_t *mino_volatile_deref(const mino_val_t *v)
{
    if (v == NULL || mino_type_of(v) != MINO_VOLATILE) return NULL;
    return v->as.volatile_.val;
}

void mino_atom_reset(mino_val_t *a, mino_val_t *val)
{
    if (a != NULL && mino_type_of(a) == MINO_ATOM) {
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
    v->as.fn.shape       = 0;
    /* Inside a macro body, current_ns is still the caller's ns (only
     * fn_ambient_ns is the macro's defining ns). Closures created here
     * are artifacts of the macro expansion -- they should resolve free
     * vars and qualify syntax-quoted symbols against the macro's ns,
     * not the caller's. Without this, `(fn [...] `(sym ...))` inside
     * a macro body emits bare `sym` instead of `defining-ns/sym` once
     * the closure runs, since invoking the closure overwrites
     * fn_ambient_ns with its (caller-derived) defining_ns. */
    if (S->fn_ambient_ns != NULL
        && S->fn_ambient_ns != S->current_ns
        && (S->current_ns == NULL
            || strcmp(S->fn_ambient_ns, S->current_ns) != 0)) {
        v->as.fn.defining_ns = S->fn_ambient_ns;
    } else {
        v->as.fn.defining_ns = S->current_ns;
    }
    return v;
}

