/*
 * runtime_var.c -- var registry: intern, lookup, root binding management.
 */

#include "runtime/internal.h"

extern mino_val_t *prim_throw_classified(mino_state_t *S, const char *kind,
                                          const char *code, const char *msg);

/* Intern a string into the state's var-string table.
 * Strings are malloc-owned and freed at state teardown. */
static const char *intern_var_str(mino_state_t *S, const char *s)
{
    size_t i, n;
    char *d;
    for (i = 0; i < S->interned_var_strs_len; i++) {
        if (strcmp(S->interned_var_strs[i], s) == 0) return S->interned_var_strs[i];
    }
    /* Always allocate a copy — the input may be a stack-local buffer. */
    n = strlen(s);
    d = (char *)malloc(n + 1);
    if (d == NULL) return s;
    memcpy(d, s, n + 1);
    if (S->interned_var_strs_len == S->interned_var_strs_cap) {
        size_t new_cap = S->interned_var_strs_cap == 0
                         ? 64 : S->interned_var_strs_cap * 2;
        const char **nb = (const char **)realloc(
            S->interned_var_strs, new_cap * sizeof(*nb));
        if (nb == NULL) {
            free(d);
            return s;
        }
        S->interned_var_strs     = nb;
        S->interned_var_strs_cap = new_cap;
    }
    S->interned_var_strs[S->interned_var_strs_len++] = d;
    return d;
}

mino_val_t *var_intern(mino_state_t *S, const char *ns, const char *name)
{
    size_t i;
    mino_val_t *v;
    const char *i_ns, *i_name;

    /* Look for existing var with matching ns + name. */
    for (i = 0; i < S->var_registry_len; i++) {
        if (strcmp(S->var_registry[i].ns, ns) == 0
            && strcmp(S->var_registry[i].name, name) == 0) {
            return S->var_registry[i].var;
        }
    }

    /* Intern the strings so they outlive any stack-local buffers. */
    i_ns   = intern_var_str(S, ns);
    i_name = intern_var_str(S, name);

    /* Create new var. */
    v = mino_mk_var(S, i_ns, i_name, mino_nil(S));
    gc_pin(v);

    /* Grow registry if needed. */
    if (S->var_registry_len == S->var_registry_cap) {
        size_t new_cap = S->var_registry_cap == 0 ? 64 : S->var_registry_cap * 2;
        var_entry_t *nb = (var_entry_t *)realloc(
            S->var_registry, new_cap * sizeof(*nb));
        if (nb == NULL) {
            gc_unpin(1);
            set_eval_diag(S, mino_current_ctx(S)->eval_current_form, "internal", "MIN001", "out of memory");
            return NULL;
        }
        S->var_registry     = nb;
        S->var_registry_cap = new_cap;
    }

    S->var_registry[S->var_registry_len].ns   = i_ns;
    S->var_registry[S->var_registry_len].name = i_name;
    S->var_registry[S->var_registry_len].var  = v;
    S->var_registry_len++;

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
        && (watches == NULL || watches->type != MINO_MAP
            || watches->as.map.len == 0)) {
        gc_write_barrier(S, var, var->as.var.root, val);
        var->as.var.root  = val;
        var->as.var.bound = 1;
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
    /* Watches: dispatch after the publish. JVM Clojure's Var watches
     * fire on (alter-var-root v f) and on def with rebind. The
     * callback signature is (fn key var old new). A watch that throws
     * propagates via mino_call returning NULL, matching atoms/refs. */
    if (watches != NULL && watches->type == MINO_MAP
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
    size_t i;
    for (i = 0; i < S->var_registry_len; i++) {
        if (strcmp(S->var_registry[i].ns, ns) == 0
            && strcmp(S->var_registry[i].name, name) == 0) {
            return S->var_registry[i].var;
        }
    }
    return NULL;
}

void var_unintern(mino_state_t *S, const char *ns, const char *name)
{
    size_t i, j;
    for (i = 0; i < S->var_registry_len; i++) {
        if (strcmp(S->var_registry[i].ns, ns) == 0
            && strcmp(S->var_registry[i].name, name) == 0) {
            for (j = i + 1; j < S->var_registry_len; j++) {
                S->var_registry[j - 1] = S->var_registry[j];
            }
            S->var_registry_len--;
            return;
        }
    }
}
