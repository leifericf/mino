/*
 * runtime_var.c -- var registry: intern, lookup, root binding management.
 *
 * Two open-addressing hash indices accelerate the cold path:
 *
 *   interned_var_strs_hash : keyed on string contents, returns the
 *     canonical interned pointer. Replaces the old O(n) linear scan
 *     in intern_var_str. At install time the registry sees ~640
 *     unique strings; the linear scan was O(n^2) = ~400k strcmps.
 *
 *   var_hash : keyed on the (ns*, name*) pointer pair. Both are
 *     interned, so equality is pointer equality. Replaces the
 *     two-strcmp linear scan in var_find / var_intern / var_unintern.
 *
 * The linear arrays (interned_var_strs and var_registry) remain the
 * source of truth: GC root walks the registry, state teardown frees
 * the strings. The hashes are pure caches and are rebuilt by the
 * resize / unintern paths.
 */

#include "runtime/internal.h"

extern mino_val_t *prim_throw_classified(mino_state_t *S, const char *kind,
                                          const char *code, const char *msg);

/* FNV-1a over the bytes of s. 32-bit form is enough for the table
 * sizes mino actually sees (< 2^16 entries) while staying portable. */
static unsigned str_hash_fnv1a(const char *s)
{
    unsigned h = 2166136261u;
    while (*s) {
        h ^= (unsigned char)*s++;
        h *= 16777619u;
    }
    return h;
}

/* Mix two interned-string pointers into a hash. Uses the high-quality
 * Knuth golden-ratio multiplier on each half then xors. Pointer values
 * are ABI-stable for the state's lifetime. */
static unsigned ptr_pair_hash(const char *a, const char *b)
{
    uintptr_t ua = (uintptr_t)a;
    uintptr_t ub = (uintptr_t)b;
    uintptr_t h  = ua * (uintptr_t)2654435761ull;
    h ^= ub * (uintptr_t)40503ull;
    return (unsigned)(h ^ (h >> 32));
}

/* ----- interned_var_strs_hash --------------------------------------- */

static void intern_str_hash_resize(mino_state_t *S, size_t new_cap)
{
    size_t       i;
    const char **new_tbl;
    new_tbl = (const char **)calloc(new_cap, sizeof(*new_tbl));
    if (new_tbl == NULL) return;  /* lookups silently fall back to linear */
    /* Replay every interned string into the new table. */
    for (i = 0; i < S->interned_var_strs_len; i++) {
        const char *s     = S->interned_var_strs[i];
        unsigned    h     = str_hash_fnv1a(s);
        size_t      probe = (size_t)h & (new_cap - 1u);
        while (new_tbl[probe] != NULL) probe = (probe + 1u) & (new_cap - 1u);
        new_tbl[probe] = s;
    }
    free(S->interned_var_strs_hash);
    S->interned_var_strs_hash     = new_tbl;
    S->interned_var_strs_hash_cap = new_cap;
}

/* Look up s in the hash. Returns the canonical interned pointer or
 * NULL if not present. Walks open-addressed slots until either match
 * (pointer-string-equal) or empty slot. */
static const char *intern_str_hash_lookup(mino_state_t *S, const char *s)
{
    unsigned h;
    size_t   probe;
    if (S->interned_var_strs_hash == NULL) return NULL;
    h     = str_hash_fnv1a(s);
    probe = (size_t)h & (S->interned_var_strs_hash_cap - 1u);
    while (S->interned_var_strs_hash[probe] != NULL) {
        const char *cand = S->interned_var_strs_hash[probe];
        if (cand == s || strcmp(cand, s) == 0) return cand;
        probe = (probe + 1u) & (S->interned_var_strs_hash_cap - 1u);
    }
    return NULL;
}

static void intern_str_hash_insert(mino_state_t *S, const char *s)
{
    unsigned h;
    size_t   probe;
    /* Resize trigger: keep load factor under ~0.7. */
    if (S->interned_var_strs_hash == NULL
        || S->interned_var_strs_len * 10u >= S->interned_var_strs_hash_cap * 7u) {
        size_t new_cap = (S->interned_var_strs_hash_cap == 0)
                         ? 128u : (S->interned_var_strs_hash_cap * 2u);
        intern_str_hash_resize(S, new_cap);
        if (S->interned_var_strs_hash == NULL) return;  /* OOM, give up */
    }
    h     = str_hash_fnv1a(s);
    probe = (size_t)h & (S->interned_var_strs_hash_cap - 1u);
    while (S->interned_var_strs_hash[probe] != NULL) {
        probe = (probe + 1u) & (S->interned_var_strs_hash_cap - 1u);
    }
    S->interned_var_strs_hash[probe] = s;
}

/* Intern a string into the state's var-string table. Returns a pointer
 * that is stable for the life of the state. Strings are malloc-owned. */
static const char *intern_var_str(mino_state_t *S, const char *s)
{
    const char *existing;
    size_t      n;
    char       *d;
    existing = intern_str_hash_lookup(S, s);
    if (existing != NULL) return existing;
    n = strlen(s);
    d = (char *)malloc(n + 1);
    if (d == NULL) return s;
    memcpy(d, s, n + 1);
    if (S->interned_var_strs_len == S->interned_var_strs_cap) {
        size_t       new_cap = (S->interned_var_strs_cap == 0)
                               ? 64u : (S->interned_var_strs_cap * 2u);
        const char **nb      = (const char **)realloc(
            S->interned_var_strs, new_cap * sizeof(*nb));
        if (nb == NULL) {
            free(d);
            return s;
        }
        S->interned_var_strs     = nb;
        S->interned_var_strs_cap = new_cap;
    }
    S->interned_var_strs[S->interned_var_strs_len++] = d;
    intern_str_hash_insert(S, d);
    return d;
}

/* ----- var_hash ----------------------------------------------------- */

static void var_hash_resize(mino_state_t *S, size_t new_cap)
{
    size_t           i;
    var_hash_slot_t *new_tbl;
    new_tbl = (var_hash_slot_t *)calloc(new_cap, sizeof(*new_tbl));
    if (new_tbl == NULL) return;
    for (i = 0; i < S->var_registry_len; i++) {
        const char *ns    = S->var_registry[i].ns;
        const char *name  = S->var_registry[i].name;
        unsigned    h     = ptr_pair_hash(ns, name);
        size_t      probe = (size_t)h & (new_cap - 1u);
        while (new_tbl[probe].ns != NULL) {
            probe = (probe + 1u) & (new_cap - 1u);
        }
        new_tbl[probe].ns   = ns;
        new_tbl[probe].name = name;
        new_tbl[probe].var  = S->var_registry[i].var;
    }
    free(S->var_hash);
    S->var_hash     = new_tbl;
    S->var_hash_cap = new_cap;
    S->var_hash_len = S->var_registry_len;
}

/* Hot path: lookup by (ns*, name*). Both must be interned for pointer
 * equality to hit. When ns/name are caller-supplied non-interned
 * strings the caller must first intern via intern_var_str(). */
static mino_val_t *var_hash_lookup(mino_state_t *S, const char *i_ns,
                                    const char *i_name)
{
    unsigned h;
    size_t   probe;
    if (S->var_hash == NULL) return NULL;
    h     = ptr_pair_hash(i_ns, i_name);
    probe = (size_t)h & (S->var_hash_cap - 1u);
    while (S->var_hash[probe].ns != NULL) {
        if (S->var_hash[probe].ns == i_ns
            && S->var_hash[probe].name == i_name) {
            return S->var_hash[probe].var;
        }
        probe = (probe + 1u) & (S->var_hash_cap - 1u);
    }
    return NULL;
}

static void var_hash_insert(mino_state_t *S, const char *i_ns,
                            const char *i_name, mino_val_t *var)
{
    unsigned h;
    size_t   probe;
    if (S->var_hash == NULL
        || (S->var_hash_len + 1u) * 10u >= S->var_hash_cap * 7u) {
        size_t new_cap = (S->var_hash_cap == 0) ? 128u : (S->var_hash_cap * 2u);
        var_hash_resize(S, new_cap);
        if (S->var_hash == NULL) return;
    }
    h     = ptr_pair_hash(i_ns, i_name);
    probe = (size_t)h & (S->var_hash_cap - 1u);
    while (S->var_hash[probe].ns != NULL) {
        probe = (probe + 1u) & (S->var_hash_cap - 1u);
    }
    S->var_hash[probe].ns   = i_ns;
    S->var_hash[probe].name = i_name;
    S->var_hash[probe].var  = var;
    S->var_hash_len++;
}

/* var_unintern is rare. Drop the matching entry from the linear
 * array and rebuild the hash from scratch -- simpler than tombstone
 * bookkeeping. */
static void var_hash_rebuild(mino_state_t *S)
{
    size_t cap = S->var_hash_cap > 0 ? S->var_hash_cap : 128u;
    var_hash_resize(S, cap);
}

/* ----- public surface ---------------------------------------------- */

mino_val_t *var_intern(mino_state_t *S, const char *ns, const char *name)
{
    mino_val_t *v;
    const char *i_ns, *i_name;

    /* Intern the strings first; subsequent lookups key on pointers. */
    i_ns   = intern_var_str(S, ns);
    i_name = intern_var_str(S, name);

    /* Hash hit -> existing var. */
    v = var_hash_lookup(S, i_ns, i_name);
    if (v != NULL) return v;

    /* Create a fresh var. */
    v = mino_mk_var(S, i_ns, i_name, mino_nil(S));
    gc_pin(v);

    /* Grow registry if needed. */
    if (S->var_registry_len == S->var_registry_cap) {
        size_t       new_cap = (S->var_registry_cap == 0)
                               ? 64u : (S->var_registry_cap * 2u);
        var_entry_t *nb      = (var_entry_t *)realloc(
            S->var_registry, new_cap * sizeof(*nb));
        if (nb == NULL) {
            gc_unpin(1);
            set_eval_diag(S, mino_current_ctx(S)->eval_current_form,
                          "internal", "MIN001", "out of memory");
            return NULL;
        }
        S->var_registry     = nb;
        S->var_registry_cap = new_cap;
    }

    S->var_registry[S->var_registry_len].ns   = i_ns;
    S->var_registry[S->var_registry_len].name = i_name;
    S->var_registry[S->var_registry_len].var  = v;
    S->var_registry_len++;

    var_hash_insert(S, i_ns, i_name, v);

    gc_unpin(1);
    return v;
}

void var_set_root(mino_state_t *S, mino_val_t *var, mino_val_t *val)
{
    mino_val_t *old_val   = var->as.var.root;
    mino_val_t *validator = var->as.var.validator;
    mino_val_t *watches   = var->as.var.watches;
    mino_env_t *env;

    /* Fast path: no watches, no validator, no env lookup. Early-bound
     * install paths (state init, runtime/install_stdlib bootstrap)
     * stay zero-cost. */
    if (validator == NULL
        && (watches == NULL || mino_type_of(watches) != MINO_MAP
            || watches->as.map.len == 0)) {
        gc_write_barrier(S, var, var->as.var.root, val);
        var->as.var.root  = val;
        var->as.var.bound = 1;
        var->as.var.version++;
        S->ic_gen++;
        return;
    }

    /* Watches and validators run user code, which means we need an
     * env. Use the current ns's env -- this is the natural one for
     * runtime def / alter-var-root, which always run from inside an
     * eval context with current_ns set. */
    env = current_ns_env(S);

    /* Validator: run before publishing the new root. A throw from
     * the validator (via prim_throw_classified) longjmps out without
     * mutating the var. */
    if (validator != NULL) {
        mino_val_t *vargs  = mino_cons(S, val, mino_nil(S));
        mino_val_t *result = mino_call(S, validator, vargs, env);
        if (result == NULL) return;  /* validator threw */
        if (!mino_is_truthy(result)) {
            prim_throw_classified(S, "eval/contract", "MCT001",
                "Invalid reference state");
            return;
        }
    }
    gc_write_barrier(S, var, var->as.var.root, val);
    var->as.var.root  = val;
    var->as.var.bound = 1;
    var->as.var.version++;
    /* Watches: dispatch after the publish. JVM Clojure's Var watches
     * fire on (alter-var-root v f) and on def with rebind. The
     * callback signature is (fn key var old new). A watch that throws
     * propagates via mino_call returning NULL, matching atoms/refs. */
    if (watches != NULL && mino_type_of(watches) == MINO_MAP
        && watches->as.map.len > 0) {
        size_t n = watches->as.map.len;
        size_t i;
        for (i = 0; i < n; i++) {
            mino_val_t *key = vec_nth(watches->as.map.key_order, i);
            mino_val_t *fn  = map_get_val(watches, key);
            mino_val_t *wargs;
            if (fn == NULL) continue;
            wargs = mino_cons(S, key,
                      mino_cons(S, var,
                        mino_cons(S, old_val,
                          mino_cons(S, val, mino_nil(S)))));
            if (mino_call(S, fn, wargs, env) == NULL) return;
        }
    }
}

mino_val_t *var_find(mino_state_t *S, const char *ns, const char *name)
{
    /* Caller may pass non-interned strings. Intern first; strings used
     * for lookup get registered, but the cost is amortised across the
     * same name appearing again. */
    const char *i_ns   = intern_var_str(S, ns);
    const char *i_name = intern_var_str(S, name);
    return var_hash_lookup(S, i_ns, i_name);
}

void var_unintern(mino_state_t *S, const char *ns, const char *name)
{
    const char *i_ns   = intern_var_str(S, ns);
    const char *i_name = intern_var_str(S, name);
    size_t      i, j;
    for (i = 0; i < S->var_registry_len; i++) {
        if (S->var_registry[i].ns == i_ns
            && S->var_registry[i].name == i_name) {
            for (j = i + 1; j < S->var_registry_len; j++) {
                S->var_registry[j - 1] = S->var_registry[j];
            }
            S->var_registry_len--;
            var_hash_rebuild(S);
            /* Same rationale as env_unbind: removing a var changes how
             * its symbol resolves, and the inline call cache must
             * notice. Bumping ic_gen invalidates every slot. */
            S->ic_gen++;
            return;
        }
    }
}
