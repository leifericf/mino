/*
 * collections.c -- collection primitives: car, cdr, cons, count,
 *                       nth, first, rest, vector, hash-map, assoc, get,
 *                       conj, keys, vals, hash-set, contains?, disj,
 *                       dissoc, seq, realized?, val_to_seq, set_conj1.
 */

#include "prim/internal.h"

/* Forward decl: utf8 codepoint decoder used by string-handling
 * branches of first / rest / str_rest_thunk. The body lives below
 * (before vec_rest_thunk). */
static void coll_utf8_step(const unsigned char *data, size_t len, size_t pos,
                           unsigned int *cp_out, size_t *step_out);

mino_val *prim_car(mino_state *S, mino_val *args, mino_env *env)
{
    (void)env;
    if (!mino_is_cons(args)) {
        return prim_throw_classified(S, "eval/arity", "MAR001", "car requires one argument");
    }
    return mino_car(args->as.cons.car);
}

mino_val *prim_cdr(mino_state *S, mino_val *args, mino_env *env)
{
    (void)env;
    if (!mino_is_cons(args)) {
        return prim_throw_classified(S, "eval/arity", "MAR001", "cdr requires one argument");
    }
    return mino_cdr(args->as.cons.car);
}

/* Convert a value to a seq suitable for use as the cdr of a cons cell.
 * Returns nil for empty/nil, cons for lists, and builds a cons list for
 * vectors, maps, sets, and strings.  Used by prim_cons so that
 * (cons 1 #{2 3}) returns (1 2 3), not (1 . #{2 3}). */

mino_val *val_to_seq(mino_state *S, mino_val *v)
{
    mino_val *head;
    mino_val *tail;
    size_t i;

    if (v == NULL || mino_type_of(v) == MINO_NIL) return mino_nil(S);
    /* The empty-list singleton is user-visible only. As a cons cdr it
     * collapses back to nil so cons-chain walkers (mino_is_cons-based
     * traversal) terminate without an extra case. */
    if (mino_type_of(v) == MINO_EMPTY_LIST) return mino_nil(S);
    if (mino_type_of(v) == MINO_CONS) return v;
    /* Lazy seqs are valid as the cdr of a cons cell; do not force them
     * here to avoid infinite recursion with self-referential sequences
     * like (repeat x). */
    if (mino_type_of(v) == MINO_LAZY) return v;
    /* Chunked-cons spines (the shape produced by `rest` over chunked
     * sources like vector-backed lazy maps) are valid as cons cdr;
     * seq_iter and the seq prim handle them downstream without
     * materializing. Same for a raw chunk: passing it through keeps
     * the consumer's chunked path live. */
    if (mino_type_of(v) == MINO_CHUNKED_CONS) return v;
    if (mino_type_of(v) == MINO_CHUNK) return v;
    if (mino_type_of(v) == MINO_VECTOR) {
        if (v->as.vec.len == 0) return mino_nil(S);
        head = mino_nil(S);
        tail = NULL;
        for (i = 0; i < v->as.vec.len; i++) {
            mino_val *cell = mino_cons(S, vec_nth(v, i), mino_nil(S));
            if (tail == NULL) head = cell;
            else mino_cons_cdr_set(S, tail, cell);
            tail = cell;
        }
        return head;
    }
    if (mino_type_of(v) == MINO_MAP) {
        if (v->as.map.len == 0) return mino_nil(S);
        head = mino_nil(S);
        tail = NULL;
        for (i = 0; i < v->as.map.len; i++) {
            mino_val *key = vec_nth(v->as.map.key_order, i);
            mino_val *val = map_get_val(v, key);
            mino_val *kv[2];
            mino_val *cell;
            kv[0] = key; kv[1] = val;
            cell = mino_cons(S, mino_vector(S, kv, 2), mino_nil(S));
            if (tail == NULL) head = cell;
            else mino_cons_cdr_set(S, tail, cell);
            tail = cell;
        }
        return head;
    }
    if (mino_type_of(v) == MINO_SET) {
        if (v->as.set.len == 0) return mino_nil(S);
        head = mino_nil(S);
        tail = NULL;
        for (i = 0; i < v->as.set.len; i++) {
            mino_val *elem = vec_nth(v->as.set.key_order, i);
            mino_val *cell = mino_cons(S, elem, mino_nil(S));
            if (tail == NULL) head = cell;
            else mino_cons_cdr_set(S, tail, cell);
            tail = cell;
        }
        return head;
    }
    if (mino_type_of(v) == MINO_SORTED_MAP || mino_type_of(v) == MINO_SORTED_SET) {
        return sorted_seq(S, v);
    }
    if (mino_type_of(v) == MINO_BYTES) {
        /* MINO_BYTES seqs as unsigned 0..255 integers, one per byte. */
        return mino_bytes_seq(S, v);
    }
    if (mino_type_of(v) == MINO_STRING) {
        /* Per Clojure, sequencing a string yields chars, not
         * substrings. Walk UTF-8 codepoint by codepoint. */
        const unsigned char *bytes = (const unsigned char *)v->as.s.data;
        size_t pos = 0;
        if (v->as.s.len == 0) return mino_nil(S);
        head = mino_nil(S);
        tail = NULL;
        while (pos < v->as.s.len) {
            unsigned int cp;
            size_t       step;
            mino_val  *cell;
            coll_utf8_step(bytes, v->as.s.len, pos, &cp, &step);
            cell = mino_cons(S, mino_char(S, (int)cp), mino_nil(S));
            if (tail == NULL) head = cell;
            else mino_cons_cdr_set(S, tail, cell);
            tail = cell;
            pos += step;
        }
        return head;
    }
    /* Unsupported types: throw */
    {
        char msg[96];
        snprintf(msg, sizeof(msg),
                 "seq: cannot coerce %s to a sequence", type_tag_str(v));
        return prim_throw_classified(S, "eval/type", "MTY001", msg);
    }
}

/* host-array constructor primitive: ((<kind>) size-or-coll). When
 * given a non-negative integer, allocates a fresh host array of that
 * length filled with the kind's zero value (nil for Object, 0 for
 * int / long, etc.). When given a collection, copies its elements
 * into a host array of the corresponding length. */
static mino_val *prim_host_array_helper(mino_state *S, mino_val *args,
                                          host_array_kind_t kind,
                                          const char *opname)
{
    mino_val *arg;
    if (!mino_is_cons(args) || mino_is_cons(args->as.cons.cdr)) {
        char buf[80];
        snprintf(buf, sizeof(buf), "%s requires one argument", opname);
        return prim_throw_classified(S, "eval/arity", "MAR001", buf);
    }
    arg = args->as.cons.car;
    if (arg != NULL && mino_val_int_p(arg)) {
        if (mino_val_int_get(arg) < 0) {
            char buf[80];
            snprintf(buf, sizeof(buf), "%s: negative array size", opname);
            return prim_throw_classified(S, "eval/type", "MTY001", buf);
        }
        return mino_host_array_new(S, (size_t)mino_val_int_get(arg), kind);
    }
    /* Treat as a collection (vector / seq / list / set / map). */
    return mino_host_array_from_coll(S, arg, kind);
}

static mino_val *prim_object_array(mino_state *S, mino_val *args, mino_env *env)
{
    (void)env;
    return prim_host_array_helper(S, args, HOST_ARRAY_OBJECT, "object-array");
}

static mino_val *prim_int_array(mino_state *S, mino_val *args, mino_env *env)
{
    (void)env;
    return prim_host_array_helper(S, args, HOST_ARRAY_INT, "int-array");
}

static mino_val *prim_long_array(mino_state *S, mino_val *args, mino_env *env)
{
    (void)env;
    return prim_host_array_helper(S, args, HOST_ARRAY_LONG, "long-array");
}

static mino_val *prim_short_array(mino_state *S, mino_val *args, mino_env *env)
{
    (void)env;
    return prim_host_array_helper(S, args, HOST_ARRAY_SHORT, "short-array");
}

/* byte-array constructs an immutable MINO_BYTES value. JVM Clojure's
 * byte-array returns a host-mutable java byte[]; mino's persistent-
 * value model excludes in-place writes, so we ship the immutable
 * binary-data tier instead. bytes? on the result is true; aset!
 * throws :mino/immutable. */
static mino_val *prim_byte_array(mino_state *S, mino_val *args, mino_env *env)
{
    mino_val *arg;
    (void)env;
    if (!mino_is_cons(args) || mino_is_cons(args->as.cons.cdr)) {
        return prim_throw_classified(S, "eval/arity", "MAR001",
            "byte-array requires one argument");
    }
    arg = args->as.cons.car;
    if (arg != NULL && mino_val_int_p(arg)) {
        long long n = mino_val_int_get(arg);
        if (n < 0) {
            return prim_throw_classified(S, "eval/type", "MTY001",
                "byte-array: negative array size");
        }
        return mino_bytes(S, NULL, (size_t)n);
    }
    /* Treat as a collection: walk the seq into a growable buffer so
     * lazy seqs (e.g. (range N)) materialize on the fly. Each element
     * is coerced to a byte in -128..255. */
    {
        unsigned char *buf = NULL;
        size_t cap = 0;
        size_t idx = 0;
        mino_val *seqv;
        mino_val *p;
        mino_val *result;
        seqv = val_to_seq(S, arg);
        if (seqv == NULL && mino_last_error(S) != NULL) return NULL;
        if (seqv == NULL || mino_type_of(seqv) == MINO_NIL
            || mino_type_of(seqv) == MINO_EMPTY_LIST) {
            return mino_bytes(S, NULL, 0);
        }
        p = seqv;
        for (;;) {
            mino_val *elem;
            long long bv;
            /* Force any lazy seam. */
            while (p != NULL && mino_type_of(p) == MINO_LAZY) {
                p = lazy_force(S, p);
                if (p == NULL) { free(buf); return NULL; }
            }
            if (p == NULL || mino_type_of(p) == MINO_NIL
                || mino_type_of(p) == MINO_EMPTY_LIST) break;
            if (mino_type_of(p) == MINO_CHUNKED_CONS) {
                /* Bulk-copy whole chunks. */
                const mino_val *ch = p->as.chunked_cons.chunk;
                unsigned off = p->as.chunked_cons.off;
                unsigned len = ch->as.chunk.len;
                unsigned i;
                if (idx + (len - off) > cap) {
                    size_t need = idx + (len - off);
                    size_t new_cap = cap ? cap : 16;
                    while (new_cap < need) new_cap *= 2;
                    buf = (unsigned char *)realloc(buf, new_cap);
                    if (buf == NULL) {
                        return prim_throw_classified(S, "internal", "MIN001",
                            "byte-array: out of memory");
                    }
                    cap = new_cap;
                }
                for (i = off; i < len; i++) {
                    elem = ch->as.chunk.vals[i];
                    if (elem == NULL || !mino_val_int_p(elem)) {
                        free(buf);
                        return prim_throw_classified(S, "eval/type", "MTY001",
                            "byte-array: element must be an integer in -128..255");
                    }
                    bv = mino_val_int_get(elem);
                    if (bv < -128 || bv > 255) {
                        free(buf);
                        return prim_throw_classified(S, "eval/bounds", "MBD001",
                            "byte-array: integer out of byte range (-128..255)");
                    }
                    buf[idx++] = (unsigned char)(bv & 0xff);
                }
                p = p->as.chunked_cons.more;
                continue;
            }
            if (!mino_is_cons(p)) break;
            elem = p->as.cons.car;
            if (elem == NULL || !mino_val_int_p(elem)) {
                free(buf);
                return prim_throw_classified(S, "eval/type", "MTY001",
                    "byte-array: element must be an integer in -128..255");
            }
            bv = mino_val_int_get(elem);
            if (bv < -128 || bv > 255) {
                free(buf);
                return prim_throw_classified(S, "eval/bounds", "MBD001",
                    "byte-array: integer out of byte range (-128..255)");
            }
            if (idx >= cap) {
                size_t new_cap = cap ? cap * 2 : 16;
                buf = (unsigned char *)realloc(buf, new_cap);
                if (buf == NULL) {
                    return prim_throw_classified(S, "internal", "MIN001",
                        "byte-array: out of memory");
                }
                cap = new_cap;
            }
            buf[idx++] = (unsigned char)(bv & 0xff);
            p = p->as.cons.cdr;
        }
        if (idx == 0) {
            free(buf);
            return mino_bytes(S, NULL, 0);
        }
        result = mino_bytes(S, buf, idx);
        free(buf);
        return result;
    }
}

static mino_val *prim_float_array(mino_state *S, mino_val *args, mino_env *env)
{
    (void)env;
    return prim_host_array_helper(S, args, HOST_ARRAY_FLOAT, "float-array");
}

static mino_val *prim_double_array(mino_state *S, mino_val *args, mino_env *env)
{
    (void)env;
    return prim_host_array_helper(S, args, HOST_ARRAY_DOUBLE, "double-array");
}

static mino_val *prim_char_array(mino_state *S, mino_val *args, mino_env *env)
{
    (void)env;
    return prim_host_array_helper(S, args, HOST_ARRAY_CHAR, "char-array");
}

static mino_val *prim_boolean_array(mino_state *S, mino_val *args, mino_env *env)
{
    (void)env;
    return prim_host_array_helper(S, args, HOST_ARRAY_BOOLEAN, "boolean-array");
}

static mino_val *prim_to_array(mino_state *S, mino_val *args, mino_env *env)
{
    (void)env;
    return prim_host_array_helper(S, args, HOST_ARRAY_OBJECT, "to-array");
}

/* aset: in-place mutation of a host array slot. JVM Clojure's aset
 * mutates the underlying Java array; mino's MINO_HOST_ARRAY likewise
 * carries a malloc-owned vals[] that we can update in place. This is
 * the *only* mutation path mino offers outside MINO_ATOM /
 * MINO_VOLATILE -- the host-array tier exists specifically to mirror
 * JVM array semantics for cross-dialect tests. */
static mino_val *prim_aset(mino_state *S, mino_val *args, mino_env *env)
{
    mino_val *arr;
    mino_val *idx_val;
    mino_val *new_val;
    long long   idx;
    (void)env;
    if (!mino_is_cons(args) || !mino_is_cons(args->as.cons.cdr)
        || !mino_is_cons(args->as.cons.cdr->as.cons.cdr)
        || mino_is_cons(args->as.cons.cdr->as.cons.cdr->as.cons.cdr)) {
        return prim_throw_classified(S, "eval/arity", "MAR001",
            "aset requires three arguments (array, index, value)");
    }
    arr     = args->as.cons.car;
    idx_val = args->as.cons.cdr->as.cons.car;
    new_val = args->as.cons.cdr->as.cons.cdr->as.cons.car;
    if (arr != NULL && mino_type_of(arr) == MINO_BYTES) {
        return prim_throw_classified(S, "eval/state", "MST005",
            "aset: mino bytes values are immutable; build a new "
            "bytes value via byte-array instead");
    }
    if (arr == NULL || mino_type_of(arr) != MINO_HOST_ARRAY) {
        return prim_throw_classified(S, "eval/type", "MTY001",
            "aset: first argument must be a host array");
    }
    if (idx_val == NULL || !mino_val_int_p(idx_val)) {
        return prim_throw_classified(S, "eval/type", "MTY001",
            "aset: index must be an integer");
    }
    idx = mino_val_int_get(idx_val);
    if (idx < 0 || (size_t)idx >= arr->as.host_array.len) {
        return prim_throw_classified(S, "eval/bounds", "MBD001",
            "aset: index out of range");
    }
    /* host_array.vals[] is malloc-owned storage on the GC_T_VAL
     * container; the tracer walks it for free, but a slot store on a
     * promoted OLD container has to record the OLD->YOUNG edge in the
     * remset or the minor collector will reclaim new_val while the
     * slot still references it. Route the in-place write through
     * gc_write_barrier so the remset and the major-mark Dijkstra
     * insertion path both see the publication. */
    gc_write_barrier(S, arr, arr->as.host_array.vals[(size_t)idx], new_val);
    arr->as.host_array.vals[(size_t)idx] = new_val;
    return new_val;
}

/* aget: read a slot from a host array OR a MINO_BYTES value at an
 * integer index. Mirrors JVM Clojure's aget on byte[]: returns the
 * byte as an unsigned int (0..255). For MINO_HOST_ARRAY this is a
 * straight slot read; for MINO_BYTES the byte at the index is widened
 * to long. */
static mino_val *prim_aget(mino_state *S, mino_val *args, mino_env *env)
{
    mino_val *arr;
    mino_val *idx_val;
    long long   idx;
    (void)env;
    if (!mino_is_cons(args) || !mino_is_cons(args->as.cons.cdr)
        || mino_is_cons(args->as.cons.cdr->as.cons.cdr)) {
        return prim_throw_classified(S, "eval/arity", "MAR001",
            "aget requires two arguments (array, index)");
    }
    arr     = args->as.cons.car;
    idx_val = args->as.cons.cdr->as.cons.car;
    if (idx_val == NULL || !mino_val_int_p(idx_val)) {
        return prim_throw_classified(S, "eval/type", "MTY001",
            "aget: index must be an integer");
    }
    idx = mino_val_int_get(idx_val);
    if (idx < 0) {
        return prim_throw_classified(S, "eval/bounds", "MBD001",
            "aget: index out of range");
    }
    if (arr != NULL && mino_type_of(arr) == MINO_BYTES) {
        if ((size_t)idx >= arr->as.bytes.byte_len) {
            return prim_throw_classified(S, "eval/bounds", "MBD001",
                "aget: index out of range");
        }
        return mino_int(S,
            (long long)(unsigned)arr->as.bytes.data[(size_t)idx]);
    }
    if (arr == NULL || mino_type_of(arr) != MINO_HOST_ARRAY) {
        return prim_throw_classified(S, "eval/type", "MTY001",
            "aget: first argument must be a host array or bytes value");
    }
    if ((size_t)idx >= arr->as.host_array.len) {
        return prim_throw_classified(S, "eval/bounds", "MBD001",
            "aget: index out of range");
    }
    return arr->as.host_array.vals[(size_t)idx];
}

/* alength: count of elements in a host array OR bytes value. */
static mino_val *prim_alength(mino_state *S, mino_val *args, mino_env *env)
{
    mino_val *arr;
    (void)env;
    if (!mino_is_cons(args) || mino_is_cons(args->as.cons.cdr)) {
        return prim_throw_classified(S, "eval/arity", "MAR001",
            "alength requires one argument");
    }
    arr = args->as.cons.car;
    if (arr != NULL && mino_type_of(arr) == MINO_BYTES) {
        return mino_int(S, (long long)arr->as.bytes.byte_len);
    }
    if (arr == NULL || mino_type_of(arr) != MINO_HOST_ARRAY) {
        return prim_throw_classified(S, "eval/type", "MTY001",
            "alength: argument must be a host array or bytes value");
    }
    return mino_int(S, (long long)arr->as.host_array.len);
}

static mino_val *prim_map_entry(mino_state *S, mino_val *args, mino_env *env)
{
    (void)env;
    if (!mino_is_cons(args) || !mino_is_cons(args->as.cons.cdr)
        || mino_is_cons(args->as.cons.cdr->as.cons.cdr)) {
        return prim_throw_classified(S, "eval/arity", "MAR001",
                                     "map-entry requires two arguments");
    }
    return mino_map_entry(S, args->as.cons.car,
                          args->as.cons.cdr->as.cons.car);
}

static mino_val *prim_cons_step(mino_state *S, mino_val *car,
                                   mino_val *cdr_arg, mino_env *env)
{
    mino_val *cdr;
    mino_val *result;
    (void)env;
    cdr = val_to_seq(S, cdr_arg);
    if (cdr == NULL) return NULL;
    result = mino_cons(S, car, cdr);
    if (result == NULL) return NULL;
    /* `(cons x y)` returns a Cons-shaped seq that is distinct from a
     * list literal: `(list? ...)` is false, `peek`/`pop` throw. The
     * data shape stays MINO_CONS so the eval path can still apply
     * macro-built forms; we just flip a bit to mark the cell as a
     * cons-call result. List literals from the reader keep the bit
     * cleared. */
    result->as.cons.not_list = 1;
    return result;
}

mino_val *prim_cons(mino_state *S, mino_val *args, mino_env *env)
{
    if (!mino_is_cons(args) || !mino_is_cons(args->as.cons.cdr)) {
        return prim_throw_classified(S, "eval/arity", "MAR001",
            "cons requires two arguments");
    }
    return prim_cons_step(S, args->as.cons.car,
                           args->as.cons.cdr->as.cons.car, env);
}

mino_val *prim_cons_argv(mino_state *S, mino_val **argv, int argc,
                            mino_env *env)
{
    if (argc != 2) {
        return prim_throw_classified(S, "eval/arity", "MAR001",
            "cons requires two arguments");
    }
    return prim_cons_step(S, argv[0], argv[1], env);
}

/* ------------------------------------------------------------------------- */
/* Collection primitives                                                     */
/*                                                                           */
/* All collection ops treat values as immutable: every operation that        */
/* "modifies" a collection returns a freshly allocated value. Concrete       */
/* representations (array-backed list/vec/map, persistent tries) sit         */
/* behind the public primitive contracts and may change without API drift.   */
/* ------------------------------------------------------------------------- */

size_t list_length(mino_state *S, mino_val *list)
{
    size_t n = 0;
    while (list != NULL && mino_type_of(list) == MINO_LAZY) {
        list = lazy_force(S, list);
    }
    while (mino_is_cons(list)) {
        n++;
        list = list->as.cons.cdr;
        /* Force lazy tails. */
        while (list != NULL && mino_type_of(list) == MINO_LAZY) {
            list = lazy_force(S, list);
        }
    }
    return n;
}

int arg_count(mino_state *S, mino_val *args, size_t *out)
{
    *out = list_length(S, args);
    return 1;
}

static mino_val *prim_count_step(mino_state *S, mino_val *coll,
                                    mino_env *env)
{
    if (coll == NULL || mino_type_of(coll) == MINO_NIL) {
        return mino_int(S, 0);
    }
    if (mino_type_of(coll) == MINO_TRANSIENT) {
        if (!coll->as.transient.valid)
            return prim_throw_classified(S, "eval/state", "MST001",
                "count: transient is no longer valid");
        coll = coll->as.transient.current;
        if (coll == NULL || mino_type_of(coll) == MINO_NIL) return mino_int(S, 0);
    }
    switch (mino_type_of(coll)) {
    case MINO_EMPTY_LIST: return mino_int(S, 0);
    case MINO_CONS:   return mino_int(S, (long long)list_length(S, coll));
    case MINO_VECTOR: return mino_int(S, (long long)coll->as.vec.len);
    case MINO_MAP:    return mino_int(S, (long long)coll->as.map.len);
    case MINO_SET:    return mino_int(S, (long long)coll->as.set.len);
    case MINO_SORTED_MAP:
    case MINO_SORTED_SET: return mino_int(S, (long long)coll->as.sorted.len);
    case MINO_STRING:
        /* Codepoint count, matching subs / nth / char-at semantics
         * (and Clojure's char-counted strings). For ASCII content
         * the byte walk and codepoint walk coincide. */
        return mino_int(S,
            utf8_codepoint_count(coll->as.s.data, coll->as.s.len));
    case MINO_RECORD: {
        mino_val *fields = coll->as.record.type->as.record_type.fields;
        size_t n = (fields != NULL) ? fields->as.vec.len : 0;
        if (coll->as.record.ext != NULL) n += coll->as.record.ext->as.map.len;
        return mino_int(S, (long long)n);
    }
    case MINO_LAZY: {
        /* Force the entire lazy seq and count it. The forced value may
         * be a flat cons spine, a chunked-cons spine (lazy range), or
         * another lazy. Dispatch instead of assuming the cons-only
         * walk. */
        mino_val *forced = lazy_force(S, coll);
        if (forced == NULL) return NULL;
        if (mino_type_of(forced) == MINO_NIL) return mino_int(S, 0);
        if (mino_type_of(forced) == MINO_CHUNKED_CONS) {
            return prim_count_step(S, forced, env);
        }
        return mino_int(S, (long long)list_length(S, forced));
    }
    case MINO_CHUNK: return mino_int(S, (long long)coll->as.chunk.len);
    case MINO_HOST_ARRAY:
        return mino_int(S, (long long)coll->as.host_array.len);
    case MINO_MAP_ENTRY:
        return mino_int(S, 2);
    case MINO_QUEUE:
        return mino_int(S, (long long)mino_queue_count(coll));
    case MINO_BYTES:
        return mino_int(S, (long long)mino_bytes_len(coll));
    case MINO_CHUNKED_CONS: {
        long long          n = 0;
        const mino_val  *cur = coll;
        while (cur != NULL && mino_type_of(cur) == MINO_CHUNKED_CONS) {
            const mino_val *ch = cur->as.chunked_cons.chunk;
            n += (long long)(ch->as.chunk.len - cur->as.chunked_cons.off);
            cur = cur->as.chunked_cons.more;
            if (cur != NULL && mino_type_of(cur) == MINO_LAZY) {
                cur = lazy_force(S, (mino_val *)cur);
                if (cur == NULL) return NULL;
            }
        }
        if (cur != NULL && mino_type_of(cur) != MINO_NIL
            && mino_type_of(cur) != MINO_EMPTY_LIST) {
            n += (long long)list_length(S, (mino_val *)cur);
        }
        return mino_int(S, n);
    }
    default:
        {
            char msg[96];
            snprintf(msg, sizeof(msg), "count: expected a collection, got %s",
                     type_tag_str(coll));
            return prim_throw_classified(S, "eval/type", "MTY001", msg);
    }
    }
}

mino_val *prim_count(mino_state *S, mino_val *args, mino_env *env)
{
    if (!mino_is_cons(args) || mino_is_cons(args->as.cons.cdr)) {
        return prim_throw_classified(S, "eval/arity", "MAR001",
            "count requires one argument");
    }
    return prim_count_step(S, args->as.cons.car, env);
}

mino_val *prim_count_argv(mino_state *S, mino_val **argv, int argc,
                            mino_env *env)
{
    if (argc != 1) {
        return prim_throw_classified(S, "eval/arity", "MAR001",
            "count requires one argument");
    }
    return prim_count_step(S, argv[0], env);
}

static mino_val *prim_empty_queue(mino_state *S, mino_val *args, mino_env *env)
{
    (void)env;
    if (mino_is_cons(args)) {
        return prim_throw_classified(S, "eval/arity", "MAR001",
            "-empty-queue takes no arguments");
    }
    return mino_queue_empty(S);
}

static mino_val *prim_queue_p(mino_state *S, mino_val *args, mino_env *env)
{
    (void)env;
    if (!mino_is_cons(args) || mino_is_cons(args->as.cons.cdr)) {
        return prim_throw_classified(S, "eval/arity", "MAR001",
            "queue? requires one argument");
    }
    return mino_is_queue(args->as.cons.car) ? mino_true(S) : mino_false(S);
}

mino_val *prim_vector(mino_state *S, mino_val *args, mino_env *env)
{
    size_t n;
    size_t i;
    mino_val **tmp;
    mino_val *p;
    (void)env;
    arg_count(S, args, &n);
    if (n == 0) {
        return mino_vector(S, NULL, 0);
    }
    /* GC_T_VALARR keeps partially-gathered pointers visible to the collector;
     * without this, the optimizer may drop the `args` parameter and the cons
     * cells holding the element values become unreachable mid-construction. */
    tmp = (mino_val **)gc_alloc_typed(S, GC_T_VALARR, n * sizeof(*tmp));
    p = args;
    for (i = 0; i < n; i++) {
        tmp[i] = p->as.cons.car;
        p = p->as.cons.cdr;
    }
    return mino_vector(S, tmp, n);
}

mino_val *prim_hash_map(mino_state *S, mino_val *args, mino_env *env)
{
    size_t n;
    size_t pairs;
    size_t i;
    mino_val **ks;
    mino_val **vs;
    mino_val *p;
    (void)env;
    arg_count(S, args, &n);
    if (n % 2 != 0) {
        return prim_throw_classified(S, "eval/arity", "MAR001", "hash-map requires an even number of arguments");
    }
    if (n == 0) {
        return mino_map(S, NULL, NULL, 0);
    }
    pairs = n / 2;
    ks = (mino_val **)gc_alloc_typed(S, GC_T_VALARR, pairs * sizeof(*ks));
    vs = (mino_val **)gc_alloc_typed(S, GC_T_VALARR, pairs * sizeof(*vs));
    p = args;
    for (i = 0; i < pairs; i++) {
        /* Force lazy seam: callers like `(apply hash-map (rest vec))`
         * thread a CONS whose cdr is a LAZY continuation. Walking the
         * cdr without forcing reads through the LAZY tag and lands in
         * the wrong union slot. */
        while (p != NULL && mino_type_of(p) == MINO_LAZY) {
            p = lazy_force(S, p);
            if (p == NULL) return NULL;
        }
        if (!mino_is_cons(p)) {
            return prim_throw_classified(S, "eval/type", "MTY001",
                "hash-map: argument walk produced a non-cons");
        }
        ks[i] = p->as.cons.car;
        p = p->as.cons.cdr;
        while (p != NULL && mino_type_of(p) == MINO_LAZY) {
            p = lazy_force(S, p);
            if (p == NULL) return NULL;
        }
        if (!mino_is_cons(p)) {
            return prim_throw_classified(S, "eval/type", "MTY001",
                "hash-map: argument walk produced a non-cons");
        }
        vs[i] = p->as.cons.car;
        p = p->as.cons.cdr;
    }
    return mino_map(S, ks, vs, pairs);
}

mino_val *prim_nth(mino_state *S, mino_val *args, mino_env *env)
{
    mino_val *coll;
    mino_val *idx_val;
    mino_val *def_val = NULL;
    size_t      n;
    long long   idx;
    (void)env;
    arg_count(S, args, &n);
    if (n != 2 && n != 3) {
        return prim_throw_classified(S, "eval/arity", "MAR001", "nth requires 2 or 3 arguments");
    }
    coll    = args->as.cons.car;
    idx_val = args->as.cons.cdr->as.cons.car;
    if (n == 3) {
        def_val = args->as.cons.cdr->as.cons.cdr->as.cons.car;
    }
    if (idx_val == NULL || !mino_val_int_p(idx_val)) {
        return prim_throw_classified(S, "eval/type", "MTY001", "nth index must be an integer");
    }
    idx = mino_val_int_get(idx_val);
    if (idx < 0) {
        if (def_val != NULL) return def_val;
        return prim_throw_classified(S, "eval/bounds", "MBD001", "nth index out of range");
    }
    if (coll == NULL || mino_type_of(coll) == MINO_NIL) {
        /* Clojure treats nil as an empty seq for nth: returns the
         * default if supplied, otherwise nil (does NOT throw). */
        if (def_val != NULL) return def_val;
        return mino_nil(S);
    }
    if (mino_type_of(coll) == MINO_TRANSIENT) {
        if (!coll->as.transient.valid)
            return prim_throw_classified(S, "eval/state", "MST001",
                "nth: transient is no longer valid");
        coll = coll->as.transient.current;
        if (coll == NULL || mino_type_of(coll) == MINO_NIL) {
            if (def_val != NULL) return def_val;
            return prim_throw_classified(S, "eval/bounds", "MBD001",
                "nth index out of range");
        }
    }
    if (mino_type_of(coll) == MINO_LAZY) {
        coll = lazy_force(S, coll);
        if (coll == NULL) return NULL;
        if (mino_type_of(coll) == MINO_NIL) {
            if (def_val != NULL) return def_val;
            return prim_throw_classified(S, "eval/bounds", "MBD001", "nth index out of range");
        }
    }
    if (mino_type_of(coll) == MINO_VECTOR) {
        if ((size_t)idx >= coll->as.vec.len) {
            if (def_val != NULL) return def_val;
            return prim_throw_classified(S, "eval/bounds", "MBD001", "nth index out of range");
        }
        return vec_nth(coll, (size_t)idx);
    }
    if (mino_type_of(coll) == MINO_CHUNK) {
        if ((size_t)idx >= coll->as.chunk.len) {
            if (def_val != NULL) return def_val;
            return prim_throw_classified(S, "eval/bounds", "MBD001", "nth index out of range");
        }
        return coll->as.chunk.vals[(size_t)idx];
    }
    if (mino_type_of(coll) == MINO_HOST_ARRAY) {
        if ((size_t)idx >= coll->as.host_array.len) {
            if (def_val != NULL) return def_val;
            return prim_throw_classified(S, "eval/bounds", "MBD001", "nth index out of range");
        }
        return coll->as.host_array.vals[(size_t)idx];
    }
    if (mino_type_of(coll) == MINO_BYTES) {
        if ((size_t)idx >= coll->as.bytes.byte_len) {
            if (def_val != NULL) return def_val;
            return prim_throw_classified(S, "eval/bounds", "MBD001", "nth index out of range");
        }
        return mino_int(S,
            (long long)(unsigned)coll->as.bytes.data[(size_t)idx]);
    }
    if (mino_type_of(coll) == MINO_MAP_ENTRY) {
        if (idx == 0) return coll->as.map_entry.k;
        if (idx == 1) return coll->as.map_entry.v;
        if (def_val != NULL) return def_val;
        return prim_throw_classified(S, "eval/bounds", "MBD001", "nth index out of range");
    }
    if (mino_type_of(coll) == MINO_CONS) {
        mino_val *p = coll;
        long long   i;
        for (i = 0; i < idx; i++) {
            if (!mino_is_cons(p)) {
                if (def_val != NULL) return def_val;
                return prim_throw_classified(S, "eval/bounds", "MBD001", "nth index out of range");
            }
            p = p->as.cons.cdr;
            if (p != NULL && mino_type_of(p) == MINO_LAZY) {
                p = lazy_force(S, p);
                if (p == NULL) return NULL;
            }
        }
        if (!mino_is_cons(p)) {
            if (def_val != NULL) return def_val;
            return prim_throw_classified(S, "eval/bounds", "MBD001", "nth index out of range");
        }
        return p->as.cons.car;
    }
    if (mino_type_of(coll) == MINO_CHUNKED_CONS) {
        long long remaining = idx;
        mino_val *p = coll;
        while (p != NULL && mino_type_of(p) == MINO_CHUNKED_CONS) {
            const mino_val *ch = p->as.chunked_cons.chunk;
            unsigned available = ch->as.chunk.len - p->as.chunked_cons.off;
            if (remaining < (long long)available) {
                return ch->as.chunk.vals[p->as.chunked_cons.off + remaining];
            }
            remaining -= (long long)available;
            p = p->as.chunked_cons.more;
            while (p != NULL && mino_type_of(p) == MINO_LAZY) {
                p = lazy_force(S, p);
                if (p == NULL) return NULL;
            }
        }
        if (p == NULL || mino_type_of(p) == MINO_NIL || mino_type_of(p) == MINO_EMPTY_LIST) {
            if (def_val != NULL) return def_val;
            return prim_throw_classified(S, "eval/bounds", "MBD001", "nth index out of range");
        }
        /* Fall through to the cons-walk for the lazy/cons tail. */
        coll = p;
        idx  = remaining;
        if (mino_type_of(coll) == MINO_CONS) {
            long long i;
            for (i = 0; i < idx; i++) {
                if (!mino_is_cons(coll)) {
                    if (def_val != NULL) return def_val;
                    return prim_throw_classified(S, "eval/bounds", "MBD001", "nth index out of range");
                }
                coll = coll->as.cons.cdr;
                if (coll != NULL && mino_type_of(coll) == MINO_LAZY) {
                    coll = lazy_force(S, coll);
                    if (coll == NULL) return NULL;
                }
            }
            if (!mino_is_cons(coll)) {
                if (def_val != NULL) return def_val;
                return prim_throw_classified(S, "eval/bounds", "MBD001", "nth index out of range");
            }
            return coll->as.cons.car;
        }
        if (def_val != NULL) return def_val;
        return prim_throw_classified(S, "eval/bounds", "MBD001", "nth index out of range");
    }
    if (mino_type_of(coll) == MINO_STRING) {
        if ((size_t)idx >= coll->as.s.len) {
            if (def_val != NULL) return def_val;
            return prim_throw_classified(S, "eval/bounds", "MBD001", "nth index out of range");
        }
        return mino_string_n(S, coll->as.s.data + idx, 1);
    }
    {
        char msg[96];
        snprintf(msg, sizeof(msg), "nth: expected a list, vector, or string, got %s",
                 type_tag_str(coll));
        return prim_throw_classified(S, "eval/type", "MTY001", msg);
    }
}

static mino_val *prim_first_step(mino_state *S, mino_val *coll,
                                    mino_env *env)
{
    (void)env;
    if (coll == NULL || mino_type_of(coll) == MINO_NIL) {
        return mino_nil(S);
    }
    if (mino_type_of(coll) == MINO_EMPTY_LIST) {
        return mino_nil(S);
    }
    if (mino_type_of(coll) == MINO_CONS) {
        return coll->as.cons.car;
    }
    if (mino_type_of(coll) == MINO_VECTOR) {
        if (coll->as.vec.len == 0) {
            return mino_nil(S);
        }
        return vec_nth(coll, 0);
    }
    if (mino_type_of(coll) == MINO_LAZY) {
        mino_val *s = lazy_force(S, coll);
        if (s == NULL) return NULL;
        if (mino_type_of(s) == MINO_NIL || s == NULL) return mino_nil(S);
        if (mino_type_of(s) == MINO_CONS) return s->as.cons.car;
        if (mino_type_of(s) == MINO_CHUNKED_CONS) {
            const mino_val *ch = s->as.chunked_cons.chunk;
            return ch->as.chunk.vals[s->as.chunked_cons.off];
        }
        return mino_nil(S);
    }
    if (mino_type_of(coll) == MINO_CHUNKED_CONS) {
        const mino_val *ch = coll->as.chunked_cons.chunk;
        return ch->as.chunk.vals[coll->as.chunked_cons.off];
    }
    if (mino_type_of(coll) == MINO_HOST_ARRAY) {
        if (coll->as.host_array.len == 0) return mino_nil(S);
        return coll->as.host_array.vals[0];
    }
    if (mino_type_of(coll) == MINO_MAP_ENTRY) {
        return coll->as.map_entry.k;
    }
    if (mino_type_of(coll) == MINO_STRING) {
        unsigned int cp;
        size_t       step;
        if (coll->as.s.len == 0) return mino_nil(S);
        coll_utf8_step((const unsigned char *)coll->as.s.data,
                       coll->as.s.len, 0, &cp, &step);
        (void)step;
        return mino_char(S, (int)cp);
    }
    if (mino_type_of(coll) == MINO_MAP) {
        if (coll->as.map.len == 0) return mino_nil(S);
        {
            mino_val *key = vec_nth(coll->as.map.key_order, 0);
            mino_val *val = map_get_val(coll, key);
            return mino_map_entry(S, key, val);
        }
    }
    if (mino_type_of(coll) == MINO_SET) {
        if (coll->as.set.len == 0) return mino_nil(S);
        return vec_nth(coll->as.set.key_order, 0);
    }
    if (mino_type_of(coll) == MINO_SORTED_MAP || mino_type_of(coll) == MINO_SORTED_SET) {
        const mino_rb_node_t *n = coll->as.sorted.root;
        if (n == NULL) return mino_nil(S);
        while (n->left != NULL) n = n->left;
        if (mino_type_of(coll) == MINO_SORTED_MAP) {
            return mino_map_entry(S, n->key, n->val);
        }
        return n->key;
    }
    if (mino_type_of(coll) == MINO_QUEUE) {
        mino_val *first = mino_queue_peek(coll);
        return (first != NULL) ? first : mino_nil(S);
    }
    if (mino_type_of(coll) == MINO_BYTES) {
        if (coll->as.bytes.byte_len == 0) return mino_nil(S);
        return mino_int(S, (long long)(unsigned)coll->as.bytes.data[0]);
    }
    {
        char msg[96];
        snprintf(msg, sizeof(msg), "first: expected a list or vector, got %s",
                 type_tag_str(coll));
        return prim_throw_classified(S, "eval/type", "MTY001", msg);
    }
}

mino_val *prim_first(mino_state *S, mino_val *args, mino_env *env)
{
    if (!mino_is_cons(args) || mino_is_cons(args->as.cons.cdr)) {
        return prim_throw_classified(S, "eval/arity", "MAR001",
            "first requires one argument");
    }
    return prim_first_step(S, args->as.cons.car, env);
}

mino_val *prim_first_argv(mino_state *S, mino_val **argv, int argc,
                            mino_env *env)
{
    if (argc != 1) {
        return prim_throw_classified(S, "eval/arity", "MAR001",
            "first requires one argument");
    }
    return prim_first_step(S, argv[0], env);
}

/* Lazy rest thunks: each takes a cons(collection, int-index) as context. */
static mino_val *make_c_lazy(mino_state *S, mino_val *ctx,
                               mino_val *(*thunk)(mino_state *, mino_val *))
{
    mino_val *lz = alloc_val(S, MINO_LAZY);
    lz->as.lazy.body    = ctx;
    lz->as.lazy.c_thunk = thunk;
    return lz;
}

/* (Already forward-declared at the top of the file.) */
static void coll_utf8_step(const unsigned char *data, size_t len, size_t pos,
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

static mino_val *vec_rest_thunk(mino_state *S, mino_val *ctx)
{
    mino_val *vec = ctx->as.cons.car;
    size_t idx      = (size_t)mino_val_int_get(ctx->as.cons.cdr);
    if (idx >= vec->as.vec.len) return mino_nil(S);
    return mino_cons(S, vec_nth(vec, idx),
        make_c_lazy(S, mino_cons(S, vec, mino_int(S, (long long)(idx + 1))),
                    vec_rest_thunk));
}

static mino_val *str_rest_thunk(mino_state *S, mino_val *ctx)
{
    mino_val  *str = ctx->as.cons.car;
    size_t idx       = (size_t)mino_val_int_get(ctx->as.cons.cdr);
    unsigned int cp;
    size_t       step;
    if (idx >= str->as.s.len) return mino_nil(S);
    coll_utf8_step((const unsigned char *)str->as.s.data, str->as.s.len,
                   idx, &cp, &step);
    return mino_cons(S, mino_char(S, (int)cp),
        make_c_lazy(S, mino_cons(S, str,
                                 mino_int(S, (long long)(idx + step))),
                    str_rest_thunk));
}

static mino_val *map_rest_thunk(mino_state *S, mino_val *ctx)
{
    mino_val *m  = ctx->as.cons.car;
    size_t idx     = (size_t)mino_val_int_get(ctx->as.cons.cdr);
    if (idx >= m->as.map.len) return mino_nil(S);
    {
        mino_val *k = vec_nth(m->as.map.key_order, idx);
        mino_val *v = map_get_val(m, k);
        return mino_cons(S, mino_map_entry(S, k, v),
            make_c_lazy(S, mino_cons(S, m, mino_int(S, (long long)(idx + 1))),
                        map_rest_thunk));
    }
}

static mino_val *set_rest_thunk(mino_state *S, mino_val *ctx)
{
    mino_val *s = ctx->as.cons.car;
    size_t idx    = (size_t)mino_val_int_get(ctx->as.cons.cdr);
    if (idx >= s->as.set.len) return mino_nil(S);
    return mino_cons(S, vec_nth(s->as.set.key_order, idx),
        make_c_lazy(S, mino_cons(S, s, mino_int(S, (long long)(idx + 1))),
                    set_rest_thunk));
}

static mino_val *prim_rest_step(mino_state *S, mino_val *coll,
                                   mino_env *env)
{
    /* User-visible empty rest is the empty-list singleton, not nil:
     * (rest '()) -> (), (rest nil) -> (), (rest '(1)) -> (). */
    if (coll == NULL || mino_type_of(coll) == MINO_NIL) {
        return mino_empty_list(S);
    }
    if (mino_type_of(coll) == MINO_EMPTY_LIST) {
        return mino_empty_list(S);
    }
    if (mino_type_of(coll) == MINO_CONS) {
        mino_val *cdr = coll->as.cons.cdr;
        if (cdr == NULL || mino_type_of(cdr) == MINO_NIL) return mino_empty_list(S);
        /* Lazy cdr stays lazy here so infinite seqs don't blow the
         * stack; the lazy-seq seam is handled at the next prim_rest /
         * prim_first / prim_seq call. */
        return cdr;
    }
    if (mino_type_of(coll) == MINO_VECTOR) {
        if (coll->as.vec.len <= 1) return mino_empty_list(S);
        return mino_cons(S, vec_nth(coll, 1),
            make_c_lazy(S, mino_cons(S, coll, mino_int(S, 2)),
                        vec_rest_thunk));
    }
    if (mino_type_of(coll) == MINO_LAZY) {
        mino_val *s = lazy_force(S, coll);
        if (s == NULL) return NULL;
        if (mino_type_of(s) == MINO_NIL) return mino_empty_list(S);
        if (mino_type_of(s) == MINO_EMPTY_LIST) return mino_empty_list(S);
        if (mino_type_of(s) == MINO_CONS) {
            mino_val *cdr = s->as.cons.cdr;
            if (cdr == NULL || mino_type_of(cdr) == MINO_NIL) return mino_empty_list(S);
            return cdr;
        }
        if (mino_type_of(s) == MINO_CHUNKED_CONS) {
            mino_val *r = mino_chunked_cons_advance(S, s);
            if (r == NULL || mino_type_of(r) == MINO_NIL) return mino_empty_list(S);
            return r;
        }
        return mino_empty_list(S);
    }
    if (mino_type_of(coll) == MINO_CHUNKED_CONS) {
        mino_val *r = mino_chunked_cons_advance(S, coll);
        if (r == NULL || mino_type_of(r) == MINO_NIL) return mino_empty_list(S);
        return r;
    }
    if (mino_type_of(coll) == MINO_HOST_ARRAY) {
        /* Defer to seq, which builds a chunked-cons; rest of that is
         * the chunked-cons advance. Seq returns nil on empty. */
        mino_val *seq_args = mino_cons(S, coll, mino_nil(S));
        mino_val *s = prim_seq(S, seq_args, env);
        if (s == NULL) return NULL;
        if (mino_type_of(s) == MINO_NIL) return mino_empty_list(S);
        if (mino_type_of(s) == MINO_CHUNKED_CONS) {
            mino_val *r = mino_chunked_cons_advance(S, s);
            if (r == NULL || mino_type_of(r) == MINO_NIL) return mino_empty_list(S);
            return r;
        }
        return mino_empty_list(S);
    }
    if (mino_type_of(coll) == MINO_MAP_ENTRY) {
        /* Rest of (k v) is a 1-element seq holding v. */
        return mino_cons(S, coll->as.map_entry.v, mino_nil(S));
    }
    if (mino_type_of(coll) == MINO_STRING) {
        /* Skip the first codepoint (1-4 bytes), then construct a
         * cons whose car is the next character (decoded as MINO_CHAR)
         * and whose cdr is a lazy continuation for the remainder. */
        unsigned int first_cp;
        size_t       first_step;
        unsigned int second_cp;
        size_t       second_step;
        if (coll->as.s.len == 0) return mino_empty_list(S);
        coll_utf8_step((const unsigned char *)coll->as.s.data,
                       coll->as.s.len, 0, &first_cp, &first_step);
        (void)first_cp;
        if (first_step >= coll->as.s.len) return mino_empty_list(S);
        coll_utf8_step((const unsigned char *)coll->as.s.data,
                       coll->as.s.len, first_step,
                       &second_cp, &second_step);
        return mino_cons(S, mino_char(S, (int)second_cp),
            make_c_lazy(S, mino_cons(S, coll,
                                     mino_int(S, (long long)(first_step + second_step))),
                        str_rest_thunk));
    }
    if (mino_type_of(coll) == MINO_MAP) {
        if (coll->as.map.len <= 1) return mino_empty_list(S);
        {
            mino_val *k = vec_nth(coll->as.map.key_order, 1);
            mino_val *v = map_get_val(coll, k);
            return mino_cons(S, mino_map_entry(S, k, v),
                make_c_lazy(S, mino_cons(S, coll, mino_int(S, 2)),
                            map_rest_thunk));
        }
    }
    if (mino_type_of(coll) == MINO_SET) {
        if (coll->as.set.len <= 1) return mino_empty_list(S);
        return mino_cons(S, vec_nth(coll->as.set.key_order, 1),
            make_c_lazy(S, mino_cons(S, coll, mino_int(S, 2)),
                        set_rest_thunk));
    }
    if (mino_type_of(coll) == MINO_SORTED_MAP || mino_type_of(coll) == MINO_SORTED_SET) {
        mino_val *r = sorted_rest(S, coll);
        if (r == NULL) return NULL;
        if (mino_type_of(r) == MINO_NIL) return mino_empty_list(S);
        return r;
    }
    if (mino_type_of(coll) == MINO_QUEUE) {
        if (mino_queue_count(coll) <= 1) return mino_empty_list(S);
        {
            mino_val *seqv = mino_queue_seq(S, coll);
            if (seqv == NULL || !mino_is_cons(seqv)) return mino_empty_list(S);
            return seqv->as.cons.cdr;
        }
    }
    if (mino_type_of(coll) == MINO_BYTES) {
        /* mino_bytes_seq returns a chunked-cons spine for non-empty
         * values. Advance the chunked-cons one step to get the rest. */
        mino_val *seqv;
        if (coll->as.bytes.byte_len <= 1) return mino_empty_list(S);
        seqv = mino_bytes_seq(S, coll);
        if (seqv == NULL) return NULL;
        if (mino_type_of(seqv) == MINO_NIL) return mino_empty_list(S);
        if (mino_type_of(seqv) == MINO_CHUNKED_CONS) {
            mino_val *r = mino_chunked_cons_advance(S, seqv);
            if (r == NULL || mino_type_of(r) == MINO_NIL) return mino_empty_list(S);
            return r;
        }
        if (mino_is_cons(seqv)) return seqv->as.cons.cdr;
        return mino_empty_list(S);
    }
    {
        char msg[96];
        snprintf(msg, sizeof(msg), "rest: expected a list or vector, got %s",
                 type_tag_str(coll));
        return prim_throw_classified(S, "eval/type", "MTY001", msg);
    }
}

mino_val *prim_rest(mino_state *S, mino_val *args, mino_env *env)
{
    if (!mino_is_cons(args) || mino_is_cons(args->as.cons.cdr)) {
        return prim_throw_classified(S, "eval/arity", "MAR001",
            "rest requires one argument");
    }
    return prim_rest_step(S, args->as.cons.car, env);
}

mino_val *prim_rest_argv(mino_state *S, mino_val **argv, int argc,
                            mino_env *env)
{
    if (argc != 1) {
        return prim_throw_classified(S, "eval/arity", "MAR001",
            "rest requires one argument");
    }
    return prim_rest_step(S, argv[0], env);
}

/* Layer n k/v pairs onto an existing map, returning a new map value that
 * shares structure with `coll`. Nil is treated as an empty map. Routes
 * through mino_map_assoc1 so both flatmap and HAMT representations are
 * handled transparently, including any flatmap -> HAMT promotion when
 * the accumulating size crosses the threshold. */
static mino_val *map_assoc_pairs(mino_state *S, mino_val *coll,
                                    mino_val *p, size_t extra_pairs)
{
    mino_val *acc;
    size_t      i;
    mino_current_ctx(S)->gc_depth++;
    if (coll == NULL || mino_type_of(coll) == MINO_NIL) {
        mino_val **noargs = NULL;
        acc = mino_map(S, noargs, noargs, 0);
    } else {
        acc = coll;
    }
    for (i = 0; i < extra_pairs; i++) {
        mino_val *k = p->as.cons.car;
        mino_val *v = p->as.cons.cdr->as.cons.car;
        acc = mino_map_assoc1(S, acc, k, v);
        if (acc == NULL) { mino_current_ctx(S)->gc_depth--; return NULL; }
        p = p->as.cons.cdr->as.cons.cdr;
    }
    if (coll != NULL && mino_type_of(coll) == MINO_MAP) {
        acc->meta = coll->meta;
    }
    mino_current_ctx(S)->gc_depth--;
    return acc;
}

mino_val *prim_assoc(mino_state *S, mino_val *args, mino_env *env)
{
    mino_val *coll;
    size_t      n;
    size_t      extra_pairs;
    size_t      i;
    mino_val *p;
    (void)env;
    arg_count(S, args, &n);
    if (n < 3 || (n - 1) % 2 != 0) {
        return prim_throw_classified(S, "eval/arity", "MAR001", "assoc requires a collection and an even number of k/v pairs");
    }
    coll = args->as.cons.car;
    extra_pairs = (n - 1) / 2;
    if (coll != NULL && mino_type_of(coll) == MINO_VECTOR) {
        /* Vector assoc: each key must be an integer index in [0, len]; an
         * index == len is a one-past-end append. Apply pairs in order on
         * successively-derived vectors so each update shares structure with
         * its predecessor. */
        mino_val *acc = coll;
        p = args->as.cons.cdr;
        for (i = 0; i < extra_pairs; i++) {
            mino_val *k = p->as.cons.car;
            mino_val *v = p->as.cons.cdr->as.cons.car;
            long long   idx;
            if (k == NULL || !mino_val_int_p(k)) {
                return prim_throw_classified(S, "eval/type", "MTY001", "assoc on vector requires integer indices");
            }
            idx = mino_val_int_get(k);
            if (idx < 0 || (size_t)idx > acc->as.vec.len) {
                return prim_throw_classified(S, "eval/bounds", "MBD001", "assoc on vector: index out of range");
            }
            acc = vec_assoc1(S, acc, (size_t)idx, v);
            p = p->as.cons.cdr->as.cons.cdr;
        }
        return acc;
    }
    if (coll == NULL || mino_type_of(coll) == MINO_NIL || mino_type_of(coll) == MINO_MAP) {
        return map_assoc_pairs(S, coll, args->as.cons.cdr, extra_pairs);
    }
    if (mino_type_of(coll) == MINO_SORTED_MAP) {
        mino_val *acc = coll;
        p = args->as.cons.cdr;
        for (i = 0; i < extra_pairs; i++) {
            acc = sorted_map_assoc1(S, acc, p->as.cons.car,
                                    p->as.cons.cdr->as.cons.car);
            p = p->as.cons.cdr->as.cons.cdr;
        }
        return acc;
    }
    if (mino_type_of(coll) == MINO_RECORD) {
        /* Each pair: declared field => copy vals, update slot. Ext
         * key => share vals, update ext map (allocating one if
         * needed). The new record keeps its type, so (record? r')
         * stays true and dispatch on (type r') is unchanged. */
        mino_val *acc = coll;
        p = args->as.cons.cdr;
        for (i = 0; i < extra_pairs; i++) {
            mino_val *k = p->as.cons.car;
            mino_val *v = p->as.cons.cdr->as.cons.car;
            int         idx;
            mino_val *new_rec;
            mino_val *fields = acc->as.record.type->as.record_type.fields;
            size_t      n_fields = (fields != NULL) ? fields->as.vec.len : 0;
            idx = record_field_index(acc, k);
            new_rec = alloc_val(S, MINO_RECORD);
            if (new_rec == NULL) {
                return prim_throw_classified(S, "internal", "MIN001",
                    "assoc: failed to allocate record");
            }
            new_rec->as.record.type = acc->as.record.type;
            if (n_fields > 0) {
                size_t j;
                mino_val **slots = (mino_val **)malloc(
                    n_fields * sizeof(*slots));
                if (slots == NULL) {
                    return prim_throw_classified(S, "internal", "MIN001",
                        "assoc: out of memory");
                }
                for (j = 0; j < n_fields; j++) slots[j] = acc->as.record.vals[j];
                if (idx >= 0) slots[idx] = v;
                new_rec->as.record.vals = slots;
            }
            if (idx < 0) {
                /* Ext key: build/extend the ext map. */
                mino_val *ext = acc->as.record.ext;
                mino_val *new_ext;
                mino_val *kv_args = mino_cons(S, k,
                                       mino_cons(S, v, mino_nil(S)));
                if (ext == NULL) {
                    new_ext = mino_map(S, NULL, NULL, 0);
                } else {
                    new_ext = ext;
                }
                new_ext = map_assoc_pairs(S, new_ext, kv_args, 1);
                new_rec->as.record.ext = new_ext;
            } else {
                new_rec->as.record.ext = acc->as.record.ext;
            }
            new_rec->meta = acc->meta;
            acc = new_rec;
            p = p->as.cons.cdr->as.cons.cdr;
        }
        return acc;
    }
    {
        char msg[96];
        snprintf(msg, sizeof(msg), "assoc: expected a map or vector, got %s",
                 type_tag_str(coll));
        return prim_throw_classified(S, "eval/type", "MTY001", msg);
    }
}

mino_val *prim_get(mino_state *S, mino_val *args, mino_env *env)
{
    mino_val *coll;
    mino_val *key;
    mino_val *def_val = mino_nil(S);
    size_t      n;
    (void)env;
    arg_count(S, args, &n);
    if (n != 2 && n != 3) {
        return prim_throw_classified(S, "eval/arity", "MAR001", "get requires 2 or 3 arguments");
    }
    coll = args->as.cons.car;
    key  = args->as.cons.cdr->as.cons.car;
    if (n == 3) {
        def_val = args->as.cons.cdr->as.cons.cdr->as.cons.car;
    }
    if (coll == NULL || mino_type_of(coll) == MINO_NIL) {
        return def_val;
    }
    if (mino_type_of(coll) == MINO_TRANSIENT) {
        if (!coll->as.transient.valid)
            return prim_throw_classified(S, "eval/state", "MST001",
                "get: transient is no longer valid");
        coll = coll->as.transient.current;
        if (coll == NULL || mino_type_of(coll) == MINO_NIL) return def_val;
    }
    if (mino_type_of(coll) == MINO_MAP) {
        mino_val *v = map_get_val(coll, key);
        return v == NULL ? def_val : v;
    }
    if (mino_type_of(coll) == MINO_VECTOR) {
        long long idx;
        if (key == NULL || !mino_val_int_p(key)) {
            return def_val;
        }
        idx = mino_val_int_get(key);
        if (idx < 0 || (size_t)idx >= coll->as.vec.len) {
            return def_val;
        }
        return vec_nth(coll, (size_t)idx);
    }
    if (mino_type_of(coll) == MINO_HOST_ARRAY) {
        long long idx;
        if (key == NULL || !mino_val_int_p(key)) return def_val;
        idx = mino_val_int_get(key);
        if (idx < 0 || (size_t)idx >= coll->as.host_array.len) return def_val;
        return coll->as.host_array.vals[(size_t)idx];
    }
    if (mino_type_of(coll) == MINO_BYTES) {
        long long idx;
        if (key == NULL || !mino_val_int_p(key)) return def_val;
        idx = mino_val_int_get(key);
        if (idx < 0 || (size_t)idx >= coll->as.bytes.byte_len) return def_val;
        return mino_int(S,
            (long long)(unsigned)coll->as.bytes.data[(size_t)idx]);
    }
    if (mino_type_of(coll) == MINO_MAP_ENTRY) {
        if (key != NULL && mino_val_int_p(key)) {
            if (mino_val_int_get(key) == 0) return coll->as.map_entry.k;
            if (mino_val_int_get(key) == 1) return coll->as.map_entry.v;
        }
        return def_val;
    }
    if (mino_type_of(coll) == MINO_SET) {
        uint32_t h = hash_val(key);
        mino_val *found = hamt_get(coll->as.set.root, key, h, 0u);
        return found != NULL ? key : def_val;
    }
    if (mino_type_of(coll) == MINO_SORTED_MAP) {
        mino_val *v = rb_get(S, coll->as.sorted.root, key, coll->as.sorted.comparator);
        return v == NULL ? def_val : v;
    }
    if (mino_type_of(coll) == MINO_SORTED_SET) {
        return rb_contains(S, coll->as.sorted.root, key, coll->as.sorted.comparator)
            ? key : def_val;
    }
    if (mino_type_of(coll) == MINO_STRING) {
        /* Match Clojure: indexing a string returns a `\char`. The
         * walk is codepoint-counted so multi-byte characters count as
         * one position. ASCII content is unaffected since the
         * codepoint walk is byte-equivalent there. */
        long long idx;
        size_t pos = 0;
        long long seen = 0;
        unsigned int cp;
        size_t step;
        if (key == NULL || !mino_val_int_p(key)) {
            return def_val;
        }
        idx = mino_val_int_get(key);
        if (idx < 0) return def_val;
        while (pos < coll->as.s.len) {
            const unsigned char *p = (const unsigned char *)coll->as.s.data + pos;
            if (*p < 0x80) { cp = *p; step = 1; }
            else if ((*p & 0xE0) == 0xC0 && pos + 1 < coll->as.s.len) {
                cp = ((unsigned)(*p & 0x1F) << 6) | (p[1] & 0x3F);
                step = 2;
            } else if ((*p & 0xF0) == 0xE0 && pos + 2 < coll->as.s.len) {
                cp = ((unsigned)(*p & 0x0F) << 12)
                   | ((unsigned)(p[1] & 0x3F) << 6)
                   | (p[2] & 0x3F);
                step = 3;
            } else if ((*p & 0xF8) == 0xF0 && pos + 3 < coll->as.s.len) {
                cp = ((unsigned)(*p & 0x07) << 18)
                   | ((unsigned)(p[1] & 0x3F) << 12)
                   | ((unsigned)(p[2] & 0x3F) << 6)
                   | (p[3] & 0x3F);
                step = 4;
            } else {
                cp = *p; step = 1;
            }
            if (seen == idx) return mino_char(S, cp);
            seen++;
            pos += step;
        }
        return def_val;
    }
    if (mino_type_of(coll) == MINO_RECORD) {
        int idx = record_field_index(coll, key);
        if (idx >= 0) return coll->as.record.vals[idx];
        if (coll->as.record.ext != NULL) {
            mino_val *v = map_get_val(coll->as.record.ext, key);
            if (v != NULL) return v;
        }
        return def_val;
    }
    return def_val;
}

/* Per-collection-kind conj implementations. `items` is a cons-list of
 * arguments to prepend / append, in left-to-right argument order. */

static mino_val *conj_list(mino_state *S, mino_val *coll,
                             mino_val *items)
{
    /* List/nil/empty-list: prepend each item so
     * (conj '(1 2) 3 4) => (4 3 1 2). */
    mino_val *out = (coll == NULL || mino_type_of(coll) == MINO_NIL
                       || mino_type_of(coll) == MINO_EMPTY_LIST)
        ? mino_nil(S) : coll;
    while (mino_is_cons(items)) {
        out = mino_cons(S, items->as.cons.car, out);
        items = items->as.cons.cdr;
    }
    if (out == NULL || mino_type_of(out) == MINO_NIL) return mino_empty_list(S);
    return out;
}

static mino_val *conj_vector(mino_state *S, mino_val *coll,
                               mino_val *items)
{
    mino_val *acc = coll;
    while (mino_is_cons(items)) {
        acc = vec_conj1(S, acc, items->as.cons.car);
        items = items->as.cons.cdr;
    }
    return acc;
}

static mino_val *conj_map_entry(mino_state *S, mino_val *coll,
                                  mino_val *items)
{
    /* (conj entry x ...) is JVM-equivalent to (conj [k v] x ...).
     * Promote to a 2-vector then fall through to vector conj. */
    mino_val *kv[2];
    kv[0] = coll->as.map_entry.k;
    kv[1] = coll->as.map_entry.v;
    return conj_vector(S, mino_vector(S, kv, 2), items);
}

static mino_val *conj_map(mino_state *S, mino_val *coll,
                            mino_val *items)
{
    /* Each added item must be a 2-element vector [k v], a map
     * (entries are merged), a map-entry, or nil (no-op, per Clojure's
     * (conj coll nil) returning coll unchanged). Assoc each onto the
     * accumulator so successor maps share structure with the source. */
    mino_val *acc = coll;
    while (mino_is_cons(items)) {
        mino_val *item = items->as.cons.car;
        if (item == NULL || mino_type_of(item) == MINO_NIL) {
            items = items->as.cons.cdr;
            continue;
        }
        if (mino_type_of(item) == MINO_MAP) {
            /* Merge map entries: (conj {:a 0} {:b 1}) */
            size_t j;
            for (j = 0; j < item->as.map.len; j++) {
                mino_val *k = vec_nth(item->as.map.key_order, j);
                mino_val *v = map_get_val(item, k);
                mino_val *pair_args =
                    mino_cons(S, k, mino_cons(S, v, mino_nil(S)));
                acc = map_assoc_pairs(S, acc, pair_args, 1);
            }
        } else if (mino_type_of(item) == MINO_VECTOR
                   && item->as.vec.len == 2) {
            mino_val *pair_args =
                mino_cons(S, vec_nth(item, 0),
                    mino_cons(S, vec_nth(item, 1), mino_nil(S)));
            acc = map_assoc_pairs(S, acc, pair_args, 1);
        } else if (mino_type_of(item) == MINO_MAP_ENTRY) {
            mino_val *pair_args =
                mino_cons(S, item->as.map_entry.k,
                    mino_cons(S, item->as.map_entry.v, mino_nil(S)));
            acc = map_assoc_pairs(S, acc, pair_args, 1);
        } else {
            return prim_throw_classified(S, "eval/type", "MTY001",
                "conj on map requires map entries or 2-element vectors");
        }
        items = items->as.cons.cdr;
    }
    return acc;
}

static mino_val *conj_set(mino_state *S, mino_val *coll,
                            mino_val *items)
{
    mino_val *acc = coll;
    while (mino_is_cons(items)) {
        acc = set_conj1(S, acc, items->as.cons.car);
        items = items->as.cons.cdr;
    }
    return acc;
}

static mino_val *conj_sorted_map(mino_state *S, mino_val *coll,
                                   mino_val *items)
{
    mino_val *v = coll;
    while (mino_is_cons(items)) {
        mino_val *item = items->as.cons.car;
        mino_val *ek, *ev;
        if (item != NULL && mino_type_of(item) == MINO_VECTOR
            && item->as.vec.len == 2) {
            ek = vec_nth(item, 0);
            ev = vec_nth(item, 1);
        } else if (item != NULL && mino_type_of(item) == MINO_MAP_ENTRY) {
            ek = item->as.map_entry.k;
            ev = item->as.map_entry.v;
        } else {
            return prim_throw_classified(S, "eval/type", "MTY001",
                "conj on sorted-map requires map entries or 2-element vectors");
        }
        v = sorted_map_assoc1(S, v, ek, ev);
        items = items->as.cons.cdr;
    }
    return v;
}

static mino_val *conj_sorted_set(mino_state *S, mino_val *coll,
                                   mino_val *items)
{
    mino_val *v = coll;
    while (mino_is_cons(items)) {
        v = sorted_set_conj1(S, v, items->as.cons.car);
        items = items->as.cons.cdr;
    }
    return v;
}

mino_val *prim_conj(mino_state *S, mino_val *args, mino_env *env)
{
    mino_val *coll;
    size_t      n;
    mino_val *items;
    mino_type kind;
    (void)env;
    arg_count(S, args, &n);
    if (n == 0) {
        /* (conj) => [] — identity element, matches Clojure. */
        return mino_vector(S, NULL, 0);
    }
    if (n < 2) {
        /* (conj coll) => coll — single arg returns the collection. */
        return args->as.cons.car;
    }
    coll  = args->as.cons.car;
    items = args->as.cons.cdr;
    kind  = (coll == NULL) ? MINO_NIL : mino_type_of(coll);
    switch (kind) {
    case MINO_NIL:
    case MINO_EMPTY_LIST:
    case MINO_CONS:
    case MINO_LAZY:
        return conj_list(S, coll, items);
    case MINO_VECTOR:
        return conj_vector(S, coll, items);
    case MINO_MAP_ENTRY:
        return conj_map_entry(S, coll, items);
    case MINO_MAP:
        return conj_map(S, coll, items);
    case MINO_SET:
        return conj_set(S, coll, items);
    case MINO_SORTED_MAP:
        return conj_sorted_map(S, coll, items);
    case MINO_SORTED_SET:
        return conj_sorted_set(S, coll, items);
    case MINO_QUEUE: {
        mino_val *q = coll;
        while (mino_is_cons(items)) {
            q = mino_queue_conj(S, q, items->as.cons.car);
            items = items->as.cons.cdr;
        }
        return q;
    }
    default: {
        char msg[96];
        snprintf(msg, sizeof(msg),
                 "conj: expected a list, vector, map, or set, got %s",
                 type_tag_str(coll));
        return prim_throw_classified(S, "eval/type", "MTY001", msg);
    }
    }
}

mino_val *prim_keys(mino_state *S, mino_val *args, mino_env *env)
{
    mino_val *coll;
    mino_val *head = mino_nil(S);
    mino_val *tail = NULL;
    size_t i;
    (void)env;
    if (!mino_is_cons(args) || mino_is_cons(args->as.cons.cdr)) {
        return prim_throw_classified(S, "eval/arity", "MAR001", "keys requires one argument");
    }
    coll = args->as.cons.car;
    if (coll == NULL || mino_type_of(coll) == MINO_NIL) {
        return mino_nil(S);
    }
    if (mino_type_of(coll) == MINO_SORTED_MAP) {
        rb_to_list(S, coll->as.sorted.root, &head, &tail);
        return head;
    }
    if (mino_type_of(coll) == MINO_EMPTY_LIST)                    return mino_nil(S);
    if (mino_type_of(coll) == MINO_VECTOR && coll->as.vec.len == 0) return mino_nil(S);
    if (mino_type_of(coll) == MINO_SET    && coll->as.set.len == 0) return mino_nil(S);
    if (mino_type_of(coll) == MINO_SORTED_SET)                      return mino_nil(S);
    if (mino_type_of(coll) == MINO_STRING && coll->as.s.len == 0) return mino_nil(S);
    if (mino_type_of(coll) == MINO_RECORD) {
        /* Declared field keywords first in declared order, then ext
         * keys in insertion order. */
        mino_val *fields = coll->as.record.type->as.record_type.fields;
        size_t n_fields = (fields != NULL) ? fields->as.vec.len : 0;
        for (i = 0; i < n_fields; i++) {
            mino_val *cell = mino_cons(S, vec_nth(fields, i), mino_nil(S));
            if (tail == NULL) head = cell;
            else mino_cons_cdr_set(S, tail, cell);
            tail = cell;
        }
        if (coll->as.record.ext != NULL) {
            const mino_val *e = coll->as.record.ext;
            size_t k;
            for (k = 0; k < e->as.map.len; k++) {
                mino_val *cell = mino_cons(S,
                    vec_nth(e->as.map.key_order, k), mino_nil(S));
                if (tail == NULL) head = cell;
                else mino_cons_cdr_set(S, tail, cell);
                tail = cell;
            }
        }
        return head;
    }
    if (mino_type_of(coll) != MINO_MAP) {
        /* JVM Clojure accepts any seqable whose elements are MapEntries
         * (or [k v] vectors), so `(keys (remove pred (frequencies m)))`
         * works on the seq of entries that filter/remove yield. */
        if (mino_type_of(coll) == MINO_CONS
            || mino_type_of(coll) == MINO_VECTOR
            || mino_type_of(coll) == MINO_LAZY) {
            seq_iter_t it;
            seq_iter_init(S, &it, coll);
            while (!seq_iter_done(&it)) {
                mino_val *entry = seq_iter_val(S, &it);
                mino_val *k;
                if (entry == NULL) {
                    return prim_throw_classified(
                        S, "eval/type", "MTY001",
                        "keys: seq element is not a map entry (expected [k v])");
                }
                if (mino_type_of(entry) == MINO_MAP_ENTRY) {
                    k = entry->as.map_entry.k;
                } else if (mino_type_of(entry) == MINO_VECTOR
                           && entry->as.vec.len == 2) {
                    k = vec_nth(entry, 0);
                } else {
                    return prim_throw_classified(
                        S, "eval/type", "MTY001",
                        "keys: seq element is not a map entry (expected [k v])");
                }
                {
                    mino_val *cell = mino_cons(S, k, mino_nil(S));
                    if (tail == NULL) head = cell;
                    else mino_cons_cdr_set(S, tail, cell);
                    tail = cell;
                }
                seq_iter_next(S, &it);
            }
            return head;
        }
        return prim_throw_classified(S, "eval/type", "MTY001", "keys: argument must be a map or seq of map entries");
    }
    for (i = 0; i < coll->as.map.len; i++) {
        mino_val *cell = mino_cons(S, vec_nth(coll->as.map.key_order, i),
                                      mino_nil(S));
        if (tail == NULL) {
            head = cell;
        } else {
            mino_cons_cdr_set(S, tail, cell);
        }
        tail = cell;
    }
    return head;
}

mino_val *prim_vals(mino_state *S, mino_val *args, mino_env *env)
{
    mino_val *coll;
    mino_val *head = mino_nil(S);
    mino_val *tail = NULL;
    size_t i;
    (void)env;
    if (!mino_is_cons(args) || mino_is_cons(args->as.cons.cdr)) {
        return prim_throw_classified(S, "eval/arity", "MAR001", "vals requires one argument");
    }
    coll = args->as.cons.car;
    if (coll == NULL || mino_type_of(coll) == MINO_NIL) {
        return mino_nil(S);
    }
    if (mino_type_of(coll) == MINO_SORTED_MAP) {
        mino_val *keys = mino_nil(S);
        mino_val *kt   = NULL;
        rb_to_list(S, coll->as.sorted.root, &keys, &kt);
        while (mino_is_cons(keys)) {
            mino_val *v = rb_get(S, coll->as.sorted.root, keys->as.cons.car,
                                   coll->as.sorted.comparator);
            mino_val *cell = mino_cons(S, v, mino_nil(S));
            if (tail == NULL) head = cell; else mino_cons_cdr_set(S, tail, cell);
            tail = cell;
            keys = keys->as.cons.cdr;
        }
        return head;
    }
    if (mino_type_of(coll) == MINO_EMPTY_LIST)                      return mino_nil(S);
    if (mino_type_of(coll) == MINO_VECTOR && coll->as.vec.len == 0) return mino_nil(S);
    if (mino_type_of(coll) == MINO_SET    && coll->as.set.len == 0) return mino_nil(S);
    if (mino_type_of(coll) == MINO_SORTED_SET)                      return mino_nil(S);
    if (mino_type_of(coll) == MINO_STRING && coll->as.s.len == 0)   return mino_nil(S);
    if (mino_type_of(coll) == MINO_RECORD) {
        mino_val *fields = coll->as.record.type->as.record_type.fields;
        size_t n_fields = (fields != NULL) ? fields->as.vec.len : 0;
        for (i = 0; i < n_fields; i++) {
            mino_val *cell = mino_cons(S, coll->as.record.vals[i],
                                          mino_nil(S));
            if (tail == NULL) head = cell;
            else mino_cons_cdr_set(S, tail, cell);
            tail = cell;
        }
        if (coll->as.record.ext != NULL) {
            const mino_val *e = coll->as.record.ext;
            size_t k;
            for (k = 0; k < e->as.map.len; k++) {
                mino_val *ek = vec_nth(e->as.map.key_order, k);
                mino_val *cell = mino_cons(S, map_get_val(e, ek),
                                              mino_nil(S));
                if (tail == NULL) head = cell;
                else mino_cons_cdr_set(S, tail, cell);
                tail = cell;
            }
        }
        return head;
    }
    if (mino_type_of(coll) != MINO_MAP) {
        if (mino_type_of(coll) == MINO_CONS
            || mino_type_of(coll) == MINO_VECTOR
            || mino_type_of(coll) == MINO_LAZY) {
            seq_iter_t it;
            seq_iter_init(S, &it, coll);
            while (!seq_iter_done(&it)) {
                mino_val *entry = seq_iter_val(S, &it);
                mino_val *v;
                if (entry == NULL) {
                    return prim_throw_classified(
                        S, "eval/type", "MTY001",
                        "vals: seq element is not a map entry (expected [k v])");
                }
                if (mino_type_of(entry) == MINO_MAP_ENTRY) {
                    v = entry->as.map_entry.v;
                } else if (mino_type_of(entry) == MINO_VECTOR
                           && entry->as.vec.len == 2) {
                    v = vec_nth(entry, 1);
                } else {
                    return prim_throw_classified(
                        S, "eval/type", "MTY001",
                        "vals: seq element is not a map entry (expected [k v])");
                }
                {
                    mino_val *cell = mino_cons(S, v, mino_nil(S));
                    if (tail == NULL) head = cell;
                    else mino_cons_cdr_set(S, tail, cell);
                    tail = cell;
                }
                seq_iter_next(S, &it);
            }
            return head;
        }
        return prim_throw_classified(S, "eval/type", "MTY001", "vals: argument must be a map or seq of map entries");
    }
    for (i = 0; i < coll->as.map.len; i++) {
        mino_val *key  = vec_nth(coll->as.map.key_order, i);
        mino_val *cell = mino_cons(S, map_get_val(coll, key), mino_nil(S));
        if (tail == NULL) {
            head = cell;
        } else {
            mino_cons_cdr_set(S, tail, cell);
        }
        tail = cell;
    }
    return head;
}

/* Set helper: add one element to a set, returning a new set. */
mino_val *set_conj1(mino_state *S, const mino_val *s, mino_val *elem)
{
    mino_val       *v;
    mino_val       *sentinel;
    hamt_entry_t     *e;
    uint32_t          h;
    int               replaced = 0;
    mino_hamt_node_t *root;
    /* Identity short-circuit: if elem is already in the set, return s
     * unchanged. Lets `(identical? s (conj s x))` hold for the
     * "already-present" idiom and saves the HAMT rebuild traffic. */
    if (s->as.set.len > 0
        && hamt_get(s->as.set.root, elem, hash_val(elem), 0u) != NULL) {
        return (mino_val *)s;
    }
    v        = alloc_val(S, MINO_SET);
    sentinel = mino_true(S);
    e        = hamt_entry_new(S, elem, sentinel);
    h        = hash_val(elem);
    root     = hamt_assoc(S, s->as.set.root, e, h, 0u, &replaced);
    v->as.set.root      = root;
    v->meta              = s->meta;
    if (replaced) {
        v->as.set.key_order = s->as.set.key_order;
        v->as.set.len       = s->as.set.len;
    } else {
        v->as.set.key_order = vec_conj1(S, s->as.set.key_order, elem);
        v->as.set.len       = s->as.set.len + 1;
    }
    return v;
}

/* Owner-tagged set conj. Mirrors set_conj1 but routes the HAMT
 * walk and the key_order conj through the owned variants so a
 * transient batch reuses spine nodes and tail-chunk slots in place
 * after the first touch. */
mino_val *set_conj1_owned(mino_state *S, mino_val *s, mino_val *elem,
                             uintptr_t owner)
{
    mino_val       *v;
    mino_val       *sentinel;
    hamt_entry_t     *e;
    uint32_t          h;
    int               replaced = 0;
    mino_hamt_node_t *root;
    if (s->as.set.len > 0
        && hamt_get(s->as.set.root, elem, hash_val(elem), 0u) != NULL) {
        return s;
    }
    v        = alloc_val(S, MINO_SET);
    sentinel = mino_true(S);
    e        = hamt_entry_new(S, elem, sentinel);
    if (e == NULL) return NULL;
    h        = hash_val(elem);
    root     = hamt_assoc_owned(S, s->as.set.root, e, h, 0u, &replaced, owner);
    if (root == NULL) return NULL;
    v->as.set.root      = root;
    v->meta             = s->meta;
    if (replaced) {
        v->as.set.key_order = s->as.set.key_order;
        v->as.set.len       = s->as.set.len;
    } else {
        v->as.set.key_order = vec_conj1_owned(S, s->as.set.key_order, elem,
                                                owner);
        v->as.set.len       = s->as.set.len + 1;
    }
    return v;
}

/* Owner-tagged set disj. */
mino_val *set_disj1_owned(mino_state *S, mino_val *s,
                             const mino_val *elem, uintptr_t owner)
{
    mino_val       *v;
    mino_hamt_node_t *root;
    mino_val       *order;
    int               removed = 0;
    size_t            i;
    if (s->as.set.len == 0) return s;
    root = hamt_dissoc_owned(S, s->as.set.root, elem, hash_val(elem), 0u,
                              &removed, owner);
    if (!removed) return s;
    order = mino_vector(S, NULL, 0);
    for (i = 0; i < s->as.set.len; i++) {
        mino_val *cur = vec_nth(s->as.set.key_order, i);
        if (mino_eq(cur, elem)) continue;
        order = vec_conj1_owned(S, order, cur, owner);
    }
    v = alloc_val(S, MINO_SET);
    v->as.set.root      = root;
    v->as.set.key_order = order;
    v->as.set.len       = s->as.set.len - 1;
    v->meta             = s->meta;
    return v;
}

mino_val *prim_hash_set(mino_state *S, mino_val *args, mino_env *env)
{
    size_t      n;
    size_t      i;
    mino_val **tmp;
    mino_val *p;
    (void)env;
    arg_count(S, args, &n);
    tmp = (mino_val **)gc_alloc_typed(S, GC_T_VALARR, (n > 0 ? n : 1) * sizeof(*tmp));
    p = args;
    for (i = 0; i < n; i++) {
        tmp[i] = p->as.cons.car;
        p = p->as.cons.cdr;
    }
    return mino_set(S, tmp, n);
}

mino_val *prim_contains_p(mino_state *S, mino_val *args, mino_env *env)
{
    mino_val *coll;
    mino_val *key;
    (void)env;
    if (!mino_is_cons(args) || !mino_is_cons(args->as.cons.cdr)
        || mino_is_cons(args->as.cons.cdr->as.cons.cdr)) {
        return prim_throw_classified(S, "eval/arity", "MAR001", "contains? requires two arguments");
    }
    coll = args->as.cons.car;
    key  = args->as.cons.cdr->as.cons.car;
    if (coll == NULL || mino_type_of(coll) == MINO_NIL) {
        return mino_false(S);
    }
    if (mino_type_of(coll) == MINO_TRANSIENT) {
        if (!coll->as.transient.valid)
            return prim_throw_classified(S, "eval/state", "MST001",
                "contains?: transient is no longer valid");
        coll = coll->as.transient.current;
        if (coll == NULL || mino_type_of(coll) == MINO_NIL) return mino_false(S);
    }
    if (mino_type_of(coll) == MINO_MAP) {
        return map_get_val(coll, key) != NULL ? mino_true(S) : mino_false(S);
    }
    if (mino_type_of(coll) == MINO_SET) {
        uint32_t h = hash_val(key);
        return hamt_get(coll->as.set.root, key, h, 0u) != NULL
            ? mino_true(S) : mino_false(S);
    }
    if (mino_type_of(coll) == MINO_SORTED_MAP || mino_type_of(coll) == MINO_SORTED_SET) {
        return rb_contains(S, coll->as.sorted.root, key, coll->as.sorted.comparator)
            ? mino_true(S) : mino_false(S);
    }
    if (mino_type_of(coll) == MINO_VECTOR) {
        /* For vectors, key is an index. */
        if (key != NULL && mino_val_int_p(key)) {
            long long idx = mino_val_int_get(key);
            return (idx >= 0 && (size_t)idx < coll->as.vec.len)
                ? mino_true(S) : mino_false(S);
        }
        return mino_false(S);
    }
    if (mino_type_of(coll) == MINO_STRING) {
        /* For strings, key must be an integer index. */
        if (key == NULL || mino_type_of(key) == MINO_NIL)
            return prim_throw_classified(S, "eval/type", "MTY001",
                "contains?: string key must be an integer");
        if (mino_val_int_p(key)) {
            long long idx = mino_val_int_get(key);
            return (idx >= 0 && (size_t)idx < coll->as.s.len)
                ? mino_true(S) : mino_false(S);
        }
        return prim_throw_classified(S, "eval/type", "MTY001",
            "contains?: string key must be an integer");
    }
    if (mino_type_of(coll) == MINO_RECORD) {
        if (record_field_index(coll, key) >= 0) return mino_true(S);
        if (coll->as.record.ext != NULL
            && map_get_val(coll->as.record.ext, key) != NULL) {
            return mino_true(S);
        }
        return mino_false(S);
    }
    {
        char msg[96];
        snprintf(msg, sizeof(msg),
                 "contains?: expected a map, set, vector, or string, got %s",
                 type_tag_str(coll));
        return prim_throw_classified(S, "eval/type", "MTY001", msg);
    }
}

mino_val *prim_disj(mino_state *S, mino_val *args, mino_env *env)
{
    mino_val *coll;
    mino_val *p;
    size_t      n;
    (void)env;
    arg_count(S, args, &n);
    if (n < 2) {
        return prim_throw_classified(S, "eval/arity", "MAR001", "disj requires a set and at least one key");
    }
    coll = args->as.cons.car;
    if (coll == NULL || mino_type_of(coll) == MINO_NIL) {
        return mino_nil(S);
    }
    if (mino_type_of(coll) == MINO_SORTED_SET) {
        p = args->as.cons.cdr;
        while (mino_is_cons(p)) {
            coll = sorted_set_disj1(S, coll, p->as.cons.car);
            p = p->as.cons.cdr;
        }
        return coll;
    }
    if (mino_type_of(coll) != MINO_SET) {
        return prim_throw_classified(S, "eval/type", "MTY001", "disj: first argument must be a set");
    }
    /* Rebuild set excluding the specified elements. Not the most efficient
     * approach, but keeps the code simple and correct. */
    p = args->as.cons.cdr;
    while (mino_is_cons(p)) {
        mino_val *key = p->as.cons.car;
        uint32_t    h   = hash_val(key);
        if (hamt_get(coll->as.set.root, key, h, 0u) != NULL) {
            /* Element exists; rebuild without it. */
            mino_val *new_set = alloc_val(S, MINO_SET);
            mino_val *order   = mino_vector(S, NULL, 0);
            mino_hamt_node_t *root = NULL;
            size_t i;
            size_t new_len = 0;
            for (i = 0; i < coll->as.set.len; i++) {
                mino_val *elem = vec_nth(coll->as.set.key_order, i);
                if (!mino_eq(elem, key)) {
                    hamt_entry_t *e2 = hamt_entry_new(S, elem, mino_true(S));
                    uint32_t h2 = hash_val(elem);
                    int rep = 0;
                    root = hamt_assoc(S, root, e2, h2, 0u, &rep);
                    order = vec_conj1(S, order, elem);
                    new_len++;
                }
            }
            new_set->as.set.root      = root;
            new_set->as.set.key_order = order;
            new_set->as.set.len       = new_len;
            new_set->meta             = coll->meta;
            coll = new_set;
        }
        p = p->as.cons.cdr;
    }
    return coll;
}

mino_val *prim_dissoc(mino_state *S, mino_val *args, mino_env *env)
{
    mino_val *coll;
    mino_val *p;
    size_t      n;
    (void)env;
    arg_count(S, args, &n);
    if (n < 1) {
        return prim_throw_classified(S, "eval/arity", "MAR001", "dissoc requires at least one argument");
    }
    coll = args->as.cons.car;
    if (coll == NULL || mino_type_of(coll) == MINO_NIL) {
        return mino_nil(S);
    }
    /* (dissoc m) with no keys returns the map unchanged, per Clojure. */
    if (n == 1) {
        return coll;
    }
    if (mino_type_of(coll) == MINO_SORTED_MAP) {
        p = args->as.cons.cdr;
        while (mino_is_cons(p)) {
            coll = sorted_map_dissoc1(S, coll, p->as.cons.car);
            p = p->as.cons.cdr;
        }
        return coll;
    }
    if (mino_type_of(coll) == MINO_RECORD) {
        /* Record dissoc: dropping a declared field degrades to a
         * plain map (canonical Clojure behaviour); dropping an ext
         * key returns a new record with the ext map updated. Each
         * key is processed in order against the running accumulator,
         * which may flip from RECORD to MAP partway through. */
        mino_val *acc = coll;
        p = args->as.cons.cdr;
        while (mino_is_cons(p)) {
            mino_val *key = p->as.cons.car;
            if (mino_type_of(acc) == MINO_RECORD) {
                int idx = record_field_index(acc, key);
                if (idx >= 0) {
                    /* Degrade: build a plain map from declared fields
                     * (skipping the dissoc'd one) and the ext entries. */
                    mino_val *fields = acc->as.record.type
                        ->as.record_type.fields;
                    size_t n_fields = (fields != NULL) ? fields->as.vec.len : 0;
                    size_t cap = n_fields - 1;
                    size_t mi  = 0;
                    size_t j;
                    mino_val **mk;
                    mino_val **mv;
                    if (acc->as.record.ext != NULL) {
                        cap += acc->as.record.ext->as.map.len;
                    }
                    mk = (cap > 0) ? (mino_val **)gc_alloc_typed(S,
                        GC_T_VALARR, cap * sizeof(*mk)) : NULL;
                    mv = (cap > 0) ? (mino_val **)gc_alloc_typed(S,
                        GC_T_VALARR, cap * sizeof(*mv)) : NULL;
                    for (j = 0; j < n_fields; j++) {
                        if ((int)j == idx) continue;
                        mk[mi] = vec_nth(fields, j);
                        mv[mi] = acc->as.record.vals[j];
                        mi++;
                    }
                    if (acc->as.record.ext != NULL) {
                        const mino_val *e = acc->as.record.ext;
                        size_t k;
                        for (k = 0; k < e->as.map.len; k++) {
                            mino_val *ek = vec_nth(e->as.map.key_order, k);
                            mk[mi] = ek;
                            mv[mi] = map_get_val(e, ek);
                            mi++;
                        }
                    }
                    acc = mino_map(S, mk, mv, mi);
                } else if (acc->as.record.ext != NULL
                           && map_get_val(acc->as.record.ext, key) != NULL) {
                    /* Ext key drop: rebuild ext without it, keep
                     * record. */
                    const mino_val *e   = acc->as.record.ext;
                    size_t            len = e->as.map.len;
                    mino_val      **ek  = (mino_val **)gc_alloc_typed(S,
                        GC_T_VALARR, (len > 0 ? len : 1) * sizeof(*ek));
                    mino_val      **ev  = (mino_val **)gc_alloc_typed(S,
                        GC_T_VALARR, (len > 0 ? len : 1) * sizeof(*ev));
                    size_t  ext_n  = 0;
                    size_t  k, n_fields_acc;
                    mino_val  *new_rec, *new_ext;
                    mino_val **slots;
                    mino_val  *fields_acc;
                    for (k = 0; k < len; k++) {
                        mino_val *kk = vec_nth(e->as.map.key_order, k);
                        if (!mino_eq(kk, key)) {
                            ek[ext_n] = kk;
                            ev[ext_n] = map_get_val(e, kk);
                            ext_n++;
                        }
                    }
                    new_ext = (ext_n > 0)
                        ? mino_map(S, ek, ev, ext_n) : NULL;
                    new_rec = alloc_val(S, MINO_RECORD);
                    if (new_rec == NULL) {
                        return prim_throw_classified(S, "internal", "MIN001",
                            "dissoc: failed to allocate record");
                    }
                    new_rec->as.record.type = acc->as.record.type;
                    fields_acc = acc->as.record.type->as.record_type.fields;
                    n_fields_acc = (fields_acc != NULL)
                        ? fields_acc->as.vec.len : 0;
                    if (n_fields_acc > 0) {
                        slots = (mino_val **)malloc(
                            n_fields_acc * sizeof(*slots));
                        if (slots == NULL) {
                            return prim_throw_classified(S, "internal",
                                "MIN001", "dissoc: out of memory");
                        }
                        for (k = 0; k < n_fields_acc; k++) {
                            slots[k] = acc->as.record.vals[k];
                        }
                        new_rec->as.record.vals = slots;
                    }
                    new_rec->as.record.ext = new_ext;
                    new_rec->meta          = acc->meta;
                    acc = new_rec;
                }
                /* else: key not in declared fields and not in ext;
                 * dissoc is a no-op (matches map dissoc semantics). */
            } else {
                /* acc is already a plain map (degraded earlier); drop
                 * the key the normal way. */
                acc = mino_map_dissoc1(S, acc, key);
                if (acc == NULL) return NULL;
            }
            p = p->as.cons.cdr;
        }
        return acc;
    }
    if (mino_type_of(coll) != MINO_MAP) {
        return prim_throw_classified(S, "eval/type", "MTY001", "dissoc: first argument must be a map");
    }
    p = args->as.cons.cdr;
    while (mino_is_cons(p)) {
        mino_val *key = p->as.cons.car;
        coll = mino_map_dissoc1(S, coll, key);
        if (coll == NULL) return NULL;
        p = p->as.cons.cdr;
    }
    return coll;
}

/* Coerce any numeric tier (int, float, bigint, ratio, bigdec) to long
 * via JVM-style truncation. NaN -> 0, matching Java's (long) cast.
 * Returns 0 with *ok=0 if v is non-numeric. */
static long long subvec_to_long(const mino_val *v, int *ok)
{
    *ok = 1;
    if (v == NULL) { *ok = 0; return 0; }
    switch (mino_type_of(v)) {
    case MINO_INT:    return mino_val_int_get(v);
    case MINO_FLOAT:  return v->as.f != v->as.f ? 0 : (long long)v->as.f;
    case MINO_RATIO:  return (long long)mino_ratio_to_double(v);
    case MINO_BIGDEC: return (long long)mino_bigdec_to_double(v);
    case MINO_BIGINT: {
        long long ll;
        if (mino_as_ll(v, &ll)) return ll;
        return (long long)mino_bigint_to_double(v);
    }
    default: *ok = 0; return 0;
    }
}

mino_val *prim_subvec(mino_state *S, mino_val *args, mino_env *env)
{
    mino_val *v;
    long long   start, end;
    size_t      nargs = 0;
    mino_val *p;
    int         ok;
    (void)env;
    for (p = args; mino_is_cons(p); p = p->as.cons.cdr) nargs++;
    if (nargs < 2 || nargs > 3) {
        return prim_throw_classified(S, "eval/arity", "MAR001", "subvec requires 2 or 3 arguments");
    }
    v = args->as.cons.car;
    if (v == NULL || mino_type_of(v) != MINO_VECTOR) {
        return prim_throw_classified(S, "eval/type", "MTY001", "subvec: first argument must be a vector");
    }
    start = subvec_to_long(args->as.cons.cdr->as.cons.car, &ok);
    if (!ok) {
        return prim_throw_classified(S, "eval/type", "MTY001", "subvec: start must be a number");
    }
    if (nargs == 3) {
        end = subvec_to_long(args->as.cons.cdr->as.cons.cdr->as.cons.car, &ok);
        if (!ok) {
            return prim_throw_classified(S, "eval/type", "MTY001", "subvec: end must be a number");
        }
    } else {
        end = (long long)v->as.vec.len;
    }
    if (start < 0 || end < start || (size_t)end > v->as.vec.len) {
        return prim_throw_classified(S, "eval/bounds", "MBD001", "subvec: index out of bounds");
    }
    return vec_subvec(S, v, (size_t)start, (size_t)end);
}


mino_val *prim_list(mino_state *S, mino_val *args, mino_env *env)
{
    (void)env;
    if (args == NULL || !mino_is_cons(args)) return mino_empty_list(S);
    return args;
}

const mino_prim_def k_prims_collections[] = {
    {"car",      prim_car,
     "Returns the first element of a cons cell."},
    {"cdr",      prim_cdr,
     "Returns the rest of a cons cell."},
    {"cons",     prim_cons,
     "Returns a new list with x prepended to coll.", prim_cons_argv},
    {"count",    prim_count,
     "Returns the number of items in a collection.", prim_count_argv},
    {"nth",      prim_nth,
     "Returns the item at index n in a collection."},
    {"first",    prim_first,
     "Returns the first item in a collection, or nil if empty.",
     prim_first_argv},
    {"rest",     prim_rest,
     "Returns all but the first item in a collection.", prim_rest_argv},
    {"list",     prim_list,
     "Returns a list of the supplied arguments; () with no args."},
    {"vector",   prim_vector,
     "Returns a new vector containing the arguments."},
    {"hash-map", prim_hash_map,
     "Returns a new hash map with the given key-value pairs."},
    {"assoc",    prim_assoc,
     "Returns a new map with the given key-value pairs added."},
    {"get",      prim_get,
     "Returns the value mapped to key in a collection, or not-found."},
    {"conj",     prim_conj,
     "Returns a new collection with items added."},
    {"-empty-queue", prim_empty_queue,
     "Internal: return an empty PersistentQueue. Public surface is "
     "clojure.lang.PersistentQueue/EMPTY (a bound var) plus conj."},
    {"queue?",   prim_queue_p,
     "Returns true if x is a PersistentQueue."},
    {"keys",     prim_keys,
     "Returns a sequence of the keys in a map."},
    {"vals",     prim_vals,
     "Returns a sequence of the values in a map."},
    {"hash-set", prim_hash_set,
     "Returns a new hash set containing the arguments."},
    {"contains?", prim_contains_p,
     "Returns true if the collection contains the key."},
    {"disj",     prim_disj,
     "Returns a set with the given keys removed."},
    {"dissoc",   prim_dissoc,
     "Returns a map with the given keys removed."},
    {"subvec",   prim_subvec,
     "Returns a subvector from start (inclusive) to end (exclusive)."},
    {"transient",   prim_transient,
     "Returns a transient view of coll for batch mutation."},
    {"persistent!", prim_persistent_bang,
     "Seals a transient and returns its persistent collection."},
    {"assoc!",      prim_assoc_bang,
     "Associates key with val in a transient map or vector."},
    {"conj!",       prim_conj_bang,
     "Conjoins val onto a transient vector, map, or set."},
    {"dissoc!",     prim_dissoc_bang,
     "Removes key from a transient map."},
    {"disj!",       prim_disj_bang,
     "Removes key from a transient set."},
    {"pop!",        prim_pop_bang,
     "Removes the last element from a transient vector."},
    {"transient?",  prim_transient_p,
     "Returns true if x is a transient."},
    {"object-array", prim_object_array,
     "Creates a host-style Object array. With a non-negative integer, "
     "returns an array of that length filled with nil. With a "
     "collection, returns an array of its elements. Distinct from "
     "vector: vector? / coll? / counted? / sequential? / associative? "
     "all return false on the result, matching JVM Java arrays."},
    {"int-array",  prim_int_array,
     "Creates a host-style int array. Zero-fills on size argument; "
     "copies elements from a collection."},
    {"long-array", prim_long_array,
     "Creates a host-style long array. Zero-fills on size argument; "
     "copies elements from a collection."},
    {"short-array", prim_short_array,
     "Creates a host-style short array. Zero-fills on size argument; "
     "copies elements from a collection."},
    {"byte-array", prim_byte_array,
     "Creates a host-style byte array. Zero-fills on size argument; "
     "copies elements from a collection."},
    {"float-array", prim_float_array,
     "Creates a host-style float array. Zero-fills (0.0) on size; "
     "copies elements from a collection."},
    {"double-array", prim_double_array,
     "Creates a host-style double array. Zero-fills (0.0) on size; "
     "copies elements from a collection."},
    {"char-array", prim_char_array,
     "Creates a host-style char array. Nul-fills on size argument; "
     "copies elements from a collection."},
    {"boolean-array", prim_boolean_array,
     "Creates a host-style boolean array. Fills with false on size; "
     "copies elements from a collection."},
    {"to-array",   prim_to_array,
     "Converts a collection to an Object array (host-style)."},
    {"map-entry",  prim_map_entry,
     "Constructs a (k, v) map entry. Distinct from a 2-vector: "
     "key/val accept only map entries, not plain vectors. Equality "
     "with [k v] still compares element-wise."},
    {"aset",       prim_aset,
     "Mutates the host array at index, storing val. Returns val. The "
     "host-array tier is the only path that exposes in-place mutation "
     "outside MINO_ATOM / MINO_VOLATILE. Throws :mino/state on a "
     "MINO_BYTES value -- the immutable bytes tier rejects in-place "
     "writes."},
    {"aget",       prim_aget,
     "Reads slot `index` from a host array or a bytes value. On a "
     "bytes value, returns the byte at that index as an unsigned int "
     "(0..255)."},
    {"alength",    prim_alength,
     "Returns the slot count of a host array or the byte length of a "
     "bytes value."},
};

const size_t k_prims_collections_count =
    sizeof(k_prims_collections) / sizeof(k_prims_collections[0]);
