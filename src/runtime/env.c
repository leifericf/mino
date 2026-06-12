/*
 * env.c -- environment allocation, binding, lookup, dynamic bindings.
 */

#include "runtime/internal.h"

/*
 * Environment: a chain of frames. Each frame is a flat (name, value) array
 * with linear search. The root frame has parent == NULL and holds globals;
 * child frames are created by let, fn application, and loop. Lookup walks
 * parents; binding always writes to the current frame so that let and fn
 * parameters shadow rather than mutate outer bindings.
 *
 * Envs and their binding arrays are GC-managed. `mino_env_new` registers
 * the returned env as a persistent root so the whole chain (and everything
 * reachable from its bindings) survives collection; `mino_env_free` unroots
 * it, letting the next sweep reclaim the frame and any closures that were
 * only reachable through it.
 */

mino_env *env_alloc(mino_state *S, mino_env *parent)
{
    mino_env *env = (mino_env *)gc_alloc_typed(S, GC_T_ENV, sizeof(*env));
    env->parent = parent;
    return env;
}

mino_env *mino_env_new(mino_state *S)
{
    volatile char probe = 0;
    mino_env   *env;
    root_env_t   *r;
    /* Record the host's stack frame: this is typically the earliest point
     * the host calls into mino, so it fixes a generous stack bottom before
     * any allocator runs. */
    gc_note_host_frame(S, (void *)&probe);
    (void)probe;
    env = env_alloc(S, NULL);
    r   = (root_env_t *)malloc(sizeof(*r));
    if (r == NULL) {
        set_eval_diag(S, mino_current_ctx(S)->eval_current_form, "internal", "MIN001", "out of memory");
        return NULL;
    }
    r->env       = env;
    r->next      = S->gc.root_envs;
    S->gc.root_envs = r;
    return env;
}

mino_env *env_child(mino_state *S, mino_env *parent)
{
    return env_alloc(S, parent);
}

void mino_env_free(mino_state *S, mino_env *env)
{
    /* Unroot the env. Its memory, along with any closures and bindings
     * reachable only through it, is reclaimed at the next collection. */
    root_env_t **pp = &S->gc.root_envs;
    if (env == NULL) {
        return;
    }
    while (*pp != NULL) {
        if ((*pp)->env == env) {
            root_env_t *dead = *pp;
            *pp = dead->next;
            free(dead);
            return;
        }
        pp = &(*pp)->next;
    }
}

static uint32_t env_hash_name(const char *name, size_t len)
{
    return fnv_bytes(2166136261u, (const unsigned char *)name, len);
}

static void env_ht_rebuild(mino_state *S, mino_env *env)
{
    size_t new_cap;
    size_t target;
    size_t alloc_sz;
    size_t *buckets;
    size_t i, mask;
    if (env->ht_cap == 0) {
        new_cap = 64;
    } else if (!checked_double_sz(env->ht_cap, &new_cap)) {
        gc_oom_throw(S, "env table grow: size overflow");
        return; /* unreachable */
    }
    /* Target is twice the live binding count; if `env->len * 2` would
     * wrap, the table is bigger than addressable memory and we must
     * fail rather than under-allocate. */
    if (!checked_mul_sz(env->len, 2, &target)) {
        gc_oom_throw(S, "env table grow: size overflow");
        return; /* unreachable */
    }
    while (new_cap < target) {
        if (!checked_double_sz(new_cap, &new_cap)) {
            gc_oom_throw(S, "env table grow: size overflow");
            return; /* unreachable */
        }
    }
    if (!checked_mul_sz(new_cap, sizeof(*buckets), &alloc_sz)) {
        gc_oom_throw(S, "env table grow: size overflow");
        return; /* unreachable */
    }
    buckets = (size_t *)gc_alloc_typed(S, GC_T_RAW, alloc_sz);
    mask = new_cap - 1;
    for (i = 0; i < new_cap; i++) buckets[i] = SIZE_MAX;
    for (i = 0; i < env->len; i++) {
        size_t nlen = strlen(env->bindings[i].name);
        uint32_t h = env_hash_name(env->bindings[i].name, nlen);
        size_t idx = h & mask;
        while (buckets[idx] != SIZE_MAX) idx = (idx + 1) & mask;
        buckets[idx] = i;
    }
    gc_write_barrier(S, env, env->ht_buckets, buckets);
    env->ht_buckets = buckets;
    env->ht_cap = new_cap;
}

/* Hash-aware probe. `hash` is non-zero iff the caller already knows
 * `env_hash_name(name, nlen)` (e.g. from an interned symbol's
 * `sym->as.s.hash`). When zero, the hashed-frame path computes it on
 * the spot. Linear-scan frames ignore `hash` entirely. */
static env_binding_t *env_find_here_hashed(mino_env *env, const char *name,
                                           size_t nlen, uint32_t hash)
{
    /* Hash-indexed lookup for large frames. */
    if (env->ht_buckets != NULL) {
        uint32_t h    = hash != 0 ? hash : env_hash_name(name, nlen);
        size_t   mask = env->ht_cap - 1;
        size_t   idx  = h & mask;
        while (env->ht_buckets[idx] != SIZE_MAX) {
            env_binding_t *b = &env->bindings[env->ht_buckets[idx]];
            /* Pointer-eq fast path: names are stored by their interned
             * symbol data pointer, so if the caller passes the same
             * pointer (common from eval_symbol) we skip strcmp. */
            if (b->name == name || strcmp(b->name, name) == 0) return b;
            idx = (idx + 1) & mask;
        }
        return NULL;
    }
    /* Linear scan for small frames. */
    {
        size_t i;
        for (i = 0; i < env->len; i++) {
            const char *bn = env->bindings[i].name;
            if (bn == name || strcmp(bn, name) == 0) {
                return &env->bindings[i];
            }
        }
    }
    return NULL;
}

/* Length-aware variant. The caller already paid for strlen(name)
 * (typically reading sym->as.s.len from an interned symbol), so we
 * skip recomputing it on every probe. */
env_binding_t *env_find_here_n(mino_env *env, const char *name, size_t nlen)
{
    return env_find_here_hashed(env, name, nlen, 0);
}

env_binding_t *env_find_here(mino_env *env, const char *name)
{
    /* Most callers already paid for strlen via sym->as.s.len. Compute
     * once here only if the lookup actually needs it (hash-indexed
     * frame); the linear-scan path ignores nlen entirely. */
    size_t nlen = (env->ht_buckets != NULL) ? strlen(name) : 0;
    return env_find_here_n(env, name, nlen);
}

/* Shared implementation: interned_name is a stable pointer (e.g. from
 * an intern entry) when known, otherwise NULL. */
static void env_bind_impl(mino_state *S, mino_env *env,
                          const char *name, size_t nlen,
                          const char *interned_name,
                          mino_val *val)
{
    env_binding_t *b = env_find_here(env, name);
    if (b != NULL) {
        /* The env owns the bindings array, so route the barrier
         * through env: gc_trace_children walks every binding's
         * name/val when env is in the remembered set. Pointing at
         * the bindings buffer directly would hit a GC_T_RAW trace
         * stub and miss the new young target. */
        gc_write_barrier(S, env, b->val, val);
        b->val = val;
        return;
    }
    if (env->len == env->cap) {
        size_t         new_cap;
        size_t         alloc_sz;
        env_binding_t *nb;
        if (env->cap == 0) {
            new_cap = 4;
        } else if (!checked_double_sz(env->cap, &new_cap)) {
            gc_oom_throw(S, "env bindings grow: size overflow");
            return; /* unreachable */
        }
        if (!checked_mul_sz(new_cap, sizeof(*nb), &alloc_sz)) {
            gc_oom_throw(S, "env bindings grow: size overflow");
            return; /* unreachable */
        }
        nb = (env_binding_t *)gc_alloc_typed(S, GC_T_RAW, alloc_sz);
        if (env->bindings != NULL && env->len > 0) {
            memcpy(nb, env->bindings, env->len * sizeof(*nb));
        }
        gc_write_barrier(S, env, env->bindings, nb);
        env->bindings = nb;
        env->cap      = new_cap;
    }
    /* Store a stable name pointer so env_find_here can use pointer
     * equality on the hot path. If the caller supplied an already-
     * interned pointer, reuse it; otherwise intern now so future
     * bindings with the same name share a pointer. */
    if (interned_name != NULL) {
        /* New slot: the name/val fields are zero-initialised by
         * gc_alloc_typed on the bindings buffer, so old_value is NULL
         * on each of the three stores below. */
        gc_write_barrier(S, env, NULL, interned_name);
        env->bindings[env->len].name = (char *)interned_name;
    } else {
        mino_val *sym  = mino_symbol_n(S, name, nlen);
        char       *nm   = (sym != NULL) ? sym->as.s.data
                                         : dup_n(S, name, nlen);
        gc_write_barrier(S, env, NULL, nm);
        env->bindings[env->len].name = nm;
    }
    gc_write_barrier(S, env, NULL, val);
    env->bindings[env->len].val  = val;
    env->len++;

    /* Build hash index when frame crosses threshold. */
    if (env->len == ENV_HASH_THRESHOLD) {
        env_ht_rebuild(S, env);
    } else if (env->ht_buckets != NULL) {
        /* Already has index — insert into it. */
        uint32_t h = env_hash_name(name, nlen);
        size_t mask = env->ht_cap - 1;
        size_t idx = h & mask;
        while (env->ht_buckets[idx] != SIZE_MAX) idx = (idx + 1) & mask;
        env->ht_buckets[idx] = env->len - 1;
        /* Rehash if load > 75%. */
        if (env->len * 4 > env->ht_cap * 3) {
            env_ht_rebuild(S, env);
        }
    }
}

void env_bind(mino_state *S, mino_env *env, const char *name,
              mino_val *val)
{
    env_bind_impl(S, env, name, strlen(name), NULL, val);
}

int env_unbind(mino_state *S, mino_env *env, const char *name)
{
    size_t i;
    for (i = 0; i < env->len; i++) {
        const char *bn = env->bindings[i].name;
        if (bn == name || strcmp(bn, name) == 0) {
            size_t j;
            for (j = i + 1; j < env->len; j++) {
                env->bindings[j - 1] = env->bindings[j];
            }
            env->len--;
            /* Hash buckets index by slot number, so a shift invalidates
             * everything; cheapest correct fix is to rebuild the table.
             * Leave it dropped if we fall under threshold so future
             * env_bind paths re-create it on demand. */
            if (env->ht_buckets != NULL) {
                if (env->len < ENV_HASH_THRESHOLD) {
                    env->ht_buckets = NULL;
                    env->ht_cap = 0;
                } else {
                    env_ht_rebuild(S, env);
                }
            }
            /* Invalidate the eval-side inline call cache. Removing a
             * binding can change how a previously-cached call form
             * resolves, and the cache only checks gen_at_fill against
             * S->ns_vars.ic_gen. ns-unmap and similar paths reach here. */
            S->ns_vars.ic_gen++;
            return 1;
        }
    }
    return 0;
}

/* Hot-path variant: caller supplies an already-interned symbol, so the
 * binding name pointer and length come free and we skip strlen plus the
 * intern hash-table probe. */
void env_bind_sym(mino_state *S, mino_env *env, mino_val *sym,
                  mino_val *val)
{
    env_bind_impl(S, env, sym->as.s.data, sym->as.s.len,
                  sym->as.s.data, val);
}

mino_env *env_root(mino_state *S, mino_env *env)
{
    (void)S;
    while (env->parent != NULL) {
        env = env->parent;
    }
    return env;
}

mino_env *mino_env_clone(mino_state *S, mino_env *env)
{
    if (env == NULL) return NULL;

    /* Allocate a new root env and copy all bindings from the source. */
    mino_env *clone = mino_env_new(S);
    size_t i;
    for (i = 0; i < env->len; i++) {
        env_bind(S, clone, env->bindings[i].name, env->bindings[i].val);
    }
    return clone;
}

void mino_env_set(mino_state *S, mino_env *env, const char *name, mino_val *val)
{
    env_bind(S, env, name, val);
}

mino_val *mino_env_get(mino_env *env, const char *name)
{
    /* Cache strlen(name) across the parent walk: every hash-indexed
     * frame in the chain would otherwise pay for it again. The cost
     * is a single conditional plus the strlen on the first frame that
     * actually needs it. */
    size_t nlen      = 0;
    int    nlen_done = 0;
    while (env != NULL) {
        env_binding_t *b;
        if (env->ht_buckets != NULL && !nlen_done) {
            nlen      = strlen(name);
            nlen_done = 1;
        }
        b = env_find_here_n(env, name, nlen);
        if (b != NULL) return b->val;
        env = env->parent;
    }
    return NULL;
}

/* Symbol-aware variant: caller has the symbol's interned len in hand
 * (sym->as.s.len), so we skip strlen entirely. Also reads
 * sym->as.s.hash to skip FNV recomputation per probed parent frame.
 * Used by eval_symbol on the hot lookup path. */
mino_val *mino_env_get_sym(mino_env *env, const mino_val *sym)
{
    const char *name = sym->as.s.data;
    size_t      nlen = sym->as.s.len;
    uint32_t    h    = sym->as.s.hash;
    while (env != NULL) {
        env_binding_t *b = env_find_here_hashed(env, name, nlen, h);
        if (b != NULL) return b->val;
        env = env->parent;
    }
    return NULL;
}

/* Free a chain of dynamic bindings (node storage only). */
void dyn_binding_list_free(dyn_binding_t *head)
{
    while (head != NULL) {
        dyn_binding_t *next = head->next;
        free(head);
        head = next;
    }
}

/* Look up a var-less dynamic-scope name in the binding stack. Only
 * entries with no canonical var match by text; var-backed bindings
 * are keyed by var identity (dyn_lookup_var) so two same-named vars
 * in different namespaces cannot cross-talk. Frames still building
 * their bindings (`binding` evaluating its value forms) are skipped
 * so the install stays parallel. Returns the value or NULL. */
mino_val *dyn_lookup(mino_state *S, const char *name)
{
    dyn_frame_t *f;
    dyn_binding_t *b;
    for (f = mino_current_ctx(S)->dyn_stack; f != NULL; f = f->prev) {
        if (f->building) continue;
        for (b = f->bindings; b != NULL; b = b->next) {
            if (b->var == NULL && strcmp(b->name, name) == 0) return b->val;
        }
    }
    return NULL;
}

/* Look up a var's thread binding by canonical identity. var_intern
 * dedupes per (ns, name), so pointer equality is the identity test.
 * Returns the bound value or NULL when the var is not thread-bound. */
mino_val *dyn_lookup_var(mino_state *S, const mino_val *var)
{
    dyn_frame_t *f;
    dyn_binding_t *b;
    if (var == NULL) return NULL;
    for (f = mino_current_ctx(S)->dyn_stack; f != NULL; f = f->prev) {
        if (f->building) continue;
        for (b = f->bindings; b != NULL; b = b->next) {
            if (b->var == var) return b->val;
        }
    }
    return NULL;
}

/* Single-walk lookup for a resolved var: newest frame first, an entry
 * matches by var identity or -- for var-less entries only -- by
 * bare-name text. The text criterion bridges the refer gap (a
 * `binding` on a referred name resolves no var at push time and
 * lands var-less), and keeping both criteria in ONE walk preserves
 * shadowing when such an entry sits above a var-keyed one. Var-keyed
 * entries never match by text, so two same-named vars in different
 * namespaces cannot cross-talk. */
mino_val *dyn_lookup_var_or_name(mino_state *S, const mino_val *var,
                                   const char *name)
{
    dyn_frame_t *f;
    dyn_binding_t *b;
    for (f = mino_current_ctx(S)->dyn_stack; f != NULL; f = f->prev) {
        if (f->building) continue;
        for (b = f->bindings; b != NULL; b = b->next) {
            if (b->var == var
                || (b->var == NULL && name != NULL
                    && strcmp(b->name, name) == 0)) {
                return b->val;
            }
        }
    }
    return NULL;
}

/* Resolve a binding-form name (bare or ns-qualified) to its canonical
 * var. Qualified names resolve their alias against the current
 * namespace, mirroring eval_qualified_symbol. Bare names probe the
 * current namespace, then the executing fn's defining namespace, then
 * clojure.core (the referred set) -- the same cascade a root read of
 * the name walks. Returns NULL when no var exists under any
 * candidate (pure dynamic-scope names). `data` must be the
 * NUL-terminated text of an interned symbol. */
mino_val *dyn_resolve_var(mino_state *S, const char *data, size_t n)
{
    const char *cur = S->ns_vars.current_ns != NULL
        ? S->ns_vars.current_ns : "user";
    const char *slash = (n > 1) ? memchr(data, '/', n) : NULL;
    if (slash != NULL && slash != data && (size_t)(slash - data) + 1 < n) {
        char        ns_buf[256];
        size_t      ns_len = (size_t)(slash - data);
        const char *resolved = NULL;
        size_t      i;
        if (ns_len >= sizeof(ns_buf)) return NULL;
        memcpy(ns_buf, data, ns_len);
        ns_buf[ns_len] = '\0';
        for (i = 0; i < S->ns_vars.ns_alias_len; i++) {
            if (S->ns_vars.ns_aliases[i].owning_ns != NULL
                && strcmp(S->ns_vars.ns_aliases[i].owning_ns, cur) == 0
                && strcmp(S->ns_vars.ns_aliases[i].alias, ns_buf) == 0) {
                resolved = S->ns_vars.ns_aliases[i].full_name;
                break;
            }
        }
        return var_find(S, resolved != NULL ? resolved : ns_buf, slash + 1);
    }
    {
        mino_val *var = var_find(S, cur, data);
        if (var == NULL && S->ns_vars.fn_ambient_ns != NULL
            && strcmp(S->ns_vars.fn_ambient_ns, cur) != 0) {
            var = var_find(S, S->ns_vars.fn_ambient_ns, data);
        }
        if (var == NULL && strcmp(cur, "clojure.core") != 0) {
            var = var_find(S, "clojure.core", data);
        }
        return var;
    }
}

/* Combined read-side consult: resolve the symbol text to its
 * canonical var and run the single-walk var-or-name lookup (var-less
 * names fall back to pure text matching). The read paths
 * (eval_symbol, the BC IC resolver) call this only when the dyn
 * stack is non-empty. */
mino_val *dyn_lookup_sym(mino_state *S, const char *data, size_t n)
{
    mino_val *var;
    if (mino_current_ctx(S)->dyn_stack == NULL) return NULL;
    var = dyn_resolve_var(S, data, n);
    if (var != NULL) {
        return dyn_lookup_var_or_name(S, var, var->as.var.sym);
    }
    return dyn_lookup(S, data);
}

/* Build one malloc-owned dyn_binding for `key` (var, symbol, or
 * string) bound to val, resolving symbol/string keys to their
 * canonical var. Returns NULL on OOM or a non-bindable key type;
 * the caller distinguishes the two by pre-validating the key. The
 * node's name borrows interned storage (var-string table for
 * var-backed entries, the symbol intern for var-less ones), so the
 * caller never frees it -- dyn_binding_list_free releases the node
 * only. */
dyn_binding_t *dyn_binding_make(mino_state *S, mino_val *key,
                                mino_val *val, dyn_binding_t *next)
{
    dyn_binding_t *b;
    mino_val    *var      = NULL;
    const char    *name_str = NULL;
    if (key == NULL) return NULL;
    if (mino_type_of(key) == MINO_VAR) {
        var      = key;
        name_str = key->as.var.sym;
    } else if (mino_type_of(key) == MINO_SYMBOL
               || mino_type_of(key) == MINO_STRING) {
        var = dyn_resolve_var(S, key->as.s.data, key->as.s.len);
        name_str = (var != NULL)
            ? var->as.var.sym
            : mino_symbol(S, key->as.s.data)->as.s.data;
    } else {
        return NULL;
    }
    if (name_str == NULL) return NULL;
    b = (dyn_binding_t *)malloc(sizeof(*b));
    if (b == NULL) return NULL;
    b->name = name_str;
    b->var  = var;
    b->val  = val;
    b->next = next;
    return b;
}
