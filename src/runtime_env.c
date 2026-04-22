/*
 * runtime_env.c -- environment allocation, binding, lookup, dynamic bindings.
 *
 * Extracted from mino.c. No behavior change.
 */

#include "mino_internal.h"

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

mino_env_t *env_alloc(mino_state_t *S, mino_env_t *parent)
{
    mino_env_t *env = (mino_env_t *)gc_alloc_typed(S, GC_T_ENV, sizeof(*env));
    env->parent = parent;
    return env;
}

mino_env_t *mino_env_new(mino_state_t *S)
{
    volatile char probe = 0;
    mino_env_t   *env;
    root_env_t   *r;
    /* Record the host's stack frame: this is typically the earliest point
     * the host calls into mino, so it fixes a generous stack bottom before
     * any allocator runs. */
    gc_note_host_frame(S, (void *)&probe);
    (void)probe;
    env = env_alloc(S, NULL);
    r   = (root_env_t *)malloc(sizeof(*r));
    if (r == NULL) {
        set_eval_diag(S, S->eval_current_form, "internal", "MIN001", "out of memory");
        return NULL;
    }
    r->env       = env;
    r->next      = S->gc_root_envs;
    S->gc_root_envs = r;
    return env;
}

mino_env_t *env_child(mino_state_t *S, mino_env_t *parent)
{
    return env_alloc(S, parent);
}

void mino_env_free(mino_state_t *S, mino_env_t *env)
{
    /* Unroot the env. Its memory, along with any closures and bindings
     * reachable only through it, is reclaimed at the next collection. */
    root_env_t **pp = &S->gc_root_envs;
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

static void env_ht_rebuild(mino_state_t *S, mino_env_t *env)
{
    size_t new_cap = env->ht_cap == 0 ? 64 : env->ht_cap * 2;
    size_t *buckets;
    size_t i, mask;
    while (new_cap < env->len * 2) new_cap *= 2;
    buckets = (size_t *)gc_alloc_typed(S, GC_T_RAW, new_cap * sizeof(*buckets));
    mask = new_cap - 1;
    for (i = 0; i < new_cap; i++) buckets[i] = SIZE_MAX;
    for (i = 0; i < env->len; i++) {
        size_t nlen = strlen(env->bindings[i].name);
        uint32_t h = env_hash_name(env->bindings[i].name, nlen);
        size_t idx = h & mask;
        while (buckets[idx] != SIZE_MAX) idx = (idx + 1) & mask;
        buckets[idx] = i;
    }
    gc_write_barrier(S, env, buckets);
    env->ht_buckets = buckets;
    env->ht_cap = new_cap;
}

env_binding_t *env_find_here(mino_env_t *env, const char *name)
{
    /* Hash-indexed lookup for large frames. */
    if (env->ht_buckets != NULL) {
        size_t nlen = strlen(name);
        uint32_t h = env_hash_name(name, nlen);
        size_t mask = env->ht_cap - 1;
        size_t idx = h & mask;
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

/* Shared implementation: interned_name is a stable pointer (e.g. from
 * an intern entry) when known, otherwise NULL. */
static void env_bind_impl(mino_state_t *S, mino_env_t *env,
                          const char *name, size_t nlen,
                          const char *interned_name,
                          mino_val_t *val)
{
    env_binding_t *b = env_find_here(env, name);
    if (b != NULL) {
        /* The env owns the bindings array, so route the barrier
         * through env: gc_trace_children walks every binding's
         * name/val when env is in the remembered set. Pointing at
         * the bindings buffer directly would hit a GC_T_RAW trace
         * stub and miss the new young target. */
        gc_write_barrier(S, env, val);
        b->val = val;
        return;
    }
    if (env->len == env->cap) {
        size_t         new_cap = env->cap == 0 ? 4 : env->cap * 2;
        env_binding_t *nb      = (env_binding_t *)gc_alloc_typed(
            S, GC_T_RAW, new_cap * sizeof(*nb));
        if (env->bindings != NULL && env->len > 0) {
            memcpy(nb, env->bindings, env->len * sizeof(*nb));
        }
        gc_write_barrier(S, env, nb);
        env->bindings = nb;
        env->cap      = new_cap;
    }
    /* Store a stable name pointer so env_find_here can use pointer
     * equality on the hot path. If the caller supplied an already-
     * interned pointer, reuse it; otherwise intern now so future
     * bindings with the same name share a pointer. */
    if (interned_name != NULL) {
        gc_write_barrier(S, env, interned_name);
        env->bindings[env->len].name = (char *)interned_name;
    } else {
        mino_val_t *sym  = mino_symbol_n(S, name, nlen);
        char       *nm   = (sym != NULL) ? sym->as.s.data
                                         : dup_n(S, name, nlen);
        gc_write_barrier(S, env, nm);
        env->bindings[env->len].name = nm;
    }
    gc_write_barrier(S, env, val);
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

void env_bind(mino_state_t *S, mino_env_t *env, const char *name,
              mino_val_t *val)
{
    env_bind_impl(S, env, name, strlen(name), NULL, val);
}

/* Hot-path variant: caller supplies an already-interned symbol, so the
 * binding name pointer and length come free and we skip strlen plus the
 * intern hash-table probe. */
void env_bind_sym(mino_state_t *S, mino_env_t *env, mino_val_t *sym,
                  mino_val_t *val)
{
    env_bind_impl(S, env, sym->as.s.data, sym->as.s.len,
                  sym->as.s.data, val);
}

mino_env_t *env_root(mino_state_t *S, mino_env_t *env)
{
    (void)S;
    while (env->parent != NULL) {
        env = env->parent;
    }
    return env;
}

mino_env_t *mino_env_clone(mino_state_t *S, mino_env_t *env)
{
    if (env == NULL) return NULL;

    /* Allocate a new root env and copy all bindings from the source. */
    mino_env_t *clone = mino_env_new(S);
    size_t i;
    for (i = 0; i < env->len; i++) {
        env_bind(S, clone, env->bindings[i].name, env->bindings[i].val);
    }
    return clone;
}

void mino_env_set(mino_state_t *S, mino_env_t *env, const char *name, mino_val_t *val)
{
    env_bind(S, env, name, val);
}

mino_val_t *mino_env_get(mino_env_t *env, const char *name)
{
    while (env != NULL) {
        env_binding_t *b = env_find_here(env, name);
        if (b != NULL) {
            return b->val;
        }
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

/* Look up a name in the dynamic binding stack.  Returns the value if
 * found, NULL otherwise. */
mino_val_t *dyn_lookup(mino_state_t *S, const char *name)
{
    dyn_frame_t *f;
    dyn_binding_t *b;
    for (f = S->dyn_stack; f != NULL; f = f->prev) {
        for (b = f->bindings; b != NULL; b = b->next) {
            if (strcmp(b->name, name) == 0) return b->val;
        }
    }
    return NULL;
}
