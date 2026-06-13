/*
 * sequences_seq.c -- prim_seq + prim_realized_p and the seq-building
 * helper seq_cons_append. Carved out of prim/sequences.c.
 *
 * Internal to the prim subsystem; embedders should only use mino.h.
 */

#include "prim/internal.h"
#include "runtime/host_threads.h"   /* mino_future_realized_p */

#include <stdio.h>
#include <string.h>

/* ------------------------------------------------------------------------- */
/* seq and realized?                                                         */
/* ------------------------------------------------------------------------- */

/* seq_cons_append -- append one cell carrying elem to the (head, tail)
 * cons chain. Used while building seq lists from indexable sources. */
static void seq_cons_append(mino_state *S, mino_val **head,
                            mino_val **tail, mino_val *elem)
{
    mino_val *cell = mino_cons(S, elem, mino_nil(S));
    if (*tail == NULL) *head = cell;
    else mino_cons_cdr_set(S, *tail, cell);
    *tail = cell;
}

/* seq_kv_pair -- build the (k, v) entry value used for map/record
 * seqs. Returns a MINO_MAP_ENTRY so key / val type-check on the
 * result; equality with `[k v]` compares element-wise via the
 * cross-type sequential path in mino_eq. */
static mino_val *seq_kv_pair(mino_state *S, mino_val *k, mino_val *v)
{
    return mino_map_entry(S, k, v);
}

/* Public seq quartet. Each wraps the corresponding prim_* with a
 * one-shot cons-spine arg list so embedders don't have to construct
 * the list themselves. */

mino_val *mino_seq(mino_state *S, mino_val *coll)
{
    mino_val *args = mino_cons(S, coll, mino_nil(S));
    return prim_seq(S, args, NULL);
}

mino_val *mino_first(mino_val *coll)
{
    if (coll == NULL) return NULL;
    if (mino_type_of(coll) == MINO_NIL
        || mino_type_of(coll) == MINO_EMPTY_LIST) {
        return NULL;
    }
    if (mino_type_of(coll) == MINO_CONS) return coll->as.cons.car;
    /* For non-cons collections, embedders should call mino_seq first
     * (or use mino_iter). This entry point is the cheap branch: it
     * does not force lazy thunks or build a seq spine. */
    return NULL;
}

mino_val *mino_rest(mino_state *S, mino_val *coll)
{
    mino_val *args = mino_cons(S, coll, mino_nil(S));
    return prim_rest(S, args, NULL);
}

mino_val *mino_next(mino_state *S, mino_val *coll)
{
    mino_val *r = mino_rest(S, coll);
    if (r == NULL) return NULL;
    return mino_seq(S, r);
}

mino_val *prim_seq(mino_state *S, mino_val *args, mino_env *env)
{
    mino_val *coll;
    (void)env;
    if (!mino_is_cons(args) || mino_is_cons(args->as.cons.cdr)) {
        return prim_throw_classified(S, "eval/arity", "MAR001", "seq requires one argument");
    }
    coll = args->as.cons.car;
    if (coll == NULL || mino_type_of(coll) == MINO_NIL) return mino_nil(S);
    if (mino_type_of(coll) == MINO_EMPTY_LIST) return mino_nil(S);
    if (mino_type_of(coll) == MINO_LAZY) {
        mino_val *forced = lazy_force(S, coll);
        if (forced == NULL) return NULL;
        if (mino_type_of(forced) == MINO_NIL) return mino_nil(S);
        if (mino_type_of(forced) == MINO_EMPTY_LIST) return mino_nil(S);
        return forced;
    }
    if (mino_type_of(coll) == MINO_CONS) return coll;
    if (mino_type_of(coll) == MINO_CHUNKED_CONS) return coll;
    if (mino_type_of(coll) == MINO_VECTOR) {
        /* Emit a chunked-cons spine of 32-element chunks. Vector
         * leaves are already 32-wide (MINO_VEC_WIDTH), so consumers
         * that propagate chunkedness (map/filter/take/keep/...) get
         * end-to-end chunk pipelines without per-element allocator
         * overhead. */
        size_t total = coll->as.vec.len;
        size_t i;
        size_t n_chunks;
        size_t c;
        mino_val **chunks;
        mino_val  *more;
        if (total == 0) return mino_nil(S);
        n_chunks = (total + 31u) / 32u;
        chunks = (mino_val **)gc_alloc_typed(S, GC_T_VALARR,
            n_chunks * sizeof(*chunks));
        if (chunks == NULL) return NULL;
        gc_pin((mino_val *)chunks);
        for (c = 0; c < n_chunks; c++) {
            size_t base = c * 32u;
            unsigned cap = (unsigned)(total - base < 32u ? total - base : 32u);
            mino_val *buf = mino_chunk_buffer(S, cap);
            if (buf == NULL) { gc_unpin(1); return NULL; }
            for (i = 0; i < cap; i++) {
                if (!mino_chunk_append(buf, vec_nth(coll, base + i))) {
                    gc_unpin(1);
                    return NULL;
                }
            }
            mino_chunk_seal(buf);
            chunks[c] = buf;
        }
        more = mino_nil(S);
        for (c = n_chunks; c-- > 0; ) {
            more = mino_chunked_cons(S, chunks[c], more);
            if (more == NULL) { gc_unpin(1); return NULL; }
        }
        gc_unpin(1);
        return more;
    }
    if (mino_type_of(coll) == MINO_MAP_ENTRY) {
        /* Two-element chunked-seq over (k, v). */
        mino_val *buf = mino_chunk_buffer(S, 2);
        if (buf == NULL) return NULL;
        if (!mino_chunk_append(buf, coll->as.map_entry.k)) return NULL;
        if (!mino_chunk_append(buf, coll->as.map_entry.v)) return NULL;
        mino_chunk_seal(buf);
        return mino_chunked_cons(S, buf, mino_nil(S));
    }
    if (mino_type_of(coll) == MINO_HOST_ARRAY) {
        /* Emit a single-chunk MINO_CHUNKED_CONS so seq? is true and
         * first/rest/count walk the elements; predicates like
         * vector? / coll? / counted? remain false (no MINO_HOST_ARRAY
         * in their type-tag lists). */
        size_t total = coll->as.host_array.len;
        mino_val *buf;
        size_t i;
        if (total == 0) return mino_nil(S);
        buf = mino_chunk_buffer(S, (unsigned)total);
        if (buf == NULL) return NULL;
        for (i = 0; i < total; i++) {
            if (!mino_chunk_append(buf, coll->as.host_array.vals[i])) return NULL;
        }
        mino_chunk_seal(buf);
        return mino_chunked_cons(S, buf, mino_nil(S));
    }
    if (mino_type_of(coll) == MINO_MAP) {
        mino_val *head = mino_nil(S), *tail = NULL;
        size_t i;
        if (coll->as.map.len == 0) return mino_nil(S);
        for (i = 0; i < coll->as.map.len; i++) {
            mino_val *key = vec_nth(coll->as.map.key_order, i);
            seq_cons_append(S, &head, &tail,
                seq_kv_pair(S, key, map_get_val(coll, key)));
        }
        return head;
    }
    if (mino_type_of(coll) == MINO_SET) {
        mino_val *head = mino_nil(S), *tail = NULL;
        size_t i;
        if (coll->as.set.len == 0) return mino_nil(S);
        for (i = 0; i < coll->as.set.len; i++) {
            seq_cons_append(S, &head, &tail,
                vec_nth(coll->as.set.key_order, i));
        }
        return head;
    }
    if (mino_type_of(coll) == MINO_SORTED_MAP || mino_type_of(coll) == MINO_SORTED_SET) {
        /* sorted_seq returns a MINO_CONS chain, but the JVM Clojure
         * (seq sorted-map) is a Seq, not a PersistentList -- (list?
         * (seq (sorted-map ...))) is false. Re-package the cons chain
         * as a single-chunk MINO_CHUNKED_CONS so list? returns false
         * while everything else (first, rest, count, ...) keeps
         * working. Sorted collections are typically small enough that
         * one chunk is fine. */
        mino_val *cons_seq = sorted_seq(S, coll);
        mino_val *p, *buf, *result;
        unsigned    n, i = 0;
        if (cons_seq == NULL || !mino_is_cons(cons_seq)) return cons_seq;
        n = 0;
        for (p = cons_seq; mino_is_cons(p); p = p->as.cons.cdr) n++;
        buf = mino_chunk_buffer(S, n);
        if (buf == NULL) return NULL;
        for (p = cons_seq; mino_is_cons(p); p = p->as.cons.cdr) {
            if (!mino_chunk_append(buf, p->as.cons.car)) return NULL;
            i++;
        }
        mino_chunk_seal(buf);
        result = mino_chunked_cons(S, buf, mino_nil(S));
        (void)i;
        return result;
    }
    if (mino_type_of(coll) == MINO_STRING) {
        /* Per Clojure, (seq "abc") yields a sequence of chars, not
         * substrings. Walk UTF-8 codepoint by codepoint and emit
         * MINO_CHAR values. Malformed bytes are emitted as their
         * raw byte value (matching the read path's lenient byte
         * fallback). */
        mino_val  *head = mino_nil(S);
        mino_val  *tail = NULL;
        const unsigned char *bytes = (const unsigned char *)coll->as.s.data;
        size_t i = 0;
        size_t len = coll->as.s.len;
        if (len == 0) return mino_nil(S);
        while (i < len) {
            unsigned int cp;
            unsigned int b = bytes[i];
            size_t n;
            if (b < 0x80u)        { cp = b;          n = 1; }
            else if ((b & 0xE0u) == 0xC0u) { cp = b & 0x1Fu;  n = 2; }
            else if ((b & 0xF0u) == 0xE0u) { cp = b & 0x0Fu;  n = 3; }
            else if ((b & 0xF8u) == 0xF0u) { cp = b & 0x07u;  n = 4; }
            else                  { cp = b;          n = 1; }
            if (i + n > len) { cp = b; n = 1; }
            else {
                size_t k;
                int valid = 1;
                for (k = 1; k < n; k++) {
                    unsigned int c2 = bytes[i + k];
                    if ((c2 & 0xC0u) != 0x80u) { valid = 0; break; }
                    cp = (cp << 6) | (c2 & 0x3Fu);
                }
                if (!valid) { cp = b; n = 1; }
            }
            seq_cons_append(S, &head, &tail, mino_char(S, (int)cp));
            i += n;
        }
        return head;
    }
    if (mino_type_of(coll) == MINO_RECORD) {
        /* Declared field [k v] pairs first in declared order, then
         * ext entries in insertion order. Returns nil for an empty
         * record (no fields and no ext entries). */
        mino_val *fields = coll->as.record.type->as.record_type.fields;
        size_t n_fields = (fields != NULL) ? fields->as.vec.len : 0;
        size_t ext_n = (coll->as.record.ext != NULL)
            ? coll->as.record.ext->as.map.len : 0;
        mino_val *head = mino_nil(S), *tail = NULL;
        size_t i;
        if (n_fields == 0 && ext_n == 0) return mino_nil(S);
        for (i = 0; i < n_fields; i++) {
            seq_cons_append(S, &head, &tail,
                seq_kv_pair(S, vec_nth(fields, i),
                               coll->as.record.vals[i]));
        }
        if (ext_n > 0) {
            const mino_val *e = coll->as.record.ext;
            size_t k;
            for (k = 0; k < e->as.map.len; k++) {
                mino_val *ek = vec_nth(e->as.map.key_order, k);
                seq_cons_append(S, &head, &tail,
                    seq_kv_pair(S, ek, map_get_val(e, ek)));
            }
        }
        return head;
    }
    if (mino_type_of(coll) == MINO_QUEUE) {
        if (mino_queue_count(coll) == 0) return mino_nil(S);
        return mino_queue_seq(S, coll);
    }
    if (mino_type_of(coll) == MINO_BYTES) {
        if (mino_bytes_len(coll) == 0) return mino_nil(S);
        return mino_bytes_seq(S, coll);
    }
    {
        char msg[96];
        snprintf(msg, sizeof(msg), "seq: cannot coerce %s to a sequence",
                 type_tag_str(coll));
        return prim_throw_classified(S, "eval/type", "MTY001", msg);
    }
}

mino_val *prim_realized_p(mino_state *S, mino_val *args, mino_env *env)
{
    /* Per Clojure, realized? accepts only "pending" values
     * (lazy seqs, delays, promises, futures) and throws on any
     * other input. mino's pending types are MINO_LAZY (lazy seqs
     * and delays share this representation) and MINO_FUTURE. */
    mino_val *v;
    (void)env;
    if (!mino_is_cons(args) || mino_is_cons(args->as.cons.cdr)) {
        return prim_throw_classified(S, "eval/arity", "MAR001", "realized? requires one argument");
    }
    v = args->as.cons.car;
    if (v != NULL && mino_type_of(v) == MINO_LAZY) {
        return v->as.lazy.realized == LAZY_REALIZED
               ? mino_true(S) : mino_false(S);
    }
    if (v != NULL && mino_type_of(v) == MINO_FUTURE) {
        return mino_future_realized_p(v) ? mino_true(S) : mino_false(S);
    }
    return prim_throw_classified(S, "eval/type", "MTY001",
        "realized? expects a lazy seq, delay, promise, or future");
}
