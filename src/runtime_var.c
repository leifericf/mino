/*
 * runtime_var.c -- var registry: intern, lookup, root binding management.
 */

#include "mino_internal.h"

mino_val_t *var_intern(mino_state_t *S, const char *ns, const char *name)
{
    size_t i;
    mino_val_t *v;

    /* Look for existing var with matching ns + name. */
    for (i = 0; i < S->var_registry_len; i++) {
        if (strcmp(S->var_registry[i].ns, ns) == 0
            && strcmp(S->var_registry[i].name, name) == 0) {
            return S->var_registry[i].var;
        }
    }

    /* Create new var. */
    v = mino_mk_var(S, intern_filename(ns), intern_filename(name),
                    mino_nil(S));
    gc_pin(v);

    /* Grow registry if needed. */
    if (S->var_registry_len == S->var_registry_cap) {
        size_t new_cap = S->var_registry_cap == 0 ? 64 : S->var_registry_cap * 2;
        var_entry_t *nb = (var_entry_t *)realloc(
            S->var_registry, new_cap * sizeof(*nb));
        if (nb == NULL) {
            gc_unpin(1);
            set_error(S, "out of memory");
            return NULL;
        }
        S->var_registry     = nb;
        S->var_registry_cap = new_cap;
    }

    S->var_registry[S->var_registry_len].ns   = v->as.var.ns;
    S->var_registry[S->var_registry_len].name = v->as.var.sym;
    S->var_registry[S->var_registry_len].var  = v;
    S->var_registry_len++;

    gc_unpin(1);
    return v;
}

void var_set_root(mino_state_t *S, mino_val_t *var, mino_val_t *val)
{
    (void)S;
    var->as.var.root = val;
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
