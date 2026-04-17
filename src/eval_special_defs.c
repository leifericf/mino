/*
 * eval_special_defs.c -- def, defmacro, declare special forms.
 *
 * Extracted from eval_special.c. No behavior change.
 */

#include "eval_special_internal.h"

/* --- ns special form helpers --- */

static char *dup_str(const char *s)
{
    size_t n = strlen(s);
    char *d = (char *)malloc(n + 1);
    if (d != NULL) memcpy(d, s, n + 1);
    return d;
}

static void ns_add_alias(mino_state_t *S, const char *alias, const char *full)
{
    size_t i;
    /* Update existing alias if present. */
    for (i = 0; i < S->ns_alias_len; i++) {
        if (strcmp(S->ns_aliases[i].alias, alias) == 0) {
            free(S->ns_aliases[i].full_name);
            S->ns_aliases[i].full_name = dup_str(full);
            return;
        }
    }
    /* Grow if needed. */
    if (S->ns_alias_len == S->ns_alias_cap) {
        size_t new_cap = S->ns_alias_cap == 0 ? 8 : S->ns_alias_cap * 2;
        ns_alias_t *nb = (ns_alias_t *)realloc(
            S->ns_aliases, new_cap * sizeof(*nb));
        if (nb == NULL) return;
        S->ns_aliases   = nb;
        S->ns_alias_cap = new_cap;
    }
    S->ns_aliases[S->ns_alias_len].alias     = dup_str(alias);
    S->ns_aliases[S->ns_alias_len].full_name = dup_str(full);
    S->ns_alias_len++;
}

/* Convert dotted module name to path: "some.lib" -> "some/lib".
 * Writes into buf (which must be at least bufsize bytes). Returns 0 on
 * success, -1 on error (name too long or malformed). */
static int dotted_to_path(const char *name, size_t nlen,
                          char *buf, size_t bufsize)
{
    size_t i;
    if (nlen == 0 || nlen >= bufsize) return -1;
    if (name[0] == '.' || name[nlen - 1] == '.') return -1;
    for (i = 0; i < nlen; i++) {
        buf[i] = (name[i] == '.') ? '/' : name[i];
    }
    buf[nlen] = '\0';
    return 0;
}

/* Process a single require spec from within an ns form.
 * spec is either a symbol (bare require) or a vector [mod.name :as alias ...].
 * Attempts to load the module via the resolver and stores aliases. */
static void ns_process_require_spec(mino_state_t *S, mino_val_t *spec,
                                    mino_env_t *env)
{
    char pathbuf[256];
    const char *modname;
    size_t      modlen;
    const char *alias_name = NULL;
    size_t      alias_len  = 0;

    if (spec->type == MINO_SYMBOL) {
        modname = spec->as.s.data;
        modlen  = spec->as.s.len;
    } else if (spec->type == MINO_VECTOR && spec->as.vec.len >= 1) {
        mino_val_t *first = vec_nth(spec, 0);
        size_t i;
        if (first == NULL || first->type != MINO_SYMBOL) return;
        modname = first->as.s.data;
        modlen  = first->as.s.len;
        /* Parse keyword args: :as, :refer (no-op) */
        for (i = 1; i + 1 < spec->as.vec.len; i += 2) {
            mino_val_t *k = vec_nth(spec, i);
            mino_val_t *v = vec_nth(spec, i + 1);
            if (kw_eq(k, "as") && v->type == MINO_SYMBOL) {
                alias_name = v->as.s.data;
                alias_len  = v->as.s.len;
            }
            /* :refer is a no-op in the flat env */
        }
    } else {
        return; /* skip unrecognized spec form */
    }

    /* Convert dotted name to path and try to load. */
    if (dotted_to_path(modname, modlen, pathbuf, sizeof(pathbuf)) == 0) {
        /* Build require arg as a string and call through existing loader. */
        mino_val_t *path_str = mino_string(S, pathbuf);
        mino_val_t *req_args = mino_cons(S, path_str, mino_nil(S));
        gc_pin(req_args);
        /* Silently ignore load failures for modules we don't have. */
        prim_require(S, req_args, env);
        /* Clear any error from failed require (missing module is OK). */
        S->error_buf[0] = '\0';
        gc_unpin(1);
    }

    /* Store alias if provided. */
    if (alias_name != NULL && alias_len > 0) {
        char abuf[256];
        char fbuf[256];
        if (alias_len < sizeof(abuf) && modlen < sizeof(fbuf)) {
            memcpy(abuf, alias_name, alias_len);
            abuf[alias_len] = '\0';
            memcpy(fbuf, modname, modlen);
            fbuf[modlen] = '\0';
            ns_add_alias(S, abuf, fbuf);
        }
    }
}

mino_val_t *eval_ns(mino_state_t *S, mino_val_t *form,
                    mino_val_t *args, mino_env_t *env)
{
    mino_val_t *rest;
    (void)form;
    if (!mino_is_cons(args)) {
        set_error_at(S, form, "ns requires a name");
        return NULL;
    }
    /* First arg: namespace name symbol. */
    {
        mino_val_t *name_form = args->as.cons.car;
        if (name_form != NULL && name_form->type == MINO_SYMBOL) {
            /* Store as interned string. */
            char buf[256];
            size_t n = name_form->as.s.len;
            if (n < sizeof(buf)) {
                memcpy(buf, name_form->as.s.data, n);
                buf[n] = '\0';
                S->current_ns = intern_filename(buf);
            }
        }
    }
    /* Walk remaining args for (:require ...) and other clauses. */
    rest = args->as.cons.cdr;
    while (mino_is_cons(rest)) {
        mino_val_t *clause = rest->as.cons.car;
        if (mino_is_cons(clause)) {
            mino_val_t *head = clause->as.cons.car;
            if (kw_eq(head, "require")) {
                /* Process each require spec in the clause. */
                mino_val_t *specs = clause->as.cons.cdr;
                while (mino_is_cons(specs)) {
                    ns_process_require_spec(S, specs->as.cons.car, env);
                    specs = specs->as.cons.cdr;
                }
            }
            /* Silently skip :import, :gen-class, :refer-clojure, etc. */
        }
        rest = rest->as.cons.cdr;
    }
    return mino_nil(S);
}

/* --- def, defmacro, declare --- */

mino_val_t *eval_defmacro(mino_state_t *S, mino_val_t *form,
                          mino_val_t *args, mino_env_t *env)
{
    mino_val_t *name_form;
    mino_val_t *params;
    mino_val_t *body;
    mino_val_t *mac;
    mino_val_t *p;
    const char *doc     = NULL;
    size_t      doc_len = 0;
    char        buf[256];
    size_t      n;
    if (!mino_is_cons(args) || !mino_is_cons(args->as.cons.cdr)) {
        set_error_at(S, form, "defmacro requires a name, parameters, and body");
        return NULL;
    }
    name_form = args->as.cons.car;
    if (name_form == NULL || name_form->type != MINO_SYMBOL) {
        set_error_at(S, form, "defmacro name must be a symbol");
        return NULL;
    }
    /* Optional docstring and attr-map:
     *   (defmacro name "doc" {:added "1.0"} [params] body)
     *   (defmacro name "doc" [params] body)
     *   (defmacro name {:added "1.0"} [params] body)
     *   (defmacro name [params] body)
     */
    {
        mino_val_t *rest = args->as.cons.cdr;
        mino_val_t *cur  = rest->as.cons.car;
        /* Optional docstring. */
        if (cur != NULL && cur->type == MINO_STRING
            && mino_is_cons(rest->as.cons.cdr)) {
            doc     = cur->as.s.data;
            doc_len = cur->as.s.len;
            rest    = rest->as.cons.cdr;
            cur     = rest->as.cons.car;
        }
        /* Optional attr-map (skip it). */
        if (cur != NULL && cur->type == MINO_MAP
            && mino_is_cons(rest->as.cons.cdr)) {
            rest = rest->as.cons.cdr;
        }
        params = rest->as.cons.car;
        body   = rest->as.cons.cdr;
    }
    if (!mino_is_cons(params) && !mino_is_nil(params)
        && params->type != MINO_VECTOR) {
        set_error_at(S, form, "defmacro parameter list must be a list or vector");
        return NULL;
    }
    if (mino_is_cons(params) || mino_is_nil(params)) {
        for (p = params; mino_is_cons(p); p = p->as.cons.cdr) {
            mino_val_t *pn = p->as.cons.car;
            if (pn == NULL || pn->type != MINO_SYMBOL) {
                set_error_at(S, form, "defmacro parameter must be a symbol");
                return NULL;
            }
        }
    }
    mac = alloc_val(S, MINO_MACRO);
    mac->as.fn.params = params;
    mac->as.fn.body   = body;
    mac->as.fn.env    = env;
    n = name_form->as.s.len;
    if (n >= sizeof(buf)) {
        set_error_at(S, form, "defmacro name too long");
        return NULL;
    }
    memcpy(buf, name_form->as.s.data, n);
    buf[n] = '\0';
    gc_pin(mac);
    env_bind(S, env_root(S, env), buf, mac);
    gc_unpin(1);
    meta_set(S, buf, doc, doc_len, form);
    return mac;
}

mino_val_t *eval_declare(mino_state_t *S, mino_val_t *form,
                         mino_val_t *args, mino_env_t *env)
{
    mino_val_t *rest = args;
    while (mino_is_cons(rest)) {
        mino_val_t *sym = rest->as.cons.car;
        char buf[256];
        size_t n;
        if (sym == NULL || sym->type != MINO_SYMBOL) {
            set_error_at(S, form, "declare: arguments must be symbols");
            return NULL;
        }
        n = sym->as.s.len;
        if (n >= sizeof(buf)) {
            set_error_at(S, form, "declare: name too long");
            return NULL;
        }
        memcpy(buf, sym->as.s.data, n);
        buf[n] = '\0';
        env_bind(S, env_root(S, env), buf, mino_nil(S));
        rest = rest->as.cons.cdr;
    }
    return mino_nil(S);
}

mino_val_t *eval_def(mino_state_t *S, mino_val_t *form,
                     mino_val_t *args, mino_env_t *env)
{
    mino_val_t *name_form;
    mino_val_t *value_form;
    mino_val_t *value;
    const char *doc     = NULL;
    size_t      doc_len = 0;
    char buf[256];
    size_t n;
    if (!mino_is_cons(args)) {
        set_error_at(S, form, "def requires a name");
        return NULL;
    }
    name_form  = args->as.cons.car;
    if (name_form == NULL || name_form->type != MINO_SYMBOL) {
        set_error_at(S, form, "def name must be a symbol");
        return NULL;
    }
    n = name_form->as.s.len;
    if (n >= sizeof(buf)) {
        set_error_at(S, form, "def name too long");
        return NULL;
    }
    memcpy(buf, name_form->as.s.data, n);
    buf[n] = '\0';
    /* (def name) -- declaration only, bind to nil. */
    if (!mino_is_cons(args->as.cons.cdr)) {
        env_bind(S, env_root(S, env), buf, mino_nil(S));
        meta_set(S, buf, NULL, 0, form);
        return mino_nil(S);
    }
    /* Optional docstring: (def name "doc" value) */
    if (mino_is_cons(args->as.cons.cdr->as.cons.cdr)) {
        mino_val_t *maybe_doc = args->as.cons.cdr->as.cons.car;
        if (maybe_doc != NULL && maybe_doc->type == MINO_STRING) {
            doc       = maybe_doc->as.s.data;
            doc_len   = maybe_doc->as.s.len;
            value_form = args->as.cons.cdr->as.cons.cdr->as.cons.car;
        } else {
            value_form = args->as.cons.cdr->as.cons.car;
        }
    } else {
        value_form = args->as.cons.cdr->as.cons.car;
    }
    value = eval_value(S, value_form, env);
    if (value == NULL) {
        return NULL;
    }
    gc_pin(value);
    env_bind(S, env_root(S, env), buf, value);
    gc_unpin(1);
    meta_set(S, buf, doc, doc_len, form);
    return value;
}
