/*
 * prim_module.c -- module, require, doc, source, apropos primitives.
 *
 * Extracted from prim.c. No behavior change.
 */

#include "prim_internal.h"

static int kw_match(const mino_val_t *v, const char *s)
{
    size_t n;
    if (v == NULL || v->type != MINO_KEYWORD) return 0;
    n = strlen(s);
    return v->as.s.len == n && memcmp(v->as.s.data, s, n) == 0;
}

/* Convert dotted symbol name to slash-separated path in buf.
 * Returns 0 on success, -1 on error. */
static int dots_to_slashes(const char *src, size_t srclen,
                           char *buf, size_t bufsize)
{
    size_t i;
    if (srclen == 0 || srclen >= bufsize) return -1;
    if (src[0] == '.' || src[srclen - 1] == '.') return -1;
    for (i = 0; i < srclen; i++)
        buf[i] = (src[i] == '.') ? '/' : src[i];
    buf[srclen] = '\0';
    return 0;
}

/* (require name) -- load a module by name using the host-supplied resolver.
 * name can be a string ("path/to/mod") or a quoted vector
 * '[mod.name :as alias :refer [syms]]. */
mino_val_t *prim_require(mino_state_t *S, mino_val_t *args, mino_env_t *env)
{
    mino_val_t *name_val;
    const char *name;
    const char *path;
    size_t      i;
    mino_val_t *result;
    if (!mino_is_cons(args) || mino_is_cons(args->as.cons.cdr)) {
        set_eval_diag(S, S->eval_current_form, "eval/arity", "MAR001", "require requires one argument");
        return NULL;
    }
    name_val = args->as.cons.car;

    /* Vector form: '[mod.name :as alias ...] */
    if (name_val != NULL && name_val->type == MINO_VECTOR
        && name_val->as.vec.len >= 1) {
        mino_val_t *mod_sym = vec_nth(name_val, 0);
        char pathbuf[256];
        const char *alias_name = NULL;
        size_t      alias_len  = 0;
        size_t      vi;
        if (mod_sym == NULL || mod_sym->type != MINO_SYMBOL) {
            set_eval_diag(S, S->eval_current_form, "eval/type", "MTY001", "require: vector first element must be a symbol");
            return NULL;
        }
        /* Parse keyword args. */
        for (vi = 1; vi + 1 < name_val->as.vec.len; vi += 2) {
            mino_val_t *k = vec_nth(name_val, vi);
            mino_val_t *v = vec_nth(name_val, vi + 1);
            if (kw_match(k, "as") && v->type == MINO_SYMBOL) {
                alias_name = v->as.s.data;
                alias_len  = v->as.s.len;
            }
            /* :refer is a no-op */
        }
        /* Convert dotted name and load. */
        if (dots_to_slashes(mod_sym->as.s.data, mod_sym->as.s.len,
                            pathbuf, sizeof(pathbuf)) != 0) {
            set_eval_diag(S, S->eval_current_form, "name", "MNS001", "require: invalid module name");
            return NULL;
        }
        {
            mino_val_t *path_str = mino_string(S, pathbuf);
            mino_val_t *str_args = mino_cons(S, path_str, mino_nil(S));
            gc_pin(str_args);
            result = prim_require(S, str_args, env);
            gc_unpin(1);
        }
        /* Store alias. */
        if (result != NULL && alias_name != NULL && alias_len > 0
            && alias_len < 256 && mod_sym->as.s.len < 256) {
            char abuf[256], fbuf[256];
            memcpy(abuf, alias_name, alias_len);
            abuf[alias_len] = '\0';
            memcpy(fbuf, mod_sym->as.s.data, mod_sym->as.s.len);
            fbuf[mod_sym->as.s.len] = '\0';
            /* Add to alias table. */
            if (S->ns_alias_len == S->ns_alias_cap) {
                size_t nc = S->ns_alias_cap == 0 ? 8 : S->ns_alias_cap * 2;
                ns_alias_t *nb = (ns_alias_t *)realloc(
                    S->ns_aliases, nc * sizeof(*nb));
                if (nb != NULL) {
                    S->ns_aliases   = nb;
                    S->ns_alias_cap = nc;
                }
            }
            if (S->ns_alias_len < S->ns_alias_cap) {
                size_t an = alias_len + 1;
                size_t fn = mod_sym->as.s.len + 1;
                S->ns_aliases[S->ns_alias_len].alias =
                    (char *)malloc(an);
                S->ns_aliases[S->ns_alias_len].full_name =
                    (char *)malloc(fn);
                if (S->ns_aliases[S->ns_alias_len].alias != NULL
                    && S->ns_aliases[S->ns_alias_len].full_name != NULL) {
                    memcpy(S->ns_aliases[S->ns_alias_len].alias, abuf, an);
                    memcpy(S->ns_aliases[S->ns_alias_len].full_name, fbuf, fn);
                    S->ns_alias_len++;
                }
            }
        }
        return result != NULL ? result : mino_nil(S);
    }

    if (name_val == NULL || name_val->type != MINO_STRING) {
        set_eval_diag(S, S->eval_current_form, "eval/type", "MTY001", "require: argument must be a string or vector");
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
        set_eval_diag(S, S->eval_current_form, "name", "MNS001", "require: no module resolver configured");
        return NULL;
    }
    path = S->module_resolver(name, S->module_resolver_ctx);
    if (path == NULL) {
        char msg[300];
        snprintf(msg, sizeof(msg), "require: cannot resolve module: %s", name);
        set_eval_diag(S, S->eval_current_form, "name", "MNS001", msg);
        return NULL;
    }
    /* Load — save/restore current namespace so ns forms inside the
     * loaded file don't leak into the caller's namespace. */
    {
        const char *saved_ns = S->current_ns;
        result = mino_load_file(S, path, env);
        S->current_ns = saved_ns;
        if (result == NULL) {
            return NULL;
        }
    }
    /* Cache. */
    if (S->module_cache_len == S->module_cache_cap) {
        size_t         new_cap = S->module_cache_cap == 0 ? 8 : S->module_cache_cap * 2;
        module_entry_t *nb     = (module_entry_t *)realloc(
            S->module_cache, new_cap * sizeof(*nb));
        if (nb == NULL) {
            set_eval_diag(S, S->eval_current_form, "internal", "MIN001", "require: out of memory");
            return NULL;
        }
        S->module_cache     = nb;
        S->module_cache_cap = new_cap;
    }
    {
        size_t nlen = strlen(name);
        char *dup   = (char *)malloc(nlen + 1);
        if (dup == NULL) {
            set_eval_diag(S, S->eval_current_form, "internal", "MIN001", "require: out of memory");
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
        set_eval_diag(S, S->eval_current_form, "eval/arity", "MAR001", "doc requires one argument");
        return NULL;
    }
    name_val = args->as.cons.car;
    if (name_val == NULL || name_val->type != MINO_SYMBOL) {
        set_eval_diag(S, S->eval_current_form, "eval/type", "MTY001", "doc: argument must be a symbol");
        return NULL;
    }
    n = name_val->as.s.len;
    if (n >= sizeof(buf)) {
        set_eval_diag(S, S->eval_current_form, "eval/type", "MTY001", "doc: name too long");
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
        set_eval_diag(S, S->eval_current_form, "eval/arity", "MAR001", "source requires one argument");
        return NULL;
    }
    name_val = args->as.cons.car;
    if (name_val == NULL || name_val->type != MINO_SYMBOL) {
        set_eval_diag(S, S->eval_current_form, "eval/type", "MTY001", "source: argument must be a symbol");
        return NULL;
    }
    n = name_val->as.s.len;
    if (n >= sizeof(buf)) {
        set_eval_diag(S, S->eval_current_form, "eval/type", "MTY001", "source: name too long");
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
        set_eval_diag(S, S->eval_current_form, "eval/arity", "MAR001", "apropos requires one argument");
        return NULL;
    }
    pat_val = args->as.cons.car;
    if (pat_val == NULL || pat_val->type != MINO_STRING) {
        set_eval_diag(S, S->eval_current_form, "eval/type", "MTY001", "apropos: argument must be a string");
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
