/*
 * eval_special_defs.c -- def, defmacro, declare special forms.
 *
 * Extracted from eval_special.c. No behavior change.
 */

#include "eval_special_internal.h"

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
