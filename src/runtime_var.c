/*
 * runtime_var.c -- var registry: intern, lookup, root binding management.
 */

#include "mino_internal.h"

/* Intern a string into a persistent table (separate from the filename table). */
static const char *intern_var_str(const char *s)
{
    static const char *tbl[4096];
    static size_t      tbl_len = 0;
    size_t i, n;
    char *d;
    for (i = 0; i < tbl_len; i++) {
        if (strcmp(tbl[i], s) == 0) return tbl[i];
    }
    /* Always allocate a copy — the input may be a stack-local buffer. */
    n = strlen(s);
    d = (char *)malloc(n + 1);
    if (d == NULL) return s;
    memcpy(d, s, n + 1);
    if (tbl_len < sizeof(tbl) / sizeof(tbl[0])) {
        tbl[tbl_len++] = d;
    }
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
    i_ns   = intern_var_str(ns);
    i_name = intern_var_str(name);

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
            set_eval_diag(S, S->eval_current_form, "internal", "MIN001", "out of memory");
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
    gc_write_barrier(S, var, val);
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
