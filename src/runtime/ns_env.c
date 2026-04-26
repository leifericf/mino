/*
 * ns_env.c -- per-namespace root env table.
 *
 * Every namespace gets a root env owning its def/refer bindings. The
 * mino.core env is special: parent NULL, holds primitives plus everything
 * core.mino installs. Every other ns env has parent → mino.core, so
 * unqualified lookup that misses a ns env walks naturally into core
 * without an explicit auto-refer.
 *
 * Envs are GC-owned. Each is registered as a GC root the same way
 * mino_env_new does it, so the table can outlive any single eval frame.
 */

#include "runtime/internal.h"

static void ns_env_register_root(mino_state_t *S, mino_env_t *env)
{
    root_env_t *r = (root_env_t *)malloc(sizeof(*r));
    if (r == NULL) {
        /* Init-time path: no try-frame to recover through. */
        fprintf(stderr, "ns_env: out of memory registering root\n");
        abort();
    }
    r->env  = env;
    r->next = S->gc_root_envs;
    S->gc_root_envs = r;
}

static void ns_env_table_grow(mino_state_t *S)
{
    size_t new_cap = S->ns_env_cap == 0 ? 8 : S->ns_env_cap * 2;
    ns_env_entry_t *nb = (ns_env_entry_t *)realloc(
        S->ns_env_table, new_cap * sizeof(*nb));
    if (nb == NULL) {
        fprintf(stderr, "ns_env: out of memory growing table\n");
        abort();
    }
    S->ns_env_table = nb;
    S->ns_env_cap   = new_cap;
}

mino_env_t *ns_env_lookup(mino_state_t *S, const char *name)
{
    size_t i;
    if (name == NULL) return NULL;
    for (i = 0; i < S->ns_env_len; i++) {
        const char *n = S->ns_env_table[i].name;
        if (n == name || strcmp(n, name) == 0) {
            return S->ns_env_table[i].env;
        }
    }
    return NULL;
}

mino_env_t *ns_env_ensure(mino_state_t *S, const char *name)
{
    mino_env_t *e;
    const char *iname;
    if (name == NULL) name = "user";

    e = ns_env_lookup(S, name);
    if (e != NULL) return e;

    /* mino.core must exist before any other ns env so we can wire the
     * parent pointer. Create it lazily on first request. */
    if (S->mino_core_env == NULL) {
        S->mino_core_env = env_alloc(S, NULL);
        ns_env_register_root(S, S->mino_core_env);
        if (S->ns_env_len == S->ns_env_cap) ns_env_table_grow(S);
        S->ns_env_table[S->ns_env_len].name = intern_filename(S, "mino.core");
        S->ns_env_table[S->ns_env_len].env  = S->mino_core_env;
        S->ns_env_table[S->ns_env_len].meta = NULL;
        S->ns_env_len++;
        if (strcmp(name, "mino.core") == 0) return S->mino_core_env;
    }

    /* Create the requested ns env with parent → mino.core. */
    e = env_alloc(S, S->mino_core_env);
    ns_env_register_root(S, e);
    iname = intern_filename(S, name);
    if (S->ns_env_len == S->ns_env_cap) ns_env_table_grow(S);
    S->ns_env_table[S->ns_env_len].name = iname;
    S->ns_env_table[S->ns_env_len].env  = e;
    S->ns_env_table[S->ns_env_len].meta = NULL;
    S->ns_env_len++;
    return e;
}

mino_val_t *ns_env_get_meta(mino_state_t *S, const char *name)
{
    size_t i;
    if (name == NULL) return NULL;
    for (i = 0; i < S->ns_env_len; i++) {
        if (strcmp(S->ns_env_table[i].name, name) == 0)
            return S->ns_env_table[i].meta;
    }
    return NULL;
}

void ns_env_set_meta(mino_state_t *S, const char *name, mino_val_t *meta)
{
    size_t i;
    if (name == NULL) return;
    for (i = 0; i < S->ns_env_len; i++) {
        if (strcmp(S->ns_env_table[i].name, name) == 0) {
            S->ns_env_table[i].meta = meta;
            return;
        }
    }
}

mino_env_t *current_ns_env(mino_state_t *S)
{
    return ns_env_ensure(S, S->current_ns);
}
