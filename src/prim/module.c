/*
 * module.c -- module, require, doc, source, apropos primitives.
 */

#include "prim/internal.h"

static int kw_match(const mino_val_t *v, const char *s)
{
    size_t n;
    if (v == NULL || v->type != MINO_KEYWORD) return 0;
    n = strlen(s);
    return v->as.s.len == n && memcmp(v->as.s.data, s, n) == 0;
}

/* Dotted-name conversion and alias table mutation live in
 * runtime/module.c so this file and eval/defs.c share the
 * same logic. */

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

    /* Prefix list: '[prefix sub1 sub2 ...] where each sub is either a
     * bare symbol or a vector libspec. Expands to multiple individual
     * (require '[prefix.sub ...]) calls. The disambiguator: if the
     * second element isn't a keyword, the form is a prefix list. */
    if (name_val != NULL && name_val->type == MINO_VECTOR
        && name_val->as.vec.len >= 2) {
        mino_val_t *first  = vec_nth(name_val, 0);
        mino_val_t *second = vec_nth(name_val, 1);
        if (first != NULL && first->type == MINO_SYMBOL
            && second != NULL && second->type != MINO_KEYWORD) {
            size_t      i;
            mino_val_t *last_result = mino_nil(S);
            for (i = 1; i < name_val->as.vec.len; i++) {
                mino_val_t *sub = vec_nth(name_val, i);
                mino_val_t *sub_name;
                mino_val_t *sub_rest = mino_nil(S);
                char        joined[512];
                size_t      sn;
                if (sub == NULL) continue;
                if (sub->type == MINO_SYMBOL) {
                    sub_name = sub;
                } else if (sub->type == MINO_VECTOR
                           && sub->as.vec.len >= 1) {
                    sub_name = vec_nth(sub, 0);
                    if (sub_name == NULL || sub_name->type != MINO_SYMBOL) {
                        return prim_throw_classified(S,
                            "eval/type", "MTY001",
                            "require prefix list: subspec head must be a symbol");
                    }
                } else {
                    return prim_throw_classified(S,
                        "eval/type", "MTY001",
                        "require prefix list: subspec must be symbol or vector");
                }
                if (memchr(sub_name->as.s.data, '.',
                           sub_name->as.s.len) != NULL) {
                    return prim_throw_classified(S,
                        "name", "MNS001",
                        "lib names inside prefix lists must not contain periods");
                }
                sn = first->as.s.len + 1 + sub_name->as.s.len;
                if (sn >= sizeof(joined)) {
                    return prim_throw_classified(S,
                        "eval/type", "MTY001",
                        "require: prefix list name too long");
                }
                memcpy(joined, first->as.s.data, first->as.s.len);
                joined[first->as.s.len] = '.';
                memcpy(joined + first->as.s.len + 1,
                       sub_name->as.s.data, sub_name->as.s.len);
                joined[sn] = '\0';
                /* Build [joined-symbol opt1 v1 opt2 v2 ...] vector. */
                {
                    mino_val_t *joined_sym = mino_symbol_n(S, joined, sn);
                    size_t      tail_len   = (sub->type == MINO_VECTOR)
                        ? sub->as.vec.len - 1 : 0;
                    size_t      total      = 1 + tail_len;
                    mino_val_t **tmp       = (mino_val_t **)gc_alloc_typed(
                        S, GC_T_VALARR, total * sizeof(*tmp));
                    size_t      j;
                    mino_val_t *libspec;
                    mino_val_t *call_args;
                    gc_valarr_set(S, tmp, 0, joined_sym);
                    for (j = 0; j < tail_len; j++) {
                        gc_valarr_set(S, tmp, 1 + j,
                                      vec_nth(sub, j + 1));
                    }
                    libspec   = mino_vector(S, tmp, total);
                    call_args = mino_cons(S, libspec, mino_nil(S));
                    gc_pin(call_args);
                    last_result = prim_require(S, call_args, env);
                    gc_unpin(1);
                    if (last_result == NULL) return NULL;
                    (void)sub_rest;
                }
            }
            return last_result;
        }
    }

    /* Symbol form: '(require 'foo.bar) — Clojure's everyday form. */
    if (name_val != NULL && name_val->type == MINO_SYMBOL) {
        char pathbuf[256];
        if (runtime_module_dotted_to_path(name_val->as.s.data,
                                          name_val->as.s.len,
                                          pathbuf, sizeof(pathbuf)) != 0) {
            set_eval_diag(S, S->eval_current_form, "name", "MNS001",
                          "require: invalid module name");
            return NULL;
        }
        {
            mino_val_t *path_str = mino_string(S, pathbuf);
            mino_val_t *str_args = mino_cons(S, path_str, mino_nil(S));
            gc_pin(str_args);
            result = prim_require(S, str_args, env);
            gc_unpin(1);
        }
        return result;
    }

    /* Vector form: '[mod.name :as alias :refer [syms]] */
    if (name_val != NULL && name_val->type == MINO_VECTOR
        && name_val->as.vec.len >= 1) {
        mino_val_t *mod_sym = vec_nth(name_val, 0);
        char pathbuf[256];
        const char *alias_name = NULL;
        size_t      alias_len  = 0;
        mino_val_t *refer_vec  = NULL;
        int         refer_all  = 0;
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
            if (kw_match(k, "refer") && v->type == MINO_VECTOR) {
                refer_vec = v;
            }
            if (kw_match(k, "refer") && v->type == MINO_KEYWORD
                && kw_match(v, "all")) {
                refer_all = 1;
            }
        }
        /* Convert dotted name and load. */
        if (runtime_module_dotted_to_path(mod_sym->as.s.data,
                                          mod_sym->as.s.len,
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
        if (result == NULL) return NULL;
        /* Store alias. */
        if (alias_name != NULL && alias_len > 0
            && alias_len < 256 && mod_sym->as.s.len < 256) {
            char abuf[256];
            char fbuf[256];
            memcpy(abuf, alias_name, alias_len);
            abuf[alias_len] = '\0';
            memcpy(fbuf, mod_sym->as.s.data, mod_sym->as.s.len);
            fbuf[mod_sym->as.s.len] = '\0';
            runtime_module_add_alias(S, abuf, fbuf);
        }
        /* Process :refer — bind referred names into current ns env.
         * Iterate the source ns env directly so macros (which live in
         * the env without a var registry entry) come through too. */
        if (mod_sym->as.s.len < 256 && (refer_vec != NULL || refer_all)) {
            char modbuf[256];
            mino_env_t *target = current_ns_env(S);
            mino_env_t *src;
            memcpy(modbuf, mod_sym->as.s.data, mod_sym->as.s.len);
            modbuf[mod_sym->as.s.len] = '\0';
            src = ns_env_lookup(S, modbuf);
            if (refer_vec != NULL) {
                size_t ri;
                for (ri = 0; ri < refer_vec->as.vec.len; ri++) {
                    mino_val_t *rsym = vec_nth(refer_vec, ri);
                    if (rsym != NULL && rsym->type == MINO_SYMBOL
                        && rsym->as.s.len < 256) {
                        char rbuf[256];
                        size_t rn = rsym->as.s.len;
                        mino_val_t *val = NULL;
                        memcpy(rbuf, rsym->as.s.data, rn);
                        rbuf[rn] = '\0';
                        if (src != NULL) {
                            env_binding_t *b = env_find_here(src, rbuf);
                            if (b != NULL) val = b->val;
                        }
                        if (val == NULL) {
                            mino_val_t *var = var_find(S, modbuf, rbuf);
                            if (var != NULL) val = var->as.var.root;
                        }
                        if (val != NULL) env_bind(S, target, rbuf, val);
                    }
                }
            }
            if (refer_all && src != NULL) {
                size_t ri;
                for (ri = 0; ri < src->len; ri++) {
                    env_bind(S, target,
                             src->bindings[ri].name,
                             src->bindings[ri].val);
                }
            }
        }
        return result;
    }

    if (name_val == NULL || name_val->type != MINO_STRING) {
        set_eval_diag(S, S->eval_current_form, "eval/type", "MTY001", "require: argument must be a string, symbol, or vector");
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
                    mino_cons_cdr_set(S, tail, cell);
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

const mino_prim_def k_prims_module[] = {
    {"require", prim_require,
     "Loads and evaluates a mino source file."},
    {"doc",     prim_doc,
     "Prints the documentation for the named var."},
    {"source",  prim_source,
     "Prints the source code of the named var."},
    {"apropos", prim_apropos,
     "Returns a list of vars whose names match the given pattern."},
};

const size_t k_prims_module_count =
    sizeof(k_prims_module) / sizeof(k_prims_module[0]);
