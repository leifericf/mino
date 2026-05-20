/*
 * val.c -- value constructors, predicates, accessors, and equality.
 */

#include "runtime/internal.h"

/* ------------------------------------------------------------------------- */
/* Singletons                                                                */
/* ------------------------------------------------------------------------- */

mino_val *mino_nil(mino_state *S)
{
    (void)S;
    return MINO_MAKE_NIL;
}

mino_val *mino_true(mino_state *S)
{
    (void)S;
    return MINO_MAKE_BOOL(1);
}

mino_val *mino_false(mino_state *S)
{
    (void)S;
    return MINO_MAKE_BOOL(0);
}

mino_val *mino_empty_list(mino_state *S)
{
    return &S->empty_list_singleton;
}

/* ------------------------------------------------------------------------- */
/* Constructors                                                              */
/* ------------------------------------------------------------------------- */

mino_val *mino_int(mino_state *S, long long n)
{
    mino_val *v;
#ifdef MINO_BC_PROFILE_COUNTS
    S->bc.bc_int_make_count++;
#endif
    /* Inline-tag every int that fits in the 61-bit signed range. The
     * fallback below handles the narrow band between MINO_INT_MAX and
     * LLONG_MAX where the tag would lose precision. With MINO_CAP_BIGNUM
     * installed, that overflow promotes to a MINO_BIGINT so callers can
     * keep using the tower transparently; without it, the value is
     * boxed as a MINO_INT carrying the full 64-bit signed integer. */
    if (n >= MINO_INT_MIN && n <= MINO_INT_MAX) {
#ifdef MINO_BC_PROFILE_COUNTS
        S->bc.bc_int_alloc_avoided++;
#endif
        return MINO_MAKE_INT(n);
    }
    if (mino_capability_installed(S, MINO_CAP_BIGNUM)) {
        return mino_bigint_from_ll(S, n);
    }
    v = alloc_val(S, MINO_INT);
    v->as.i = n;
    return v;
}

mino_val *mino_int_wrap(mino_state *S, long long n)
{
#ifdef MINO_BC_PROFILE_COUNTS
    S->bc.bc_int_make_count++;
#endif
    if (n >= MINO_INT_MIN && n <= MINO_INT_MAX) {
#ifdef MINO_BC_PROFILE_COUNTS
        S->bc.bc_int_alloc_avoided++;
#endif
        return MINO_MAKE_INT(n);
    }
    {
        mino_val *v = alloc_val(S, MINO_INT);
        v->as.i = n;
        return v;
    }
}

mino_val *mino_float(mino_state *S, double f)
{
    mino_val *v = alloc_val(S, MINO_FLOAT);
    v->as.f = f;
    return v;
}

mino_val *mino_float32(mino_state *S, double f)
{
    mino_val *v = alloc_val(S, MINO_FLOAT32);
    /* Narrow precision via the hardware float cast so the stored
     * double sees the rounding -- equality with another float32 then
     * compares values bit-equivalent to a Java float. NaN passes
     * through unchanged. */
    v->as.f = f != f ? f : (double)(float)f;
    return v;
}

mino_val *mino_char(mino_state *S, int codepoint)
{
    (void)S;
    return MINO_MAKE_CHAR(codepoint);
}

mino_val *mino_string_n(mino_state *S, const char *s, size_t len)
{
    /* Allocate the data buffer first so alloc_val runs last; if a
     * minor collection fires between the two allocations, v is the
     * younger of the two and the store is a safe YOUNG->anything. */
    char       *data = dup_n(S, s, len);
    mino_val *v    = alloc_val(S, MINO_STRING);
    v->as.s.data = data;
    v->as.s.len  = len;
    return v;
}

mino_val *mino_string(mino_state *S, const char *s)
{
    return mino_string_n(S, s, strlen(s));
}

/*
 * Symbols and keywords are interned through tables with an open-addressing
 * hash index for O(1) lookup. The flat entries[] array is kept for GC marking.
 * Entries live for the life of the process.
 */

#define INTERN_HT_INIT      64
#define INTERN_HT_LOAD      75  /* percent */

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
    /* Rebuild skips NULL entries (tombstoned slots whose underlying
     * sym/keyword header was swept). The rebuild compacts away the
     * tombstones; subsequent inserts append at tbl->len. */
    for (i = 0; i < tbl->len; i++) {
        mino_val *e = tbl->entries[i];
        uint32_t h;
        size_t idx;
        if (e == NULL) continue;
        h = intern_hash(e->as.s.data, e->as.s.len);
        idx = h & mask;
        while (buckets[idx] != INTERN_HT_EMPTY) idx = (idx + 1) & mask;
        buckets[idx] = i;
    }
    free(tbl->ht_buckets);
    tbl->ht_buckets = buckets;
    tbl->ht_cap = new_ht_cap;
}

/* Forward decl; defined below. */
mino_val *intern_lookup_or_create_ns(mino_state *S, intern_table_t *tbl,
                                              mino_type type,
                                              const char *s, size_t len,
                                              size_t ns_len_hint);

mino_val *intern_lookup_or_create(mino_state *S, intern_table_t *tbl,
                                           mino_type type,
                                           const char *s, size_t len)
{
    return intern_lookup_or_create_ns(S, tbl, type, s, len, (size_t)-1);
}

mino_val *intern_lookup_or_create_ns(mino_state *S, intern_table_t *tbl,
                                              mino_type type,
                                              const char *s, size_t len,
                                              size_t ns_len_hint)
{
    uint32_t h;
    size_t mask, idx;
    mino_val *v;
    size_t ns_len;

    /* Caller-must-hold-state_lock contract: the intern table is shared
     * across host worker threads, but its open-addressing hash, the
     * append-only entries[] array, and the GC-coupled allocations
     * inside the insert path all assume serialized writers. Surface
     * a missing lock at the call site (debug builds) instead of
     * letting a torn entries[] cell escape into production. */
    MINO_ASSERT_STATE_SAFE(S);

    /* ns_len resolution: callers that constructed via 2-arg (keyword
     * ns name) pass an explicit `ns_len_hint`. Single-string callers
     * pass (size_t)-1; we derive ns_len from the LAST '/' in `s` so a
     * read-back form like `:a/b/c` parses ns="a/b", name="c". Strings
     * always carry ns_len=0. */
    if (type == MINO_STRING) {
        ns_len = 0;
    } else if (ns_len_hint != (size_t)-1) {
        ns_len = ns_len_hint;
    } else {
        size_t i;
        ns_len = 0;
        if (len > 1) {
            for (i = 0; i < len; i++) {
                if (s[i] == '/') ns_len = i;
            }
        }
    }

    /* Bootstrap: build hash table on first call. */
    if (tbl->ht_buckets == NULL) {
        size_t init_cap = INTERN_HT_INIT;
        while (init_cap < tbl->len * 2) init_cap *= 2;
        intern_ht_rebuild(tbl, init_cap);
    }

    h = intern_hash(s, len);
    mask = tbl->ht_cap - 1;
    idx = h & mask;

    /* Probe for existing entry. Match (data, len, ns_len) so that
     * `(keyword "a/b" "c")` and `(keyword "a" "b/c")` produce
     * distinct vals even though their flat string is identical.
     * Tombstone slots (entries[bucket] == NULL after a sweep cleared
     * the underlying header) are skipped during lookup but remembered
     * so the new insert below can reuse them instead of appending. */
    size_t first_tombstone = INTERN_HT_EMPTY;
    while (tbl->ht_buckets[idx] != INTERN_HT_EMPTY) {
        if (tbl->ht_buckets[idx] == INTERN_HT_TOMBSTONE) {
            if (first_tombstone == INTERN_HT_EMPTY) first_tombstone = idx;
            idx = (idx + 1) & mask;
            continue;
        }
        mino_val *e = tbl->entries[tbl->ht_buckets[idx]];
        if (e != NULL
            && e->as.s.len == len
            && e->as.s.ns_len == ns_len
            && memcmp(e->as.s.data, s, len) == 0) {
            return e;
        }
        idx = (idx + 1) & mask;
    }

    /* Not found — grow entries array if needed. */
    if (tbl->len == tbl->cap) {
        size_t new_cap = tbl->cap == 0 ? 64 : tbl->cap * 2;
        mino_val **ne = (mino_val **)realloc(
            tbl->entries, new_cap * sizeof(*ne));
        if (ne == NULL) {
            set_eval_diag(S, mino_current_ctx(S)->eval_current_form, "internal", "MIN001", "out of memory");
            return NULL;
        }
        tbl->entries = ne;
        tbl->cap = new_cap;
    }

    /* Allocate data first so the second (smaller, fixed-size) alloc
     * for v runs last. If v were allocated first, a collection
     * triggered by dup_n's alloc could promote v to OLD before data
     * exists, turning the data-store below into an unbarriered
     * OLD-to-YOUNG write that minor would not see.
     *
     * Suppress collection across the alloc pair: data is GC-
     * allocated by dup_n but not yet referenced from a rooted val,
     * and the conservative stack scan can miss the local `data`
     * variable when the optimizer keeps it in a register. ASan
     * caught this freed under load; gc_depth is the reliable
     * protection. */
    mino_current_ctx(S)->gc_depth++;
    {
        char *data = dup_n(S, s, len);
        v = alloc_val(S, type);
        v->as.s.data   = data;
        v->as.s.len    = len;
        v->as.s.hash   = h;
        v->as.s.ns_len = ns_len;
    }
    mino_current_ctx(S)->gc_depth--;
    tbl->entries[tbl->len] = v;

    /* Insert index into hash table. Prefer the first tombstone slot
     * along the probe chain if one was seen; otherwise land on the
     * empty slot the lookup probe stopped at. */
    {
        size_t target_bucket = (first_tombstone != INTERN_HT_EMPTY)
            ? first_tombstone : idx;
        tbl->ht_buckets[target_bucket] = tbl->len;
    }
    tbl->len++;

    /* Rehash if load exceeds threshold. */
    if (tbl->len * 100 > tbl->ht_cap * INTERN_HT_LOAD) {
        intern_ht_rebuild(tbl, tbl->ht_cap * 2);
    }

    return v;
}

mino_val *mino_symbol_n(mino_state *S, const char *s, size_t len)
{
    return intern_lookup_or_create(S, &S->sym_intern, MINO_SYMBOL, s, len);
}

mino_val *mino_symbol(mino_state *S, const char *s)
{
    return mino_symbol_n(S, s, strlen(s));
}

mino_val *mino_keyword_n(mino_state *S, const char *s, size_t len)
{
    return intern_lookup_or_create(S, &S->kw_intern, MINO_KEYWORD, s, len);
}

mino_val *mino_keyword(mino_state *S, const char *s)
{
    return mino_keyword_n(S, s, strlen(s));
}

/* Explicit 2-arg constructors: the caller passes ns and name as two
 * strings; we record the boundary so name/namespace can recover them
 * exactly. (keyword "a/b" "c") interns ns_len=3 even though the flat
 * data ("a/b/c") would otherwise be interpreted with last-slash split
 * (which would also give ns_len=3 here — but for (keyword "a" "b/c")
 * the explicit boundary preserves ns_len=1 and distinguishes the two
 * keywords). */
mino_val *mino_keyword_ns_n(mino_state *S,
                              const char *ns, size_t ns_len,
                              const char *name, size_t name_len)
{
    if (ns == NULL || ns_len == 0) {
        return intern_lookup_or_create_ns(S, &S->kw_intern, MINO_KEYWORD,
                                          name, name_len, 0);
    }
    {
        size_t total = ns_len + 1 + name_len;
        char  *buf;
        mino_val *v;
        if (total < 256) {
            char stack_buf[256];
            buf = stack_buf;
            memcpy(buf, ns, ns_len);
            buf[ns_len] = '/';
            memcpy(buf + ns_len + 1, name, name_len);
            v = intern_lookup_or_create_ns(S, &S->kw_intern, MINO_KEYWORD,
                                           buf, total, ns_len);
            return v;
        }
        buf = (char *)malloc(total);
        memcpy(buf, ns, ns_len);
        buf[ns_len] = '/';
        memcpy(buf + ns_len + 1, name, name_len);
        v = intern_lookup_or_create_ns(S, &S->kw_intern, MINO_KEYWORD,
                                       buf, total, ns_len);
        free(buf);
        return v;
    }
}

mino_val *mino_symbol_ns_n(mino_state *S,
                             const char *ns, size_t ns_len,
                             const char *name, size_t name_len)
{
    if (ns == NULL || ns_len == 0) {
        return intern_lookup_or_create_ns(S, &S->sym_intern, MINO_SYMBOL,
                                          name, name_len, 0);
    }
    {
        size_t total = ns_len + 1 + name_len;
        char  *buf;
        mino_val *v;
        if (total < 256) {
            char stack_buf[256];
            buf = stack_buf;
            memcpy(buf, ns, ns_len);
            buf[ns_len] = '/';
            memcpy(buf + ns_len + 1, name, name_len);
            v = intern_lookup_or_create_ns(S, &S->sym_intern, MINO_SYMBOL,
                                           buf, total, ns_len);
            return v;
        }
        buf = (char *)malloc(total);
        memcpy(buf, ns, ns_len);
        buf[ns_len] = '/';
        memcpy(buf + ns_len + 1, name, name_len);
        v = intern_lookup_or_create_ns(S, &S->sym_intern, MINO_SYMBOL,
                                       buf, total, ns_len);
        free(buf);
        return v;
    }
}

mino_val *mino_mk_var(mino_state *S, const char *ns, const char *name,
                        mino_val *root)
{
    mino_val *v = alloc_val(S, MINO_VAR);
    v->as.var.ns      = ns;
    v->as.var.sym     = name;
    v->as.var.root    = root;
    v->as.var.dynamic    = 0;
    v->as.var.bound      = 0;
    v->as.var.is_private = 0;
    v->as.var.watches    = NULL;
    v->as.var.validator  = NULL;
    return v;
}

mino_val *mino_cons(mino_state *S, mino_val *car, mino_val *cdr)
{
    mino_val *v = alloc_val(S, MINO_CONS);
    v->as.cons.car      = car;
    v->as.cons.cdr      = cdr;
    v->as.cons.file     = NULL;
    v->as.cons.line     = 0;
    v->as.cons.column   = 0;
    v->as.cons.not_list = 0;
    return v;
}

mino_val *mino_map_entry(mino_state *S, mino_val *k, mino_val *v)
{
    mino_val *e = alloc_val(S, MINO_MAP_ENTRY);
    e->as.map_entry.k = k;
    e->as.map_entry.v = v;
    return e;
}

/* Construct an STM ref holding the given committed value. The watches
 * map and validator slots start empty; install them via add-watch /
 * set-validator! on the returned cell. */
mino_val *mino_tx_ref(mino_state *S, mino_val *val)
{
    mino_val *v = alloc_val(S, MINO_TX_REF);
    v->as.tx_ref.val          = val;
    v->as.tx_ref.watches      = NULL;
    v->as.tx_ref.validator    = NULL;
    v->as.tx_ref.version      = 0;
    v->as.tx_ref.ref_id       = ++S->stm.next_ref_id;
    v->as.tx_ref.owning_state = S;
    return v;
}

/* ------------------------------------------------------------------------- */
/* Host arrays                                                               */
/* ------------------------------------------------------------------------- */

mino_val *mino_host_array_new(mino_state *S, size_t len,
                                host_array_kind_t kind)
{
    mino_val  *v;
    mino_val **vals = NULL;
    mino_val  *fill;
    size_t       i;
    if (len > 0) {
        vals = (mino_val **)malloc(len * sizeof(*vals));
        if (vals == NULL) return NULL;
    }
    /* Object arrays nil-fill; numeric primitive variants 0-fill;
     * boolean false-fills; char nul-fills. Matches JVM array
     * default initialization. */
    if (kind == HOST_ARRAY_OBJECT) {
        fill = mino_nil(S);
    } else if (kind == HOST_ARRAY_BOOLEAN) {
        fill = mino_false(S);
    } else if (kind == HOST_ARRAY_FLOAT || kind == HOST_ARRAY_DOUBLE) {
        fill = mino_float(S, 0.0);
    } else if (kind == HOST_ARRAY_CHAR) {
        fill = mino_char(S, '\0');
    } else {
        fill = mino_int(S, 0);
    }
    for (i = 0; i < len; i++) vals[i] = fill;
    v = alloc_val(S, MINO_HOST_ARRAY);
    if (v == NULL) {
        free(vals);
        return NULL;
    }
    v->as.host_array.vals         = vals;
    v->as.host_array.len          = len;
    v->as.host_array.element_kind = (unsigned char)kind;
    return v;
}

mino_val *mino_host_array_from_coll(mino_state *S, mino_val *coll,
                                      host_array_kind_t kind)
{
    mino_val  *v;
    mino_val **vals = NULL;
    size_t       len = 0, i;
    /* Vector fast path. */
    if (coll != NULL && mino_type_of(coll) == MINO_VECTOR) {
        len = coll->as.vec.len;
        if (len > 0) {
            vals = (mino_val **)malloc(len * sizeof(*vals));
            if (vals == NULL) return NULL;
            for (i = 0; i < len; i++) vals[i] = vec_nth(coll, i);
        }
        v = alloc_val(S, MINO_HOST_ARRAY);
        if (v == NULL) { free(vals); return NULL; }
        v->as.host_array.vals = vals;
        v->as.host_array.len  = len;
        v->as.host_array.element_kind = (unsigned char)kind;
        return v;
    }
    /* Generic seq path: walk the seq into a temp dynamic buffer. */
    {
        size_t cap = 0;
        mino_val *s = coll;
        while (s != NULL && mino_type_of(s) == MINO_LAZY) s = lazy_force(S, s);
        while (mino_is_cons(s) || (s != NULL && mino_type_of(s) == MINO_CHUNKED_CONS)) {
            mino_val *head;
            if (mino_is_cons(s)) {
                head = s->as.cons.car;
                s    = s->as.cons.cdr;
            } else {
                head = s->as.chunked_cons.chunk
                          ->as.chunk.vals[s->as.chunked_cons.off];
                s    = mino_chunked_cons_advance(S, s);
            }
            while (s != NULL && mino_type_of(s) == MINO_LAZY) s = lazy_force(S, s);
            if (len >= cap) {
                size_t ncap = cap == 0 ? 8 : cap * 2;
                mino_val **nvals = (mino_val **)realloc(vals,
                    ncap * sizeof(*nvals));
                if (nvals == NULL) { free(vals); return NULL; }
                vals = nvals;
                cap  = ncap;
            }
            vals[len++] = head;
        }
        v = alloc_val(S, MINO_HOST_ARRAY);
        if (v == NULL) { free(vals); return NULL; }
        v->as.host_array.vals = vals;
        v->as.host_array.len  = len;
        v->as.host_array.element_kind = (unsigned char)kind;
        return v;
    }
}

/* ------------------------------------------------------------------------- */
/* Record types                                                              */
/* ------------------------------------------------------------------------- */

/*
 * mino_defrecord interns a record type by (ns, name). On re-eval the
 * existing MINO_TYPE pointer is returned so already-built record
 * instances stay (instance? T r) true across script reloads.
 *
 * The registry is a singly-linked list: defrecord is rare on the
 * timescale of script execution, the list stays short in practice
 * (one entry per defrecord form), and walking it is faster than the
 * cache misses a hash table introduces at this scale. If a host
 * registers thousands of types, we revisit.
 */
mino_val *mino_defrecord(mino_state *S,
                           const char *ns,
                           const char *name,
                           const char *const *field_names,
                           size_t n_fields)
{
    record_type_entry_t *e;
    mino_val          *fields_vec;
    mino_val          *type_val;
    const char          *ns_interned;
    const char          *name_interned;
    mino_val          *ns_sym;
    mino_val          *name_sym;

    if (S == NULL || ns == NULL || name == NULL) {
        return NULL;
    }

    /* Caller-must-hold-state_lock contract: the linked record-type
     * registry is shared across host worker threads but mutated with
     * a plain prepend below; concurrent defrecords would race on the
     * head pointer. Surface a missing lock at the call site (debug
     * builds) before the torn list escapes. */
    MINO_ASSERT_STATE_SAFE(S);

    /* Intern ns and name strings via the symbol table so we can compare
     * with pointer equality and the storage outlives any caller-owned
     * buffer. */
    ns_sym         = mino_symbol(S, ns);
    name_sym       = mino_symbol(S, name);
    if (ns_sym == NULL || name_sym == NULL) return NULL;
    ns_interned    = ns_sym->as.s.data;
    name_interned  = name_sym->as.s.data;

    for (e = S->record_types; e != NULL; e = e->next) {
        if (e->ns == ns_interned && e->name == name_interned) {
            return e->type;
        }
    }

    /* Build the fields vector first, then the type cell, then the
     * registry entry. Allocator order is YOUNG-front so a minor
     * collection mid-build cannot drop the fields vector before the
     * type points at it. */
    {
        mino_val **field_kws = NULL;
        size_t       i;
        if (n_fields > 0) {
            field_kws = (mino_val **)malloc(n_fields * sizeof(*field_kws));
            if (field_kws == NULL) return NULL;
            for (i = 0; i < n_fields; i++) {
                field_kws[i] = mino_keyword(S, field_names[i]);
                if (field_kws[i] == NULL) {
                    free(field_kws);
                    return NULL;
                }
            }
        }
        fields_vec = mino_vector(S, field_kws, n_fields);
        free(field_kws);
        if (fields_vec == NULL) return NULL;
    }

    type_val = alloc_val(S, MINO_TYPE);
    if (type_val == NULL) return NULL;
    type_val->as.record_type.ns     = ns_interned;
    type_val->as.record_type.name   = name_interned;
    type_val->as.record_type.fields = fields_vec;

    e = (record_type_entry_t *)malloc(sizeof(*e));
    if (e == NULL) return NULL;
    e->ns   = ns_interned;
    e->name = name_interned;
    e->type = type_val;
    e->next = S->record_types;
    S->record_types = e;

    return type_val;
}

int mino_is_record_type(const mino_val *v)
{
    return v != NULL && mino_type_of(v) == MINO_TYPE;
}

mino_val *mino_record(mino_state *S, mino_val *type,
                        mino_val **vals, size_t n_vals)
{
    mino_val  *v;
    mino_val **slots;
    size_t       expected;
    size_t       i;

    if (S == NULL || type == NULL || mino_type_of(type) != MINO_TYPE) {
        return NULL;
    }
    expected = (type->as.record_type.fields != NULL)
        ? type->as.record_type.fields->as.vec.len : 0;
    if (n_vals != expected) {
        return NULL;
    }

    slots = NULL;
    if (n_vals > 0) {
        slots = (mino_val **)malloc(n_vals * sizeof(*slots));
        if (slots == NULL) return NULL;
        for (i = 0; i < n_vals; i++) slots[i] = vals[i];
    }
    v = alloc_val(S, MINO_RECORD);
    if (v == NULL) {
        free(slots);
        return NULL;
    }
    v->as.record.type = type;
    v->as.record.vals = slots;
    v->as.record.ext  = NULL;
    return v;
}

/* Look up a declared field by index. Keywords are interned, so the
 * field vector's entries and the lookup key share pointer identity
 * when they're the same keyword. The pointer check resolves the hot
 * path; the byte comparison stays as a defensive fallback for any
 * not-yet-interned shape that reaches here. Returns the index in
 * [0, n) or -1 if not found. */
int record_field_index(const mino_val *r, const mino_val *key)
{
    mino_val *fields;
    size_t      i, n;
    if (r == NULL || mino_type_of(r) != MINO_RECORD) return -1;
    if (key == NULL || mino_type_of(key) != MINO_KEYWORD) return -1;
    fields = r->as.record.type->as.record_type.fields;
    if (fields == NULL) return -1;
    n = fields->as.vec.len;
    for (i = 0; i < n; i++) {
        mino_val *fk = vec_nth(fields, i);
        if (fk == key) return (int)i;
    }
    for (i = 0; i < n; i++) {
        mino_val *fk = vec_nth(fields, i);
        if (fk == NULL) continue;
        if (mino_type_of(fk) == mino_type_of(key)
            && fk->as.s.len == key->as.s.len
            && memcmp(fk->as.s.data, key->as.s.data, key->as.s.len) == 0) {
            return (int)i;
        }
    }
    return -1;
}

mino_val *mino_record_field(const mino_val *record, const char *name)
{
    mino_val *fields;
    size_t      i, n, name_len;
    if (record == NULL || mino_type_of(record) != MINO_RECORD || name == NULL) {
        return NULL;
    }
    fields = record->as.record.type->as.record_type.fields;
    if (fields == NULL) return NULL;
    n = fields->as.vec.len;
    name_len = strlen(name);
    for (i = 0; i < n; i++) {
        mino_val *fk = vec_nth(fields, i);
        if (fk == NULL) continue;
        if (fk->as.s.len == name_len
            && memcmp(fk->as.s.data, name, name_len) == 0) {
            return record->as.record.vals[i];
        }
    }
    return NULL;
}

int mino_is_record(const mino_val *v)
{
    return v != NULL && mino_type_of(v) == MINO_RECORD;
}


/* ------------------------------------------------------------------------- */
/* Predicates and accessors                                                  */
/* ------------------------------------------------------------------------- */

int mino_is_nil(const mino_val *v)
{
    return v == NULL || mino_type_of(v) == MINO_NIL;
}

int mino_is_truthy(const mino_val *v)
{
    if (v == NULL) {
        return 0;
    }
    if (mino_type_of(v) == MINO_NIL) {
        return 0;
    }
    if (mino_type_of(v) == MINO_BOOL) {
        return mino_val_bool_get(v) != 0;
    }
    return 1;
}

int mino_is_cons(const mino_val *v)
{
    return v != NULL && mino_type_of(v) == MINO_CONS;
}

int mino_is_empty_list(const mino_val *v)
{
    return v != NULL && mino_type_of(v) == MINO_EMPTY_LIST;
}

mino_val *mino_car(const mino_val *v)
{
    if (!mino_is_cons(v)) {
        return NULL;
    }
    return v->as.cons.car;
}

mino_val *mino_cdr(const mino_val *v)
{
    if (!mino_is_cons(v)) {
        return NULL;
    }
    return v->as.cons.cdr;
}

size_t mino_length(const mino_val *list)
{
    size_t n = 0;
    while (mino_is_cons(list)) {
        n++;
        list = list->as.cons.cdr;
    }
    return n;
}

int mino_to_int(const mino_val *v, long long *out)
{
    if (v == NULL) return 0;
    if (mino_val_int_p(v)) {
        if (out != NULL) *out = mino_val_int_get(v);
        return 1;
    }
    /* Mirror mino_int's auto-promote contract: when bignum is in play
     * a bigint that fits in long long round-trips like a boxed int. */
    if (mino_type_of(v) == MINO_BIGINT) {
        long long tmp = 0;
        if (!mino_as_ll(v, &tmp)) return 0;
        if (out != NULL) *out = tmp;
        return 1;
    }
    return 0;
}

int mino_to_float(const mino_val *v, double *out)
{
    if (v == NULL || mino_type_of(v) != MINO_FLOAT) {
        return 0;
    }
    if (out != NULL) {
        *out = v->as.f;
    }
    return 1;
}

int mino_to_string(const mino_val *v, const char **out, size_t *len)
{
    if (v == NULL || mino_type_of(v) != MINO_STRING) {
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

int mino_to_bool(const mino_val *v)
{
    return mino_is_truthy(v);
}

int mino_to_char(const mino_val *v, int *cp)
{
    if (v == NULL || !mino_val_char_p(v)) {
        return 0;
    }
    if (cp != NULL) {
        *cp = mino_val_char_get(v);
    }
    return 1;
}

int mino_to_keyword(const mino_val *v, const char **out, size_t *len)
{
    if (v == NULL || mino_type_of(v) != MINO_KEYWORD) {
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

int mino_to_symbol(const mino_val *v, const char **out, size_t *len)
{
    if (v == NULL || mino_type_of(v) != MINO_SYMBOL) {
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

int mino_to_float32(const mino_val *v, float *out)
{
    if (v == NULL || mino_type_of(v) != MINO_FLOAT32) return 0;
    if (out != NULL) *out = (float)v->as.f;
    return 1;
}

int mino_to_uuid_bytes(const mino_val *v, uint8_t out[16])
{
    if (v == NULL || mino_type_of(v) != MINO_UUID) return 0;
    if (out != NULL) memcpy(out, v->as.uuid.bytes, 16);
    return 1;
}

int mino_to_regex_source(const mino_val *v, const char **out, size_t *len)
{
    if (v == NULL || mino_type_of(v) != MINO_REGEX) return 0;
    if (v->as.regex.source == NULL
        || mino_type_of(v->as.regex.source) != MINO_STRING) {
        return 0;
    }
    if (out != NULL) *out = v->as.regex.source->as.s.data;
    if (len != NULL) *len = v->as.regex.source->as.s.len;
    return 1;
}

/* Public 32-bit canonical hash. Routes through the internal hash_val
 * helper that the HAMT and ratbset paths share, so a value retrieved
 * via this API hashes identically to one keyed in a collection. */
uint32_t mino_hash(const mino_val *v)
{
    return hash_val(v);
}

/* Forward declaration -- prim_compare lives in prim/numeric.c. We
 * don't pull in prim/internal.h from the values layer (which would
 * invert the layering); a focused extern works here. */
mino_val *prim_compare(mino_state *S, mino_val *args, mino_env *env);

/* Public 3-way compare. Routes through prim_compare via a one-shot
 * cons-spine arg list; prim_compare throws on cross-type compares the
 * runtime can't order. Returns -1 / 0 / 1 on success; in the throw
 * path the function returns 0 and the runtime's last_error carries
 * the diagnostic (same contract as other mino_* functions that
 * route through prim_*). */
int mino_compare(mino_state *S, const mino_val *a, const mino_val *b)
{
    mino_val *args, *r;
    long long n = 0;
    args = mino_cons(S, (mino_val *)b, mino_nil(S));
    args = mino_cons(S, (mino_val *)a, args);
    r = prim_compare(S, args, NULL);
    if (r == NULL) return 0;
    if (mino_to_int(r, &n)) {
        if (n < 0) return -1;
        if (n > 0) return 1;
        return 0;
    }
    return 0;
}

mino_type mino_typeof(const mino_val *v)
{
    return mino_type_of(v);
}

/* Type-predicate grid. Each returns 1 iff v has the given effective
 * type. Tag-aware: works for inline scalars (int, bool, char, nil)
 * as well as boxed cells. NULL is treated as MINO_NIL. */

int mino_is_bool(const mino_val *v)    { return mino_type_of(v) == MINO_BOOL; }
int mino_is_int(const mino_val *v)     { return mino_type_of(v) == MINO_INT; }
int mino_is_float(const mino_val *v)
{
    mino_type t = mino_type_of(v);
    return t == MINO_FLOAT || t == MINO_FLOAT32;
}
int mino_is_char(const mino_val *v)    { return mino_type_of(v) == MINO_CHAR; }
int mino_is_string(const mino_val *v)  { return mino_type_of(v) == MINO_STRING; }
int mino_is_symbol(const mino_val *v)  { return mino_type_of(v) == MINO_SYMBOL; }
int mino_is_keyword(const mino_val *v) { return mino_type_of(v) == MINO_KEYWORD; }
int mino_is_vector(const mino_val *v)  { return mino_type_of(v) == MINO_VECTOR; }
int mino_is_map(const mino_val *v)
{
    mino_type t = mino_type_of(v);
    return t == MINO_MAP || t == MINO_SORTED_MAP;
}
int mino_is_set(const mino_val *v)
{
    mino_type t = mino_type_of(v);
    return t == MINO_SET || t == MINO_SORTED_SET;
}
int mino_is_fn(const mino_val *v)      { return mino_type_of(v) == MINO_FN; }
int mino_is_macro(const mino_val *v)   { return mino_type_of(v) == MINO_MACRO; }
int mino_is_prim(const mino_val *v)    { return mino_type_of(v) == MINO_PRIM; }
int mino_is_lazy(const mino_val *v)    { return mino_type_of(v) == MINO_LAZY; }
int mino_is_var(const mino_val *v)     { return mino_type_of(v) == MINO_VAR; }
int mino_is_bigint(const mino_val *v)  { return mino_type_of(v) == MINO_BIGINT; }
int mino_is_ratio(const mino_val *v)   { return mino_type_of(v) == MINO_RATIO; }
int mino_is_bigdec(const mino_val *v)  { return mino_type_of(v) == MINO_BIGDEC; }
int mino_is_uuid(const mino_val *v)    { return mino_type_of(v) == MINO_UUID; }
int mino_is_regex(const mino_val *v)   { return mino_type_of(v) == MINO_REGEX; }
int mino_is_float32(const mino_val *v) { return mino_type_of(v) == MINO_FLOAT32; }
int mino_is_sorted_map(const mino_val *v) { return mino_type_of(v) == MINO_SORTED_MAP; }
int mino_is_sorted_set(const mino_val *v) { return mino_type_of(v) == MINO_SORTED_SET; }
int mino_is_map_entry(const mino_val *v) { return mino_type_of(v) == MINO_MAP_ENTRY; }
int mino_is_host_array(const mino_val *v) { return mino_type_of(v) == MINO_HOST_ARRAY; }
int mino_is_future(const mino_val *v)  { return mino_type_of(v) == MINO_FUTURE; }


/* ------------------------------------------------------------------------- */
/* Equality                                                                  */
/* ------------------------------------------------------------------------- */

/* Forward declarations. */
int mino_eq(const mino_val *a, const mino_val *b);
int mino_eq_force(mino_state *S, const mino_val *a, const mino_val *b);
static int eq_seq_like_force(mino_state *S, const mino_val *a,
                           const mino_val *b);

/*
 * Check whether a type is sequential (list, vector, empty-list, or
 * lazy-seq). NIL is intentionally NOT sequential under canonical
 * equality: `(= nil '())` and `(= nil [])` are false. The empty-list
 * singleton is the canonical zero-length sequence; nil is its own
 * thing.
 */
static int is_sequential(mino_type t)
{
    return (t == MINO_CONS || t == MINO_VECTOR || t == MINO_EMPTY_LIST
            || t == MINO_LAZY || t == MINO_CHUNKED_CONS
            || t == MINO_MAP_ENTRY);
}

/*
 * Resolve a value: if it is a realized lazy seq, return the cached form.
 * Does NOT force unrealized lazy seqs (that requires the state).
 */
static const mino_val *resolve_lazy(const mino_val *v)
{
    while (v != NULL && mino_type_of(v) == MINO_LAZY
           && v->as.lazy.realized == LAZY_REALIZED) {
        v = v->as.lazy.cached;
    }
    return v;
}

/*
 * Compare two sequential values element-by-element (non-forcing version).
 * Handles cons lists, vectors, nil, and realized lazy seqs.
 * Returns 1 if they contain the same elements in the same order.
 */
/* Per-side step state for eq_seq_like / eq_seq_like_force.
 * For chunked-cons, ia is the offset within the current chunk: the
 * next element is at chunk.vals[ia], and ia transitions to .more once
 * ia == chunk.len. */
static void eq_seq_step(const mino_val **cur, size_t *idx)
{
    const mino_val *c = *cur;
    if (c == NULL) return;
    if (mino_type_of(c) == MINO_CONS) { *cur = c->as.cons.cdr; return; }
    if (mino_type_of(c) == MINO_CHUNKED_CONS) {
        const mino_val *ch = c->as.chunked_cons.chunk;
        size_t next = (*idx) + 1;
        if (next < ch->as.chunk.len) { *idx = next; return; }
        *cur = c->as.chunked_cons.more;
        *idx = 0;
        if (*cur != NULL && mino_type_of(*cur) == MINO_CHUNKED_CONS) {
            *idx = (*cur)->as.chunked_cons.off;
        }
        return;
    }
    /* MINO_VECTOR or MINO_MAP_ENTRY (treated as 2-element vector). */
    (*idx)++;
}

static int eq_seq_like(const mino_val *a, const mino_val *b)
{
    const mino_val *ca = resolve_lazy(a);
    const mino_val *cb = resolve_lazy(b);
    size_t ia = (ca != NULL && mino_type_of(ca) == MINO_CHUNKED_CONS)
                    ? ca->as.chunked_cons.off : 0;
    size_t ib = (cb != NULL && mino_type_of(cb) == MINO_CHUNKED_CONS)
                    ? cb->as.chunked_cons.off : 0;

    for (;;) {
        const mino_val *ea;
        const mino_val *eb;
        int a_end;
        int b_end;

        ca = resolve_lazy(ca);
        cb = resolve_lazy(cb);

        a_end = (ca == NULL || mino_type_of(ca) == MINO_NIL
                 || mino_type_of(ca) == MINO_EMPTY_LIST
                 || (mino_type_of(ca) == MINO_VECTOR && ia >= ca->as.vec.len)
                 || (mino_type_of(ca) == MINO_MAP_ENTRY && ia >= 2)
                 || mino_type_of(ca) == MINO_LAZY /* unrealized */);
        b_end = (cb == NULL || mino_type_of(cb) == MINO_NIL
                 || mino_type_of(cb) == MINO_EMPTY_LIST
                 || (mino_type_of(cb) == MINO_VECTOR && ib >= cb->as.vec.len)
                 || (mino_type_of(cb) == MINO_MAP_ENTRY && ib >= 2)
                 || mino_type_of(cb) == MINO_LAZY /* unrealized */);

        if (a_end && b_end) return 1;
        if (a_end || b_end) return 0;

        if (mino_type_of(ca) == MINO_CONS) ea = ca->as.cons.car;
        else if (mino_type_of(ca) == MINO_CHUNKED_CONS)
            ea = ca->as.chunked_cons.chunk->as.chunk.vals[ia];
        else if (mino_type_of(ca) == MINO_MAP_ENTRY)
            ea = ia == 0 ? ca->as.map_entry.k : ca->as.map_entry.v;
        else ea = vec_nth(ca, ia);

        if (mino_type_of(cb) == MINO_CONS) eb = cb->as.cons.car;
        else if (mino_type_of(cb) == MINO_CHUNKED_CONS)
            eb = cb->as.chunked_cons.chunk->as.chunk.vals[ib];
        else if (mino_type_of(cb) == MINO_MAP_ENTRY)
            eb = ib == 0 ? cb->as.map_entry.k : cb->as.map_entry.v;
        else eb = vec_nth(cb, ib);

        if (!mino_eq(ea, eb)) return 0;

        eq_seq_step(&ca, &ia);
        eq_seq_step(&cb, &ib);
    }
}

/*
 * Cross-type map equality: compare MINO_MAP with MINO_SORTED_MAP.
 * Both must have the same length and identical key-value pairs.
 */
static int eq_map_like_cross(const mino_val *a, const mino_val *b)
{
    const mino_val *hmap, *smap;
    size_t i;

    /* Normalize: hmap is the HAMT map, smap is the sorted map. */
    if (mino_type_of(a) == MINO_MAP) { hmap = a; smap = b; }
    else                     { hmap = b; smap = a; }

    if (hmap->as.map.len != smap->as.sorted.len) return 0;

    /* Custom comparators need a state for eval; skip cross-type equality. */
    if (smap->as.sorted.comparator != NULL) return 0;

    /* Iterate HAMT entries, look each up in the sorted map. */
    for (i = 0; i < hmap->as.map.len; i++) {
        mino_val *key = vec_nth(hmap->as.map.key_order, i);
        mino_val *hv  = map_get_val(hmap, key);
        mino_val *sv  = rb_get(NULL, smap->as.sorted.root, key, NULL);
        if (sv == NULL || !mino_eq(hv, sv)) return 0;
    }
    return 1;
}

/*
 * Cross-type set equality: compare MINO_SET with MINO_SORTED_SET.
 */
static int eq_set_like_cross(const mino_val *a, const mino_val *b)
{
    const mino_val *hset, *sset;
    size_t i;

    if (mino_type_of(a) == MINO_SET) { hset = a; sset = b; }
    else                     { hset = b; sset = a; }

    if (hset->as.set.len != sset->as.sorted.len) return 0;

    if (sset->as.sorted.comparator != NULL) return 0;

    for (i = 0; i < hset->as.set.len; i++) {
        mino_val *elem = vec_nth(hset->as.set.key_order, i);
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
/* Cross-type equality: a and b have different tags but may still be
 * = under Clojure's promotion rules (int/bigint share a value class;
 * cons/vector/etc compare as seqs; map/sorted-map compare by entry
 * pairs; set/sorted-set compare by element membership). Everything
 * else with mismatched tags is unequal. */
static int eq_cross_type(const mino_val *a, const mino_val *b)
{
    /* Cross-tier integer equality: int and bigint represent the same
     * arbitrary-precision integer kind, and Clojure treats them as
     * `=` when they hold the same value (`(= 1 1N)` is true). Other
     * tier combinations -- int/float, ratio/float, bigdec/anything --
     * are NOT equal under `=`; use `==` for cross-tier numeric
     * equality. */
    if (mino_val_int_p(a) && mino_type_of(b) == MINO_BIGINT) {
        return mino_bigint_equals_ll(b, mino_val_int_get(a));
    }
    if (mino_type_of(a) == MINO_BIGINT && mino_val_int_p(b)) {
        return mino_bigint_equals_ll(a, mino_val_int_get(b));
    }
    /* Cross-type sequential equality: cons, vector, lazy, chunked-cons,
     * map-entry, nil all compare element-wise. Matches Clojure where
     * (= '(1 2) [1 2]) is true. */
    if (is_sequential(mino_type_of(a)) && is_sequential(mino_type_of(b))) {
        return eq_seq_like(a, b);
    }
    /* Cross-type map equality: sorted-map and map compare by entries. */
    {
        int a_map = (mino_type_of(a) == MINO_MAP
                     || mino_type_of(a) == MINO_SORTED_MAP);
        int b_map = (mino_type_of(b) == MINO_MAP
                     || mino_type_of(b) == MINO_SORTED_MAP);
        if (a_map && b_map) return eq_map_like_cross(a, b);
    }
    /* Cross-type set equality: sorted-set and set compare by elements. */
    {
        int a_set = (mino_type_of(a) == MINO_SET
                     || mino_type_of(a) == MINO_SORTED_SET);
        int b_set = (mino_type_of(b) == MINO_SET
                     || mino_type_of(b) == MINO_SORTED_SET);
        if (a_set && b_set) return eq_set_like_cross(a, b);
    }
    return 0;
}

int mino_eq(const mino_val *a, const mino_val *b)
{
    if (a == b) {
        return 1;
    }
    if (a == NULL || b == NULL) {
        return 0;
    }
    /* Force lazy seqs before comparison (use cached value if realized).
     * A realized lazy whose cache is nil/empty-list is still semantically
     * an empty seq; preserve the LAZY tag so cross-type seq equality
     * routes through eq_seq_like instead of degenerating to nil. */
    if (mino_type_of(a) == MINO_LAZY
        && a->as.lazy.realized == LAZY_REALIZED) {
        mino_val *cached = a->as.lazy.cached;
        if (cached != NULL && mino_type_of(cached) != MINO_NIL
            && mino_type_of(cached) != MINO_EMPTY_LIST) {
            a = cached;
        }
    }
    if (mino_type_of(b) == MINO_LAZY
        && b->as.lazy.realized == LAZY_REALIZED) {
        mino_val *cached = b->as.lazy.cached;
        if (cached != NULL && mino_type_of(cached) != MINO_NIL
            && mino_type_of(cached) != MINO_EMPTY_LIST) {
            b = cached;
        }
    }
    if (a == NULL || b == NULL) {
        return mino_is_nil(a) && mino_is_nil(b);
    }
    if (a == b) return 1;
    /* Same-type hash-cache short-circuit on the immutable collection
     * types. Both values are immutable, so once their hash field is
     * populated it stays correct. A mismatch here means the values
     * cannot be `=` (the equal-implies-equal-hash invariant) -- skip
     * the structural compare. We only consult cached hashes that are
     * ALREADY populated; computing the hash on-demand here would
     * cost as much as the structural compare for first-time pairs. */
    if (mino_type_of(a) == mino_type_of(b)) {
        switch (mino_type_of(a)) {
        case MINO_VECTOR: {
            uint32_t ha = a->as.vec.cached_hash;
            uint32_t hb = b->as.vec.cached_hash;
            if (ha != 0 && hb != 0 && ha != hb) return 0;
            break;
        }
        case MINO_MAP: {
            uint32_t ha = a->as.map.cached_hash;
            uint32_t hb = b->as.map.cached_hash;
            if (ha != 0 && hb != 0 && ha != hb) return 0;
            break;
        }
        case MINO_SET: {
            uint32_t ha = a->as.set.cached_hash;
            uint32_t hb = b->as.set.cached_hash;
            if (ha != 0 && hb != 0 && ha != hb) return 0;
            break;
        }
        default: break;
        }
    }
    if (mino_type_of(a) != mino_type_of(b)) {
        return eq_cross_type(a, b);
    }
    switch (mino_type_of(a)) {
    case MINO_NIL:
    case MINO_EMPTY_LIST:
        return 1;
    case MINO_BOOL:
        return mino_val_bool_get(a) == mino_val_bool_get(b);
    case MINO_INT:
        return mino_val_int_get(a) == mino_val_int_get(b);
    case MINO_FLOAT:
    case MINO_FLOAT32:
        return a->as.f == b->as.f;
    case MINO_CHAR:
        return mino_val_char_get(a) == mino_val_char_get(b);
    case MINO_STRING:
        return a->as.s.len == b->as.s.len
            && memcmp(a->as.s.data, b->as.s.data, a->as.s.len) == 0;
    case MINO_SYMBOL:
    case MINO_KEYWORD:
        /* Keywords/symbols compare equal only when their (data, len,
         * ns_len) all match. ns_len distinguishes (keyword "a/b" "c")
         * from (keyword "a" "b/c") — they print the same but are
         * structurally different. */
        return a->as.s.len    == b->as.s.len
            && a->as.s.ns_len == b->as.s.ns_len
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
            mino_val *key = vec_nth(a->as.map.key_order, i);
            mino_val *bv  = map_get_val(b, key);
            mino_val *av  = map_get_val(a, key);
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
            mino_val *elem = vec_nth(a->as.set.key_order, i);
            uint32_t    h    = hash_val(elem);
            if (hamt_get(b->as.set.root, elem, h, 0u) == NULL) {
                return 0;
            }
        }
        return 1;
    }
    /* Identity-equality kinds: callables, mutable cells, opaque values,
     * and internal sentinels. For these, two distinct allocations are
     * never `=`. Each primitive is allocated once per state by
     * prim_install_table; macros/fns close over their own env;
     * atoms/volatiles/refs/agents are identity cells; the JVM-aligned
     * types (regex, host-array, future) match Object.equals
     * defaults; transients are short-lived mutable handles;
     * record-types intern by (ns, name) so they compare pointer-
     * equal already; recur/tail-call/reduced are runtime sentinels
     * that shouldn't escape user code. */
    case MINO_PRIM:
    case MINO_FN:
    case MINO_MACRO:
    case MINO_ATOM:
    case MINO_VOLATILE:
    case MINO_RECUR:
    case MINO_TAIL_CALL:
    case MINO_REDUCED:
    case MINO_VAR:
    case MINO_TRANSIENT:
    case MINO_TYPE:
    case MINO_FUTURE:
    case MINO_REGEX:
    case MINO_HOST_ARRAY:
    case MINO_TX_REF:
    case MINO_AGENT:
    case MINO_CHAN:
        return a == b;
    case MINO_HANDLE:
        /* Two MINO_HANDLE values are equal iff they wrap the same host
         * pointer; tag matching is left to the host. */
        return a->as.handle.ptr == b->as.handle.ptr;
    case MINO_LAZY:
        /* Both sides are MINO_LAZY of the same realized state. Two
         * realized-to-empty lazy seqs both keep the LAZY tag (the
         * unwrap policy at the top of mino_eq preserves it so cross-
         * type seq equality routes through eq_seq_like), so this
         * arm fires for the empty-empty case. eq_seq_like handles
         * any pair of seq-shaped values uniformly, including two
         * empty lazies (both walks immediately terminate). */
        return eq_seq_like(a, b);
    case MINO_CHUNK:
        /* Internal seq leaf; identity equality (chunk-buffer state
         * is mutable and not meaningfully comparable across instances). */
        return a == b;
    case MINO_CHUNKED_CONS:
        /* Should not reach here — handled by the cross-type sequential
         * path via is_sequential. */
        return eq_seq_like(a, b);
    case MINO_SORTED_MAP:
    case MINO_SORTED_SET:
        /* Same length is necessary either way. When the comparators are
         * identical pointers (or both default) the trees share an
         * ordering and the cheap structural walk is correct. Otherwise
         * the trees are arranged differently for the same content
         * (e.g. `<` vs `>`); fall back to an O(n*log n) content walk
         * that pairs entries by `mino_eq` on the key, ignoring tree
         * shape. */
        if (a->as.sorted.len != b->as.sorted.len) return 0;
        if (a->as.sorted.comparator == b->as.sorted.comparator) {
            return rb_trees_equal(a->as.sorted.root, b->as.sorted.root,
                                  mino_type_of(a) == MINO_SORTED_MAP);
        }
        return rb_trees_content_equal(a->as.sorted.root,
                                      b->as.sorted.root,
                                      mino_type_of(a) == MINO_SORTED_MAP);
    case MINO_BIGINT:
        return mino_bigint_equals(a, b);
    case MINO_RATIO:
        return mino_ratio_equals(a, b);
    case MINO_BIGDEC:
        return mino_bigdec_equals(a, b);
    case MINO_RECORD: {
        /* Records are equal iff their types are pointer-identical AND
         * each declared field value is mino_eq AND the ext maps are
         * mino_eq. Records are never equal to plain maps with the
         * same content — type identity is part of the contract. */
        size_t i, n;
        if (a->as.record.type != b->as.record.type) return 0;
        n = (a->as.record.type->as.record_type.fields != NULL)
            ? a->as.record.type->as.record_type.fields->as.vec.len : 0;
        for (i = 0; i < n; i++) {
            if (!mino_eq(a->as.record.vals[i], b->as.record.vals[i])) return 0;
        }
        if (a->as.record.ext == NULL && b->as.record.ext == NULL) return 1;
        if (a->as.record.ext == NULL || b->as.record.ext == NULL) return 0;
        return mino_eq(a->as.record.ext, b->as.record.ext);
    }
    case MINO_UUID:
        return memcmp(a->as.uuid.bytes, b->as.uuid.bytes, 16) == 0;
    case MINO_MAP_ENTRY:
        return mino_eq(a->as.map_entry.k, b->as.map_entry.k)
            && mino_eq(a->as.map_entry.v, b->as.map_entry.v);
    }
    return 0;
}

/*
 * Compare two sequential values element-by-element, forcing lazy seqs.
 */
static int eq_seq_like_force(mino_state *S, const mino_val *a,
                           const mino_val *b)
{
    const mino_val *ca = a;
    const mino_val *cb = b;
    size_t ia = 0, ib = 0;

    if (ca != NULL && mino_type_of(ca) == MINO_CHUNKED_CONS)
        ia = ca->as.chunked_cons.off;
    if (cb != NULL && mino_type_of(cb) == MINO_CHUNKED_CONS)
        ib = cb->as.chunked_cons.off;

    for (;;) {
        const mino_val *ea;
        const mino_val *eb;
        int a_end;
        int b_end;

        /* Force lazy seqs */
        if (ca != NULL && mino_type_of(ca) == MINO_LAZY)
            ca = lazy_force(S, (mino_val *)ca);
        if (cb != NULL && mino_type_of(cb) == MINO_LAZY)
            cb = lazy_force(S, (mino_val *)cb);

        a_end = (ca == NULL || mino_type_of(ca) == MINO_NIL
                 || mino_type_of(ca) == MINO_EMPTY_LIST
                 || (mino_type_of(ca) == MINO_VECTOR && ia >= ca->as.vec.len)
                 || (mino_type_of(ca) == MINO_MAP_ENTRY && ia >= 2));
        b_end = (cb == NULL || mino_type_of(cb) == MINO_NIL
                 || mino_type_of(cb) == MINO_EMPTY_LIST
                 || (mino_type_of(cb) == MINO_VECTOR && ib >= cb->as.vec.len)
                 || (mino_type_of(cb) == MINO_MAP_ENTRY && ib >= 2));

        if (a_end && b_end) return 1;
        if (a_end || b_end) return 0;

        if (mino_type_of(ca) == MINO_CONS) ea = ca->as.cons.car;
        else if (mino_type_of(ca) == MINO_CHUNKED_CONS)
            ea = ca->as.chunked_cons.chunk->as.chunk.vals[ia];
        else if (mino_type_of(ca) == MINO_MAP_ENTRY)
            ea = ia == 0 ? ca->as.map_entry.k : ca->as.map_entry.v;
        else ea = vec_nth(ca, ia);

        if (mino_type_of(cb) == MINO_CONS) eb = cb->as.cons.car;
        else if (mino_type_of(cb) == MINO_CHUNKED_CONS)
            eb = cb->as.chunked_cons.chunk->as.chunk.vals[ib];
        else if (mino_type_of(cb) == MINO_MAP_ENTRY)
            eb = ib == 0 ? cb->as.map_entry.k : cb->as.map_entry.v;
        else eb = vec_nth(cb, ib);

        if (!mino_eq_force(S, ea, eb)) return 0;

        eq_seq_step(&ca, &ia);
        eq_seq_step(&cb, &ib);
    }
}

int mino_eq_force(mino_state *S, const mino_val *a, const mino_val *b)
{
    /* Force lazy seqs, but preserve the LAZY tag when the forced
     * result is nil/empty-list. A lazy seq that resolves to nothing is
     * still semantically an empty seq; collapsing it to nil here would
     * make `(= [] (lazy-seq nil))` false, contradicting canon. */
    if (a != NULL && mino_type_of(a) == MINO_LAZY) {
        mino_val *forced = lazy_force(S, (mino_val *)a);
        if (forced != NULL && mino_type_of(forced) != MINO_NIL
            && mino_type_of(forced) != MINO_EMPTY_LIST) {
            a = forced;
        }
    }
    if (b != NULL && mino_type_of(b) == MINO_LAZY) {
        mino_val *forced = lazy_force(S, (mino_val *)b);
        if (forced != NULL && mino_type_of(forced) != MINO_NIL
            && mino_type_of(forced) != MINO_EMPTY_LIST) {
            b = forced;
        }
    }
    if (a == b) return 1;
    if (a == NULL || b == NULL) return mino_is_nil(a) && mino_is_nil(b);
    /* For cons-vs-cons, walk both chains side-by-side via the
     * sequential helper so the loop's terminator predicate (which
     * recognises NIL, EMPTY_LIST, and lazy-empty all as "end") also
     * applies to nested cdr positions. Recursing through this function
     * via cdr would expose the cross-type asymmetry: nil and a
     * lazy-realized-to-nil are equivalent at end-of-seq but
     * is_sequential(NIL) is false at top level. */
    if (mino_type_of(a) == MINO_CONS && mino_type_of(b) == MINO_CONS) {
        return eq_seq_like_force(S, a, b);
    }
    /* Same-tag chunked sequential: a chunked-cons spine can have a
     * lazy seq in its `more` field (the typical shape filter/range
     * produce). The non-forcing eq_seq_like would see that unrealized
     * lazy as end-of-seq and short-circuit incorrectly. Force on both
     * sides instead. */
    if (mino_type_of(a) == MINO_CHUNKED_CONS && mino_type_of(b) == MINO_CHUNKED_CONS) {
        return eq_seq_like_force(S, a, b);
    }
    /* Cross-type sequential: cons vs vector, nil vs vector, etc. */
    if (mino_type_of(a) != mino_type_of(b) && is_sequential(mino_type_of(a)) && is_sequential(mino_type_of(b))) {
        /* Force any remaining lazy seqs in elements during comparison. */
        return eq_seq_like_force(S, a, b);
    }
    /* Vectors: compare elements with forcing. */
    if (mino_type_of(a) == MINO_VECTOR && mino_type_of(b) == MINO_VECTOR) {
        size_t i;
        if (a->as.vec.len != b->as.vec.len) return 0;
        for (i = 0; i < a->as.vec.len; i++) {
            if (!mino_eq_force(S, vec_nth(a, i), vec_nth(b, i))) return 0;
        }
        return 1;
    }
    /* Maps and sets can hold lazy seqs as values / elements (e.g. the
     * `& rest` binding from a bc-compiled fn lands as a chunked /
     * lazy seq, which a literal-quoted cons would equal under
     * `=`). Forcing has to walk into each entry; delegating to the
     * non-forcing mino_eq would short-circuit on the lazy-cdr-end
     * heuristic and incorrectly answer false. */
    if (mino_type_of(a) == MINO_MAP && mino_type_of(b) == MINO_MAP) {
        size_t i;
        if (a->as.map.len != b->as.map.len) return 0;
        for (i = 0; i < a->as.map.len; i++) {
            mino_val *key = vec_nth(a->as.map.key_order, i);
            mino_val *av  = map_get_val(a, key);
            mino_val *bv  = map_get_val(b, key);
            if (bv == NULL) return 0;
            if (!mino_eq_force(S, av, bv)) return 0;
        }
        return 1;
    }
    if (mino_type_of(a) == MINO_SET && mino_type_of(b) == MINO_SET) {
        size_t i;
        if (a->as.set.len != b->as.set.len) return 0;
        for (i = 0; i < a->as.set.len; i++) {
            mino_val *elem = vec_nth(a->as.set.key_order, i);
            /* Set membership is keyed by mino_eq (=> hashed). For
             * lazy elements, force before looking up so the
             * structural compare in hamt_get sees the forced shape. */
            mino_val *e_forced = elem;
            if (e_forced != NULL && mino_type_of(e_forced) == MINO_LAZY) {
                mino_val *f = lazy_force(S, (mino_val *)e_forced);
                if (f != NULL) e_forced = f;
            }
            if (hamt_get(b->as.set.root, e_forced,
                         hash_val(e_forced), 0u) == NULL) {
                return 0;
            }
        }
        return 1;
    }
    return mino_eq(a, b);
}
