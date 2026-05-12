/*
 * prim_sequences.c -- sequence iteration and higher-order primitives:
 *                     seq, realized?, seq_iter_*, reduce, reduced,
 *                     into, apply, reverse, sort, set, rangev, mapv,
 *                     filterv, peek, pop, find, empty, rseq,
 *                     sorted-map, sorted-set.
 */

#include "prim/internal.h"
#include "runtime/host_threads.h"

/* ------------------------------------------------------------------------- */
/* seq and realized?                                                         */
/* ------------------------------------------------------------------------- */

/* seq_cons_append -- append one cell carrying elem to the (head, tail)
 * cons chain. Used while building seq lists from indexable sources. */
static void seq_cons_append(mino_state_t *S, mino_val_t **head,
                            mino_val_t **tail, mino_val_t *elem)
{
    mino_val_t *cell = mino_cons(S, elem, mino_nil(S));
    if (*tail == NULL) *head = cell;
    else mino_cons_cdr_set(S, *tail, cell);
    *tail = cell;
}

/* seq_kv_pair -- build the (k, v) entry value used for map/record
 * seqs. Returns a MINO_MAP_ENTRY so key / val type-check on the
 * result; equality with `[k v]` compares element-wise via the
 * cross-type sequential path in mino_eq. */
static mino_val_t *seq_kv_pair(mino_state_t *S, mino_val_t *k, mino_val_t *v)
{
    return mino_map_entry(S, k, v);
}

mino_val_t *prim_seq(mino_state_t *S, mino_val_t *args, mino_env_t *env)
{
    mino_val_t *coll;
    (void)env;
    if (!mino_is_cons(args) || mino_is_cons(args->as.cons.cdr)) {
        return prim_throw_classified(S, "eval/arity", "MAR001", "seq requires one argument");
    }
    coll = args->as.cons.car;
    if (coll == NULL || mino_type_of(coll) == MINO_NIL) return mino_nil(S);
    if (mino_type_of(coll) == MINO_EMPTY_LIST) return mino_nil(S);
    if (mino_type_of(coll) == MINO_LAZY) {
        mino_val_t *forced = lazy_force(S, coll);
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
        mino_val_t **chunks;
        mino_val_t  *more;
        if (total == 0) return mino_nil(S);
        n_chunks = (total + 31u) / 32u;
        chunks = (mino_val_t **)gc_alloc_typed(S, GC_T_VALARR,
            n_chunks * sizeof(*chunks));
        for (c = 0; c < n_chunks; c++) {
            size_t base = c * 32u;
            unsigned cap = (unsigned)(total - base < 32u ? total - base : 32u);
            mino_val_t *buf = mino_chunk_buffer(S, cap);
            if (buf == NULL) return NULL;
            for (i = 0; i < cap; i++) {
                if (!mino_chunk_append(buf, vec_nth(coll, base + i))) return NULL;
            }
            mino_chunk_seal(buf);
            chunks[c] = buf;
        }
        more = mino_nil(S);
        for (c = n_chunks; c-- > 0; ) {
            more = mino_chunked_cons(S, chunks[c], more);
            if (more == NULL) return NULL;
        }
        return more;
    }
    if (mino_type_of(coll) == MINO_MAP_ENTRY) {
        /* Two-element chunked-seq over (k, v). */
        mino_val_t *buf = mino_chunk_buffer(S, 2);
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
        mino_val_t *buf;
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
        mino_val_t *head = mino_nil(S), *tail = NULL;
        size_t i;
        if (coll->as.map.len == 0) return mino_nil(S);
        for (i = 0; i < coll->as.map.len; i++) {
            mino_val_t *key = vec_nth(coll->as.map.key_order, i);
            seq_cons_append(S, &head, &tail,
                seq_kv_pair(S, key, map_get_val(coll, key)));
        }
        return head;
    }
    if (mino_type_of(coll) == MINO_SET) {
        mino_val_t *head = mino_nil(S), *tail = NULL;
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
        mino_val_t *cons_seq = sorted_seq(S, coll);
        mino_val_t *p, *buf, *result;
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
        mino_val_t  *head = mino_nil(S);
        mino_val_t  *tail = NULL;
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
        mino_val_t *fields = coll->as.record.type->as.record_type.fields;
        size_t n_fields = (fields != NULL) ? fields->as.vec.len : 0;
        size_t ext_n = (coll->as.record.ext != NULL)
            ? coll->as.record.ext->as.map.len : 0;
        mino_val_t *head = mino_nil(S), *tail = NULL;
        size_t i;
        if (n_fields == 0 && ext_n == 0) return mino_nil(S);
        for (i = 0; i < n_fields; i++) {
            seq_cons_append(S, &head, &tail,
                seq_kv_pair(S, vec_nth(fields, i),
                               coll->as.record.vals[i]));
        }
        if (ext_n > 0) {
            const mino_val_t *e = coll->as.record.ext;
            size_t k;
            for (k = 0; k < e->as.map.len; k++) {
                mino_val_t *ek = vec_nth(e->as.map.key_order, k);
                seq_cons_append(S, &head, &tail,
                    seq_kv_pair(S, ek, map_get_val(e, ek)));
            }
        }
        return head;
    }
    {
        char msg[96];
        snprintf(msg, sizeof(msg), "seq: cannot coerce %s to a sequence",
                 type_tag_str(coll));
        return prim_throw_classified(S, "eval/type", "MTY001", msg);
    }
}

mino_val_t *prim_realized_p(mino_state_t *S, mino_val_t *args, mino_env_t *env)
{
    /* Per Clojure, realized? accepts only "pending" values
     * (lazy seqs, delays, promises, futures) and throws on any
     * other input. mino's pending types are MINO_LAZY (lazy seqs
     * and delays share this representation) and MINO_FUTURE. */
    mino_val_t *v;
    (void)env;
    if (!mino_is_cons(args) || mino_is_cons(args->as.cons.cdr)) {
        return prim_throw_classified(S, "eval/arity", "MAR001", "realized? requires one argument");
    }
    v = args->as.cons.car;
    if (v != NULL && mino_type_of(v) == MINO_LAZY) {
        return v->as.lazy.realized ? mino_true(S) : mino_false(S);
    }
    if (v != NULL && mino_type_of(v) == MINO_FUTURE) {
        return mino_future_realized_p(v) ? mino_true(S) : mino_false(S);
    }
    return prim_throw_classified(S, "eval/type", "MTY001",
        "realized? expects a lazy seq, delay, promise, or future");
}

/* ------------------------------------------------------------------------- */
/* Sequence iterator                                                         */
/* ------------------------------------------------------------------------- */

void seq_iter_init(mino_state_t *S, seq_iter_t *it, const mino_val_t *coll)
{
    /* Force lazy seqs so they behave as cons lists. */
    if (coll != NULL && mino_type_of(coll) == MINO_LAZY) {
        coll = lazy_force(S, (mino_val_t *)coll);
    }
    /* Sorted collections: flatten to cons list for uniform iteration. */
    if (coll != NULL &&
        (mino_type_of(coll) == MINO_SORTED_MAP || mino_type_of(coll) == MINO_SORTED_SET)) {
        coll = sorted_seq(S, coll);
    }
    it->coll  = coll;
    it->idx   = 0;
    it->cons_p = (coll != NULL && mino_type_of(coll) == MINO_CONS) ? coll : NULL;
    if (coll != NULL && mino_type_of(coll) == MINO_CHUNKED_CONS) {
        it->cons_p = coll;
        it->idx    = (size_t)coll->as.chunked_cons.off;
    }
}

int seq_iter_done(const seq_iter_t *it)
{
    const mino_val_t *c = it->coll;
    if (c == NULL || mino_type_of(c) == MINO_NIL) return 1;
    switch (mino_type_of(c)) {
    case MINO_CONS:   return it->cons_p == NULL || mino_type_of(it->cons_p) != MINO_CONS;
    case MINO_CHUNKED_CONS:
        return it->cons_p == NULL || mino_type_of(it->cons_p) != MINO_CHUNKED_CONS;
    case MINO_VECTOR:     return it->idx >= c->as.vec.len;
    case MINO_HOST_ARRAY: return it->idx >= c->as.host_array.len;
    case MINO_MAP_ENTRY:  return it->idx >= 2;
    case MINO_MAP:    return it->idx >= c->as.map.len;
    case MINO_SET:    return it->idx >= c->as.set.len;
    case MINO_STRING: return it->idx >= c->as.s.len;
    default:          return 1;
    }
}

/* Decode the UTF-8 codepoint at byte position pos in `data` (length
 * len bytes), writing the decoded codepoint to *cp_out and the byte
 * step to *step_out. Falls back to a 1-byte literal for malformed
 * leading bytes or truncated continuations, matching prim_seq's
 * lenient string walk. Caller must ensure pos < len. */
static void utf8_step(const unsigned char *data, size_t len, size_t pos,
                      unsigned int *cp_out, size_t *step_out)
{
    unsigned int b = data[pos];
    unsigned int cp;
    size_t       n;
    if (b < 0x80u)                 { cp = b;          n = 1; }
    else if ((b & 0xE0u) == 0xC0u) { cp = b & 0x1Fu;  n = 2; }
    else if ((b & 0xF0u) == 0xE0u) { cp = b & 0x0Fu;  n = 3; }
    else if ((b & 0xF8u) == 0xF0u) { cp = b & 0x07u;  n = 4; }
    else                           { cp = b;          n = 1; }
    if (pos + n > len) { cp = b; n = 1; }
    else {
        size_t k;
        int    valid = 1;
        for (k = 1; k < n; k++) {
            unsigned int c2 = data[pos + k];
            if ((c2 & 0xC0u) != 0x80u) { valid = 0; break; }
            cp = (cp << 6) | (c2 & 0x3Fu);
        }
        if (!valid) { cp = b; n = 1; }
    }
    *cp_out   = cp;
    *step_out = n;
}

mino_val_t *seq_iter_val(mino_state_t *S, const seq_iter_t *it)
{
    const mino_val_t *c = it->coll;
    switch (mino_type_of(c)) {
    case MINO_CONS:   return it->cons_p->as.cons.car;
    case MINO_CHUNKED_CONS: {
        const mino_val_t *cell = it->cons_p;
        const mino_val_t *ch   = cell->as.chunked_cons.chunk;
        return ch->as.chunk.vals[it->idx];
    }
    case MINO_VECTOR: return vec_nth(c, it->idx);
    case MINO_HOST_ARRAY: return c->as.host_array.vals[it->idx];
    case MINO_MAP_ENTRY:
        return it->idx == 0 ? c->as.map_entry.k : c->as.map_entry.v;
    case MINO_MAP: {
        /* Yield MINO_MAP_ENTRY (one alloc) instead of a 2-vector
         * (header + 2-slot trie node, two allocs). Equal to [k v]
         * across mino_eq's cross-type sequential path, prints as
         * `[k v]`, and `vector?` is true -- the same contract
         * prim_seq's seq_kv_pair already uses for maps. */
        mino_val_t *key = vec_nth(c->as.map.key_order, it->idx);
        mino_val_t *val = c->as.map.val_order != NULL
                              ? vec_nth(c->as.map.val_order, it->idx)
                              : map_get_val(c, key);
        return mino_map_entry(S, key, val);
    }
    case MINO_SET:    return vec_nth(c->as.set.key_order, it->idx);
    case MINO_STRING: {
        /* Yield a MINO_CHAR per codepoint, matching prim_seq. */
        unsigned int cp;
        size_t       step;
        utf8_step((const unsigned char *)c->as.s.data, c->as.s.len,
                  it->idx, &cp, &step);
        (void)step;
        return mino_char(S, (int)cp);
    }
    default:          return mino_nil(S);
    }
}

void seq_iter_next(mino_state_t *S, seq_iter_t *it)
{
    if (it->coll != NULL && mino_type_of(it->coll) == MINO_CONS) {
        if (it->cons_p != NULL && mino_type_of(it->cons_p) == MINO_CONS) {
            const mino_val_t *next = it->cons_p->as.cons.cdr;
            /* Force lazy tail if present. */
            if (next != NULL && mino_type_of(next) == MINO_LAZY) {
                next = lazy_force(S, (mino_val_t *)next);
            }
            it->cons_p = next;
        }
    } else if (it->coll != NULL && mino_type_of(it->coll) == MINO_CHUNKED_CONS) {
        if (it->cons_p != NULL && mino_type_of(it->cons_p) == MINO_CHUNKED_CONS) {
            const mino_val_t *cell = it->cons_p;
            const mino_val_t *ch   = cell->as.chunked_cons.chunk;
            unsigned          next_idx = (unsigned)(it->idx + 1);
            if (next_idx < ch->as.chunk.len) {
                it->idx = next_idx;
                return;
            }
            {
                const mino_val_t *more = cell->as.chunked_cons.more;
                if (more != NULL && mino_type_of(more) == MINO_LAZY) {
                    more = lazy_force(S, (mino_val_t *)more);
                }
                if (more != NULL && mino_type_of(more) == MINO_CONS) {
                    /* Switch dispatch to cons-mode for the rest of the
                     * walk. */
                    it->coll   = more;
                    it->cons_p = more;
                    it->idx    = 0;
                } else if (more != NULL
                           && mino_type_of(more) == MINO_CHUNKED_CONS) {
                    it->cons_p = more;
                    it->idx    = (size_t)more->as.chunked_cons.off;
                } else {
                    it->cons_p = NULL;
                }
            }
        }
    } else if (it->coll != NULL && mino_type_of(it->coll) == MINO_STRING) {
        /* Step by the current codepoint's byte length so multi-byte
         * UTF-8 characters advance correctly. */
        unsigned int cp;
        size_t       step;
        utf8_step((const unsigned char *)it->coll->as.s.data,
                  it->coll->as.s.len, it->idx, &cp, &step);
        (void)cp;
        it->idx += step;
    } else {
        it->idx++;
    }
}

/* ------------------------------------------------------------------------- */
/* Sequence primitives (strict — no lazy seqs)                               */
/* ------------------------------------------------------------------------- */

/* (reduced val) — wrap val to signal early termination in reduce. */
mino_val_t *prim_reduced(mino_state_t *S, mino_val_t *args, mino_env_t *env)
{
    mino_val_t *v;
    (void)env;
    if (!mino_is_cons(args) || mino_is_cons(args->as.cons.cdr)) {
        return prim_throw_classified(S, "eval/arity", "MAR001", "reduced requires exactly 1 argument");
    }
    v = alloc_val(S, MINO_REDUCED);
    v->as.reduced.val = args->as.cons.car;
    return v;
}

/* (reduced? x) — true if x is a reduced wrapper. */
mino_val_t *prim_reduced_p(mino_state_t *S, mino_val_t *args, mino_env_t *env)
{
    (void)env;
    if (!mino_is_cons(args) || mino_is_cons(args->as.cons.cdr)) {
        return prim_throw_classified(S, "eval/arity", "MAR001", "reduced? requires exactly 1 argument");
    }
    return (args->as.cons.car != NULL && mino_type_of(args->as.cons.car) == MINO_REDUCED)
        ? mino_true(S) : mino_false(S);
}

/* Numeric reducer fast path: (reduce <op> [init] (range start end step))
 * where <op> is one of the canonical arithmetic prims (currently +).
 * Walks the range as a tight integer loop, never materialising chunks
 * or boxing intermediates. Returns NULL on miss (overflow or
 * unsupported reducer); the caller falls through to the generic path. */
static mino_val_t *reduce_int_range(mino_state_t *S, mino_val_t *fn,
                                     mino_val_t *init, int has_init,
                                     long long start, long long end,
                                     long long step)
{
    long long acc = 0;
    long long i;
    if (fn == NULL || mino_type_of(fn) != MINO_PRIM) return NULL;
    if (fn->as.prim.fn != prim_add) return NULL;  /* extend later */
    /* Compute element count; range is finite (caller already checked). */
    if (step == 0) return NULL;
    if (has_init) {
        if (init == NULL || !mino_val_int_p(init)) return NULL;
        acc = mino_val_int_get(init);
    } else {
        if (step > 0 ? start >= end : start <= end) {
            return apply_callable(S, fn, mino_nil(S), NULL);
        }
        acc   = start;
        start = start + step;
    }
    if (step > 0) {
        for (i = start; i < end; i += step) {
#if defined(__GNUC__) || defined(__clang__)
            long long r;
            if (__builtin_add_overflow(acc, i, &r)) return NULL;
            acc = r;
#else
            acc += i;
#endif
        }
    } else {
        for (i = start; i > end; i += step) {
#if defined(__GNUC__) || defined(__clang__)
            long long r;
            if (__builtin_add_overflow(acc, i, &r)) return NULL;
            acc = r;
#else
            acc += i;
#endif
        }
    }
    return mino_int(S, acc);
}

/* Inner reduction step: combine acc and elem under fn, honoring the
 * int+int arithmetic fast lane and MINO_REDUCED early-exit. Returns
 * 0 = continue, 1 = stop (acc holds the final), -1 = error. */
static int reduce_step(mino_state_t *S, mino_val_t *fn, mino_val_t **acc_io,
                       mino_val_t *elem, mino_env_t *env)
{
    mino_val_t *acc = *acc_io;
    if (fn != NULL && mino_type_of(fn) == MINO_PRIM
        && fn->as.prim.fn != NULL
        && acc != NULL && mino_val_int_p(acc)
        && elem != NULL && mino_val_int_p(elem)) {
        mino_prim_fn p  = fn->as.prim.fn;
        long long    ai = mino_val_int_get(acc);
        long long    bi = mino_val_int_get(elem);
        long long    r;
        int          handled = 0;
        if (p == prim_add) {
#if defined(__GNUC__) || defined(__clang__)
            if (!__builtin_add_overflow(ai, bi, &r)) {
                *acc_io = mino_int(S, r);
                handled = 1;
            }
#endif
        } else if (p == prim_mul) {
#if defined(__GNUC__) || defined(__clang__)
            if (!__builtin_mul_overflow(ai, bi, &r)) {
                *acc_io = mino_int(S, r);
                handled = 1;
            }
#endif
        } else if (p == prim_sub) {
#if defined(__GNUC__) || defined(__clang__)
            if (!__builtin_sub_overflow(ai, bi, &r)) {
                *acc_io = mino_int(S, r);
                handled = 1;
            }
#endif
        } else if (p == prim_bit_and) {
            *acc_io = mino_int(S, ai & bi);
            handled = 1;
        } else if (p == prim_bit_or) {
            *acc_io = mino_int(S, ai | bi);
            handled = 1;
        } else if (p == prim_bit_xor) {
            *acc_io = mino_int(S, ai ^ bi);
            handled = 1;
        }
        if (handled) return 0;
    }
    {
        mino_val_t *call_a = mino_cons(
            S, acc, mino_cons(S, elem, mino_nil(S)));
        *acc_io = apply_callable(S, fn, call_a, env);
    }
    if (*acc_io == NULL) return -1;
    if (mino_type_of(*acc_io) == MINO_REDUCED) {
        *acc_io = (*acc_io)->as.reduced.val;
        return 1;
    }
    return 0;
}

/* Direct-walk fast path for (reduce fn coll [init]) over a persistent
 * map. Yields a MINO_MAP_ENTRY per pair (cheaper than the [k v] vector
 * pair seq_iter_val once produced) and skips the seq_iter switch
 * dispatch. Iteration order matches key_order (insertion order),
 * which is the order seq_iter walks. For flatmaps we read val_order
 * in parallel; for HAMT we resolve via map_get_val. */
static mino_val_t *reduce_map_direct(mino_state_t *S, mino_val_t *fn,
                                     mino_val_t *acc, int has_init,
                                     mino_val_t *m, mino_env_t *env)
{
    size_t      i, n = m->as.map.len;
    int         is_flat = (m->as.map.val_order != NULL);
    mino_val_t *ko, *vo;
    if (n == 0) {
        if (has_init) return acc;
        return apply_callable(S, fn, mino_nil(S), env);
    }
    ko = m->as.map.key_order;
    vo = m->as.map.val_order;
    if (!has_init) {
        mino_val_t *k0 = vec_nth(ko, 0);
        mino_val_t *v0 = is_flat ? vec_nth(vo, 0) : map_get_val(m, k0);
        acc = mino_map_entry(S, k0, v0);
        i   = 1;
    } else {
        i = 0;
    }
    for (; i < n; i++) {
        mino_val_t *k = vec_nth(ko, i);
        mino_val_t *v = is_flat ? vec_nth(vo, i) : map_get_val(m, k);
        mino_val_t *entry = mino_map_entry(S, k, v);
        int rc = reduce_step(S, fn, &acc, entry, env);
        if (rc == -1) return NULL;
        if (rc == 1)  return acc;
    }
    return acc;
}

/* Recursive trie walker for vector reduce. `pos_io` tracks the
 * absolute backing position visited so far so subvec offset/len
 * windows can be honored without a separate offset-aware codepath.
 * Returns 0 = continue, 1 = stop (Reduced fired), -1 = error. */
static int reduce_vec_trie_walk(mino_state_t *S, mino_val_t *fn,
                                mino_val_t **acc_io, mino_env_t *env,
                                const mino_vec_node_t *node, unsigned shift,
                                size_t *pos_io, size_t start, size_t end)
{
    unsigned i;
    if (shift == 0) {
        /* Leaf: iterate the 32-slot block directly. The hot inner loop
         * is the only path that allocates an inline-tagged int via
         * reduce_step's int+int lane -- no per-step vec_nth, no
         * seq_iter switch. */
        for (i = 0; i < node->count; i++) {
            size_t p = *pos_io;
            (*pos_io)++;
            if (p >= end) return 0;
            if (p < start) continue;
            {
                int rc = reduce_step(S, fn, acc_io,
                                     (mino_val_t *)node->slots[i], env);
                if (rc != 0) return rc;
            }
        }
        return 0;
    }
    for (i = 0; i < node->count; i++) {
        mino_vec_node_t *child = (mino_vec_node_t *)node->slots[i];
        int rc;
        if (*pos_io >= end) return 0;
        if (child == NULL) continue;
        rc = reduce_vec_trie_walk(S, fn, acc_io, env, child,
                                  shift - MINO_VEC_B, pos_io, start, end);
        if (rc != 0) return rc;
    }
    return 0;
}

/* Walk a vector (or, indirectly, a set's key_order vector) applying
 * reduce_step. Skips the seq_iter dispatch and the per-element
 * vec_nth's O(log32) trie navigation; one leaf pass visits each
 * element with a tight 32-slot inner loop. Honors offset (subvec)
 * by passing absolute backing positions into the walker. */
static mino_val_t *reduce_vec_apply(mino_state_t *S, mino_val_t *fn,
                                    mino_val_t *acc, int has_init,
                                    const mino_val_t *v, mino_env_t *env)
{
    size_t n          = v->as.vec.len;
    size_t offset     = v->as.vec.offset;
    size_t trie_count = v->as.vec.blen - v->as.vec.tail_len;
    size_t start      = offset;
    size_t end        = offset + n;
    size_t pos        = 0;
    if (n == 0) {
        if (has_init) return acc;
        return apply_callable(S, fn, mino_nil(S), env);
    }
    if (!has_init) {
        acc   = vec_nth(v, 0);
        start = offset + 1;
    }
    if (trie_count > 0 && v->as.vec.root != NULL && start < trie_count
        && end > 0) {
        size_t trie_end = end < trie_count ? end : trie_count;
        int rc = reduce_vec_trie_walk(S, fn, &acc, env, v->as.vec.root,
                                      v->as.vec.shift, &pos, start,
                                      trie_end);
        if (rc == -1) return NULL;
        if (rc == 1)  return acc;
    }
    /* Tail: first element sits at absolute position trie_count. */
    if (v->as.vec.tail_len > 0 && end > trie_count) {
        unsigned i;
        for (i = 0; i < v->as.vec.tail_len; i++) {
            size_t p = trie_count + i;
            if (p >= end) break;
            if (p < start) continue;
            {
                int rc = reduce_step(S, fn, &acc,
                                     (mino_val_t *)v->as.vec.tail->slots[i],
                                     env);
                if (rc == -1) return NULL;
                if (rc == 1)  return acc;
            }
        }
    }
    return acc;
}

/* Direct-walk fast path for (reduce fn vec [init]). */
static mino_val_t *reduce_vec_direct(mino_state_t *S, mino_val_t *fn,
                                     mino_val_t *acc, int has_init,
                                     mino_val_t *v, mino_env_t *env)
{
    return reduce_vec_apply(S, fn, acc, has_init, v, env);
}

/* Direct-walk fast path for (reduce fn set [init]). Routes through
 * reduce_vec_apply since the set's insertion-order key_order is
 * itself a vector. */
static mino_val_t *reduce_set_direct(mino_state_t *S, mino_val_t *fn,
                                     mino_val_t *acc, int has_init,
                                     mino_val_t *s, mino_env_t *env)
{
    if (s->as.set.len == 0) {
        if (has_init) return acc;
        return apply_callable(S, fn, mino_nil(S), env);
    }
    return reduce_vec_apply(S, fn, acc, has_init, s->as.set.key_order, env);
}

mino_val_t *prim_reduce(mino_state_t *S, mino_val_t *args, mino_env_t *env)
{
    mino_val_t *fn;
    mino_val_t *acc;
    mino_val_t *coll;
    seq_iter_t  it;
    size_t      n;
    long long   r_start = 0, r_end = 0, r_step = 1;
    int         r_inf   = 0;
    arg_count(S, args, &n);
    if (n == 2) {
        /* (reduce f coll) — first element is the initial accumulator. */
        fn   = args->as.cons.car;
        coll = args->as.cons.cdr->as.cons.car;
        if (coll == NULL || mino_type_of(coll) == MINO_NIL) {
            /* (reduce f nil) → (f) */
            return apply_callable(S, fn, mino_nil(S), env);
        }
        if (lazy_is_int_range(coll, &r_start, &r_end, &r_step, &r_inf)
            && !r_inf) {
            mino_val_t *fast = reduce_int_range(
                S, fn, NULL, 0, r_start, r_end, r_step);
            if (fast != NULL) return fast;
        }
        if (mino_type_of(coll) == MINO_MAP)
            return reduce_map_direct(S, fn, NULL, 0, coll, env);
        if (mino_type_of(coll) == MINO_SET)
            return reduce_set_direct(S, fn, NULL, 0, coll, env);
        if (mino_type_of(coll) == MINO_VECTOR)
            return reduce_vec_direct(S, fn, NULL, 0, coll, env);
        seq_iter_init(S, &it, coll);
        if (seq_iter_done(&it)) {
            return apply_callable(S, fn, mino_nil(S), env);
        }
        acc = seq_iter_val(S, &it);
        seq_iter_next(S, &it);
    } else if (n == 3) {
        /* (reduce f init coll) */
        fn   = args->as.cons.car;
        acc  = args->as.cons.cdr->as.cons.car;
        coll = args->as.cons.cdr->as.cons.cdr->as.cons.car;
        if (coll == NULL || mino_type_of(coll) == MINO_NIL) {
            return acc;
        }
        if (lazy_is_int_range(coll, &r_start, &r_end, &r_step, &r_inf)
            && !r_inf) {
            mino_val_t *fast = reduce_int_range(
                S, fn, acc, 1, r_start, r_end, r_step);
            if (fast != NULL) return fast;
        }
        if (mino_type_of(coll) == MINO_MAP)
            return reduce_map_direct(S, fn, acc, 1, coll, env);
        if (mino_type_of(coll) == MINO_SET)
            return reduce_set_direct(S, fn, acc, 1, coll, env);
        if (mino_type_of(coll) == MINO_VECTOR)
            return reduce_vec_direct(S, fn, acc, 1, coll, env);
        seq_iter_init(S, &it, coll);
    } else {
        return prim_throw_classified(S, "eval/arity", "MAR001", "reduce requires 2 or 3 arguments");
    }
    /* Inner loop: reduce_step handles the int+int fast lane and the
     * MINO_REDUCED early-exit; we just drive seq_iter. */
    while (!seq_iter_done(&it)) {
        mino_val_t *elem = seq_iter_val(S, &it);
        int rc = reduce_step(S, fn, &acc, elem, env);
        if (rc == -1) return NULL;
        if (rc == 1)  return acc;
        seq_iter_next(S, &it);
    }
    return acc;
}

/* (set coll) — create a set from a collection. */
mino_val_t *prim_set(mino_state_t *S, mino_val_t *args, mino_env_t *env)
{
    mino_val_t *coll;
    mino_val_t *result;
    seq_iter_t  it;
    (void)env;
    if (!mino_is_cons(args) || mino_is_cons(args->as.cons.cdr)) {
        return prim_throw_classified(S, "eval/arity", "MAR001", "set requires exactly 1 argument");
    }
    coll = args->as.cons.car;
    if (coll == NULL || mino_type_of(coll) == MINO_NIL) {
        return mino_set(S, NULL, 0);
    }
    /* Validate seqability eagerly. */
    {
        mino_val_t *seq_args = mino_cons(S, coll, mino_nil(S));
        mino_val_t *seqd = prim_seq(S, seq_args, env);
        if (seqd == NULL) return NULL;
        if (mino_type_of(seqd) == MINO_NIL) return mino_set(S, NULL, 0);
        coll = seqd;
    }
    result = mino_set(S, NULL, 0);
    gc_pin(result);
    seq_iter_init(S, &it, coll);
    while (!seq_iter_done(&it)) {
        result = set_conj1(S, result, seq_iter_val(S, &it));
        seq_iter_next(S, &it);
    }
    gc_unpin(1);
    return result;
}

/* Lazy primitives (range, lazy-map-1, lazy-filter) live in prim_lazy.c. */

/* (doall coll) -- walks coll forcing every lazy cell, returns coll.
 * (dorun coll) -- same but returns nil. Both iterate in C so each step
 * avoids the fn-call + env frame that a mino-level walk would pay. */
static mino_val_t *realize_seq(mino_state_t *S, mino_val_t *coll)
{
    while (coll != NULL) {
        if (mino_type_of(coll) == MINO_LAZY) {
            coll = lazy_force(S, coll);
            if (coll == NULL) return NULL;
            continue;
        }
        if (mino_type_of(coll) == MINO_NIL) return coll;
        if (mino_type_of(coll) == MINO_CONS) {
            coll = coll->as.cons.cdr;
            continue;
        }
        /* Non-lazy, non-cons collections are already fully realized. */
        return coll;
    }
    return mino_nil(S);
}

mino_val_t *prim_doall(mino_state_t *S, mino_val_t *args, mino_env_t *env)
{
    mino_val_t *coll;
    (void)env;
    if (!mino_is_cons(args) || mino_is_cons(args->as.cons.cdr)) {
        return prim_throw_classified(S, "eval/arity", "MAR001",
            "doall requires 1 argument");
    }
    coll = args->as.cons.car;
    if (realize_seq(S, coll) == NULL) return NULL;
    return coll;
}

mino_val_t *prim_dorun(mino_state_t *S, mino_val_t *args, mino_env_t *env)
{
    mino_val_t *coll;
    (void)env;
    if (!mino_is_cons(args) || mino_is_cons(args->as.cons.cdr)) {
        return prim_throw_classified(S, "eval/arity", "MAR001",
            "dorun requires 1 argument");
    }
    coll = args->as.cons.car;
    if (realize_seq(S, coll) == NULL) return NULL;
    return mino_nil(S);
}

/* Eager range returning a vector. Avoids lazy thunk overhead for tight loops.
 * (rangev end) or (rangev start end) or (rangev start end step). */
mino_val_t *prim_rangev(mino_state_t *S, mino_val_t *args, mino_env_t *env)
{
    long long start = 0, end = 0, step = 1, i;
    size_t n, len;
    mino_val_t **items;
    mino_val_t *result;
    (void)env;
    arg_count(S, args, &n);
    if (n == 1) {
        if (!mino_to_int(args->as.cons.car, &end)) {
            return prim_throw_classified(S, "eval/type", "MTY001", "rangev argument must be an integer");
        }
    } else if (n == 2) {
        if (!mino_to_int(args->as.cons.car, &start) ||
            !mino_to_int(args->as.cons.cdr->as.cons.car, &end)) {
            return prim_throw_classified(S, "eval/type", "MTY001", "rangev arguments must be integers");
        }
    } else if (n == 3) {
        if (!mino_to_int(args->as.cons.car, &start) ||
            !mino_to_int(args->as.cons.cdr->as.cons.car, &end) ||
            !mino_to_int(args->as.cons.cdr->as.cons.cdr->as.cons.car, &step)) {
            return prim_throw_classified(S, "eval/type", "MTY001", "rangev arguments must be integers");
        }
        if (step == 0) {
            return prim_throw_classified(S, "eval/bounds", "MBD001", "rangev step must not be zero");
        }
    } else {
        return prim_throw_classified(S, "eval/arity", "MAR001", "rangev requires 1, 2, or 3 arguments");
    }
    /* Compute length. */
    if (step > 0) {
        len = (end > start) ? (size_t)((end - start + step - 1) / step) : 0;
    } else {
        len = (start > end) ? (size_t)((start - end + (-step) - 1) / (-step)) : 0;
    }
    items = malloc(len * sizeof(mino_val_t *));
    if (!items && len > 0) { return prim_throw_classified(S, "eval/bounds", "MBD001", "rangev: out of memory"); }
    /* C-heap items[] is invisible to the precise GC; suppress collection
     * so freshly minted ints cannot be swept before mino_vector rehomes
     * them into GC-visible storage. */
    mino_current_ctx(S)->gc_depth++;
    for (i = start, n = 0; n < len; i += step, n++) {
        items[n] = mino_int(S, i);
    }
    result = mino_vector(S, items, len);
    mino_current_ctx(S)->gc_depth--;
    free(items);
    return result;
}

/* Grow a PTRARR in place by reallocating into a larger GC block.
 * The new block replaces the old on the pin slot and becomes the
 * returned buffer; the old is left for the sweep. Returns NULL on
 * OOM (diag already set by gc_alloc_typed). */
static mino_val_t **ptrarr_grow(mino_state_t *S, mino_val_t **old,
                                size_t old_len, size_t new_cap, int pin_slot)
{
    mino_val_t **nb = (mino_val_t **)gc_alloc_typed(
        S, GC_T_PTRARR, new_cap * sizeof(mino_val_t *));
    size_t i;
    if (nb == NULL) return NULL;
    for (i = 0; i < old_len; i++) {
        nb[i] = old[i];
    }
    mino_current_ctx(S)->gc_save[pin_slot] = (mino_val_t *)nb;
    return nb;
}

/* Eager map returning a vector. (mapv f coll)
 *
 * Accumulate into a GC-tracked PTRARR pinned on the save stack so the
 * buffer and its contents stay marked across the apply_callable call.
 * A malloc'd items[] would be invisible to the precise collector, and a
 * blanket gc_depth++ would pin every user-code allocation for the
 * duration of the mapv. Pinning the accumulator lets GC continue normally
 * inside the fn while still preserving what we've produced so far. */
mino_val_t *prim_mapv(mino_state_t *S, mino_val_t *args, mino_env_t *env)
{
    mino_val_t *fn, *coll;
    seq_iter_t  it;
    size_t      cap = 64, len = 0;
    mino_val_t **items;
    mino_val_t *result;
    int         pin_slot;
    size_t n;
    arg_count(S, args, &n);
    if (n != 2) {
        return prim_throw_classified(S, "eval/arity", "MAR001", "mapv requires 2 arguments: function and collection");
    }
    fn   = args->as.cons.car;
    coll = args->as.cons.cdr->as.cons.car;
    if (coll == NULL || mino_is_nil(coll)) {
        return mino_vector(S, NULL, 0);
    }
    items = (mino_val_t **)gc_alloc_typed(S, GC_T_PTRARR,
                                          cap * sizeof(mino_val_t *));
    if (items == NULL) return NULL;
    pin_slot = mino_current_ctx(S)->gc_save_len;
    gc_pin((mino_val_t *)items);
    seq_iter_init(S, &it, coll);
    while (!seq_iter_done(&it)) {
        mino_val_t *elem = seq_iter_val(S, &it);
        mino_val_t *call_args = mino_cons(S, elem, mino_nil(S));
        mino_val_t *val = apply_callable(S, fn, call_args, env);
        if (val == NULL) { gc_unpin(1); return NULL; }
        if (len >= cap) {
            cap *= 2;
            items = ptrarr_grow(S, items, len, cap, pin_slot);
            if (items == NULL) { gc_unpin(1); return NULL; }
        }
        gc_valarr_set(S, items, len, val);
        len++;
        seq_iter_next(S, &it);
    }
    result = mino_vector(S, items, len);
    gc_unpin(1);
    return result;
}

/* Eager filter returning a vector. (filterv pred coll). Same precise-GC
 * caveat as prim_mapv: pin a GC-tracked accumulator on the save stack. */
mino_val_t *prim_filterv(mino_state_t *S, mino_val_t *args, mino_env_t *env)
{
    mino_val_t *pred, *coll;
    seq_iter_t  it;
    size_t      cap = 64, len = 0;
    mino_val_t **items;
    mino_val_t *result;
    int         pin_slot;
    size_t n;
    arg_count(S, args, &n);
    if (n != 2) {
        return prim_throw_classified(S, "eval/arity", "MAR001", "filterv requires 2 arguments: predicate and collection");
    }
    pred = args->as.cons.car;
    coll = args->as.cons.cdr->as.cons.car;
    if (coll == NULL || mino_is_nil(coll)) {
        return mino_vector(S, NULL, 0);
    }
    items = (mino_val_t **)gc_alloc_typed(S, GC_T_PTRARR,
                                          cap * sizeof(mino_val_t *));
    if (items == NULL) return NULL;
    pin_slot = mino_current_ctx(S)->gc_save_len;
    gc_pin((mino_val_t *)items);
    seq_iter_init(S, &it, coll);
    while (!seq_iter_done(&it)) {
        mino_val_t *elem = seq_iter_val(S, &it);
        mino_val_t *call_args = mino_cons(S, elem, mino_nil(S));
        mino_val_t *test = apply_callable(S, pred, call_args, env);
        if (test == NULL) { gc_unpin(1); return NULL; }
        if (mino_is_truthy_inline(test)) {
            if (len >= cap) {
                cap *= 2;
                items = ptrarr_grow(S, items, len, cap, pin_slot);
                if (items == NULL) { gc_unpin(1); return NULL; }
            }
            gc_valarr_set(S, items, len, elem);
            len++;
        }
        seq_iter_next(S, &it);
    }
    result = mino_vector(S, items, len);
    gc_unpin(1);
    return result;
}

mino_val_t *prim_into(mino_state_t *S, mino_val_t *args, mino_env_t *env)
{
    mino_val_t *to;
    mino_val_t *from;
    seq_iter_t  it;
    (void)env;
    if (!mino_is_cons(args) || !mino_is_cons(args->as.cons.cdr)) {
        return prim_throw_classified(S, "eval/arity", "MAR001", "into requires two arguments");
    }
    to   = args->as.cons.car;
    from = args->as.cons.cdr->as.cons.car;
    if (from == NULL || mino_type_of(from) == MINO_NIL) {
        return to;
    }
    /* Conj each element of `from` into `to`. The type of `to` determines
     * the conj semantics (vector appends, list prepends, map/set merges). */
    if (to == NULL || mino_type_of(to) == MINO_NIL || mino_type_of(to) == MINO_EMPTY_LIST) {
        /* Into nil or empty-list: build a list (prepend each element). */
        mino_val_t *out = mino_nil(S);
        seq_iter_init(S, &it, from);
        while (!seq_iter_done(&it)) {
            out = mino_cons(S, seq_iter_val(S, &it), out);
            seq_iter_next(S, &it);
        }
        if (out == NULL || mino_type_of(out) == MINO_NIL) return mino_empty_list(S);
        return out;
    }
    if (mino_type_of(to) == MINO_VECTOR) {
        mino_val_t *acc = to;
        seq_iter_init(S, &it, from);
        while (!seq_iter_done(&it)) {
            acc = vec_conj1(S, acc, seq_iter_val(S, &it));
            seq_iter_next(S, &it);
        }
        return acc;
    }
    if (mino_type_of(to) == MINO_MAP) {
        mino_val_t *acc = to;
        seq_iter_init(S, &it, from);
        while (!seq_iter_done(&it)) {
            mino_val_t *item = seq_iter_val(S, &it);
            mino_val_t *pair_args;
            mino_val_t *ek, *ev;
            if (item != NULL && mino_type_of(item) == MINO_VECTOR
                && item->as.vec.len == 2) {
                ek = vec_nth(item, 0);
                ev = vec_nth(item, 1);
            } else if (item != NULL && mino_type_of(item) == MINO_MAP_ENTRY) {
                ek = item->as.map_entry.k;
                ev = item->as.map_entry.v;
            } else {
                return prim_throw_classified(S, "eval/type", "MTY001",
                    "into map: each element must be a map entry or 2-element vector");
            }
            pair_args = mino_cons(S, ek, mino_cons(S, ev, mino_nil(S)));
            acc = prim_assoc(S, mino_cons(S, acc, pair_args), env);
            seq_iter_next(S, &it);
        }
        return acc;
    }
    if (mino_type_of(to) == MINO_SET) {
        mino_val_t *acc = to;
        seq_iter_init(S, &it, from);
        while (!seq_iter_done(&it)) {
            acc = set_conj1(S, acc, seq_iter_val(S, &it));
            seq_iter_next(S, &it);
        }
        return acc;
    }
    if (mino_type_of(to) == MINO_SORTED_MAP) {
        mino_val_t *acc = to;
        seq_iter_init(S, &it, from);
        while (!seq_iter_done(&it)) {
            mino_val_t *item = seq_iter_val(S, &it);
            mino_val_t *ek, *ev;
            if (item != NULL && mino_type_of(item) == MINO_VECTOR
                && item->as.vec.len == 2) {
                ek = vec_nth(item, 0);
                ev = vec_nth(item, 1);
            } else if (item != NULL && mino_type_of(item) == MINO_MAP_ENTRY) {
                ek = item->as.map_entry.k;
                ev = item->as.map_entry.v;
            } else {
                return prim_throw_classified(S, "eval/type", "MTY001",
                    "into sorted-map: each element must be a map entry or 2-element vector");
            }
            acc = sorted_map_assoc1(S, acc, ek, ev);
            seq_iter_next(S, &it);
        }
        return acc;
    }
    if (mino_type_of(to) == MINO_SORTED_SET) {
        mino_val_t *acc = to;
        seq_iter_init(S, &it, from);
        while (!seq_iter_done(&it)) {
            acc = sorted_set_conj1(S, acc, seq_iter_val(S, &it));
            seq_iter_next(S, &it);
        }
        return acc;
    }
    if (mino_type_of(to) == MINO_CONS) {
        mino_val_t *out = to;
        seq_iter_init(S, &it, from);
        while (!seq_iter_done(&it)) {
            out = mino_cons(S, seq_iter_val(S, &it), out);
            seq_iter_next(S, &it);
        }
        return out;
    }
    {
        char msg[96];
        snprintf(msg, sizeof(msg),
                 "into: expected a list, vector, map, or set as target, got %s",
                 type_tag_str(to));
        return prim_throw_classified(S, "eval/type", "MTY001", msg);
    }
}

mino_val_t *prim_apply(mino_state_t *S, mino_val_t *args, mino_env_t *env)
{
    mino_val_t *fn;
    mino_val_t *last;
    mino_val_t *call_args;
    mino_val_t *p;
    size_t      n;
    arg_count(S, args, &n);
    if (n < 2) {
        return prim_throw_classified(S, "eval/arity", "MAR001", "apply requires a function and arguments");
    }
    fn = args->as.cons.car;
    if (n == 2) {
        /* (apply f coll) — spread coll as args. */
        last = args->as.cons.cdr->as.cons.car;
    } else {
        /* (apply f a b ... coll) — prepend individual args, then spread coll. */
        mino_val_t *head = mino_nil(S);
        mino_val_t *tail2 = NULL;
        p = args->as.cons.cdr;
        /* Collect all but the last arg as individual args. */
        while (mino_is_cons(p) && mino_is_cons(p->as.cons.cdr)) {
            mino_val_t *cell = mino_cons(S, p->as.cons.car, mino_nil(S));
            if (tail2 == NULL) { head = cell; } else { mino_cons_cdr_set(S, tail2, cell); }
            tail2 = cell;
            p = p->as.cons.cdr;
        }
        last = p->as.cons.car; /* the final collection argument */
        /* Append elements from `last` collection. */
        if (last != NULL && mino_type_of(last) != MINO_NIL) {
            seq_iter_t it;
            seq_iter_init(S, &it, last);
            while (!seq_iter_done(&it)) {
                mino_val_t *cell = mino_cons(S, seq_iter_val(S, &it), mino_nil(S));
                if (tail2 == NULL) { head = cell; } else { mino_cons_cdr_set(S, tail2, cell); }
                tail2 = cell;
                seq_iter_next(S, &it);
            }
        }
        return apply_callable(S, fn, head, env);
    }
    /* (apply f coll) — convert coll to a cons arg list. */
    if (last == NULL || mino_type_of(last) == MINO_NIL
        || mino_type_of(last) == MINO_EMPTY_LIST) {
        return apply_callable(S, fn, mino_nil(S), env);
    }
    if (mino_type_of(last) == MINO_CONS) {
        return apply_callable(S, fn, last, env);
    }
    /* Convert non-list collection to cons list. */
    {
        mino_val_t *head = mino_nil(S);
        mino_val_t *tail2 = NULL;
        seq_iter_t it;
        seq_iter_init(S, &it, last);
        while (!seq_iter_done(&it)) {
            mino_val_t *cell = mino_cons(S, seq_iter_val(S, &it), mino_nil(S));
            if (tail2 == NULL) { head = cell; } else { mino_cons_cdr_set(S, tail2, cell); }
            tail2 = cell;
            seq_iter_next(S, &it);
        }
        call_args = head;
    }
    return apply_callable(S, fn, call_args, env);
}

mino_val_t *prim_reverse(mino_state_t *S, mino_val_t *args, mino_env_t *env)
{
    /* Per Clojure, (reverse nil) and (reverse <empty>) return the
     * empty list (), not nil. Otherwise iterate the collection and
     * cons each element onto the running head (matching Clojure's
     * reverse contract: returns a sequence). Non-seqable inputs
     * (numbers, keywords, symbols, ...) throw via `prim_seq`'s
     * coercion check; the silent "treat as empty" path was wrong
     * because it suppressed the type error Clojure raises. */
    mino_val_t *coll;
    mino_val_t *seqd;
    mino_val_t *out = mino_empty_list(S);
    mino_val_t *seq_args;
    seq_iter_t  it;
    (void)env;
    if (!mino_is_cons(args) || mino_is_cons(args->as.cons.cdr)) {
        return prim_throw_classified(S, "eval/arity", "MAR001", "reverse requires one argument");
    }
    coll = args->as.cons.car;
    if (coll == NULL || mino_type_of(coll) == MINO_NIL) {
        return mino_empty_list(S);
    }
    seq_args = mino_cons(S, coll, mino_nil(S));
    seqd = prim_seq(S, seq_args, env);
    if (seqd == NULL) return NULL;
    if (mino_type_of(seqd) == MINO_NIL) return mino_empty_list(S);
    seq_iter_init(S, &it, seqd);
    while (!seq_iter_done(&it)) {
        out = mino_cons(S, seq_iter_val(S, &it), out);
        seq_iter_next(S, &it);
    }
    return out;
}

/* val_compare is now in rbtree.c (shared with sorted map/set).
 * Sort comparator state: when sort_comp_fn is non-NULL, the merge sort
 * calls the user-supplied comparison function instead of val_compare. */

static int sort_compare(mino_state_t *S, const mino_val_t *a, const mino_val_t *b)
{
    if (S->sort_comp_fn != NULL) {
        mino_val_t *call_args = mino_cons(S, (mino_val_t *)a,
                                  mino_cons(S, (mino_val_t *)b, mino_nil(S)));
        mino_val_t *result = mino_call(S, S->sort_comp_fn, call_args, S->sort_comp_env);
        if (result == NULL) return 0;
        /* Numeric result: use sign directly (compare-style) */
        if (mino_val_int_p(result)) {
            return mino_val_int_get(result) < 0 ? -1 : mino_val_int_get(result) > 0 ? 1 : 0;
        }
        if (mino_type_of(result) == MINO_FLOAT) {
            return result->as.f < 0 ? -1 : result->as.f > 0 ? 1 : 0;
        }
        /* Boolean result: true means a < b, false means a >= b */
        return mino_is_truthy_inline(result) ? -1 : 1;
    }
    /* Default comparator: route through prim_compare so cross-type
     * pairs throw a typed error instead of silently ordering by tag.
     * (sort [1 []]) -> ClassCastException-shaped error matches Clojure. */
    {
        mino_val_t *args = mino_cons(S, (mino_val_t *)a,
                              mino_cons(S, (mino_val_t *)b, mino_nil(S)));
        mino_val_t *r = prim_compare(S, args, NULL);
        if (r == NULL) return 0;
        if (mino_val_int_p(r)) {
            return mino_val_int_get(r) < 0 ? -1 : mino_val_int_get(r) > 0 ? 1 : 0;
        }
        if (mino_type_of(r) == MINO_FLOAT) {
            return r->as.f < 0 ? -1 : r->as.f > 0 ? 1 : 0;
        }
        return 0;
    }
}

/* Merge sort for mino_val_t* arrays. */
static void merge_sort_vals(mino_state_t *S, mino_val_t **arr, mino_val_t **tmp, size_t len)
{
    size_t mid, i, j, k;
    if (len <= 1) return;
    mid = len / 2;
    merge_sort_vals(S, arr, tmp, mid);
    merge_sort_vals(S, arr + mid, tmp, len - mid);
    memcpy(tmp, arr, mid * sizeof(*tmp));
    i = 0; j = mid; k = 0;
    while (i < mid && j < len) {
        if (sort_compare(S, tmp[i], arr[j]) <= 0) {
            arr[k++] = tmp[i++];
        } else {
            arr[k++] = arr[j++];
        }
    }
    while (i < mid) { arr[k++] = tmp[i++]; }
}

/* (sort coll) or (sort comp coll) */
mino_val_t *prim_sort(mino_state_t *S, mino_val_t *args, mino_env_t *env)
{
    mino_val_t *coll;
    mino_val_t *comp = NULL;
    mino_val_t **arr;
    mino_val_t **tmp;
    mino_val_t *head = mino_nil(S);
    mino_val_t *tail = NULL;
    size_t      n_items, i;
    seq_iter_t  it;
    if (!mino_is_cons(args)) {
        return prim_throw_classified(S, "eval/arity", "MAR001", "sort requires one or two arguments");
    }
    if (mino_is_cons(args->as.cons.cdr) &&
        !mino_is_cons(args->as.cons.cdr->as.cons.cdr)) {
        /* Two args: (sort comp coll) */
        comp = args->as.cons.car;
        coll = args->as.cons.cdr->as.cons.car;
    } else if (!mino_is_cons(args->as.cons.cdr)) {
        /* One arg: (sort coll) */
        coll = args->as.cons.car;
    } else {
        return prim_throw_classified(S, "eval/arity", "MAR001", "sort requires one or two arguments");
    }
    if (coll == NULL || mino_type_of(coll) == MINO_NIL) {
        return mino_empty_list(S);
    }
    /* Validate seqability eagerly: non-coll inputs throw rather than
     * silently produce an empty result. Routes through prim_seq for the
     * shared coercion check. */
    {
        mino_val_t *seq_args = mino_cons(S, coll, mino_nil(S));
        mino_val_t *seqd = prim_seq(S, seq_args, env);
        if (seqd == NULL) return NULL;
        if (mino_type_of(seqd) == MINO_NIL) return mino_empty_list(S);
        coll = seqd;
    }
    /* Collect elements into an array. */
    n_items = 0;
    seq_iter_init(S, &it, coll);
    while (!seq_iter_done(&it)) { n_items++; seq_iter_next(S, &it); }
    if (n_items == 0) return mino_empty_list(S);
    arr = (mino_val_t **)gc_alloc_typed(S, GC_T_VALARR, n_items * sizeof(*arr));
    tmp = (mino_val_t **)gc_alloc_typed(S, GC_T_VALARR, n_items * sizeof(*tmp));
    i = 0;
    seq_iter_init(S, &it, coll);
    while (!seq_iter_done(&it)) { arr[i++] = seq_iter_val(S, &it); seq_iter_next(S, &it); }
    S->sort_comp_fn  = comp;
    S->sort_comp_env = env;
    merge_sort_vals(S, arr, tmp, n_items);
    S->sort_comp_fn  = NULL;
    S->sort_comp_env = NULL;
    for (i = 0; i < n_items; i++) {
        mino_val_t *cell = mino_cons(S, arr[i], mino_nil(S));
        if (tail == NULL) { head = cell; } else { mino_cons_cdr_set(S, tail, cell); }
        tail = cell;
    }
    return head;
}

mino_val_t *prim_peek(mino_state_t *S, mino_val_t *args, mino_env_t *env)
{
    mino_val_t *coll;
    (void)env;
    if (!mino_is_cons(args) || mino_is_cons(args->as.cons.cdr)) {
        return prim_throw_classified(S, "eval/arity", "MAR001", "peek requires one argument");
    }
    coll = args->as.cons.car;
    if (coll == NULL || mino_type_of(coll) == MINO_NIL
        || mino_type_of(coll) == MINO_EMPTY_LIST) return mino_nil(S);
    if (mino_type_of(coll) == MINO_VECTOR) {
        if (coll->as.vec.len == 0) return mino_nil(S);
        return vec_nth(coll, coll->as.vec.len - 1);
    }
    /* Only list-shaped MINO_CONS cells are stack-eligible. The result
     * of a (cons x y) call has not_list=1 and falls through to throw,
     * matching JVM Clojure where Cons is not a stack. */
    if (mino_type_of(coll) == MINO_CONS && !coll->as.cons.not_list)
        return coll->as.cons.car;
    {
        char msg[96];
        snprintf(msg, sizeof(msg), "peek: expected a vector or list, got %s",
                 type_tag_str(coll));
        return prim_throw_classified(S, "eval/type", "MTY001", msg);
    }
}

mino_val_t *prim_pop(mino_state_t *S, mino_val_t *args, mino_env_t *env)
{
    mino_val_t *coll;
    (void)env;
    if (!mino_is_cons(args) || mino_is_cons(args->as.cons.cdr)) {
        return prim_throw_classified(S, "eval/arity", "MAR001", "pop requires one argument");
    }
    coll = args->as.cons.car;
    if (coll == NULL || mino_type_of(coll) == MINO_NIL) {
        return mino_nil(S);
    }
    if (mino_type_of(coll) == MINO_VECTOR) {
        if (coll->as.vec.len == 0) {
            return prim_throw_classified(S, "eval/bounds", "MBD001", "pop: cannot pop an empty vector");
        }
        return vec_pop(S, coll);
    }
    if (mino_type_of(coll) == MINO_CONS && !coll->as.cons.not_list)
        return coll->as.cons.cdr;
    {
        char msg[96];
        snprintf(msg, sizeof(msg), "pop: expected a vector or list, got %s",
                 type_tag_str(coll));
        return prim_throw_classified(S, "eval/type", "MTY001", msg);
    }
}

mino_val_t *prim_find(mino_state_t *S, mino_val_t *args, mino_env_t *env)
{
    mino_val_t *m;
    mino_val_t *k;
    mino_val_t *v;
    (void)env;
    if (!mino_is_cons(args) || !mino_is_cons(args->as.cons.cdr)
        || mino_is_cons(args->as.cons.cdr->as.cons.cdr)) {
        return prim_throw_classified(S, "eval/arity", "MAR001", "find requires two arguments");
    }
    m = args->as.cons.car;
    k = args->as.cons.cdr->as.cons.car;
    if (m == NULL || mino_type_of(m) == MINO_NIL) return mino_nil(S);
    /* Transient associatives delegate to their underlying persistent
     * collection, matching Clojure's semantics. */
    if (mino_type_of(m) == MINO_TRANSIENT) {
        if (!m->as.transient.valid)
            return prim_throw_classified(S, "eval/state", "MST001",
                "find: transient is no longer valid");
        m = m->as.transient.current;
        if (m == NULL || mino_type_of(m) == MINO_NIL) return mino_nil(S);
    }
    if (mino_type_of(m) == MINO_SORTED_MAP) {
        if (!rb_contains(S, m->as.sorted.root, k, m->as.sorted.comparator))
            return mino_nil(S);
        v = rb_get(S, m->as.sorted.root, k, m->as.sorted.comparator);
        return mino_map_entry(S, k, v);
    }
    if (mino_type_of(m) == MINO_VECTOR) {
        long long idx;
        if (!mino_val_int_p(k)) return mino_nil(S);
        idx = mino_val_int_get(k);
        if (idx < 0 || (size_t)idx >= m->as.vec.len) return mino_nil(S);
        return mino_map_entry(S, k, vec_nth(m, (size_t)idx));
    }
    if (mino_type_of(m) == MINO_RECORD) {
        int idx = record_field_index(m, k);
        if (idx >= 0) {
            mino_val_t *fields = m->as.record.type->as.record_type.fields;
            return mino_map_entry(S, vec_nth(fields, idx),
                                     m->as.record.vals[idx]);
        }
        if (m->as.record.ext != NULL) {
            v = map_get_val(m->as.record.ext, k);
            if (v != NULL) return mino_map_entry(S, k, v);
        }
        return mino_nil(S);
    }
    if (mino_type_of(m) != MINO_MAP) {
        return prim_throw_classified(S, "eval/type", "MTY001", "find: first argument must be an associative collection");
    }
    v = map_get_val(m, k);
    if (v == NULL) return mino_nil(S);
    return mino_map_entry(S, k, v);
}

mino_val_t *prim_empty(mino_state_t *S, mino_val_t *args, mino_env_t *env)
{
    mino_val_t *coll;
    (void)env;
    if (!mino_is_cons(args) || mino_is_cons(args->as.cons.cdr)) {
        return prim_throw_classified(S, "eval/arity", "MAR001", "empty requires one argument");
    }
    coll = args->as.cons.car;
    if (coll == NULL || mino_type_of(coll) == MINO_NIL) return mino_nil(S);
    {
        mino_val_t *r;
        switch (mino_type_of(coll)) {
        case MINO_VECTOR:
            r = mino_vector(S, NULL, 0);
            r->meta = coll->meta;
            return r;
        case MINO_MAP:
            r = mino_map(S, NULL, NULL, 0);
            r->meta = coll->meta;
            return r;
        case MINO_SET:
            r = mino_set(S, NULL, 0);
            r->meta = coll->meta;
            return r;
        case MINO_SORTED_MAP:
            r = mino_sorted_map(S, NULL, NULL, 0);
            r->meta = coll->meta;
            return r;
        case MINO_SORTED_SET:
            r = mino_sorted_set(S, NULL, 0);
            r->meta = coll->meta;
            return r;
        case MINO_CONS:
        case MINO_CHUNKED_CONS:
        case MINO_LAZY:
        case MINO_EMPTY_LIST:
            /* Per Clojure, (empty seq) is the empty list (). */
            return mino_empty_list(S);
        default:
            return mino_nil(S);
        }
    }
}

mino_val_t *prim_rseq(mino_state_t *S, mino_val_t *args, mino_env_t *env)
{
    mino_val_t *coll;
    mino_val_t *out;
    size_t i;
    (void)env;
    if (!mino_is_cons(args) || mino_is_cons(args->as.cons.cdr)) {
        return prim_throw_classified(S, "eval/arity", "MAR001", "rseq requires one argument");
    }
    coll = args->as.cons.car;
    if (coll == NULL || mino_type_of(coll) == MINO_NIL) {
        return prim_throw_classified(S, "eval/type", "MTY001", "rseq: argument must not be nil");
    }
    if (mino_type_of(coll) == MINO_SORTED_MAP || mino_type_of(coll) == MINO_SORTED_SET) {
        /* Reverse of sorted collection — build reverse cons list from
         * the in-order key list. */
        mino_val_t *keys = mino_nil(S);
        mino_val_t *kt   = NULL;
        rb_to_list(S, coll->as.sorted.root, &keys, &kt);
        out = mino_nil(S);
        if (mino_type_of(coll) == MINO_SORTED_MAP) {
            while (mino_is_cons(keys)) {
                mino_val_t *k = keys->as.cons.car;
                mino_val_t *v = rb_get(S, coll->as.sorted.root, k,
                                       coll->as.sorted.comparator);
                mino_val_t *kv[2]; kv[0] = k; kv[1] = v;
                out = mino_cons(S, mino_vector(S, kv, 2), out);
                keys = keys->as.cons.cdr;
            }
        } else {
            while (mino_is_cons(keys)) {
                out = mino_cons(S, keys->as.cons.car, out);
                keys = keys->as.cons.cdr;
            }
        }
        return out;
    }
    if (mino_type_of(coll) != MINO_VECTOR) {
        return prim_throw_classified(S, "eval/type", "MTY001", "rseq: argument must be a vector or sorted collection");
    }
    if (coll->as.vec.len == 0) return mino_nil(S);
    out = mino_nil(S);
    for (i = 0; i < coll->as.vec.len; i++) {
        out = mino_cons(S, vec_nth(coll, i), out);
    }
    return out;
}

mino_val_t *prim_sorted_map(mino_state_t *S, mino_val_t *args, mino_env_t *env)
{
    size_t n, pairs, i;
    mino_val_t **ks, **vs, *p;
    (void)env;
    arg_count(S, args, &n);
    if (n % 2 != 0) {
        return prim_throw_classified(S, "eval/arity", "MAR001", "sorted-map requires an even number of arguments");
    }
    pairs = n / 2;
    if (pairs == 0) return mino_sorted_map(S, NULL, NULL, 0);
    ks = (mino_val_t **)gc_alloc_typed(S, GC_T_VALARR, pairs * sizeof(*ks));
    vs = (mino_val_t **)gc_alloc_typed(S, GC_T_VALARR, pairs * sizeof(*vs));
    p = args;
    for (i = 0; i < pairs; i++) {
        ks[i] = p->as.cons.car; p = p->as.cons.cdr;
        vs[i] = p->as.cons.car; p = p->as.cons.cdr;
    }
    return mino_sorted_map(S, ks, vs, pairs);
}

mino_val_t *prim_sorted_set(mino_state_t *S, mino_val_t *args, mino_env_t *env)
{
    size_t n, i;
    mino_val_t **tmp, *p;
    (void)env;
    arg_count(S, args, &n);
    if (n == 0) return mino_sorted_set(S, NULL, 0);
    tmp = (mino_val_t **)gc_alloc_typed(S, GC_T_VALARR, n * sizeof(*tmp));
    p = args;
    for (i = 0; i < n; i++) {
        tmp[i] = p->as.cons.car;
        p = p->as.cons.cdr;
    }
    return mino_sorted_set(S, tmp, n);
}

mino_val_t *prim_sorted_map_by(mino_state_t *S, mino_val_t *args, mino_env_t *env)
{
    size_t n, pairs, i;
    mino_val_t *comparator, **ks, **vs, *p;
    (void)env;
    arg_count(S, args, &n);
    if (n < 1 || (n - 1) % 2 != 0) {
        return prim_throw_classified(S, "eval/arity", "MAR001",
            "sorted-map-by requires a comparator and an even number of keys and values");
    }
    comparator = args->as.cons.car;
    if (comparator == NULL
        || (mino_type_of(comparator) != MINO_FN && mino_type_of(comparator) != MINO_PRIM)) {
        return prim_throw_classified(S, "eval/type", "MTY001",
            "sorted-map-by: comparator must be a function");
    }
    pairs = (n - 1) / 2;
    if (pairs == 0) return mino_sorted_map_by(S, comparator, NULL, NULL, 0);
    ks = (mino_val_t **)gc_alloc_typed(S, GC_T_VALARR, pairs * sizeof(*ks));
    vs = (mino_val_t **)gc_alloc_typed(S, GC_T_VALARR, pairs * sizeof(*vs));
    p = args->as.cons.cdr;
    for (i = 0; i < pairs; i++) {
        ks[i] = p->as.cons.car; p = p->as.cons.cdr;
        vs[i] = p->as.cons.car; p = p->as.cons.cdr;
    }
    return mino_sorted_map_by(S, comparator, ks, vs, pairs);
}

mino_val_t *prim_sorted_set_by(mino_state_t *S, mino_val_t *args, mino_env_t *env)
{
    size_t n, items, i;
    mino_val_t *comparator, **tmp, *p;
    (void)env;
    arg_count(S, args, &n);
    if (n < 1) {
        return prim_throw_classified(S, "eval/arity", "MAR001",
            "sorted-set-by requires a comparator");
    }
    comparator = args->as.cons.car;
    if (comparator == NULL
        || (mino_type_of(comparator) != MINO_FN && mino_type_of(comparator) != MINO_PRIM)) {
        return prim_throw_classified(S, "eval/type", "MTY001",
            "sorted-set-by: comparator must be a function");
    }
    items = n - 1;
    if (items == 0) return mino_sorted_set_by(S, comparator, NULL, 0);
    tmp = (mino_val_t **)gc_alloc_typed(S, GC_T_VALARR, items * sizeof(*tmp));
    p = args->as.cons.cdr;
    for (i = 0; i < items; i++) {
        tmp[i] = p->as.cons.car;
        p = p->as.cons.cdr;
    }
    return mino_sorted_set_by(S, comparator, tmp, items);
}

/* Classify one of the four comparison primitives (<, <=, >, >=) by function
 * pointer. On success, *is_gt tells whether the test is a > family test and
 * *inclusive tells whether the boundary value itself is included. Returns 0
 * if v is not one of the four accepted tests. */
static int classify_subseq_test(const mino_val_t *v, int *is_gt, int *inclusive)
{
    if (v == NULL || mino_type_of(v) != MINO_PRIM) return 0;
    if (v->as.prim.fn == prim_lt)  { *is_gt = 0; *inclusive = 0; return 1; }
    if (v->as.prim.fn == prim_lte) { *is_gt = 0; *inclusive = 1; return 1; }
    if (v->as.prim.fn == prim_gt)  { *is_gt = 1; *inclusive = 0; return 1; }
    if (v->as.prim.fn == prim_gte) { *is_gt = 1; *inclusive = 1; return 1; }
    return 0;
}

/* Shared body for subseq / rsubseq. reverse = 0 for subseq, 1 for rsubseq.
 *
 * Three-arg form: (subseq sc test key)
 *   A > or >= test means "entries whose key is > / >= key" (lower bound).
 *   A < or <= test means "entries whose key is < / <= key" (upper bound).
 * Five-arg form: (subseq sc start-test start-key end-test end-key)
 *   start-test must be > or >=, end-test must be < or <=. Both bounds
 *   apply. */
static mino_val_t *subseq_impl(mino_state_t *S, mino_val_t *args, int reverse)
{
    size_t n;
    mino_val_t *sc;
    mino_val_t *head = mino_nil(S);
    mino_val_t *tail = NULL;
    int has_lo = 0, lo_inclusive = 0;
    int has_hi = 0, hi_inclusive = 0;
    mino_val_t *lo_key = NULL;
    mino_val_t *hi_key = NULL;
    const char *name = reverse ? "rsubseq" : "subseq";
    int is_map;
    arg_count(S, args, &n);
    if (n != 3 && n != 5) {
        return prim_throw_classified(S, "eval/arity", "MAR001",
            reverse
              ? "rsubseq requires 3 or 5 arguments"
              : "subseq requires 3 or 5 arguments");
    }
    sc = args->as.cons.car;
    if (sc == NULL
        || (mino_type_of(sc) != MINO_SORTED_MAP && mino_type_of(sc) != MINO_SORTED_SET)) {
        return prim_throw_classified(S, "eval/type", "MTY001",
            reverse
              ? "rsubseq: first argument must be a sorted map or sorted set"
              : "subseq: first argument must be a sorted map or sorted set");
    }
    is_map = mino_type_of(sc) == MINO_SORTED_MAP;
    (void)name;
    if (n == 3) {
        mino_val_t *test = args->as.cons.cdr->as.cons.car;
        mino_val_t *key  = args->as.cons.cdr->as.cons.cdr->as.cons.car;
        int is_gt, inclusive;
        if (!classify_subseq_test(test, &is_gt, &inclusive)) {
            return prim_throw_classified(S, "eval/type", "MTY001",
                reverse
                  ? "rsubseq: test must be <, <=, > or >="
                  : "subseq: test must be <, <=, > or >=");
        }
        if (is_gt) {
            has_lo = 1; lo_inclusive = inclusive; lo_key = key;
        } else {
            has_hi = 1; hi_inclusive = inclusive; hi_key = key;
        }
    } else {
        mino_val_t *p = args->as.cons.cdr;
        mino_val_t *start_test = p->as.cons.car; p = p->as.cons.cdr;
        mino_val_t *start_key  = p->as.cons.car; p = p->as.cons.cdr;
        mino_val_t *end_test   = p->as.cons.car; p = p->as.cons.cdr;
        mino_val_t *end_key    = p->as.cons.car;
        int is_gt, inclusive;
        if (!classify_subseq_test(start_test, &is_gt, &inclusive) || !is_gt) {
            return prim_throw_classified(S, "eval/type", "MTY001",
                reverse
                  ? "rsubseq: start-test must be > or >="
                  : "subseq: start-test must be > or >=");
        }
        has_lo = 1; lo_inclusive = inclusive; lo_key = start_key;
        if (!classify_subseq_test(end_test, &is_gt, &inclusive) || is_gt) {
            return prim_throw_classified(S, "eval/type", "MTY001",
                reverse
                  ? "rsubseq: end-test must be < or <="
                  : "subseq: end-test must be < or <=");
        }
        has_hi = 1; hi_inclusive = inclusive; hi_key = end_key;
    }
    if (sc->as.sorted.len == 0) return mino_nil(S);
    rb_bounded_seq(S, sc->as.sorted.root, is_map,
                   has_lo, lo_inclusive, lo_key,
                   has_hi, hi_inclusive, hi_key,
                   sc->as.sorted.comparator, reverse,
                   &head, &tail);
    return head;
}

mino_val_t *prim_subseq(mino_state_t *S, mino_val_t *args, mino_env_t *env)
{
    (void)env;
    return subseq_impl(S, args, 0);
}

mino_val_t *prim_rsubseq(mino_state_t *S, mino_val_t *args, mino_env_t *env)
{
    (void)env;
    return subseq_impl(S, args, 1);
}

const mino_prim_def k_prims_sequences[] = {
    {"reduce",   prim_reduce,
     "Reduces a collection using f. With no init, uses the first item."},
    {"reduced",  prim_reduced,
     "Wraps a value to signal early termination of reduce."},
    {"reduced?", prim_reduced_p,
     "Returns true if x is a reduced value."},
    {"into",     prim_into,
     "Returns a new collection with all items from the source conj'd in."},
    {"set",      prim_set,
     "Returns a set of the items in coll."},
    {"doall",    prim_doall,
     "Forces realization of a lazy sequence. Returns coll."},
    {"dorun",    prim_dorun,
     "Forces realization of a lazy sequence. Returns nil."},
    {"rangev",   prim_rangev,
     "Returns a vector of integers from start (inclusive) to end (exclusive)."},
    {"mapv",     prim_mapv,
     "Returns a vector of applying f to each item in one or more collections."},
    {"filterv",  prim_filterv,
     "Returns a vector of items in coll for which pred returns logical true."},
    {"apply",    prim_apply,
     "Applies f to the arguments, with the last argument spread as a sequence."},
    {"reverse",  prim_reverse,
     "Returns a sequence of the items in coll in reverse order."},
    {"sort",     prim_sort,
     "Returns a sorted sequence of the items in coll."},
    {"peek",     prim_peek,
     "Returns the first item of a list or last item of a vector."},
    {"pop",      prim_pop,
     "Returns a collection without the peek item."},
    {"find",     prim_find,
     "Returns the map entry for the key, or nil."},
    {"empty",    prim_empty,
     "Returns an empty collection of the same type."},
    {"rseq",     prim_rseq,
     "Returns a reverse sequence of a vector, or nil if empty."},
    {"sorted-map", prim_sorted_map,
     "Returns a new sorted map with the given key-value pairs."},
    {"sorted-set", prim_sorted_set,
     "Returns a new sorted set containing the arguments."},
    {"sorted-map-by", prim_sorted_map_by,
     "Returns a sorted map using the given comparator function."},
    {"sorted-set-by", prim_sorted_set_by,
     "Returns a sorted set using the given comparator function."},
    {"subseq",   prim_subseq,
     "Returns the entries of a sorted collection whose keys fall in the given range, ascending."},
    {"rsubseq",  prim_rsubseq,
     "Returns the entries of a sorted collection whose keys fall in the given range, descending."},
    {"seq",       prim_seq,
     "Returns a seq on the collection, or nil if empty."},
    {"realized?", prim_realized_p,
     "Returns true if the lazy value has been realized."},
};

const size_t k_prims_sequences_count =
    sizeof(k_prims_sequences) / sizeof(k_prims_sequences[0]);
