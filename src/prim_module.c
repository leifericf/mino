/*
 * prim_module.c -- module, require, doc, source, apropos primitives.
 *
 * Extracted from prim.c. No behavior change.
 */

#include "prim_internal.h"

/* (require name) -- load a module by name using the host-supplied resolver. */
mino_val_t *prim_require(mino_state_t *S, mino_val_t *args, mino_env_t *env)
{
    mino_val_t *name_val;
    const char *name;
    const char *path;
    size_t      i;
    mino_val_t *result;
    if (!mino_is_cons(args) || mino_is_cons(args->as.cons.cdr)) {
        set_error(S, "require requires one argument");
        return NULL;
    }
    name_val = args->as.cons.car;
    if (name_val == NULL || name_val->type != MINO_STRING) {
        set_error(S, "require: argument must be a string");
        return NULL;
    }
    name = name_val->as.s.data;
    /* Check cache. */
    for (i = 0; i < S->module_cache_len; i++) {
        if (strcmp(S->module_cache[i].name, name) == 0) {
            return S->module_cache[i].value;
        }
    }
    /* Resolve. */
    if (S->module_resolver == NULL) {
        set_error(S, "require: no module resolver configured");
        return NULL;
    }
    path = S->module_resolver(name, S->module_resolver_ctx);
    if (path == NULL) {
        char msg[300];
        snprintf(msg, sizeof(msg), "require: cannot resolve module: %s", name);
        set_error(S, msg);
        return NULL;
    }
    /* Load. */
    result = mino_load_file(S, path, env);
    if (result == NULL) {
        return NULL;
    }
    /* Cache. */
    if (S->module_cache_len == S->module_cache_cap) {
        size_t         new_cap = S->module_cache_cap == 0 ? 8 : S->module_cache_cap * 2;
        module_entry_t *nb     = (module_entry_t *)realloc(
            S->module_cache, new_cap * sizeof(*nb));
        if (nb == NULL) {
            set_error(S, "require: out of memory");
            return NULL;
        }
        S->module_cache     = nb;
        S->module_cache_cap = new_cap;
    }
    {
        size_t nlen = strlen(name);
        char *dup   = (char *)malloc(nlen + 1);
        if (dup == NULL) {
            set_error(S, "require: out of memory");
            return NULL;
        }
        memcpy(dup, name, nlen + 1);
        S->module_cache[S->module_cache_len].name  = dup;
        S->module_cache[S->module_cache_len].value = result;
        S->module_cache_len++;
    }
    return result;
}

/* (doc name) -- print the docstring for a def/defmacro binding. */
mino_val_t *prim_doc(mino_state_t *S, mino_val_t *args, mino_env_t *env)
{
    mino_val_t   *name_val;
    char          buf[256];
    size_t        n;
    meta_entry_t *e;
    (void)env;
    if (!mino_is_cons(args) || mino_is_cons(args->as.cons.cdr)) {
        set_error(S, "doc requires one argument");
        return NULL;
    }
    name_val = args->as.cons.car;
    if (name_val == NULL || name_val->type != MINO_SYMBOL) {
        set_error(S, "doc: argument must be a symbol");
        return NULL;
    }
    n = name_val->as.s.len;
    if (n >= sizeof(buf)) {
        set_error(S, "doc: name too long");
        return NULL;
    }
    memcpy(buf, name_val->as.s.data, n);
    buf[n] = '\0';
    e = meta_find(S, buf);
    if (e != NULL && e->docstring != NULL) {
        return mino_string(S, e->docstring);
    }
    return mino_nil(S);
}

/* (source name) -- return the source form of a def/defmacro binding. */
mino_val_t *prim_source(mino_state_t *S, mino_val_t *args, mino_env_t *env)
{
    mino_val_t   *name_val;
    char          buf[256];
    size_t        n;
    meta_entry_t *e;
    (void)env;
    if (!mino_is_cons(args) || mino_is_cons(args->as.cons.cdr)) {
        set_error(S, "source requires one argument");
        return NULL;
    }
    name_val = args->as.cons.car;
    if (name_val == NULL || name_val->type != MINO_SYMBOL) {
        set_error(S, "source: argument must be a symbol");
        return NULL;
    }
    n = name_val->as.s.len;
    if (n >= sizeof(buf)) {
        set_error(S, "source: name too long");
        return NULL;
    }
    memcpy(buf, name_val->as.s.data, n);
    buf[n] = '\0';
    e = meta_find(S, buf);
    if (e != NULL && e->source != NULL) {
        return e->source;
    }
    return mino_nil(S);
}

/* (apropos substring) -- return a list of bound names containing substring. */
mino_val_t *prim_apropos(mino_state_t *S, mino_val_t *args, mino_env_t *env)
{
    mino_val_t *pat_val;
    const char *pat;
    mino_val_t *head = mino_nil(S);
    mino_val_t *tail = NULL;
    mino_env_t *e;
    if (!mino_is_cons(args) || mino_is_cons(args->as.cons.cdr)) {
        set_error(S, "apropos requires one argument");
        return NULL;
    }
    pat_val = args->as.cons.car;
    if (pat_val == NULL || pat_val->type != MINO_STRING) {
        set_error(S, "apropos: argument must be a string");
        return NULL;
    }
    pat = pat_val->as.s.data;
    /* Walk every env frame from the given env up to root. */
    for (e = env; e != NULL; e = e->parent) {
        size_t i;
        for (i = 0; i < e->len; i++) {
            if (strstr(e->bindings[i].name, pat) != NULL) {
                mino_val_t *sym  = mino_symbol(S, e->bindings[i].name);
                mino_val_t *cell = mino_cons(S, sym, mino_nil(S));
                if (tail == NULL) {
                    head = cell;
                } else {
                    tail->as.cons.cdr = cell;
                }
                tail = cell;
            }
        }
    }
    return head;
}

void mino_set_resolver(mino_state_t *S, mino_resolve_fn fn, void *ctx)
{
    S->module_resolver     = fn;
    S->module_resolver_ctx = ctx;
}
