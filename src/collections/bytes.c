/*
 * bytes.c -- MINO_BYTES: immutable binary-data value.
 *
 * Carries a malloc-owned unsigned char buffer plus byte length plus a
 * bit_tail count (0 for byte-aligned values, 1..7 for bit-aligned
 * bitstrings). v0.415 constructors always set bit_tail = 0; the field
 * is the forward-compatibility hook for the bit-syntax surface that
 * ships in a follow-on cycle.
 *
 * Equality is byte-for-byte over the value's bit-length. Hash uses
 * Clojure-shape hash-ordered-coll mixing over the byte values so two
 * MINO_BYTES with the same bytes share the same hash.
 *
 * The buffer is owned by the value cell, not by the GC. The
 * finalizer registered on GC_T_VAL frees it.
 */

#include "runtime/internal.h"
#include "prim/internal.h"

#include <stdlib.h>
#include <string.h>

/* Allocate a fresh MINO_BYTES cell. `src` may be NULL for a zero-fill
 * buffer of length n. */
mino_val *mino_bytes(mino_state *S, const unsigned char *src, size_t n)
{
    mino_val *v;
    unsigned char *buf = NULL;
    if (n > 0) {
        buf = (unsigned char *)calloc(1, n);
        if (buf == NULL) {
            return prim_throw_classified(S, "internal", "MIN001",
                "byte-array: out of memory");
        }
        if (src != NULL) memcpy(buf, src, n);
    }
    v = (mino_val *)gc_alloc_typed(S, GC_T_VAL, sizeof(mino_val));
    v->type             = MINO_BYTES;
    v->meta             = NULL;
    v->as.bytes.data    = buf;
    v->as.bytes.byte_len = n;
    v->as.bytes.bit_tail = 0;
    v->as.bytes.cached_hash = 0;
    return v;
}

mino_val *mino_bytes_from_array(mino_state *S, const signed char *src,
                                  size_t n)
{
    /* The unsigned vs signed distinction at this layer is purely about
     * the embedder's pointer type; the buffer is stored unsigned. */
    return mino_bytes(S, (const unsigned char *)src, n);
}

int mino_is_bytes(const mino_val *v)
{
    if (v == NULL || mino_type_of(v) != MINO_BYTES) return 0;
    return v->as.bytes.bit_tail == 0;
}

int mino_is_bitstring(const mino_val *v)
{
    return v != NULL && mino_type_of(v) == MINO_BYTES;
}

size_t mino_bytes_len(const mino_val *v)
{
    if (v == NULL || mino_type_of(v) != MINO_BYTES) return 0;
    return v->as.bytes.byte_len;
}

size_t mino_bytes_bit_len(const mino_val *v)
{
    if (v == NULL || mino_type_of(v) != MINO_BYTES) return 0;
    return v->as.bytes.byte_len * 8 + v->as.bytes.bit_tail;
}

const unsigned char *mino_bytes_data(const mino_val *v)
{
    if (v == NULL || mino_type_of(v) != MINO_BYTES) return NULL;
    return v->as.bytes.data;
}

int mino_bytes_get(const mino_val *v, size_t i)
{
    if (v == NULL || mino_type_of(v) != MINO_BYTES) return -1;
    if (i >= v->as.bytes.byte_len) return -1;
    return (int)v->as.bytes.data[i];
}

/* Byte-for-byte equality. Two MINO_BYTES are equal when they have the
 * same byte_len, the same bit_tail, and the same bytes. */
int mino_bytes_eq(const mino_val *a, const mino_val *b)
{
    if (a == NULL || b == NULL) return 0;
    if (mino_type_of(a) != MINO_BYTES || mino_type_of(b) != MINO_BYTES) {
        return 0;
    }
    if (a->as.bytes.byte_len != b->as.bytes.byte_len) return 0;
    if (a->as.bytes.bit_tail != b->as.bytes.bit_tail) return 0;
    if (a->as.bytes.byte_len == 0) return 1;
    return memcmp(a->as.bytes.data, b->as.bytes.data,
                  a->as.bytes.byte_len) == 0;
}

/* Hash matching the sequence-hash contract (Clojure's hash-ordered-
 * coll algorithm). The bit_tail is folded into the mix so a byte-
 * aligned and a bit-aligned value with the same byte payload don't
 * collide. */
uint32_t mino_bytes_hash(const mino_val *v)
{
    size_t i, n;
    uint32_t h = 1u;
    if (v == NULL || mino_type_of(v) != MINO_BYTES) return 0u;
    n = v->as.bytes.byte_len;
    if (n == 0 && v->as.bytes.bit_tail == 0) return 0u;
    for (i = 0; i < n; i++) {
        h = 31u * h + (uint32_t)v->as.bytes.data[i];
    }
    h ^= (uint32_t)n;
    h ^= ((uint32_t)v->as.bytes.bit_tail << 16);
    h *= 0x9e3779b9u;
    return h;
}

/* Build a seq view of the bytes value: a cons spine of unsigned 0..255
 * integers, one per byte. Returns the empty-list singleton for an
 * empty value. The bit_tail of a bit-aligned value is included as a
 * final byte where the unused low bits are zero-padded; this matches
 * Erlang's binary-to-list semantics for the byte-aligned subset and
 * keeps the seq deterministic for bit-aligned values without throwing.
 */
mino_val *mino_bytes_seq(mino_state *S, const mino_val *v)
{
    mino_val *head;
    mino_val *tail;
    size_t i, n;
    if (v == NULL || mino_type_of(v) != MINO_BYTES) {
        return mino_empty_list(S);
    }
    n = v->as.bytes.byte_len;
    if (n == 0) return mino_empty_list(S);
    head = mino_nil(S);
    tail = NULL;
    for (i = 0; i < n; i++) {
        mino_val *cell = mino_cons(S,
            mino_int(S, (long long)(unsigned)v->as.bytes.data[i]),
            mino_nil(S));
        if (tail == NULL) head = cell;
        else mino_cons_cdr_set(S, tail, cell);
        tail = cell;
    }
    return head;
}
