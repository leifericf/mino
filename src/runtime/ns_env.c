/*
 * ns_env.c -- per-namespace root env table.
 *
 * Every namespace gets a root env owning its def/refer bindings. The
 * clojure.core env is special: parent NULL, holds primitives plus
 * everything core.clj installs. Every other ns env has parent →
 * clojure.core, so unqualified lookup that misses a ns env walks naturally
 * into core without an explicit auto-refer.
 *
 * Envs are GC-owned. Each is registered as a GC root the same way
 * mino_env_new does it, so the table can outlive any single eval frame.
 */

#include "runtime/internal.h"

static void ns_env_register_root(mino_state *S, mino_env *env)
{
    root_env_t *r = (root_env_t *)malloc(sizeof(*r));
    if (r == NULL) {
        /* gc_oom_throw longjmps to the active try frame (user-code path)
         * or aborts when no frame exists (init-time Class I OOM). */
        gc_oom_throw(S, "ns_env: out of memory registering root");
    }
    r->env  = env;
    r->next = S->gc.root_envs;
    S->gc.root_envs = r;
}

static void ns_env_table_grow(mino_state *S)
{
    size_t new_cap = S->ns_vars.ns_env_cap == 0 ? 8 : S->ns_vars.ns_env_cap * 2;
    ns_env_entry_t *nb = (ns_env_entry_t *)realloc(
        S->ns_vars.ns_env_table, new_cap * sizeof(*nb));
    if (nb == NULL) {
        /* gc_oom_throw longjmps to the active try frame (user-code path)
         * or aborts when no frame exists (init-time Class I OOM). */
        gc_oom_throw(S, "ns_env: out of memory growing table");
    }
    S->ns_vars.ns_env_table = nb;
    S->ns_vars.ns_env_cap   = new_cap;
}

mino_env *ns_env_lookup(mino_state *S, const char *name)
{
    size_t i;
    if (name == NULL) return NULL;
    for (i = 0; i < S->ns_vars.ns_env_len; i++) {
        const char *n = S->ns_vars.ns_env_table[i].name;
        if (n == name || strcmp(n, name) == 0) {
            return S->ns_vars.ns_env_table[i].env;
        }
    }
    return NULL;
}

mino_env *ns_env_ensure(mino_state *S, const char *name)
{
    mino_env *e;
    const char *iname;
    if (name == NULL) name = "user";

    e = ns_env_lookup(S, name);
    if (e != NULL) return e;

    /* clojure.core must exist before any other ns env so we can wire the
     * parent pointer. Create it lazily on first request. */
    if (S->ns_vars.mino_core_env == NULL) {
        S->ns_vars.mino_core_env = env_alloc(S, NULL);
        ns_env_register_root(S, S->ns_vars.mino_core_env);
        if (S->ns_vars.ns_env_len == S->ns_vars.ns_env_cap) ns_env_table_grow(S);
        S->ns_vars.ns_env_table[S->ns_vars.ns_env_len].name = intern_filename(S, "clojure.core");
        S->ns_vars.ns_env_table[S->ns_vars.ns_env_len].env  = S->ns_vars.mino_core_env;
        S->ns_vars.ns_env_table[S->ns_vars.ns_env_len].meta = NULL;
        S->ns_vars.ns_env_len++;
        if (strcmp(name, "clojure.core") == 0) return S->ns_vars.mino_core_env;
    }

    /* Create the requested ns env with parent → clojure.core. */
    e = env_alloc(S, S->ns_vars.mino_core_env);
    ns_env_register_root(S, e);
    iname = intern_filename(S, name);
    if (S->ns_vars.ns_env_len == S->ns_vars.ns_env_cap) ns_env_table_grow(S);
    S->ns_vars.ns_env_table[S->ns_vars.ns_env_len].name = iname;
    S->ns_vars.ns_env_table[S->ns_vars.ns_env_len].env  = e;
    S->ns_vars.ns_env_table[S->ns_vars.ns_env_len].meta = NULL;
    S->ns_vars.ns_env_len++;
    return e;
}

mino_val *ns_env_get_meta(mino_state *S, const char *name)
{
    size_t i;
    if (name == NULL) return NULL;
    for (i = 0; i < S->ns_vars.ns_env_len; i++) {
        if (strcmp(S->ns_vars.ns_env_table[i].name, name) == 0)
            return S->ns_vars.ns_env_table[i].meta;
    }
    return NULL;
}

void ns_env_set_meta(mino_state *S, const char *name, mino_val *meta)
{
    size_t i;
    if (name == NULL) return;
    for (i = 0; i < S->ns_vars.ns_env_len; i++) {
        if (strcmp(S->ns_vars.ns_env_table[i].name, name) == 0) {
            S->ns_vars.ns_env_table[i].meta = meta;
            return;
        }
    }
}

mino_env *current_ns_env(mino_state *S)
{
    return ns_env_ensure(S, S->ns_vars.current_ns);
}

/* Return a symbol naming NAME, carrying the namespace's metadata
 * (if any) so callers can read it back via `meta`. */
mino_val *ns_symbol_with_meta(mino_state *S, const char *name)
{
    mino_val *sym  = mino_symbol(S, name);
    mino_val *meta = ns_env_get_meta(S, name);
    if (meta != NULL && sym != NULL) {
        mino_val *copy;
        gc_pin(sym); /* alloc_val can trigger GC; keep sym live */
        copy = alloc_val(S, mino_type_of(sym));
        gc_unpin(1);
        copy->as   = sym->as;
        copy->meta = meta;
        return copy;
    }
    return sym;
}

/* Update the clojure.core *ns* var's root binding to a fresh symbol
 * naming the current namespace, so deref of (find-var) on the qualified
 * name tracks user-visible namespace switches. No-op when the var has
 * not yet been interned (init order: install.c interns it after the
 * primitives are registered). */
void mino_publish_current_ns(mino_state *S)
{
    mino_val *var;
    if (S->ns_vars.current_ns == NULL) return;
    var = var_find(S, "clojure.core", "*ns*");
    if (var == NULL) return;
    var_set_root(S, var, ns_symbol_with_meta(S, S->ns_vars.current_ns));
}
