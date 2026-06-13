/*
 * map_hash.c -- FNV-1a hash utilities and hash_val dispatch.
 *
 * Extracted from map.c to keep that TU within the 1100-LOC limit.
 * Public API is declared in values/internal.h (hash_val, fnv_mix, fnv_bytes).
 */

#include "runtime/internal.h"

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
static uint32_t hash_sequential(const mino_val *original)
{
    uint32_t h;
    const mino_val *v;
    if (original != NULL && mino_type_of(original) == MINO_VECTOR
        && original->as.vec.cached_hash != 0) {
        return original->as.vec.cached_hash;
    }
    h = 2166136261u;
    h = fnv_mix(h, 0x09);
    v = original;
    for (;;) {
        while (v != NULL && mino_type_of(v) == MINO_LAZY
               && v->as.lazy.realized == LAZY_REALIZED) {
            v = v->as.lazy.cached;
        }
        if (v == NULL) break;
        {
            mino_type t = mino_type_of(v);
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
                    const mino_val *ch = v->as.chunked_cons.chunk;
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
        ((mino_val *)original)->as.vec.cached_hash = h;
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
uint32_t hash_val(const mino_val *v)
{
    /* Deviation from JVM Clojure: (hash x) uses FNV-1a internally; JVM
     * uses Murmur3/hasheq.  The equal-implies-equal-hash invariant holds
     * within mino but hash values are not JVM-compatible. */
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
        double d = v->as.f;
        /* Guard the (long long) cast: NaN and infinity have no finite
         * long-long representation; casting them is undefined behaviour
         * in C99 (6.3.1.4p1).  Both are non-finite so they can never
         * round-trip through an integer comparison; fall through to the
         * raw-bytes path instead. */
        if (!isnan(d) && !isinf(d)) {
            long long ll = (long long)d;
            if ((double)ll == d) {
                /* Same tag as MINO_INT so (= 1 1.0) matches in hash too. */
                h = fnv_mix(h, 0x03);
                return hash_long_long_bytes(h, ll);
            }
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
            mino_val *key = vec_nth(v->as.map.key_order, i);
            uint32_t    hk  = hash_val(key);
            uint32_t    hv  = hash_val(map_get_val(v, key));
            hk ^= hv * 2654435761u;
            acc ^= hk;
        }
        h = fnv_mix(h, 0x0a);
        h = hash_uint32_bytes(h, acc);
        ((mino_val *)v)->as.map.cached_hash = h;
        return h;
    }
    case MINO_SET: {
        /* XOR-fold of element hashes for order independence. */
        uint32_t acc = 0;
        size_t   n   = v->as.set.len;
        size_t   i;
        if (v->as.set.cached_hash != 0) return v->as.set.cached_hash;
        for (i = 0; i < n; i++) {
            mino_val *elem = vec_nth(v->as.set.key_order, i);
            acc ^= hash_val(elem);
        }
        h = fnv_mix(h, 0x0d);
        h = hash_uint32_bytes(h, acc);
        ((mino_val *)v)->as.set.cached_hash = h;
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
    case MINO_QUEUE: {
        /* Sequential category: hash exactly like a vector / list with
         * the same elements, since (= queue [1 2]) is true. */
        size_t i, n = v->as.queue.len;
        h = fnv_mix(h, 0x09);
        for (i = 0; i < n; i++) {
            h = hash_uint32_bytes(h, hash_val(mino_queue_nth(v, i)));
        }
        return h;
    }
    case MINO_BYTES:
        h = fnv_mix(h, 0x18);
        return hash_uint32_bytes(h, mino_bytes_hash(v));
    default:
        /* PRIM, FN, RECUR: identity-based. */
        h = fnv_mix(h, 0x0b);
        return hash_pointer_bytes(h, (uintptr_t)v);
    }
}
