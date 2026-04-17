/*
 * prim_sequences.c -- sequence iteration and higher-order primitives:
 *                     seq_iter_*, reduce, reduced, into, apply,
 *                     reverse, sort, set, rangev, mapv, filterv.
 */

#include "prim_internal.h"

/* ------------------------------------------------------------------------- */
/* Sequence iterator                                                         */
/* ------------------------------------------------------------------------- */

void seq_iter_init(mino_state_t *S, seq_iter_t *it, const mino_val_t *coll)
{
    /* Force lazy seqs so they behave as cons lists. */
    if (coll != NULL && coll->type == MINO_LAZY) {
        coll = lazy_force(S, (mino_val_t *)coll);
    }
    it->coll  = coll;
    it->idx   = 0;
    it->cons_p = (coll != NULL && coll->type == MINO_CONS) ? coll : NULL;
}

int seq_iter_done(const seq_iter_t *it)
{
    const mino_val_t *c = it->coll;
    if (c == NULL || c->type == MINO_NIL) return 1;
    switch (c->type) {
    case MINO_CONS:   return it->cons_p == NULL || it->cons_p->type != MINO_CONS;
    case MINO_VECTOR: return it->idx >= c->as.vec.len;
    case MINO_MAP:    return it->idx >= c->as.map.len;
    case MINO_SET:    return it->idx >= c->as.set.len;
    case MINO_STRING: return it->idx >= c->as.s.len;
    default:          return 1;
    }
}

mino_val_t *seq_iter_val(mino_state_t *S, const seq_iter_t *it)
{
    const mino_val_t *c = it->coll;
    switch (c->type) {
    case MINO_CONS:   return it->cons_p->as.cons.car;
    case MINO_VECTOR: return vec_nth(c, it->idx);
    case MINO_MAP: {
        /* Yield [key value] vectors for maps. */
        mino_val_t *key = vec_nth(c->as.map.key_order, it->idx);
        mino_val_t *val = map_get_val(c, key);
        mino_val_t *kv[2];
        kv[0] = key;
        kv[1] = val;
        return mino_vector(S, kv, 2);
    }
    case MINO_SET:    return vec_nth(c->as.set.key_order, it->idx);
    case MINO_STRING: return mino_string_n(S, c->as.s.data + it->idx, 1);
    default:          return mino_nil(S);
    }
}

void seq_iter_next(mino_state_t *S, seq_iter_t *it)
{
    if (it->coll != NULL && it->coll->type == MINO_CONS) {
        if (it->cons_p != NULL && it->cons_p->type == MINO_CONS) {
            const mino_val_t *next = it->cons_p->as.cons.cdr;
            /* Force lazy tail if present. */
            if (next != NULL && next->type == MINO_LAZY) {
                next = lazy_force(S, (mino_val_t *)next);
            }
            it->cons_p = next;
        }
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
        set_error(S, "reduced requires exactly 1 argument");
        return NULL;
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
        set_error(S, "reduced? requires exactly 1 argument");
        return NULL;
    }
    return (args->as.cons.car != NULL && args->as.cons.car->type == MINO_REDUCED)
        ? mino_true(S) : mino_false(S);
}

mino_val_t *prim_reduce(mino_state_t *S, mino_val_t *args, mino_env_t *env)
{
    mino_val_t *fn;
    mino_val_t *acc;
    mino_val_t *coll;
    seq_iter_t  it;
    size_t      n;
    arg_count(S, args, &n);
    if (n == 2) {
        /* (reduce f coll) — first element is the initial accumulator. */
        fn   = args->as.cons.car;
        coll = args->as.cons.cdr->as.cons.car;
        if (coll == NULL || coll->type == MINO_NIL) {
            /* (reduce f nil) → (f) */
            return apply_callable(S, fn, mino_nil(S), env);
        }
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
        if (coll == NULL || coll->type == MINO_NIL) {
            return acc;
        }
        seq_iter_init(S, &it, coll);
    } else {
        set_error(S, "reduce requires 2 or 3 arguments");
        return NULL;
    }
    while (!seq_iter_done(&it)) {
        mino_val_t *elem   = seq_iter_val(S, &it);
        mino_val_t *call_a = mino_cons(S, acc, mino_cons(S, elem, mino_nil(S)));
        acc = apply_callable(S, fn, call_a, env);
        if (acc == NULL) return NULL;
        if (acc->type == MINO_REDUCED) return acc->as.reduced.val;
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
        set_error(S, "set requires exactly 1 argument");
        return NULL;
    }
    coll = args->as.cons.car;
    if (coll == NULL || coll->type == MINO_NIL) {
        return mino_set(S, NULL, 0);
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

/* (take, drop, range, repeat, concat are now lazy in core.mino) */

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
            set_error(S, "rangev argument must be an integer"); return NULL;
        }
    } else if (n == 2) {
        if (!mino_to_int(args->as.cons.car, &start) ||
            !mino_to_int(args->as.cons.cdr->as.cons.car, &end)) {
            set_error(S, "rangev arguments must be integers"); return NULL;
        }
    } else if (n == 3) {
        if (!mino_to_int(args->as.cons.car, &start) ||
            !mino_to_int(args->as.cons.cdr->as.cons.car, &end) ||
            !mino_to_int(args->as.cons.cdr->as.cons.cdr->as.cons.car, &step)) {
            set_error(S, "rangev arguments must be integers"); return NULL;
        }
        if (step == 0) {
            set_error(S, "rangev step must not be zero"); return NULL;
        }
    } else {
        set_error(S, "rangev requires 1, 2, or 3 arguments"); return NULL;
    }
    /* Compute length. */
    if (step > 0) {
        len = (end > start) ? (size_t)((end - start + step - 1) / step) : 0;
    } else {
        len = (start > end) ? (size_t)((start - end + (-step) - 1) / (-step)) : 0;
    }
    items = malloc(len * sizeof(mino_val_t *));
    if (!items && len > 0) { set_error(S, "rangev: out of memory"); return NULL; }
    for (i = start, n = 0; n < len; i += step, n++) {
        items[n] = mino_int(S, i);
    }
    result = mino_vector(S, items, len);
    free(items);
    return result;
}

/* Eager map returning a vector. (mapv f coll) */
mino_val_t *prim_mapv(mino_state_t *S, mino_val_t *args, mino_env_t *env)
{
    mino_val_t *fn, *coll;
    seq_iter_t  it;
    size_t      cap = 64, len = 0;
    mino_val_t **items;
    mino_val_t *result;
    size_t n;
    arg_count(S, args, &n);
    if (n != 2) {
        set_error(S, "mapv requires 2 arguments: function and collection");
        return NULL;
    }
    fn   = args->as.cons.car;
    coll = args->as.cons.cdr->as.cons.car;
    if (coll == NULL || mino_is_nil(coll)) {
        return mino_vector(S, NULL, 0);
    }
    items = malloc(cap * sizeof(mino_val_t *));
    if (!items) { set_error(S, "mapv: out of memory"); return NULL; }
    seq_iter_init(S, &it, coll);
    while (!seq_iter_done(&it)) {
        mino_val_t *elem = seq_iter_val(S, &it);
        mino_val_t *call_args = mino_cons(S, elem, mino_nil(S));
        mino_val_t *val = apply_callable(S, fn, call_args, env);
        if (val == NULL) { free(items); return NULL; }
        if (len >= cap) {
            cap *= 2;
            items = realloc(items, cap * sizeof(mino_val_t *));
            if (!items) { set_error(S, "mapv: out of memory"); return NULL; }
        }
        items[len++] = val;
        seq_iter_next(S, &it);
    }
    result = mino_vector(S, items, len);
    free(items);
    return result;
}

/* Eager filter returning a vector. (filterv pred coll) */
mino_val_t *prim_filterv(mino_state_t *S, mino_val_t *args, mino_env_t *env)
{
    mino_val_t *pred, *coll;
    seq_iter_t  it;
    size_t      cap = 64, len = 0;
    mino_val_t **items;
    mino_val_t *result;
    size_t n;
    arg_count(S, args, &n);
    if (n != 2) {
        set_error(S, "filterv requires 2 arguments: predicate and collection");
        return NULL;
    }
    pred = args->as.cons.car;
    coll = args->as.cons.cdr->as.cons.car;
    if (coll == NULL || mino_is_nil(coll)) {
        return mino_vector(S, NULL, 0);
    }
    items = malloc(cap * sizeof(mino_val_t *));
    if (!items) { set_error(S, "filterv: out of memory"); return NULL; }
    seq_iter_init(S, &it, coll);
    while (!seq_iter_done(&it)) {
        mino_val_t *elem = seq_iter_val(S, &it);
        mino_val_t *call_args = mino_cons(S, elem, mino_nil(S));
        mino_val_t *test = apply_callable(S, pred, call_args, env);
        if (test == NULL) { free(items); return NULL; }
        if (mino_is_truthy(test)) {
            if (len >= cap) {
                cap *= 2;
                items = realloc(items, cap * sizeof(mino_val_t *));
                if (!items) { set_error(S, "filterv: out of memory"); return NULL; }
            }
            items[len++] = elem;
        }
        seq_iter_next(S, &it);
    }
    result = mino_vector(S, items, len);
    free(items);
    return result;
}

mino_val_t *prim_into(mino_state_t *S, mino_val_t *args, mino_env_t *env)
{
    mino_val_t *to;
    mino_val_t *from;
    seq_iter_t  it;
    (void)env;
    if (!mino_is_cons(args) || !mino_is_cons(args->as.cons.cdr)) {
        set_error(S, "into requires two arguments");
        return NULL;
    }
    to   = args->as.cons.car;
    from = args->as.cons.cdr->as.cons.car;
    if (from == NULL || from->type == MINO_NIL) {
        return to;
    }
    /* Conj each element of `from` into `to`. The type of `to` determines
     * the conj semantics (vector appends, list prepends, map/set merges). */
    if (to == NULL || to->type == MINO_NIL) {
        /* Into nil: build a list. */
        mino_val_t *out = mino_nil(S);
        seq_iter_init(S, &it, from);
        while (!seq_iter_done(&it)) {
            out = mino_cons(S, seq_iter_val(S, &it), out);
            seq_iter_next(S, &it);
        }
        return out;
    }
    if (to->type == MINO_VECTOR) {
        mino_val_t *acc = to;
        seq_iter_init(S, &it, from);
        while (!seq_iter_done(&it)) {
            acc = vec_conj1(S, acc, seq_iter_val(S, &it));
            seq_iter_next(S, &it);
        }
        return acc;
    }
    if (to->type == MINO_MAP) {
        mino_val_t *acc = to;
        seq_iter_init(S, &it, from);
        while (!seq_iter_done(&it)) {
            mino_val_t *item = seq_iter_val(S, &it);
            mino_val_t *pair_args;
            if (item == NULL || item->type != MINO_VECTOR
                || item->as.vec.len != 2) {
                set_error(S, "into map: each element must be a 2-element vector");
                return NULL;
            }
            pair_args = mino_cons(S, vec_nth(item, 0),
                                   mino_cons(S, vec_nth(item, 1), mino_nil(S)));
            acc = prim_assoc(S, mino_cons(S, acc, pair_args), env);
            seq_iter_next(S, &it);
        }
        return acc;
    }
    if (to->type == MINO_SET) {
        mino_val_t *acc = to;
        seq_iter_init(S, &it, from);
        while (!seq_iter_done(&it)) {
            acc = set_conj1(S, acc, seq_iter_val(S, &it));
            seq_iter_next(S, &it);
        }
        return acc;
    }
    if (to->type == MINO_CONS) {
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
        set_error(S, msg);
    }
    return NULL;
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
        set_error(S, "apply requires a function and arguments");
        return NULL;
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
            if (tail2 == NULL) { head = cell; } else { tail2->as.cons.cdr = cell; }
            tail2 = cell;
            p = p->as.cons.cdr;
        }
        last = p->as.cons.car; /* the final collection argument */
        /* Append elements from `last` collection. */
        if (last != NULL && last->type != MINO_NIL) {
            seq_iter_t it;
            seq_iter_init(S, &it, last);
            while (!seq_iter_done(&it)) {
                mino_val_t *cell = mino_cons(S, seq_iter_val(S, &it), mino_nil(S));
                if (tail2 == NULL) { head = cell; } else { tail2->as.cons.cdr = cell; }
                tail2 = cell;
                seq_iter_next(S, &it);
            }
        }
        return apply_callable(S, fn, head, env);
    }
    /* (apply f coll) — convert coll to a cons arg list. */
    if (last == NULL || last->type == MINO_NIL) {
        return apply_callable(S, fn, mino_nil(S), env);
    }
    if (last->type == MINO_CONS) {
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
            if (tail2 == NULL) { head = cell; } else { tail2->as.cons.cdr = cell; }
            tail2 = cell;
            seq_iter_next(S, &it);
        }
        call_args = head;
    }
    return apply_callable(S, fn, call_args, env);
}

mino_val_t *prim_reverse(mino_state_t *S, mino_val_t *args, mino_env_t *env)
{
    mino_val_t *coll;
    mino_val_t *out = mino_nil(S);
    seq_iter_t  it;
    (void)env;
    if (!mino_is_cons(args) || mino_is_cons(args->as.cons.cdr)) {
        set_error(S, "reverse requires one argument");
        return NULL;
    }
    coll = args->as.cons.car;
    if (coll == NULL || coll->type == MINO_NIL) {
        return mino_nil(S);
    }
    seq_iter_init(S, &it, coll);
    while (!seq_iter_done(&it)) {
        out = mino_cons(S, seq_iter_val(S, &it), out);
        seq_iter_next(S, &it);
    }
    return out;
}

/* Simple comparison function for sorting: numbers by value, strings
 * lexicographically, other types by type tag then identity. */
static int val_compare(const mino_val_t *a, const mino_val_t *b)
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
    if ((a->type == MINO_STRING || a->type == MINO_SYMBOL || a->type == MINO_KEYWORD)
        && a->type == b->type) {
        size_t min_len = a->as.s.len < b->as.s.len ? a->as.s.len : b->as.s.len;
        int c = memcmp(a->as.s.data, b->as.s.data, min_len);
        if (c != 0) return c;
        return a->as.s.len < b->as.s.len ? -1 : a->as.s.len > b->as.s.len ? 1 : 0;
    }
    /* Fall back to type tag ordering. */
    return a->type < b->type ? -1 : a->type > b->type ? 1 : 0;
}

/* Sort comparator state: when sort_comp_fn is non-NULL, the merge sort
 * calls the user-supplied comparison function instead of val_compare. */

static int sort_compare(mino_state_t *S, const mino_val_t *a, const mino_val_t *b)
{
    if (S->sort_comp_fn != NULL) {
        mino_val_t *call_args = mino_cons(S, (mino_val_t *)a,
                                  mino_cons(S, (mino_val_t *)b, mino_nil(S)));
        mino_val_t *result = mino_call(S, S->sort_comp_fn, call_args, S->sort_comp_env);
        if (result == NULL) return 0;
        /* Numeric result: use sign directly (compare-style) */
        if (result->type == MINO_INT) {
            return result->as.i < 0 ? -1 : result->as.i > 0 ? 1 : 0;
        }
        if (result->type == MINO_FLOAT) {
            return result->as.f < 0 ? -1 : result->as.f > 0 ? 1 : 0;
        }
        /* Boolean result: true means a < b, false means a >= b */
        return mino_is_truthy(result) ? -1 : 1;
    }
    return val_compare(a, b);
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
        set_error(S, "sort requires one or two arguments");
        return NULL;
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
        set_error(S, "sort requires one or two arguments");
        return NULL;
    }
    if (coll == NULL || coll->type == MINO_NIL) {
        return mino_nil(S);
    }
    /* Collect elements into an array. */
    n_items = 0;
    seq_iter_init(S, &it, coll);
    while (!seq_iter_done(&it)) { n_items++; seq_iter_next(S, &it); }
    if (n_items == 0) return mino_nil(S);
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
        if (tail == NULL) { head = cell; } else { tail->as.cons.cdr = cell; }
        tail = cell;
    }
    return head;
}

mino_val_t *prim_peek(mino_state_t *S, mino_val_t *args, mino_env_t *env)
{
    mino_val_t *coll;
    (void)env;
    if (!mino_is_cons(args) || mino_is_cons(args->as.cons.cdr)) {
        set_error(S, "peek requires one argument");
        return NULL;
    }
    coll = args->as.cons.car;
    if (coll == NULL || coll->type == MINO_NIL) return mino_nil(S);
    if (coll->type == MINO_VECTOR) {
        if (coll->as.vec.len == 0) return mino_nil(S);
        return vec_nth(coll, coll->as.vec.len - 1);
    }
    if (coll->type == MINO_CONS) return coll->as.cons.car;
    {
        char msg[96];
        snprintf(msg, sizeof(msg), "peek: expected a vector or list, got %s",
                 type_tag_str(coll));
        set_error(S, msg);
    }
    return NULL;
}

mino_val_t *prim_pop(mino_state_t *S, mino_val_t *args, mino_env_t *env)
{
    mino_val_t *coll;
    (void)env;
    if (!mino_is_cons(args) || mino_is_cons(args->as.cons.cdr)) {
        set_error(S, "pop requires one argument");
        return NULL;
    }
    coll = args->as.cons.car;
    if (coll == NULL || coll->type == MINO_NIL) {
        return prim_throw_error(S, "pop: cannot pop an empty collection");
    }
    if (coll->type == MINO_VECTOR) {
        if (coll->as.vec.len == 0) {
            return prim_throw_error(S, "pop: cannot pop an empty vector");
        }
        return vec_pop(S, coll);
    }
    if (coll->type == MINO_CONS) return coll->as.cons.cdr;
    {
        char msg[96];
        snprintf(msg, sizeof(msg), "pop: expected a vector or list, got %s",
                 type_tag_str(coll));
        set_error(S, msg);
    }
    return NULL;
}

mino_val_t *prim_find(mino_state_t *S, mino_val_t *args, mino_env_t *env)
{
    mino_val_t *m;
    mino_val_t *k;
    mino_val_t *v;
    mino_val_t *kv[2];
    (void)env;
    if (!mino_is_cons(args) || !mino_is_cons(args->as.cons.cdr)
        || mino_is_cons(args->as.cons.cdr->as.cons.cdr)) {
        set_error(S, "find requires two arguments");
        return NULL;
    }
    m = args->as.cons.car;
    k = args->as.cons.cdr->as.cons.car;
    if (m == NULL || m->type == MINO_NIL) return mino_nil(S);
    if (m->type != MINO_MAP) {
        set_error(S, "find: first argument must be a map");
        return NULL;
    }
    v = map_get_val(m, k);
    if (v == NULL) return mino_nil(S);
    kv[0] = k;
    kv[1] = v;
    return mino_vector(S, kv, 2);
}
