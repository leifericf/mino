/*
 * eval_special_defs.c -- def, defmacro, declare special forms.
 *
 * Extracted from eval_special.c. No behavior change.
 */

#include "eval/special_internal.h"

/* --- ns special form helpers ---
 *
 * Module-name path conversion and alias table mutation live in
 * runtime_module.c so this file and prim_module.c share one
 * implementation. */

/* Process a single require spec from within an ns form.
 * spec is either a symbol (bare require) or a vector [mod.name :as alias ...].
 * Attempts to load the module via the resolver and stores aliases.
 * When use_mode is true, default to refer-all (:use semantics).
 * Returns 0 on success, -1 on failure (with a diagnostic set). */
static int ns_process_require_spec_ex(mino_state_t *S, mino_val_t *spec,
                                      mino_env_t *env, int use_mode)
{
    char pathbuf[256];
    const char *modname;
    size_t      modlen;
    const char *alias_name = NULL;
    size_t       alias_len  = 0;
    mino_val_t  *refer_vec  = NULL;
    int          refer_all  = use_mode; /* :use defaults to refer-all */

    if (spec->type == MINO_SYMBOL) {
        modname = spec->as.s.data;
        modlen  = spec->as.s.len;
    } else if (spec->type == MINO_VECTOR && spec->as.vec.len >= 1) {
        mino_val_t *first = vec_nth(spec, 0);
        size_t i;
        if (first == NULL || first->type != MINO_SYMBOL) return 0;
        modname = first->as.s.data;
        modlen  = first->as.s.len;
        /* Parse keyword args: :as, :refer, :only */
        for (i = 1; i + 1 < spec->as.vec.len; i += 2) {
            mino_val_t *k = vec_nth(spec, i);
            mino_val_t *v = vec_nth(spec, i + 1);
            if (kw_eq(k, "as") && v->type == MINO_SYMBOL) {
                alias_name = v->as.s.data;
                alias_len  = v->as.s.len;
            }
            if (kw_eq(k, "refer") && v->type == MINO_VECTOR) {
                refer_vec = v;
                refer_all = 0;
            }
            if (kw_eq(k, "refer") && v->type == MINO_KEYWORD
                && kw_eq(v, "all")) {
                refer_all = 1;
            }
            /* :only is the :use equivalent of :refer */
            if (kw_eq(k, "only") && v->type == MINO_VECTOR) {
                refer_vec = v;
                refer_all = 0;
            }
            /* :only with a list form — build a vector from it */
            if (kw_eq(k, "only") && mino_is_cons(v)) {
                mino_val_t *tmp;
                refer_vec = mino_vector(S, NULL, 0);
                for (tmp = v; mino_is_cons(tmp); tmp = tmp->as.cons.cdr)
                    refer_vec = vec_conj1(S, refer_vec, tmp->as.cons.car);
                refer_all = 0;
            }
        }
    } else {
        return 0; /* skip unrecognized spec form */
    }

    /* Convert dotted name to path and load. A missing or failing module
     * must surface as an error rather than silently succeeding, so the
     * diagnostic prim_require set is left in place when the call fails. */
    if (runtime_module_dotted_to_path(modname, modlen,
                                      pathbuf, sizeof(pathbuf)) == 0) {
        mino_val_t *path_str = mino_string(S, pathbuf);
        mino_val_t *req_args = mino_cons(S, path_str, mino_nil(S));
        mino_val_t *req_res;
        gc_pin(req_args);
        req_res = prim_require(S, req_args, env);
        gc_unpin(1);
        if (req_res == NULL) {
            return -1;
        }
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
            runtime_module_add_alias(S, abuf, fbuf);
        }
    }

    /* Process :refer — bind referred vars into current root env. */
    {
        char modbuf[256];
        if (modlen < sizeof(modbuf)) {
            memcpy(modbuf, modname, modlen);
            modbuf[modlen] = '\0';
            if (refer_vec != NULL) {
                size_t ri;
                for (ri = 0; ri < refer_vec->as.vec.len; ri++) {
                    mino_val_t *rsym = vec_nth(refer_vec, ri);
                    if (rsym != NULL && rsym->type == MINO_SYMBOL) {
                        char rbuf[256];
                        size_t rn = rsym->as.s.len;
                        mino_val_t *var;
                        if (rn >= sizeof(rbuf)) continue;
                        memcpy(rbuf, rsym->as.s.data, rn);
                        rbuf[rn] = '\0';
                        var = var_find(S, modbuf, rbuf);
                        if (var != NULL) {
                            env_bind(S, env_root(S, env), rbuf,
                                     var->as.var.root);
                        } else {
                            /* Fallback: try looking up in root env by bare name. */
                            mino_val_t *val = mino_env_get(env, rbuf);
                            if (val != NULL)
                                env_bind(S, env_root(S, env), rbuf, val);
                        }
                    }
                }
            }
            if (refer_all) {
                size_t vi;
                for (vi = 0; vi < S->var_registry_len; vi++) {
                    if (strcmp(S->var_registry[vi].ns, modbuf) == 0) {
                        env_bind(S, env_root(S, env),
                                 S->var_registry[vi].name,
                                 S->var_registry[vi].var->as.var.root);
                    }
                }
            }
        }
    }
    return 0;
}

static int ns_process_require_spec(mino_state_t *S, mino_val_t *spec,
                                   mino_env_t *env)
{
    return ns_process_require_spec_ex(S, spec, env, 0);
}

static int ns_process_use_spec(mino_state_t *S, mino_val_t *spec,
                               mino_env_t *env)
{
    return ns_process_require_spec_ex(S, spec, env, 1);
}

mino_val_t *eval_ns(mino_state_t *S, mino_val_t *form,
                    mino_val_t *args, mino_env_t *env, int tail)
{
    mino_val_t *rest;
    (void)form;
    (void)tail;
    if (!mino_is_cons(args)) {
        set_eval_diag(S, form, "syntax", "MSY001", "ns requires a name");
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
                S->current_ns = intern_filename(S, buf);
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
                    if (ns_process_require_spec(S, specs->as.cons.car, env) != 0) {
                        return NULL;
                    }
                    specs = specs->as.cons.cdr;
                }
            }
            if (kw_eq(head, "use")) {
                /* :use is like :require but with implicit :refer :all. */
                mino_val_t *specs = clause->as.cons.cdr;
                while (mino_is_cons(specs)) {
                    if (ns_process_use_spec(S, specs->as.cons.car, env) != 0) {
                        return NULL;
                    }
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
                          mino_val_t *args, mino_env_t *env, int tail)
{
    mino_val_t *name_form;
    mino_val_t *params;
    mino_val_t *body;
    mino_val_t *mac;
    const char *doc     = NULL;
    size_t      doc_len = 0;
    (void)tail;
    char        buf[256];
    size_t      n;
    if (!mino_is_cons(args) || !mino_is_cons(args->as.cons.cdr)) {
        set_eval_diag(S, form, "eval/arity", "MAR001", "defmacro requires a name, parameters, and body");
        return NULL;
    }
    name_form = args->as.cons.car;
    if (name_form == NULL || name_form->type != MINO_SYMBOL) {
        set_eval_diag(S, form, "syntax", "MSY001", "defmacro name must be a symbol");
        return NULL;
    }
    /* Optional docstring and attr-map:
     *   (defmacro name "doc" {:added "1.0"} [params] body)
     *   (defmacro name "doc" [params] body)
     *   (defmacro name {:added "1.0"} [params] body)
     *   (defmacro name [params] body)
     *   (defmacro name ([p1] b1) ([p2] b2))     -- multi-arity
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

        /* Detect multi-arity: params is a list whose car is a vector. */
        if (mino_is_cons(params) && params->as.cons.car != NULL
            && params->as.cons.car->type == MINO_VECTOR) {
            mino_val_t *clauses = build_multi_arity_clauses(
                S, form, rest, "MSY001", "defmacro");
            if (clauses == NULL) { return NULL; }
            params = NULL; /* sentinel for multi-arity */
            body   = clauses;
        } else if (!mino_is_cons(params) && !mino_is_nil(params)
                   && params->type != MINO_VECTOR) {
            set_eval_diag(S, form, "syntax", "MSY001",
                          "defmacro parameter list must be a list or vector");
            return NULL;
        }
    }
    mac = alloc_val(S, MINO_MACRO);
    mac->as.fn.params = params;
    mac->as.fn.body   = body;
    mac->as.fn.env    = env;
    n = name_form->as.s.len;
    if (n >= sizeof(buf)) {
        set_eval_diag(S, form, "syntax", "MSY001", "defmacro name too long");
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
                         mino_val_t *args, mino_env_t *env, int tail)
{
    mino_val_t *rest = args;
    (void)tail;
    while (mino_is_cons(rest)) {
        mino_val_t *sym = rest->as.cons.car;
        char buf[256];
        size_t n;
        if (sym == NULL || sym->type != MINO_SYMBOL) {
            set_eval_diag(S, form, "syntax", "MSY001", "declare: arguments must be symbols");
            return NULL;
        }
        n = sym->as.s.len;
        if (n >= sizeof(buf)) {
            set_eval_diag(S, form, "syntax", "MSY001", "declare: name too long");
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
                     mino_val_t *args, mino_env_t *env, int tail)
{
    mino_val_t *name_form;
    mino_val_t *value_form;
    mino_val_t *value;
    const char *doc     = NULL;
    size_t      doc_len = 0;
    char buf[256];
    (void)tail;
    size_t n;
    if (!mino_is_cons(args)) {
        set_eval_diag(S, form, "syntax", "MSY001", "def requires a name");
        return NULL;
    }
    name_form  = args->as.cons.car;
    if (name_form == NULL || name_form->type != MINO_SYMBOL) {
        set_eval_diag(S, form, "syntax", "MSY001", "def name must be a symbol");
        return NULL;
    }
    n = name_form->as.s.len;
    if (n >= sizeof(buf)) {
        set_eval_diag(S, form, "syntax", "MSY001", "def name too long");
        return NULL;
    }
    memcpy(buf, name_form->as.s.data, n);
    buf[n] = '\0';
    /* Check for ^:dynamic metadata on the name symbol. */
    {
        int is_dynamic = 0;
        mino_val_t *m = name_form->meta;
        if (m != NULL && m->type == MINO_MAP) {
            mino_val_t *dk = mino_keyword(S, "dynamic");
            mino_val_t *dv = map_get_val(m, dk);
            if (dv != NULL && mino_is_truthy(dv)) is_dynamic = 1;
        }
        /* (def name) -- declaration only, bind to nil. */
        if (!mino_is_cons(args->as.cons.cdr)) {
            mino_val_t *var = var_intern(S, S->current_ns, buf);
            if (var != NULL) {
                var_set_root(S, var, mino_nil(S));
                if (is_dynamic) var->as.var.dynamic = 1;
            }
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
        {
            mino_val_t *var = var_intern(S, S->current_ns, buf);
            if (var != NULL) {
                var_set_root(S, var, value);
                if (is_dynamic) var->as.var.dynamic = 1;
            }
        }
        env_bind(S, env_root(S, env), buf, value);
        gc_unpin(1);
        meta_set(S, buf, doc, doc_len, form);
        return value;
    }
}
