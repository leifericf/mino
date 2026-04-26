/*
 * val.c -- value constructors, predicates, accessors, and equality.
 */

#include "runtime/internal.h"

/* ------------------------------------------------------------------------- */
/* Singletons                                                                */
/* ------------------------------------------------------------------------- */

mino_val_t *mino_nil(mino_state_t *S)
{
    return &S->nil_singleton;
}

mino_val_t *mino_true(mino_state_t *S)
{
    return &S->true_singleton;
}

mino_val_t *mino_false(mino_state_t *S)
{
    return &S->false_singleton;
}

/* ------------------------------------------------------------------------- */
/* Constructors                                                              */
/* ------------------------------------------------------------------------- */

mino_val_t *mino_int(mino_state_t *S, long long n)
{
    mino_val_t *v;
    if (n >= MINO_SMALL_INT_LO && n <= MINO_SMALL_INT_HI) {
        return &S->small_ints[n - MINO_SMALL_INT_LO];
    }
    v = alloc_val(S, MINO_INT);
    v->as.i = n;
    return v;
}

mino_val_t *mino_float(mino_state_t *S, double f)
{
    mino_val_t *v = alloc_val(S, MINO_FLOAT);
    v->as.f = f;
    return v;
}

mino_val_t *mino_char(mino_state_t *S, int codepoint)
{
    mino_val_t *v = alloc_val(S, MINO_CHAR);
    v->as.ch = codepoint;
    return v;
}

mino_val_t *mino_string_n(mino_state_t *S, const char *s, size_t len)
{
    /* Allocate the data buffer first so alloc_val runs last; if a
     * minor collection fires between the two allocations, v is the
     * younger of the two and the store is a safe YOUNG->anything. */
    char       *data = dup_n(S, s, len);
    mino_val_t *v    = alloc_val(S, MINO_STRING);
    v->as.s.data = data;
    v->as.s.len  = len;
    return v;
}

mino_val_t *mino_string(mino_state_t *S, const char *s)
{
    return mino_string_n(S, s, strlen(s));
}

/*
 * Symbols and keywords are interned through tables with an open-addressing
 * hash index for O(1) lookup. The flat entries[] array is kept for GC marking.
 * Entries live for the life of the process.
 */

#define INTERN_HT_EMPTY SIZE_MAX
#define INTERN_HT_INIT  64
#define INTERN_HT_LOAD  75  /* percent */

static uint32_t intern_hash(const char *s, size_t len)
{
    return fnv_bytes(2166136261u, (const unsigned char *)s, len);
}

static void intern_ht_rebuild(intern_table_t *tbl, size_t new_ht_cap)
{
    size_t i;
    size_t mask = new_ht_cap - 1;
    size_t *buckets = (size_t *)malloc(new_ht_cap * sizeof(*buckets));
    if (buckets == NULL) return;  /* caller handles gracefully */
    for (i = 0; i < new_ht_cap; i++) buckets[i] = INTERN_HT_EMPTY;
    for (i = 0; i < tbl->len; i++) {
        mino_val_t *e = tbl->entries[i];
        uint32_t h = intern_hash(e->as.s.data, e->as.s.len);
        size_t idx = h & mask;
        while (buckets[idx] != INTERN_HT_EMPTY) idx = (idx + 1) & mask;
        buckets[idx] = i;
    }
    free(tbl->ht_buckets);
    tbl->ht_buckets = buckets;
    tbl->ht_cap = new_ht_cap;
}

mino_val_t *intern_lookup_or_create(mino_state_t *S, intern_table_t *tbl,
                                           mino_type_t type,
                                           const char *s, size_t len)
{
    uint32_t h;
    size_t mask, idx;
    mino_val_t *v;

    /* Bootstrap: build hash table on first call. */
    if (tbl->ht_buckets == NULL) {
        size_t init_cap = INTERN_HT_INIT;
        while (init_cap < tbl->len * 2) init_cap *= 2;
        intern_ht_rebuild(tbl, init_cap);
    }

    h = intern_hash(s, len);
    mask = tbl->ht_cap - 1;
    idx = h & mask;

    /* Probe for existing entry. */
    while (tbl->ht_buckets[idx] != INTERN_HT_EMPTY) {
        mino_val_t *e = tbl->entries[tbl->ht_buckets[idx]];
        if (e->as.s.len == len && memcmp(e->as.s.data, s, len) == 0) {
            return e;
        }
        idx = (idx + 1) & mask;
    }

    /* Not found — grow entries array if needed. */
    if (tbl->len == tbl->cap) {
        size_t new_cap = tbl->cap == 0 ? 64 : tbl->cap * 2;
        mino_val_t **ne = (mino_val_t **)realloc(
            tbl->entries, new_cap * sizeof(*ne));
        if (ne == NULL) {
            set_eval_diag(S, S->eval_current_form, "internal", "MIN001", "out of memory");
            return NULL;
        }
        tbl->entries = ne;
        tbl->cap = new_cap;
    }

    /* Allocate data first so the second (smaller, fixed-size) alloc
     * for v runs last. If v were allocated first, a collection
     * triggered by dup_n's alloc could promote v to OLD before data
     * exists, turning the data-store below into an unbarriered
     * OLD-to-YOUNG write that minor would not see. */
    {
        char *data = dup_n(S, s, len);
        v = alloc_val(S, type);
        v->as.s.data = data;
        v->as.s.len  = len;
    }
    tbl->entries[tbl->len] = v;

    /* Insert index into hash table. */
    tbl->ht_buckets[idx] = tbl->len;
    tbl->len++;

    /* Rehash if load exceeds threshold. */
    if (tbl->len * 100 > tbl->ht_cap * INTERN_HT_LOAD) {
        intern_ht_rebuild(tbl, tbl->ht_cap * 2);
    }

    return v;
}

mino_val_t *mino_symbol_n(mino_state_t *S, const char *s, size_t len)
{
    return intern_lookup_or_create(S, &S->sym_intern, MINO_SYMBOL, s, len);
}

mino_val_t *mino_symbol(mino_state_t *S, const char *s)
{
    return mino_symbol_n(S, s, strlen(s));
}

mino_val_t *mino_keyword_n(mino_state_t *S, const char *s, size_t len)
{
    return intern_lookup_or_create(S, &S->kw_intern, MINO_KEYWORD, s, len);
}

mino_val_t *mino_keyword(mino_state_t *S, const char *s)
{
    return mino_keyword_n(S, s, strlen(s));
}

mino_val_t *mino_mk_var(mino_state_t *S, const char *ns, const char *name,
                        mino_val_t *root)
{
    mino_val_t *v = alloc_val(S, MINO_VAR);
    v->as.var.ns      = ns;
    v->as.var.sym     = name;
    v->as.var.root    = root;
    v->as.var.dynamic = 0;
    v->as.var.bound   = 0;
    return v;
}

mino_val_t *mino_cons(mino_state_t *S, mino_val_t *car, mino_val_t *cdr)
{
    mino_val_t *v = alloc_val(S, MINO_CONS);
    v->as.cons.car = car;
    v->as.cons.cdr = cdr;
    return v;
}


/* ------------------------------------------------------------------------- */
/* Predicates and accessors                                                  */
/* ------------------------------------------------------------------------- */

int mino_is_nil(const mino_val_t *v)
{
    return v == NULL || v->type == MINO_NIL;
}

int mino_is_truthy(const mino_val_t *v)
{
    if (v == NULL) {
        return 0;
    }
    if (v->type == MINO_NIL) {
        return 0;
    }
    if (v->type == MINO_BOOL) {
        return v->as.b != 0;
    }
    return 1;
}

int mino_is_cons(const mino_val_t *v)
{
    return v != NULL && v->type == MINO_CONS;
}

mino_val_t *mino_car(const mino_val_t *v)
{
    if (!mino_is_cons(v)) {
        return NULL;
    }
    return v->as.cons.car;
}

mino_val_t *mino_cdr(const mino_val_t *v)
{
    if (!mino_is_cons(v)) {
        return NULL;
    }
    return v->as.cons.cdr;
}

size_t mino_length(const mino_val_t *list)
{
    size_t n = 0;
    while (mino_is_cons(list)) {
        n++;
        list = list->as.cons.cdr;
    }
    return n;
}

int mino_to_int(const mino_val_t *v, long long *out)
{
    if (v == NULL || v->type != MINO_INT) {
        return 0;
    }
    if (out != NULL) {
        *out = v->as.i;
    }
    return 1;
}

int mino_to_float(const mino_val_t *v, double *out)
{
    if (v == NULL || v->type != MINO_FLOAT) {
        return 0;
    }
    if (out != NULL) {
        *out = v->as.f;
    }
    return 1;
}

int mino_to_string(const mino_val_t *v, const char **out, size_t *len)
{
    if (v == NULL || v->type != MINO_STRING) {
        return 0;
    }
    if (out != NULL) {
        *out = v->as.s.data;
    }
    if (len != NULL) {
        *len = v->as.s.len;
    }
    return 1;
}

int mino_to_bool(const mino_val_t *v)
{
    return mino_is_truthy(v);
}


/* ------------------------------------------------------------------------- */
/* Equality                                                                  */
/* ------------------------------------------------------------------------- */

/* Forward declarations. */
int mino_eq(const mino_val_t *a, const mino_val_t *b);
int mino_eq_force(mino_state_t *S, const mino_val_t *a, const mino_val_t *b);
static int eq_seq_like_force(mino_state_t *S, const mino_val_t *a,
                           const mino_val_t *b);

/*
 * Check whether a type is sequential (list, vector, nil, or lazy-seq).
 */
static int is_sequential(mino_type_t t)
{
    return (t == MINO_CONS || t == MINO_VECTOR || t == MINO_NIL
            || t == MINO_LAZY);
}

/*
 * Resolve a value: if it is a realized lazy seq, return the cached form.
 * Does NOT force unrealized lazy seqs (that requires the state).
 */
static const mino_val_t *resolve_lazy(const mino_val_t *v)
{
    while (v != NULL && v->type == MINO_LAZY && v->as.lazy.realized) {
        v = v->as.lazy.cached;
    }
    return v;
}

/*
 * Compare two sequential values element-by-element (non-forcing version).
 * Handles cons lists, vectors, nil, and realized lazy seqs.
 * Returns 1 if they contain the same elements in the same order.
 */
static int eq_seq_like(const mino_val_t *a, const mino_val_t *b)
{
    const mino_val_t *ca = resolve_lazy(a);
    const mino_val_t *cb = resolve_lazy(b);
    size_t ia = 0, ib = 0;

    for (;;) {
        const mino_val_t *ea;
        const mino_val_t *eb;
        int a_end;
        int b_end;

        ca = resolve_lazy(ca);
        cb = resolve_lazy(cb);

        a_end = (ca == NULL || ca->type == MINO_NIL
                 || (ca->type == MINO_VECTOR && ia >= ca->as.vec.len)
                 || ca->type == MINO_LAZY /* unrealized */);
        b_end = (cb == NULL || cb->type == MINO_NIL
                 || (cb->type == MINO_VECTOR && ib >= cb->as.vec.len)
                 || cb->type == MINO_LAZY /* unrealized */);

        if (a_end && b_end) return 1;
        if (a_end || b_end) return 0;

        if (ca->type == MINO_CONS) ea = ca->as.cons.car;
        else ea = vec_nth(ca, ia);

        if (cb->type == MINO_CONS) eb = cb->as.cons.car;
        else eb = vec_nth(cb, ib);

        if (!mino_eq(ea, eb)) return 0;

        if (ca->type == MINO_CONS) ca = ca->as.cons.cdr;
        else ia++;

        if (cb->type == MINO_CONS) cb = cb->as.cons.cdr;
        else ib++;
    }
}

/*
 * Cross-type map equality: compare MINO_MAP with MINO_SORTED_MAP.
 * Both must have the same length and identical key-value pairs.
 */
static int eq_map_like_cross(const mino_val_t *a, const mino_val_t *b)
{
    const mino_val_t *hmap, *smap;
    size_t i;

    /* Normalize: hmap is the HAMT map, smap is the sorted map. */
    if (a->type == MINO_MAP) { hmap = a; smap = b; }
    else                     { hmap = b; smap = a; }

    if (hmap->as.map.len != smap->as.sorted.len) return 0;

    /* Custom comparators need a state for eval; skip cross-type equality. */
    if (smap->as.sorted.comparator != NULL) return 0;

    /* Iterate HAMT entries, look each up in the sorted map. */
    for (i = 0; i < hmap->as.map.len; i++) {
        mino_val_t *key = vec_nth(hmap->as.map.key_order, i);
        mino_val_t *hv  = map_get_val(hmap, key);
        mino_val_t *sv  = rb_get(NULL, smap->as.sorted.root, key, NULL);
        if (sv == NULL || !mino_eq(hv, sv)) return 0;
    }
    return 1;
}

/*
 * Cross-type set equality: compare MINO_SET with MINO_SORTED_SET.
 */
static int eq_set_like_cross(const mino_val_t *a, const mino_val_t *b)
{
    const mino_val_t *hset, *sset;
    size_t i;

    if (a->type == MINO_SET) { hset = a; sset = b; }
    else                     { hset = b; sset = a; }

    if (hset->as.set.len != sset->as.sorted.len) return 0;

    if (sset->as.sorted.comparator != NULL) return 0;

    for (i = 0; i < hset->as.set.len; i++) {
        mino_val_t *elem = vec_nth(hset->as.set.key_order, i);
        if (!rb_contains(NULL, sset->as.sorted.root, elem, NULL)) {
            return 0;
        }
    }
    return 1;
}

/*
 * Equal-implies-equal-hash invariant: every pair (a, b) for which this
 * function returns 1 MUST satisfy hash_val(a) == hash_val(b). The
 * grouped helpers above (eq_seq_like, eq_map_like_cross, eq_set_like_cross)
 * each have a matching branch in hash_val that funnels through the
 * same XOR-fold or per-tag byte loop so equal pairs hash identically.
 *
 * The cross-tier numeric tier (int / integral float / fits-in-ll bigint)
 * funnels through hash_long_long_bytes under tag 0x03 in hash_val for
 * the same reason: (= 1 1.0 1N) is true, so all three must hash the
 * same. New tier additions or new equality bridges must extend the
 * matching branch in hash_val in the same commit.
 */
int mino_eq(const mino_val_t *a, const mino_val_t *b)
{
    if (a == b) {
        return 1;
    }
    if (a == NULL || b == NULL) {
        return 0;
    }
    /* Force lazy seqs before comparison (use cached value if realized). */
    if (a->type == MINO_LAZY && a->as.lazy.realized) a = a->as.lazy.cached;
    if (b->type == MINO_LAZY && b->as.lazy.realized) b = b->as.lazy.cached;
    if (a == NULL || b == NULL) {
        return mino_is_nil(a) && mino_is_nil(b);
    }
    if (a == b) return 1;
    if (a->type != b->type) {
        /*
         * Cross-tier integer equality: int and bigint represent the
         * same arbitrary-precision integer kind, and Clojure treats
         * them as `=` when they hold the same value (`(= 1 1N)` is
         * true). Other tier combinations — int/float, ratio/float,
         * bigdec/anything-else — are NOT equal under `=`; use `==`
         * for cross-tier numeric equality.
         */
        if (a->type == MINO_INT && b->type == MINO_BIGINT) {
            return mino_bigint_equals_ll(b, a->as.i);
        }
        if (a->type == MINO_BIGINT && b->type == MINO_INT) {
            return mino_bigint_equals_ll(a, b->as.i);
        }
        /*
         * Cross-type sequential equality: cons, vector, and nil compare
         * element-wise.  Matches Clojure where (= '(1 2) [1 2]) is true.
         */
        {
            int a_seq = (a->type == MINO_CONS || a->type == MINO_VECTOR
                         || a->type == MINO_NIL || a->type == MINO_LAZY);
            int b_seq = (b->type == MINO_CONS || b->type == MINO_VECTOR
                         || b->type == MINO_NIL || b->type == MINO_LAZY);
            if (a_seq && b_seq) {
                return eq_seq_like(a, b);
            }
        }
        /*
         * Cross-type map equality: sorted-map and map compare by entries.
         */
        {
            int a_map = (a->type == MINO_MAP || a->type == MINO_SORTED_MAP);
            int b_map = (b->type == MINO_MAP || b->type == MINO_SORTED_MAP);
            if (a_map && b_map) {
                return eq_map_like_cross(a, b);
            }
        }
        /*
         * Cross-type set equality: sorted-set and set compare by elements.
         */
        {
            int a_set = (a->type == MINO_SET || a->type == MINO_SORTED_SET);
            int b_set = (b->type == MINO_SET || b->type == MINO_SORTED_SET);
            if (a_set && b_set) {
                return eq_set_like_cross(a, b);
            }
        }
        return 0;
    }
    switch (a->type) {
    case MINO_NIL:
        return 1;
    case MINO_BOOL:
        return a->as.b == b->as.b;
    case MINO_INT:
        return a->as.i == b->as.i;
    case MINO_FLOAT:
        return a->as.f == b->as.f;
    case MINO_CHAR:
        return a->as.ch == b->as.ch;
    case MINO_STRING:
    case MINO_SYMBOL:
    case MINO_KEYWORD:
        return a->as.s.len == b->as.s.len
            && memcmp(a->as.s.data, b->as.s.data, a->as.s.len) == 0;
    case MINO_CONS:
        return mino_eq(a->as.cons.car, b->as.cons.car)
            && mino_eq(a->as.cons.cdr, b->as.cons.cdr);
    case MINO_VECTOR: {
        size_t i;
        if (a->as.vec.len != b->as.vec.len) {
            return 0;
        }
        for (i = 0; i < a->as.vec.len; i++) {
            if (!mino_eq(vec_nth(a, i), vec_nth(b, i))) {
                return 0;
            }
        }
        return 1;
    }
    case MINO_MAP: {
        /* Map equality ignores iteration order: same key set with the same
         * values, regardless of when each was inserted. */
        size_t i;
        if (a->as.map.len != b->as.map.len) {
            return 0;
        }
        for (i = 0; i < a->as.map.len; i++) {
            mino_val_t *key = vec_nth(a->as.map.key_order, i);
            mino_val_t *bv  = map_get_val(b, key);
            mino_val_t *av  = map_get_val(a, key);
            if (bv == NULL) {
                return 0;
            }
            if (!mino_eq(av, bv)) {
                return 0;
            }
        }
        return 1;
    }
    case MINO_SET: {
        /* Set equality: same elements regardless of insertion order. */
        size_t i;
        if (a->as.set.len != b->as.set.len) {
            return 0;
        }
        for (i = 0; i < a->as.set.len; i++) {
            mino_val_t *elem = vec_nth(a->as.set.key_order, i);
            uint32_t    h    = hash_val(elem);
            if (hamt_get(b->as.set.root, elem, h, 0u) == NULL) {
                return 0;
            }
        }
        return 1;
    }
    case MINO_PRIM:
        return a->as.prim.fn == b->as.prim.fn;
    case MINO_FN:
    case MINO_MACRO:
        /* Callables compare by identity. Structural equality on bodies and
         * captured environments is neither cheap nor especially meaningful. */
        return a == b;
    case MINO_HANDLE:
        return a->as.handle.ptr == b->as.handle.ptr;
    case MINO_ATOM:
        return a == b;
    case MINO_LAZY:
        /* Should not reach here — lazy seqs are forced above. */
        return 0;
    case MINO_SORTED_MAP:
    case MINO_SORTED_SET:
        /* Structural comparison: same length + identical tree structure. */
        if (a->as.sorted.len != b->as.sorted.len) return 0;
        return rb_trees_equal(a->as.sorted.root, b->as.sorted.root,
                              a->type == MINO_SORTED_MAP);
    case MINO_RECUR:
        return a == b;
    case MINO_TAIL_CALL:
        return a == b;
    case MINO_REDUCED:
        return a == b;
    case MINO_VAR:
        return a == b; /* identity equality */
    case MINO_TRANSIENT:
        return a == b; /* identity equality; transients are mutable */
    case MINO_BIGINT:
        return mino_bigint_equals(a, b);
    case MINO_RATIO:
        return mino_ratio_equals(a, b);
    case MINO_BIGDEC:
        return mino_bigdec_equals(a, b);
    }
    return 0;
}

/*
 * Compare two sequential values element-by-element, forcing lazy seqs.
 */
static int eq_seq_like_force(mino_state_t *S, const mino_val_t *a,
                           const mino_val_t *b)
{
    const mino_val_t *ca = a;
    const mino_val_t *cb = b;
    size_t ia = 0, ib = 0;

    for (;;) {
        const mino_val_t *ea;
        const mino_val_t *eb;
        int a_end;
        int b_end;

        /* Force lazy seqs */
        if (ca != NULL && ca->type == MINO_LAZY)
            ca = lazy_force(S, (mino_val_t *)ca);
        if (cb != NULL && cb->type == MINO_LAZY)
            cb = lazy_force(S, (mino_val_t *)cb);

        a_end = (ca == NULL || ca->type == MINO_NIL
                 || (ca->type == MINO_VECTOR && ia >= ca->as.vec.len));
        b_end = (cb == NULL || cb->type == MINO_NIL
                 || (cb->type == MINO_VECTOR && ib >= cb->as.vec.len));

        if (a_end && b_end) return 1;
        if (a_end || b_end) return 0;

        if (ca->type == MINO_CONS) ea = ca->as.cons.car;
        else ea = vec_nth(ca, ia);

        if (cb->type == MINO_CONS) eb = cb->as.cons.car;
        else eb = vec_nth(cb, ib);

        if (!mino_eq_force(S, ea, eb)) return 0;

        if (ca->type == MINO_CONS) ca = ca->as.cons.cdr;
        else ia++;

        if (cb->type == MINO_CONS) cb = cb->as.cons.cdr;
        else ib++;
    }
}

int mino_eq_force(mino_state_t *S, const mino_val_t *a, const mino_val_t *b)
{
    if (a != NULL && a->type == MINO_LAZY)
        a = lazy_force(S, (mino_val_t *)a);
    if (b != NULL && b->type == MINO_LAZY)
        b = lazy_force(S, (mino_val_t *)b);
    if (a == b) return 1;
    if (a == NULL || b == NULL) return mino_is_nil(a) && mino_is_nil(b);
    /* For cons cells, recursively force lazy tails. */
    if (a->type == MINO_CONS && b->type == MINO_CONS) {
        return mino_eq_force(S, a->as.cons.car, b->as.cons.car)
            && mino_eq_force(S, a->as.cons.cdr, b->as.cons.cdr);
    }
    /* Cross-type sequential: cons vs vector, nil vs vector, etc. */
    if (a->type != b->type && is_sequential(a->type) && is_sequential(b->type)) {
        /* Force any remaining lazy seqs in elements during comparison. */
        return eq_seq_like_force(S, a, b);
    }
    /* Vectors: compare elements with forcing. */
    if (a->type == MINO_VECTOR && b->type == MINO_VECTOR) {
        size_t i;
        if (a->as.vec.len != b->as.vec.len) return 0;
        for (i = 0; i < a->as.vec.len; i++) {
            if (!mino_eq_force(S, vec_nth(a, i), vec_nth(b, i))) return 0;
        }
        return 1;
    }
    /* Maps and sets: delegate to mino_eq (no lazy forcing needed inside). */
    return mino_eq(a, b);
}
