/*
 * collections.c -- collection primitives: car, cdr, cons, count,
 *                       nth, first, rest, vector, hash-map, assoc, get,
 *                       conj, keys, vals, hash-set, contains?, disj,
 *                       dissoc, seq, realized?, val_to_seq, set_conj1.
 */

#include "prim/internal.h"

mino_val_t *prim_car(mino_state_t *S, mino_val_t *args, mino_env_t *env)
{
    (void)env;
    if (!mino_is_cons(args)) {
        return prim_throw_classified(S, "eval/arity", "MAR001", "car requires one argument");
    }
    return mino_car(args->as.cons.car);
}

mino_val_t *prim_cdr(mino_state_t *S, mino_val_t *args, mino_env_t *env)
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

mino_val_t *val_to_seq(mino_state_t *S, mino_val_t *v)
{
    mino_val_t *head;
    mino_val_t *tail;
    size_t i;

    if (v == NULL || v->type == MINO_NIL) return mino_nil(S);
    /* The empty-list singleton is user-visible only. As a cons cdr it
     * collapses back to nil so cons-chain walkers (mino_is_cons-based
     * traversal) terminate without an extra case. */
    if (v->type == MINO_EMPTY_LIST) return mino_nil(S);
    if (v->type == MINO_CONS) return v;
    /* Lazy seqs are valid as the cdr of a cons cell; do not force them
     * here to avoid infinite recursion with self-referential sequences
     * like (repeat x). */
    if (v->type == MINO_LAZY) return v;
    if (v->type == MINO_VECTOR) {
        if (v->as.vec.len == 0) return mino_nil(S);
        head = mino_nil(S);
        tail = NULL;
        for (i = 0; i < v->as.vec.len; i++) {
            mino_val_t *cell = mino_cons(S, vec_nth(v, i), mino_nil(S));
            if (tail == NULL) head = cell;
            else mino_cons_cdr_set(S, tail, cell);
            tail = cell;
        }
        return head;
    }
    if (v->type == MINO_MAP) {
        if (v->as.map.len == 0) return mino_nil(S);
        head = mino_nil(S);
        tail = NULL;
        for (i = 0; i < v->as.map.len; i++) {
            mino_val_t *key = vec_nth(v->as.map.key_order, i);
            mino_val_t *val = map_get_val(v, key);
            mino_val_t *kv[2];
            mino_val_t *cell;
            kv[0] = key; kv[1] = val;
            cell = mino_cons(S, mino_vector(S, kv, 2), mino_nil(S));
            if (tail == NULL) head = cell;
            else mino_cons_cdr_set(S, tail, cell);
            tail = cell;
        }
        return head;
    }
    if (v->type == MINO_SET) {
        if (v->as.set.len == 0) return mino_nil(S);
        head = mino_nil(S);
        tail = NULL;
        for (i = 0; i < v->as.set.len; i++) {
            mino_val_t *elem = vec_nth(v->as.set.key_order, i);
            mino_val_t *cell = mino_cons(S, elem, mino_nil(S));
            if (tail == NULL) head = cell;
            else mino_cons_cdr_set(S, tail, cell);
            tail = cell;
        }
        return head;
    }
    if (v->type == MINO_SORTED_MAP || v->type == MINO_SORTED_SET) {
        return sorted_seq(S, v);
    }
    if (v->type == MINO_STRING) {
        if (v->as.s.len == 0) return mino_nil(S);
        head = mino_nil(S);
        tail = NULL;
        for (i = 0; i < v->as.s.len; i++) {
            mino_val_t *ch = mino_string_n(S, v->as.s.data + i, 1);
            mino_val_t *cell = mino_cons(S, ch, mino_nil(S));
            if (tail == NULL) head = cell;
            else mino_cons_cdr_set(S, tail, cell);
            tail = cell;
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

mino_val_t *prim_cons(mino_state_t *S, mino_val_t *args, mino_env_t *env)
{
    mino_val_t *cdr;
    (void)env;
    if (!mino_is_cons(args) || !mino_is_cons(args->as.cons.cdr)) {
        return prim_throw_classified(S, "eval/arity", "MAR001", "cons requires two arguments");
    }
    cdr = val_to_seq(S, args->as.cons.cdr->as.cons.car);
    if (cdr == NULL) return NULL;
    return mino_cons(S, args->as.cons.car, cdr);
}

/* ------------------------------------------------------------------------- */
/* Collection primitives                                                     */
/*                                                                           */
/* All collection ops treat values as immutable: every operation that        */
/* "modifies" a collection returns a freshly allocated value. v0.3 uses      */
/* naïve array-backed representations; persistent tries arrive in v0.4/v0.5 */
/* without changing the public primitive contracts.                          */
/* ------------------------------------------------------------------------- */

size_t list_length(mino_state_t *S, mino_val_t *list)
{
    size_t n = 0;
    while (list != NULL && list->type == MINO_LAZY) {
        list = lazy_force(S, list);
    }
    while (mino_is_cons(list)) {
        n++;
        list = list->as.cons.cdr;
        /* Force lazy tails. */
        while (list != NULL && list->type == MINO_LAZY) {
            list = lazy_force(S, list);
        }
    }
    return n;
}

int arg_count(mino_state_t *S, mino_val_t *args, size_t *out)
{
    *out = list_length(S, args);
    return 1;
}

mino_val_t *prim_count(mino_state_t *S, mino_val_t *args, mino_env_t *env)
{
    mino_val_t *coll;
    (void)env;
    if (!mino_is_cons(args) || mino_is_cons(args->as.cons.cdr)) {
        return prim_throw_classified(S, "eval/arity", "MAR001", "count requires one argument");
    }
    coll = args->as.cons.car;
    if (coll == NULL || coll->type == MINO_NIL) {
        return mino_int(S, 0);
    }
    switch (coll->type) {
    case MINO_EMPTY_LIST: return mino_int(S, 0);
    case MINO_CONS:   return mino_int(S, (long long)list_length(S, coll));
    case MINO_VECTOR: return mino_int(S, (long long)coll->as.vec.len);
    case MINO_MAP:    return mino_int(S, (long long)coll->as.map.len);
    case MINO_SET:    return mino_int(S, (long long)coll->as.set.len);
    case MINO_SORTED_MAP:
    case MINO_SORTED_SET: return mino_int(S, (long long)coll->as.sorted.len);
    case MINO_STRING: return mino_int(S, (long long)coll->as.s.len);
    case MINO_RECORD: {
        mino_val_t *fields = coll->as.record.type->as.record_type.fields;
        size_t n = (fields != NULL) ? fields->as.vec.len : 0;
        if (coll->as.record.ext != NULL) n += coll->as.record.ext->as.map.len;
        return mino_int(S, (long long)n);
    }
    case MINO_LAZY: {
        /* Force the entire lazy seq and count it. */
        mino_val_t *forced = lazy_force(S, coll);
        if (forced == NULL) return NULL;
        if (forced->type == MINO_NIL) return mino_int(S, 0);
        return mino_int(S, (long long)list_length(S, forced));
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

mino_val_t *prim_vector(mino_state_t *S, mino_val_t *args, mino_env_t *env)
{
    size_t n;
    size_t i;
    mino_val_t **tmp;
    mino_val_t *p;
    (void)env;
    arg_count(S, args, &n);
    if (n == 0) {
        return mino_vector(S, NULL, 0);
    }
    /* GC_T_VALARR keeps partially-gathered pointers visible to the collector;
     * without this, the optimizer may drop the `args` parameter and the cons
     * cells holding the element values become unreachable mid-construction. */
    tmp = (mino_val_t **)gc_alloc_typed(S, GC_T_VALARR, n * sizeof(*tmp));
    p = args;
    for (i = 0; i < n; i++) {
        tmp[i] = p->as.cons.car;
        p = p->as.cons.cdr;
    }
    return mino_vector(S, tmp, n);
}

mino_val_t *prim_hash_map(mino_state_t *S, mino_val_t *args, mino_env_t *env)
{
    size_t n;
    size_t pairs;
    size_t i;
    mino_val_t **ks;
    mino_val_t **vs;
    mino_val_t *p;
    (void)env;
    arg_count(S, args, &n);
    if (n % 2 != 0) {
        return prim_throw_classified(S, "eval/arity", "MAR001", "hash-map requires an even number of arguments");
    }
    if (n == 0) {
        return mino_map(S, NULL, NULL, 0);
    }
    pairs = n / 2;
    ks = (mino_val_t **)gc_alloc_typed(S, GC_T_VALARR, pairs * sizeof(*ks));
    vs = (mino_val_t **)gc_alloc_typed(S, GC_T_VALARR, pairs * sizeof(*vs));
    p = args;
    for (i = 0; i < pairs; i++) {
        ks[i] = p->as.cons.car;
        p = p->as.cons.cdr;
        vs[i] = p->as.cons.car;
        p = p->as.cons.cdr;
    }
    return mino_map(S, ks, vs, pairs);
}

mino_val_t *prim_nth(mino_state_t *S, mino_val_t *args, mino_env_t *env)
{
    mino_val_t *coll;
    mino_val_t *idx_val;
    mino_val_t *def_val = NULL;
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
    if (idx_val == NULL || idx_val->type != MINO_INT) {
        return prim_throw_classified(S, "eval/type", "MTY001", "nth index must be an integer");
    }
    idx = idx_val->as.i;
    if (idx < 0) {
        if (def_val != NULL) return def_val;
        return prim_throw_classified(S, "eval/bounds", "MBD001", "nth index out of range");
    }
    if (coll == NULL || coll->type == MINO_NIL) {
        if (def_val != NULL) return def_val;
        return prim_throw_classified(S, "eval/bounds", "MBD001", "nth index out of range");
    }
    if (coll->type == MINO_LAZY) {
        coll = lazy_force(S, coll);
        if (coll == NULL) return NULL;
        if (coll->type == MINO_NIL) {
            if (def_val != NULL) return def_val;
            return prim_throw_classified(S, "eval/bounds", "MBD001", "nth index out of range");
        }
    }
    if (coll->type == MINO_VECTOR) {
        if ((size_t)idx >= coll->as.vec.len) {
            if (def_val != NULL) return def_val;
            return prim_throw_classified(S, "eval/bounds", "MBD001", "nth index out of range");
        }
        return vec_nth(coll, (size_t)idx);
    }
    if (coll->type == MINO_CONS) {
        mino_val_t *p = coll;
        long long   i;
        for (i = 0; i < idx; i++) {
            if (!mino_is_cons(p)) {
                if (def_val != NULL) return def_val;
                return prim_throw_classified(S, "eval/bounds", "MBD001", "nth index out of range");
            }
            p = p->as.cons.cdr;
            if (p != NULL && p->type == MINO_LAZY) {
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
    if (coll->type == MINO_STRING) {
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

mino_val_t *prim_first(mino_state_t *S, mino_val_t *args, mino_env_t *env)
{
    mino_val_t *coll;
    (void)env;
    if (!mino_is_cons(args) || mino_is_cons(args->as.cons.cdr)) {
        return prim_throw_classified(S, "eval/arity", "MAR001", "first requires one argument");
    }
    coll = args->as.cons.car;
    if (coll == NULL || coll->type == MINO_NIL) {
        return mino_nil(S);
    }
    if (coll->type == MINO_EMPTY_LIST) {
        return mino_nil(S);
    }
    if (coll->type == MINO_CONS) {
        return coll->as.cons.car;
    }
    if (coll->type == MINO_VECTOR) {
        if (coll->as.vec.len == 0) {
            return mino_nil(S);
        }
        return vec_nth(coll, 0);
    }
    if (coll->type == MINO_LAZY) {
        mino_val_t *s = lazy_force(S, coll);
        if (s == NULL) return NULL;
        if (s->type == MINO_NIL || s == NULL) return mino_nil(S);
        if (s->type == MINO_CONS) return s->as.cons.car;
        return mino_nil(S);
    }
    if (coll->type == MINO_STRING) {
        if (coll->as.s.len == 0) return mino_nil(S);
        return mino_string_n(S, coll->as.s.data, 1);
    }
    if (coll->type == MINO_MAP) {
        if (coll->as.map.len == 0) return mino_nil(S);
        {
            mino_val_t *key = vec_nth(coll->as.map.key_order, 0);
            mino_val_t *val = map_get_val(coll, key);
            mino_val_t *kv[2];
            kv[0] = key;
            kv[1] = val;
            return mino_vector(S, kv, 2);
        }
    }
    if (coll->type == MINO_SET) {
        if (coll->as.set.len == 0) return mino_nil(S);
        return vec_nth(coll->as.set.key_order, 0);
    }
    if (coll->type == MINO_SORTED_MAP || coll->type == MINO_SORTED_SET) {
        const mino_rb_node_t *n = coll->as.sorted.root;
        if (n == NULL) return mino_nil(S);
        while (n->left != NULL) n = n->left;
        if (coll->type == MINO_SORTED_MAP) {
            mino_val_t *kv[2]; kv[0] = n->key; kv[1] = n->val;
            return mino_vector(S, kv, 2);
        }
        return n->key;
    }
    {
        char msg[96];
        snprintf(msg, sizeof(msg), "first: expected a list or vector, got %s",
                 type_tag_str(coll));
        return prim_throw_classified(S, "eval/type", "MTY001", msg);
    }
}

/* Lazy rest thunks: each takes a cons(collection, int-index) as context. */
static mino_val_t *make_c_lazy(mino_state_t *S, mino_val_t *ctx,
                               mino_val_t *(*thunk)(mino_state_t *, mino_val_t *))
{
    mino_val_t *lz = alloc_val(S, MINO_LAZY);
    lz->as.lazy.body    = ctx;
    lz->as.lazy.c_thunk = thunk;
    return lz;
}

static mino_val_t *vec_rest_thunk(mino_state_t *S, mino_val_t *ctx)
{
    mino_val_t *vec = ctx->as.cons.car;
    size_t idx      = (size_t)ctx->as.cons.cdr->as.i;
    if (idx >= vec->as.vec.len) return mino_nil(S);
    return mino_cons(S, vec_nth(vec, idx),
        make_c_lazy(S, mino_cons(S, vec, mino_int(S, (long long)(idx + 1))),
                    vec_rest_thunk));
}

static mino_val_t *str_rest_thunk(mino_state_t *S, mino_val_t *ctx)
{
    mino_val_t *str = ctx->as.cons.car;
    size_t idx      = (size_t)ctx->as.cons.cdr->as.i;
    if (idx >= str->as.s.len) return mino_nil(S);
    return mino_cons(S, mino_string_n(S, str->as.s.data + idx, 1),
        make_c_lazy(S, mino_cons(S, str, mino_int(S, (long long)(idx + 1))),
                    str_rest_thunk));
}

static mino_val_t *map_rest_thunk(mino_state_t *S, mino_val_t *ctx)
{
    mino_val_t *m  = ctx->as.cons.car;
    size_t idx     = (size_t)ctx->as.cons.cdr->as.i;
    mino_val_t *kv[2];
    if (idx >= m->as.map.len) return mino_nil(S);
    kv[0] = vec_nth(m->as.map.key_order, idx);
    kv[1] = map_get_val(m, kv[0]);
    return mino_cons(S, mino_vector(S, kv, 2),
        make_c_lazy(S, mino_cons(S, m, mino_int(S, (long long)(idx + 1))),
                    map_rest_thunk));
}

static mino_val_t *set_rest_thunk(mino_state_t *S, mino_val_t *ctx)
{
    mino_val_t *s = ctx->as.cons.car;
    size_t idx    = (size_t)ctx->as.cons.cdr->as.i;
    if (idx >= s->as.set.len) return mino_nil(S);
    return mino_cons(S, vec_nth(s->as.set.key_order, idx),
        make_c_lazy(S, mino_cons(S, s, mino_int(S, (long long)(idx + 1))),
                    set_rest_thunk));
}

mino_val_t *prim_rest(mino_state_t *S, mino_val_t *args, mino_env_t *env)
{
    mino_val_t *coll;
    (void)env;
    if (!mino_is_cons(args) || mino_is_cons(args->as.cons.cdr)) {
        return prim_throw_classified(S, "eval/arity", "MAR001", "rest requires one argument");
    }
    coll = args->as.cons.car;
    /* User-visible empty rest is the empty-list singleton, not nil:
     * (rest '()) -> (), (rest nil) -> (), (rest '(1)) -> (). */
    if (coll == NULL || coll->type == MINO_NIL) {
        return mino_empty_list(S);
    }
    if (coll->type == MINO_EMPTY_LIST) {
        return mino_empty_list(S);
    }
    if (coll->type == MINO_CONS) {
        mino_val_t *cdr = coll->as.cons.cdr;
        if (cdr == NULL || cdr->type == MINO_NIL) return mino_empty_list(S);
        /* Lazy cdr stays lazy here so infinite seqs don't blow the
         * stack; the lazy-seq seam is handled at the next prim_rest /
         * prim_first / prim_seq call. */
        return cdr;
    }
    if (coll->type == MINO_VECTOR) {
        if (coll->as.vec.len <= 1) return mino_empty_list(S);
        return mino_cons(S, vec_nth(coll, 1),
            make_c_lazy(S, mino_cons(S, coll, mino_int(S, 2)),
                        vec_rest_thunk));
    }
    if (coll->type == MINO_LAZY) {
        mino_val_t *s = lazy_force(S, coll);
        if (s == NULL) return NULL;
        if (s->type == MINO_NIL) return mino_empty_list(S);
        if (s->type == MINO_EMPTY_LIST) return mino_empty_list(S);
        if (s->type == MINO_CONS) {
            mino_val_t *cdr = s->as.cons.cdr;
            if (cdr == NULL || cdr->type == MINO_NIL) return mino_empty_list(S);
            return cdr;
        }
        return mino_empty_list(S);
    }
    if (coll->type == MINO_STRING) {
        if (coll->as.s.len <= 1) return mino_empty_list(S);
        return mino_cons(S, mino_string_n(S, coll->as.s.data + 1, 1),
            make_c_lazy(S, mino_cons(S, coll, mino_int(S, 2)),
                        str_rest_thunk));
    }
    if (coll->type == MINO_MAP) {
        mino_val_t *kv[2];
        if (coll->as.map.len <= 1) return mino_empty_list(S);
        kv[0] = vec_nth(coll->as.map.key_order, 1);
        kv[1] = map_get_val(coll, kv[0]);
        return mino_cons(S, mino_vector(S, kv, 2),
            make_c_lazy(S, mino_cons(S, coll, mino_int(S, 2)),
                        map_rest_thunk));
    }
    if (coll->type == MINO_SET) {
        if (coll->as.set.len <= 1) return mino_empty_list(S);
        return mino_cons(S, vec_nth(coll->as.set.key_order, 1),
            make_c_lazy(S, mino_cons(S, coll, mino_int(S, 2)),
                        set_rest_thunk));
    }
    if (coll->type == MINO_SORTED_MAP || coll->type == MINO_SORTED_SET) {
        mino_val_t *r = sorted_rest(S, coll);
        if (r == NULL) return NULL;
        if (r->type == MINO_NIL) return mino_empty_list(S);
        return r;
    }
    {
        char msg[96];
        snprintf(msg, sizeof(msg), "rest: expected a list or vector, got %s",
                 type_tag_str(coll));
        return prim_throw_classified(S, "eval/type", "MTY001", msg);
    }
}

/* Layer n k/v pairs onto an existing map, returning a new map value that
 * shares structure with `coll`. Nil is treated as an empty map.
 *
 * Suppress GC during the rebuild: intermediate `root`/`order`/entry/slot
 * allocations live only on the C stack between iterations, and mino's GC
 * is precise over its own roots -- a minor or incremental-major step
 * mid-loop would reclaim them. Same pattern as mino_map. */
static mino_val_t *map_assoc_pairs(mino_state_t *S, mino_val_t *coll,
                                    mino_val_t *p, size_t extra_pairs)
{
    mino_hamt_node_t *root;
    mino_val_t       *order;
    mino_val_t       *out;
    size_t            len_out;
    size_t            i;
    mino_current_ctx(S)->gc_depth++;
    if (coll == NULL || coll->type == MINO_NIL) {
        root    = NULL;
        order   = mino_vector(S, NULL, 0);
        len_out = 0;
    } else {
        root    = coll->as.map.root;
        order   = coll->as.map.key_order;
        len_out = coll->as.map.len;
    }
    for (i = 0; i < extra_pairs; i++) {
        mino_val_t   *k = p->as.cons.car;
        mino_val_t   *v = p->as.cons.cdr->as.cons.car;
        hamt_entry_t *e = hamt_entry_new(S, k, v);
        uint32_t      h = hash_val(k);
        int           replaced = 0;
        root = hamt_assoc(S, root, e, h, 0u, &replaced);
        if (!replaced) {
            order = vec_conj1(S, order, k);
            len_out++;
        }
        p = p->as.cons.cdr->as.cons.cdr;
    }
    out = alloc_val(S, MINO_MAP);
    out->as.map.root      = root;
    out->as.map.key_order = order;
    out->as.map.len       = len_out;
    if (coll != NULL && coll->type == MINO_MAP) {
        out->meta = coll->meta;
    }
    mino_current_ctx(S)->gc_depth--;
    return out;
}

mino_val_t *prim_assoc(mino_state_t *S, mino_val_t *args, mino_env_t *env)
{
    mino_val_t *coll;
    size_t      n;
    size_t      extra_pairs;
    size_t      i;
    mino_val_t *p;
    (void)env;
    arg_count(S, args, &n);
    if (n < 3 || (n - 1) % 2 != 0) {
        return prim_throw_classified(S, "eval/arity", "MAR001", "assoc requires a collection and an even number of k/v pairs");
    }
    coll = args->as.cons.car;
    extra_pairs = (n - 1) / 2;
    if (coll != NULL && coll->type == MINO_VECTOR) {
        /* Vector assoc: each key must be an integer index in [0, len]; an
         * index == len is a one-past-end append. Apply pairs in order on
         * successively-derived vectors so each update shares structure with
         * its predecessor. */
        mino_val_t *acc = coll;
        p = args->as.cons.cdr;
        for (i = 0; i < extra_pairs; i++) {
            mino_val_t *k = p->as.cons.car;
            mino_val_t *v = p->as.cons.cdr->as.cons.car;
            long long   idx;
            if (k == NULL || k->type != MINO_INT) {
                return prim_throw_classified(S, "eval/type", "MTY001", "assoc on vector requires integer indices");
            }
            idx = k->as.i;
            if (idx < 0 || (size_t)idx > acc->as.vec.len) {
                return prim_throw_classified(S, "eval/bounds", "MBD001", "assoc on vector: index out of range");
            }
            acc = vec_assoc1(S, acc, (size_t)idx, v);
            p = p->as.cons.cdr->as.cons.cdr;
        }
        return acc;
    }
    if (coll == NULL || coll->type == MINO_NIL || coll->type == MINO_MAP) {
        return map_assoc_pairs(S, coll, args->as.cons.cdr, extra_pairs);
    }
    if (coll->type == MINO_SORTED_MAP) {
        mino_val_t *acc = coll;
        p = args->as.cons.cdr;
        for (i = 0; i < extra_pairs; i++) {
            acc = sorted_map_assoc1(S, acc, p->as.cons.car,
                                    p->as.cons.cdr->as.cons.car);
            p = p->as.cons.cdr->as.cons.cdr;
        }
        return acc;
    }
    if (coll->type == MINO_RECORD) {
        /* Each pair: declared field => copy vals, update slot. Ext
         * key => share vals, update ext map (allocating one if
         * needed). The new record keeps its type, so (record? r')
         * stays true and dispatch on (type r') is unchanged. */
        mino_val_t *acc = coll;
        p = args->as.cons.cdr;
        for (i = 0; i < extra_pairs; i++) {
            mino_val_t *k = p->as.cons.car;
            mino_val_t *v = p->as.cons.cdr->as.cons.car;
            int         idx;
            mino_val_t *new_rec;
            mino_val_t *fields = acc->as.record.type->as.record_type.fields;
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
                mino_val_t **slots = (mino_val_t **)malloc(
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
                mino_val_t *ext = acc->as.record.ext;
                mino_val_t *new_ext;
                mino_val_t *kv_args = mino_cons(S, k,
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

mino_val_t *prim_get(mino_state_t *S, mino_val_t *args, mino_env_t *env)
{
    mino_val_t *coll;
    mino_val_t *key;
    mino_val_t *def_val = mino_nil(S);
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
    if (coll == NULL || coll->type == MINO_NIL) {
        return def_val;
    }
    if (coll->type == MINO_MAP) {
        mino_val_t *v = map_get_val(coll, key);
        return v == NULL ? def_val : v;
    }
    if (coll->type == MINO_VECTOR) {
        long long idx;
        if (key == NULL || key->type != MINO_INT) {
            return def_val;
        }
        idx = key->as.i;
        if (idx < 0 || (size_t)idx >= coll->as.vec.len) {
            return def_val;
        }
        return vec_nth(coll, (size_t)idx);
    }
    if (coll->type == MINO_SET) {
        uint32_t h = hash_val(key);
        mino_val_t *found = hamt_get(coll->as.set.root, key, h, 0u);
        return found != NULL ? key : def_val;
    }
    if (coll->type == MINO_SORTED_MAP) {
        mino_val_t *v = rb_get(S, coll->as.sorted.root, key, coll->as.sorted.comparator);
        return v == NULL ? def_val : v;
    }
    if (coll->type == MINO_SORTED_SET) {
        return rb_contains(S, coll->as.sorted.root, key, coll->as.sorted.comparator)
            ? key : def_val;
    }
    if (coll->type == MINO_STRING) {
        long long idx;
        if (key == NULL || key->type != MINO_INT) {
            return def_val;
        }
        idx = key->as.i;
        if (idx < 0 || (size_t)idx >= coll->as.s.len) {
            return def_val;
        }
        return mino_string_n(S, coll->as.s.data + idx, 1);
    }
    if (coll->type == MINO_RECORD) {
        int idx = record_field_index(coll, key);
        if (idx >= 0) return coll->as.record.vals[idx];
        if (coll->as.record.ext != NULL) {
            mino_val_t *v = map_get_val(coll->as.record.ext, key);
            if (v != NULL) return v;
        }
        return def_val;
    }
    return def_val;
}

mino_val_t *prim_conj(mino_state_t *S, mino_val_t *args, mino_env_t *env)
{
    mino_val_t *coll;
    size_t      n;
    mino_val_t *p;
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
    coll = args->as.cons.car;
    p    = args->as.cons.cdr;
    if (coll == NULL || coll->type == MINO_NIL
        || coll->type == MINO_EMPTY_LIST
        || coll->type == MINO_CONS
        || coll->type == MINO_LAZY) {
        /* List/nil/empty-list: prepend each item so
         * (conj '(1 2) 3 4) => (4 3 1 2). */
        mino_val_t *out = (coll == NULL || coll->type == MINO_NIL
                           || coll->type == MINO_EMPTY_LIST)
            ? mino_nil(S) : coll;
        while (mino_is_cons(p)) {
            out = mino_cons(S, p->as.cons.car, out);
            p = p->as.cons.cdr;
        }
        if (out == NULL || out->type == MINO_NIL) return mino_empty_list(S);
        return out;
    }
    if (coll->type == MINO_VECTOR) {
        size_t extra = n - 1;
        mino_val_t *acc = coll;
        size_t i;
        for (i = 0; i < extra; i++) {
            acc = vec_conj1(S, acc, p->as.cons.car);
            p = p->as.cons.cdr;
        }
        return acc;
    }
    if (coll->type == MINO_MAP) {
        /* Each added item must be a 2-element vector [k v]. Assoc each onto
         * the accumulator so successor maps share structure with the source. */
        size_t      extra = n - 1;
        mino_val_t *acc   = coll;
        size_t      i;
        for (i = 0; i < extra; i++) {
            mino_val_t *item = p->as.cons.car;
            if (item != NULL && item->type == MINO_MAP) {
                /* Merge map entries: (conj {:a 0} {:b 1}) */
                size_t j;
                for (j = 0; j < item->as.map.len; j++) {
                    mino_val_t *k = vec_nth(item->as.map.key_order, j);
                    mino_val_t *v = map_get_val(item, k);
                    mino_val_t *pair_args = mino_cons(S, k, mino_cons(S, v, mino_nil(S)));
                    acc = map_assoc_pairs(S, acc, pair_args, 1);
                }
            } else if (item != NULL && item->type == MINO_VECTOR
                       && item->as.vec.len == 2) {
                mino_val_t *pair_args = mino_cons(S, vec_nth(item, 0),
                                       mino_cons(S, vec_nth(item, 1), mino_nil(S)));
                acc = map_assoc_pairs(S, acc, pair_args, 1);
            } else {
                return prim_throw_classified(S, "eval/type", "MTY001", "conj on map requires map entries or 2-element vectors");
            }
            p = p->as.cons.cdr;
        }
        return acc;
    }
    if (coll->type == MINO_SET) {
        mino_val_t *acc = coll;
        while (mino_is_cons(p)) {
            acc = set_conj1(S, acc, p->as.cons.car);
            p = p->as.cons.cdr;
        }
        return acc;
    }
    if (coll->type == MINO_SORTED_MAP) {
        mino_val_t *v = coll;
        while (mino_is_cons(p)) {
            mino_val_t *item = p->as.cons.car;
            if (item == NULL || item->type != MINO_VECTOR || item->as.vec.len != 2) {
                return prim_throw_classified(S, "eval/type", "MTY001", "conj on sorted-map requires 2-element vectors");
            }
            v = sorted_map_assoc1(S, v, vec_nth(item, 0), vec_nth(item, 1));
            p = p->as.cons.cdr;
        }
        return v;
    }
    if (coll->type == MINO_SORTED_SET) {
        mino_val_t *v = coll;
        while (mino_is_cons(p)) {
            v = sorted_set_conj1(S, v, p->as.cons.car);
            p = p->as.cons.cdr;
        }
        return v;
    }
    {
        char msg[96];
        snprintf(msg, sizeof(msg),
                 "conj: expected a list, vector, map, or set, got %s",
                 type_tag_str(coll));
        return prim_throw_classified(S, "eval/type", "MTY001", msg);
    }
}

mino_val_t *prim_keys(mino_state_t *S, mino_val_t *args, mino_env_t *env)
{
    mino_val_t *coll;
    mino_val_t *head = mino_nil(S);
    mino_val_t *tail = NULL;
    size_t i;
    (void)env;
    if (!mino_is_cons(args) || mino_is_cons(args->as.cons.cdr)) {
        return prim_throw_classified(S, "eval/arity", "MAR001", "keys requires one argument");
    }
    coll = args->as.cons.car;
    if (coll == NULL || coll->type == MINO_NIL) {
        return mino_nil(S);
    }
    if (coll->type == MINO_SORTED_MAP) {
        rb_to_list(S, coll->as.sorted.root, &head, &tail);
        return head;
    }
    if (coll->type == MINO_VECTOR && coll->as.vec.len == 0) return mino_nil(S);
    if (coll->type == MINO_SET    && coll->as.set.len == 0) return mino_nil(S);
    if (coll->type == MINO_SORTED_SET)                      return mino_nil(S);
    if (coll->type == MINO_STRING && coll->as.s.len == 0) return mino_nil(S);
    if (coll->type == MINO_RECORD) {
        /* Declared field keywords first in declared order, then ext
         * keys in insertion order. */
        mino_val_t *fields = coll->as.record.type->as.record_type.fields;
        size_t n_fields = (fields != NULL) ? fields->as.vec.len : 0;
        for (i = 0; i < n_fields; i++) {
            mino_val_t *cell = mino_cons(S, vec_nth(fields, i), mino_nil(S));
            if (tail == NULL) head = cell;
            else mino_cons_cdr_set(S, tail, cell);
            tail = cell;
        }
        if (coll->as.record.ext != NULL) {
            const mino_val_t *e = coll->as.record.ext;
            size_t k;
            for (k = 0; k < e->as.map.len; k++) {
                mino_val_t *cell = mino_cons(S,
                    vec_nth(e->as.map.key_order, k), mino_nil(S));
                if (tail == NULL) head = cell;
                else mino_cons_cdr_set(S, tail, cell);
                tail = cell;
            }
        }
        return head;
    }
    if (coll->type != MINO_MAP) {
        return prim_throw_classified(S, "eval/type", "MTY001", "keys: argument must be a map");
    }
    for (i = 0; i < coll->as.map.len; i++) {
        mino_val_t *cell = mino_cons(S, vec_nth(coll->as.map.key_order, i),
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

mino_val_t *prim_vals(mino_state_t *S, mino_val_t *args, mino_env_t *env)
{
    mino_val_t *coll;
    mino_val_t *head = mino_nil(S);
    mino_val_t *tail = NULL;
    size_t i;
    (void)env;
    if (!mino_is_cons(args) || mino_is_cons(args->as.cons.cdr)) {
        return prim_throw_classified(S, "eval/arity", "MAR001", "vals requires one argument");
    }
    coll = args->as.cons.car;
    if (coll == NULL || coll->type == MINO_NIL) {
        return mino_nil(S);
    }
    if (coll->type == MINO_SORTED_MAP) {
        mino_val_t *keys = mino_nil(S);
        mino_val_t *kt   = NULL;
        rb_to_list(S, coll->as.sorted.root, &keys, &kt);
        while (mino_is_cons(keys)) {
            mino_val_t *v = rb_get(S, coll->as.sorted.root, keys->as.cons.car,
                                   coll->as.sorted.comparator);
            mino_val_t *cell = mino_cons(S, v, mino_nil(S));
            if (tail == NULL) head = cell; else mino_cons_cdr_set(S, tail, cell);
            tail = cell;
            keys = keys->as.cons.cdr;
        }
        return head;
    }
    if (coll->type == MINO_VECTOR && coll->as.vec.len == 0) return mino_nil(S);
    if (coll->type == MINO_SET    && coll->as.set.len == 0) return mino_nil(S);
    if (coll->type == MINO_SORTED_SET)                      return mino_nil(S);
    if (coll->type == MINO_STRING && coll->as.s.len == 0)   return mino_nil(S);
    if (coll->type == MINO_RECORD) {
        mino_val_t *fields = coll->as.record.type->as.record_type.fields;
        size_t n_fields = (fields != NULL) ? fields->as.vec.len : 0;
        for (i = 0; i < n_fields; i++) {
            mino_val_t *cell = mino_cons(S, coll->as.record.vals[i],
                                          mino_nil(S));
            if (tail == NULL) head = cell;
            else mino_cons_cdr_set(S, tail, cell);
            tail = cell;
        }
        if (coll->as.record.ext != NULL) {
            const mino_val_t *e = coll->as.record.ext;
            size_t k;
            for (k = 0; k < e->as.map.len; k++) {
                mino_val_t *ek = vec_nth(e->as.map.key_order, k);
                mino_val_t *cell = mino_cons(S, map_get_val(e, ek),
                                              mino_nil(S));
                if (tail == NULL) head = cell;
                else mino_cons_cdr_set(S, tail, cell);
                tail = cell;
            }
        }
        return head;
    }
    if (coll->type != MINO_MAP) {
        return prim_throw_classified(S, "eval/type", "MTY001", "vals: argument must be a map");
    }
    for (i = 0; i < coll->as.map.len; i++) {
        mino_val_t *key  = vec_nth(coll->as.map.key_order, i);
        mino_val_t *cell = mino_cons(S, map_get_val(coll, key), mino_nil(S));
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
mino_val_t *set_conj1(mino_state_t *S, const mino_val_t *s, mino_val_t *elem)
{
    mino_val_t       *v        = alloc_val(S, MINO_SET);
    mino_val_t       *sentinel = mino_true(S);
    hamt_entry_t     *e        = hamt_entry_new(S, elem, sentinel);
    uint32_t          h        = hash_val(elem);
    int               replaced = 0;
    mino_hamt_node_t *root     = hamt_assoc(S, s->as.set.root, e, h, 0u, &replaced);
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

mino_val_t *prim_hash_set(mino_state_t *S, mino_val_t *args, mino_env_t *env)
{
    size_t      n;
    size_t      i;
    mino_val_t **tmp;
    mino_val_t *p;
    (void)env;
    arg_count(S, args, &n);
    tmp = (mino_val_t **)gc_alloc_typed(S, GC_T_VALARR, (n > 0 ? n : 1) * sizeof(*tmp));
    p = args;
    for (i = 0; i < n; i++) {
        tmp[i] = p->as.cons.car;
        p = p->as.cons.cdr;
    }
    return mino_set(S, tmp, n);
}

mino_val_t *prim_contains_p(mino_state_t *S, mino_val_t *args, mino_env_t *env)
{
    mino_val_t *coll;
    mino_val_t *key;
    (void)env;
    if (!mino_is_cons(args) || !mino_is_cons(args->as.cons.cdr)
        || mino_is_cons(args->as.cons.cdr->as.cons.cdr)) {
        return prim_throw_classified(S, "eval/arity", "MAR001", "contains? requires two arguments");
    }
    coll = args->as.cons.car;
    key  = args->as.cons.cdr->as.cons.car;
    if (coll == NULL || coll->type == MINO_NIL) {
        return mino_false(S);
    }
    if (coll->type == MINO_MAP) {
        return map_get_val(coll, key) != NULL ? mino_true(S) : mino_false(S);
    }
    if (coll->type == MINO_SET) {
        uint32_t h = hash_val(key);
        return hamt_get(coll->as.set.root, key, h, 0u) != NULL
            ? mino_true(S) : mino_false(S);
    }
    if (coll->type == MINO_SORTED_MAP || coll->type == MINO_SORTED_SET) {
        return rb_contains(S, coll->as.sorted.root, key, coll->as.sorted.comparator)
            ? mino_true(S) : mino_false(S);
    }
    if (coll->type == MINO_VECTOR) {
        /* For vectors, key is an index. */
        if (key != NULL && key->type == MINO_INT) {
            long long idx = key->as.i;
            return (idx >= 0 && (size_t)idx < coll->as.vec.len)
                ? mino_true(S) : mino_false(S);
        }
        return mino_false(S);
    }
    if (coll->type == MINO_STRING) {
        /* For strings, key must be an integer index. */
        if (key == NULL || key->type == MINO_NIL)
            return prim_throw_classified(S, "eval/type", "MTY001",
                "contains?: string key must be an integer");
        if (key->type == MINO_INT) {
            long long idx = key->as.i;
            return (idx >= 0 && (size_t)idx < coll->as.s.len)
                ? mino_true(S) : mino_false(S);
        }
        return prim_throw_classified(S, "eval/type", "MTY001",
            "contains?: string key must be an integer");
    }
    if (coll->type == MINO_RECORD) {
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

mino_val_t *prim_disj(mino_state_t *S, mino_val_t *args, mino_env_t *env)
{
    mino_val_t *coll;
    mino_val_t *p;
    size_t      n;
    (void)env;
    arg_count(S, args, &n);
    if (n < 2) {
        return prim_throw_classified(S, "eval/arity", "MAR001", "disj requires a set and at least one key");
    }
    coll = args->as.cons.car;
    if (coll == NULL || coll->type == MINO_NIL) {
        return mino_nil(S);
    }
    if (coll->type == MINO_SORTED_SET) {
        p = args->as.cons.cdr;
        while (mino_is_cons(p)) {
            coll = sorted_set_disj1(S, coll, p->as.cons.car);
            p = p->as.cons.cdr;
        }
        return coll;
    }
    if (coll->type != MINO_SET) {
        return prim_throw_classified(S, "eval/type", "MTY001", "disj: first argument must be a set");
    }
    /* Rebuild set excluding the specified elements. Not the most efficient
     * approach, but keeps the code simple and correct. */
    p = args->as.cons.cdr;
    while (mino_is_cons(p)) {
        mino_val_t *key = p->as.cons.car;
        uint32_t    h   = hash_val(key);
        if (hamt_get(coll->as.set.root, key, h, 0u) != NULL) {
            /* Element exists; rebuild without it. */
            mino_val_t *new_set = alloc_val(S, MINO_SET);
            mino_val_t *order   = mino_vector(S, NULL, 0);
            mino_hamt_node_t *root = NULL;
            size_t i;
            size_t new_len = 0;
            for (i = 0; i < coll->as.set.len; i++) {
                mino_val_t *elem = vec_nth(coll->as.set.key_order, i);
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

mino_val_t *prim_dissoc(mino_state_t *S, mino_val_t *args, mino_env_t *env)
{
    mino_val_t *coll;
    mino_val_t *p;
    size_t      n;
    (void)env;
    arg_count(S, args, &n);
    if (n < 2) {
        return prim_throw_classified(S, "eval/arity", "MAR001", "dissoc requires a map and at least one key");
    }
    coll = args->as.cons.car;
    if (coll == NULL || coll->type == MINO_NIL) {
        return mino_nil(S);
    }
    if (coll->type == MINO_SORTED_MAP) {
        p = args->as.cons.cdr;
        while (mino_is_cons(p)) {
            coll = sorted_map_dissoc1(S, coll, p->as.cons.car);
            p = p->as.cons.cdr;
        }
        return coll;
    }
    if (coll->type == MINO_RECORD) {
        /* Record dissoc: dropping a declared field degrades to a
         * plain map (canonical Clojure behaviour); dropping an ext
         * key returns a new record with the ext map updated. Each
         * key is processed in order against the running accumulator,
         * which may flip from RECORD to MAP partway through. */
        mino_val_t *acc = coll;
        p = args->as.cons.cdr;
        while (mino_is_cons(p)) {
            mino_val_t *key = p->as.cons.car;
            if (acc->type == MINO_RECORD) {
                int idx = record_field_index(acc, key);
                if (idx >= 0) {
                    /* Degrade: build a plain map from declared fields
                     * (skipping the dissoc'd one) and the ext entries. */
                    mino_val_t *fields = acc->as.record.type
                        ->as.record_type.fields;
                    size_t n_fields = (fields != NULL) ? fields->as.vec.len : 0;
                    size_t cap = n_fields - 1;
                    size_t mi  = 0;
                    size_t j;
                    mino_val_t **mk;
                    mino_val_t **mv;
                    if (acc->as.record.ext != NULL) {
                        cap += acc->as.record.ext->as.map.len;
                    }
                    mk = (cap > 0) ? (mino_val_t **)gc_alloc_typed(S,
                        GC_T_VALARR, cap * sizeof(*mk)) : NULL;
                    mv = (cap > 0) ? (mino_val_t **)gc_alloc_typed(S,
                        GC_T_VALARR, cap * sizeof(*mv)) : NULL;
                    for (j = 0; j < n_fields; j++) {
                        if ((int)j == idx) continue;
                        mk[mi] = vec_nth(fields, j);
                        mv[mi] = acc->as.record.vals[j];
                        mi++;
                    }
                    if (acc->as.record.ext != NULL) {
                        const mino_val_t *e = acc->as.record.ext;
                        size_t k;
                        for (k = 0; k < e->as.map.len; k++) {
                            mino_val_t *ek = vec_nth(e->as.map.key_order, k);
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
                    const mino_val_t *e   = acc->as.record.ext;
                    size_t            len = e->as.map.len;
                    mino_val_t      **ek  = (mino_val_t **)gc_alloc_typed(S,
                        GC_T_VALARR, (len > 0 ? len : 1) * sizeof(*ek));
                    mino_val_t      **ev  = (mino_val_t **)gc_alloc_typed(S,
                        GC_T_VALARR, (len > 0 ? len : 1) * sizeof(*ev));
                    size_t  ext_n  = 0;
                    size_t  k, n_fields_acc;
                    mino_val_t  *new_rec, *new_ext;
                    mino_val_t **slots;
                    mino_val_t  *fields_acc;
                    for (k = 0; k < len; k++) {
                        mino_val_t *kk = vec_nth(e->as.map.key_order, k);
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
                        slots = (mino_val_t **)malloc(
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
                uint32_t h = hash_val(key);
                if (hamt_get(acc->as.map.root, key, h, 0u) != NULL) {
                    mino_val_t *new_map = alloc_val(S, MINO_MAP);
                    mino_val_t *order   = mino_vector(S, NULL, 0);
                    mino_hamt_node_t *root = NULL;
                    size_t mi;
                    size_t new_len = 0;
                    for (mi = 0; mi < acc->as.map.len; mi++) {
                        mino_val_t *k = vec_nth(acc->as.map.key_order, mi);
                        if (!mino_eq(k, key)) {
                            mino_val_t   *v  = map_get_val(acc, k);
                            hamt_entry_t *e2 = hamt_entry_new(S, k, v);
                            uint32_t      h2 = hash_val(k);
                            int rep = 0;
                            root = hamt_assoc(S, root, e2, h2, 0u, &rep);
                            order = vec_conj1(S, order, k);
                            new_len++;
                        }
                    }
                    new_map->as.map.root      = root;
                    new_map->as.map.key_order = order;
                    new_map->as.map.len       = new_len;
                    new_map->meta             = acc->meta;
                    acc = new_map;
                }
            }
            p = p->as.cons.cdr;
        }
        return acc;
    }
    if (coll->type != MINO_MAP) {
        return prim_throw_classified(S, "eval/type", "MTY001", "dissoc: first argument must be a map");
    }
    p = args->as.cons.cdr;
    while (mino_is_cons(p)) {
        mino_val_t *key = p->as.cons.car;
        uint32_t    h   = hash_val(key);
        if (hamt_get(coll->as.map.root, key, h, 0u) != NULL) {
            mino_val_t *new_map = alloc_val(S, MINO_MAP);
            mino_val_t *order   = mino_vector(S, NULL, 0);
            mino_hamt_node_t *root = NULL;
            size_t i;
            size_t new_len = 0;
            for (i = 0; i < coll->as.map.len; i++) {
                mino_val_t *k = vec_nth(coll->as.map.key_order, i);
                if (!mino_eq(k, key)) {
                    mino_val_t   *v  = map_get_val(coll, k);
                    hamt_entry_t *e2 = hamt_entry_new(S, k, v);
                    uint32_t      h2 = hash_val(k);
                    int rep = 0;
                    root = hamt_assoc(S, root, e2, h2, 0u, &rep);
                    order = vec_conj1(S, order, k);
                    new_len++;
                }
            }
            new_map->as.map.root      = root;
            new_map->as.map.key_order = order;
            new_map->as.map.len       = new_len;
            new_map->meta             = coll->meta;
            coll = new_map;
        }
        p = p->as.cons.cdr;
    }
    return coll;
}

mino_val_t *prim_subvec(mino_state_t *S, mino_val_t *args, mino_env_t *env)
{
    mino_val_t *v;
    long long   start, end;
    size_t      nargs = 0;
    mino_val_t *p;
    (void)env;
    for (p = args; mino_is_cons(p); p = p->as.cons.cdr) nargs++;
    if (nargs < 2 || nargs > 3) {
        return prim_throw_classified(S, "eval/arity", "MAR001", "subvec requires 2 or 3 arguments");
    }
    v = args->as.cons.car;
    if (v == NULL || v->type != MINO_VECTOR) {
        return prim_throw_classified(S, "eval/type", "MTY001", "subvec: first argument must be a vector");
    }
    if (args->as.cons.cdr->as.cons.car == NULL
        || args->as.cons.cdr->as.cons.car->type != MINO_INT) {
        return prim_throw_classified(S, "eval/type", "MTY001", "subvec: start must be an integer");
    }
    start = args->as.cons.cdr->as.cons.car->as.i;
    if (nargs == 3) {
        if (args->as.cons.cdr->as.cons.cdr->as.cons.car == NULL
            || args->as.cons.cdr->as.cons.cdr->as.cons.car->type != MINO_INT) {
            return prim_throw_classified(S, "eval/type", "MTY001", "subvec: end must be an integer");
        }
        end = args->as.cons.cdr->as.cons.cdr->as.cons.car->as.i;
    } else {
        end = (long long)v->as.vec.len;
    }
    if (start < 0 || end < start || (size_t)end > v->as.vec.len) {
        return prim_throw_classified(S, "eval/bounds", "MBD001", "subvec: index out of bounds");
    }
    return vec_subvec(S, v, (size_t)start, (size_t)end);
}

/* ------------------------------------------------------------------------- */
/* Transient primitives                                                       */
/*                                                                            */
/* Thin wrappers around the C kernel in transient.c. The kernel enforces     */
/* the validity-bit invariant and write-barrier discipline; the primitives   */
/* just marshal args. Supported inner types are vector, map, and set.        */
/* ------------------------------------------------------------------------- */

mino_val_t *prim_transient(mino_state_t *S, mino_val_t *args, mino_env_t *env)
{
    size_t n;
    (void)env;
    arg_count(S, args, &n);
    if (n != 1) {
        return prim_throw_classified(S, "eval/arity", "MAR001",
            "transient requires one argument");
    }
    return mino_transient(S, args->as.cons.car);
}

mino_val_t *prim_persistent_bang(mino_state_t *S, mino_val_t *args, mino_env_t *env)
{
    size_t n;
    (void)env;
    arg_count(S, args, &n);
    if (n != 1) {
        return prim_throw_classified(S, "eval/arity", "MAR001",
            "persistent! requires one argument");
    }
    return mino_persistent(S, args->as.cons.car);
}

mino_val_t *prim_assoc_bang(mino_state_t *S, mino_val_t *args, mino_env_t *env)
{
    size_t n;
    mino_val_t *t, *key, *val;
    (void)env;
    arg_count(S, args, &n);
    if (n != 3) {
        return prim_throw_classified(S, "eval/arity", "MAR001",
            "assoc! requires three arguments");
    }
    t   = args->as.cons.car;
    key = args->as.cons.cdr->as.cons.car;
    val = args->as.cons.cdr->as.cons.cdr->as.cons.car;
    return mino_assoc_bang(S, t, key, val);
}

mino_val_t *prim_conj_bang(mino_state_t *S, mino_val_t *args, mino_env_t *env)
{
    size_t n;
    mino_val_t *t, *val;
    (void)env;
    arg_count(S, args, &n);
    if (n != 2) {
        return prim_throw_classified(S, "eval/arity", "MAR001",
            "conj! requires two arguments");
    }
    t   = args->as.cons.car;
    val = args->as.cons.cdr->as.cons.car;
    return mino_conj_bang(S, t, val);
}

mino_val_t *prim_dissoc_bang(mino_state_t *S, mino_val_t *args, mino_env_t *env)
{
    size_t n;
    mino_val_t *t, *key;
    (void)env;
    arg_count(S, args, &n);
    if (n != 2) {
        return prim_throw_classified(S, "eval/arity", "MAR001",
            "dissoc! requires two arguments");
    }
    t   = args->as.cons.car;
    key = args->as.cons.cdr->as.cons.car;
    return mino_dissoc_bang(S, t, key);
}

mino_val_t *prim_disj_bang(mino_state_t *S, mino_val_t *args, mino_env_t *env)
{
    size_t n;
    mino_val_t *t, *key;
    (void)env;
    arg_count(S, args, &n);
    if (n != 2) {
        return prim_throw_classified(S, "eval/arity", "MAR001",
            "disj! requires two arguments");
    }
    t   = args->as.cons.car;
    key = args->as.cons.cdr->as.cons.car;
    return mino_disj_bang(S, t, key);
}

mino_val_t *prim_pop_bang(mino_state_t *S, mino_val_t *args, mino_env_t *env)
{
    size_t n;
    (void)env;
    arg_count(S, args, &n);
    if (n != 1) {
        return prim_throw_classified(S, "eval/arity", "MAR001",
            "pop! requires one argument");
    }
    return mino_pop_bang(S, args->as.cons.car);
}

mino_val_t *prim_transient_p(mino_state_t *S, mino_val_t *args, mino_env_t *env)
{
    size_t n;
    (void)env;
    arg_count(S, args, &n);
    if (n != 1) {
        return prim_throw_classified(S, "eval/arity", "MAR001",
            "transient? requires one argument");
    }
    return mino_is_transient(args->as.cons.car) ? mino_true(S) : mino_false(S);
}

mino_val_t *prim_list(mino_state_t *S, mino_val_t *args, mino_env_t *env)
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
     "Returns a new list with x prepended to coll."},
    {"count",    prim_count,
     "Returns the number of items in a collection."},
    {"nth",      prim_nth,
     "Returns the item at index n in a collection."},
    {"first",    prim_first,
     "Returns the first item in a collection, or nil if empty."},
    {"rest",     prim_rest,
     "Returns all but the first item in a collection."},
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
};

const size_t k_prims_collections_count =
    sizeof(k_prims_collections) / sizeof(k_prims_collections[0]);
