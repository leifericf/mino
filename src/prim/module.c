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
    if (!mino_is_cons(args)) {
        set_eval_diag(S, S->eval_current_form, "eval/arity", "MAR001", "require requires at least one argument");
        return NULL;
    }
    /* Multi-arg form: (require 'foo 'bar ...) -- recurse for each. */
    if (mino_is_cons(args->as.cons.cdr)) {
        mino_val_t *cur  = args;
        mino_val_t *last = mino_nil(S);
        while (mino_is_cons(cur)) {
            mino_val_t *one = mino_cons(S, cur->as.cons.car, mino_nil(S));
            gc_pin(one);
            last = prim_require(S, one, env);
            gc_unpin(1);
            if (last == NULL) return NULL;
            cur = cur->as.cons.cdr;
        }
        return last;
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
        /* Runtime-ns shortcut: skip path conversion when the namespace
         * already has substantive bindings (i.e., something has actually
         * loaded into it). An empty placeholder created by :as-alias
         * doesn't count -- the load still has to happen. */
        if (name_val->as.s.len < 256) {
            char dotbuf[256];
            mino_env_t *e;
            memcpy(dotbuf, name_val->as.s.data, name_val->as.s.len);
            dotbuf[name_val->as.s.len] = '\0';
            e = ns_env_lookup(S, dotbuf);
            if (e != NULL && e->len > 0) {
                return mino_nil(S);
            }
        }
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
        const char *alias_name        = NULL;
        size_t      alias_len         = 0;
        const char *as_alias_name     = NULL;
        size_t      as_alias_len      = 0;
        mino_val_t *refer_vec         = NULL;
        int         refer_all         = 0;
        int         needs_load        = 0;
        mino_val_t *exclude_vec       = NULL;
        size_t      vi;
        if (mod_sym == NULL || mod_sym->type != MINO_SYMBOL) {
            set_eval_diag(S, S->eval_current_form, "eval/type", "MTY001", "require: vector first element must be a symbol");
            return NULL;
        }
        /* Parse keyword args.
         * :as / :refer require loading; :as-alias does not. The two
         * alias forms can coexist (an entry can have :as and :as-alias
         * in the same libspec to register two aliases). */
        for (vi = 1; vi + 1 < name_val->as.vec.len; vi += 2) {
            mino_val_t *k = vec_nth(name_val, vi);
            mino_val_t *v = vec_nth(name_val, vi + 1);
            if (kw_match(k, "as") && v->type == MINO_SYMBOL) {
                alias_name = v->as.s.data;
                alias_len  = v->as.s.len;
                needs_load = 1;
            } else if (kw_match(k, "as-alias") && v->type == MINO_SYMBOL) {
                as_alias_name = v->as.s.data;
                as_alias_len  = v->as.s.len;
            } else if (kw_match(k, "refer")) {
                needs_load = 1;
                if (v != NULL && v->type == MINO_VECTOR) {
                    refer_vec = v;
                } else if (v != NULL && v->type == MINO_KEYWORD
                           && kw_match(v, "all")) {
                    refer_all = 1;
                } else {
                    set_eval_diag(S, S->eval_current_form,
                        "eval/type", "MTY001",
                        "require: :refer requires a vector of symbols or :all");
                    return NULL;
                }
            } else if (kw_match(k, "exclude")
                       && v != NULL && v->type == MINO_VECTOR) {
                exclude_vec = v;
                /* :exclude is meaningful only alongside :refer :all (which
                 * use injects); does not by itself trigger a load. */
            }
        }
        /* If only :as-alias is present, register the alias without
         * loading. The target ns is created (empty) so find-ns /
         * the-ns succeed and qualified keywords resolve. */
        if (!needs_load && as_alias_name != NULL) {
            char fbuf[256];
            char abuf[256];
            if (mod_sym->as.s.len >= sizeof(fbuf)
                || as_alias_len >= sizeof(abuf)) {
                set_eval_diag(S, S->eval_current_form, "name", "MNS001",
                              "require: :as-alias name too long");
                return NULL;
            }
            memcpy(fbuf, mod_sym->as.s.data, mod_sym->as.s.len);
            fbuf[mod_sym->as.s.len] = '\0';
            memcpy(abuf, as_alias_name, as_alias_len);
            abuf[as_alias_len] = '\0';
            (void)ns_env_ensure(S, fbuf);
            runtime_module_add_alias(S, abuf, fbuf);
            return mino_nil(S);
        }
        /* Runtime-ns shortcut: skip path conversion only when the
         * namespace already has loaded bindings (an empty alias-only
         * placeholder doesn't count). */
        {
            int runtime_hit = 0;
            if (mod_sym->as.s.len < 256) {
                char dotbuf[256];
                mino_env_t *e;
                memcpy(dotbuf, mod_sym->as.s.data, mod_sym->as.s.len);
                dotbuf[mod_sym->as.s.len] = '\0';
                e = ns_env_lookup(S, dotbuf);
                if (e != NULL && e->len > 0) {
                    runtime_hit = 1;
                    result      = mino_nil(S);
                }
            }
            if (!runtime_hit) {
                /* Convert dotted name and load. */
                if (runtime_module_dotted_to_path(mod_sym->as.s.data,
                                                  mod_sym->as.s.len,
                                                  pathbuf,
                                                  sizeof(pathbuf)) != 0) {
                    set_eval_diag(S, S->eval_current_form, "name",
                                  "MNS001", "require: invalid module name");
                    return NULL;
                }
                {
                    mino_val_t *path_str = mino_string(S, pathbuf);
                    mino_val_t *str_args = mino_cons(S, path_str,
                                                     mino_nil(S));
                    gc_pin(str_args);
                    result = prim_require(S, str_args, env);
                    gc_unpin(1);
                }
                if (result == NULL) return NULL;
            }
        }
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
        /* Store :as-alias when paired with a load-triggering option;
         * the alias-only path returned earlier when no load was needed. */
        if (as_alias_name != NULL && as_alias_len > 0
            && as_alias_len < 256 && mod_sym->as.s.len < 256) {
            char abuf[256];
            char fbuf[256];
            memcpy(abuf, as_alias_name, as_alias_len);
            abuf[as_alias_len] = '\0';
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
                        if (val == NULL) {
                            char msg[300];
                            snprintf(msg, sizeof(msg),
                                "require: %s does not refer var %s",
                                modbuf, rbuf);
                            set_eval_diag(S, S->eval_current_form,
                                "name", "MNS001", msg);
                            return NULL;
                        }
                        env_bind(S, target, rbuf, val);
                    }
                }
            }
            if (refer_all && src != NULL) {
                size_t ri;
                for (ri = 0; ri < src->len; ri++) {
                    const char *bname = src->bindings[ri].name;
                    /* Honor :exclude when paired with :refer :all. */
                    if (exclude_vec != NULL) {
                        size_t  ei, blen = strlen(bname);
                        int     skip = 0;
                        for (ei = 0; ei < exclude_vec->as.vec.len; ei++) {
                            mino_val_t *e = vec_nth(exclude_vec, ei);
                            if (e != NULL && e->type == MINO_SYMBOL
                                && e->as.s.len == blen
                                && memcmp(e->as.s.data, bname, blen) == 0) {
                                skip = 1;
                                break;
                            }
                        }
                        if (skip) continue;
                    }
                    env_bind(S, target,
                             bname,
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
    /* If the dotted form of name corresponds to a runtime-created namespace
     * with substantive bindings, treat it as already loaded. This lets
     * (require 'foo) succeed when foo was made by (ns foo) in-memory.
     * Path names use underscores where ns names use dashes, so try both
     * variants. An empty placeholder created by :as-alias doesn't count. */
    {
        char dotted[512];
        char dashed[512];
        size_t nl = strlen(name);
        size_t k;
        if (nl < sizeof(dotted)) {
            mino_env_t *e;
            for (k = 0; k < nl; k++) dotted[k] = (name[k] == '/') ? '.' : name[k];
            dotted[nl] = '\0';
            e = ns_env_lookup(S, dotted);
            if (e != NULL && e->len > 0) {
                return mino_nil(S);
            }
            for (k = 0; k < nl; k++) dashed[k] = (dotted[k] == '_') ? '-' : dotted[k];
            dashed[nl] = '\0';
            e = ns_env_lookup(S, dashed);
            if (e != NULL && e->len > 0) {
                return mino_nil(S);
            }
        }
    }
    /* Cycle check: name on the load stack means a transitive require
     * looped back. Report the chain. */
    for (i = 0; i < S->load_stack_len; i++) {
        if (strcmp(S->load_stack[i], name) == 0) {
            char msg[512];
            size_t off = 0;
            size_t j;
            off += (size_t)snprintf(msg, sizeof(msg),
                "require: cyclic load dependency: ");
            for (j = i; j < S->load_stack_len && off < sizeof(msg); j++) {
                off += (size_t)snprintf(msg + off, sizeof(msg) - off,
                    "%s -> ", S->load_stack[j]);
            }
            if (off < sizeof(msg)) {
                snprintf(msg + off, sizeof(msg) - off, "%s", name);
            }
            set_eval_diag(S, S->eval_current_form, "name", "MNS001", msg);
            return NULL;
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
    /* Push name onto load stack before loading; pop after. */
    if (S->load_stack_len == S->load_stack_cap) {
        size_t new_cap = S->load_stack_cap == 0 ? 8 : S->load_stack_cap * 2;
        char **nb = (char **)realloc(S->load_stack, new_cap * sizeof(*nb));
        if (nb == NULL) {
            set_eval_diag(S, S->eval_current_form, "internal", "MIN001",
                "require: out of memory");
            return NULL;
        }
        S->load_stack     = nb;
        S->load_stack_cap = new_cap;
    }
    {
        size_t nlen = strlen(name);
        char *dup = (char *)malloc(nlen + 1);
        if (dup == NULL) {
            set_eval_diag(S, S->eval_current_form, "internal", "MIN001",
                "require: out of memory");
            return NULL;
        }
        memcpy(dup, name, nlen + 1);
        S->load_stack[S->load_stack_len++] = dup;
    }
    /* Load — save/restore current namespace so ns forms inside the
     * loaded file don't leak into the caller's namespace. */
    {
        const char *saved_ns = S->current_ns;
        const char *post_ns;
        result  = mino_load_file(S, path, env);
        post_ns = S->current_ns;
        S->current_ns = saved_ns;
        /* Pop load stack regardless of success. */
        if (S->load_stack_len > 0) {
            free(S->load_stack[--S->load_stack_len]);
        }
        if (result == NULL) {
            return NULL;
        }
        /* File-to-ns validation: if the loaded file changed current_ns
         * (i.e. it contained an `(ns x.y)` form) the resulting ns must
         * match the requested module name. Files with no `(ns ...)` are
         * accepted as-is so loading utility scripts by path still works.
         * The ns name may use dashes where the path uses underscores
         * (e.g. ns foo-bar from file foo_bar.mino), so compare in a
         * canonicalized form where both use dashes. */
        if (post_ns != NULL && saved_ns != NULL
            && strcmp(post_ns, saved_ns) != 0) {
            char        expected[256];
            char        post_canon[256];
            size_t      nl = strlen(name);
            size_t      pl = strlen(post_ns);
            size_t      k;
            if (nl < sizeof(expected) && pl < sizeof(post_canon)) {
                for (k = 0; k < nl; k++) {
                    char c = name[k];
                    if (c == '/') c = '.';
                    else if (c == '_') c = '-';
                    expected[k] = c;
                }
                expected[nl] = '\0';
                for (k = 0; k < pl; k++) {
                    char c = post_ns[k];
                    if (c == '_') c = '-';
                    post_canon[k] = c;
                }
                post_canon[pl] = '\0';
                if (strcmp(post_canon, expected) != 0) {
                    char msg[512];
                    snprintf(msg, sizeof(msg),
                        "require: file %s declared namespace %s, expected %s",
                        path, post_ns, expected);
                    set_eval_diag(S, S->eval_current_form,
                        "name", "MNS001", msg);
                    return NULL;
                }
            }
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

/* (use libspec ...) -- like (require ...) but with :refer :all default.
 * Each arg may be a symbol (refer-all) or a vector libspec. :only acts as
 * :refer; explicit :refer/:as pass through. Unrecognized options are left
 * for require to surface. */
mino_val_t *prim_use(mino_state_t *S, mino_val_t *args, mino_env_t *env)
{
    mino_val_t *arg;
    mino_val_t *last = mino_nil(S);
    if (!mino_is_cons(args)) {
        return prim_throw_classified(S, "eval/arity", "MAR001",
            "use requires at least one argument");
    }
    while (mino_is_cons(args)) {
        mino_val_t *libspec;
        size_t      vi, total = 0, j;
        mino_val_t **tmp;
        int         has_refer = 0;
        int         has_only  = 0;
        mino_val_t *only_v    = NULL;
        arg = args->as.cons.car;
        if (arg != NULL && arg->type == MINO_SYMBOL) {
            /* Symbol form: (use 'foo) -> (require '[foo :refer :all]) */
            mino_val_t **buf = (mino_val_t **)gc_alloc_typed(
                S, GC_T_VALARR, 3 * sizeof(*buf));
            gc_valarr_set(S, buf, 0, arg);
            gc_valarr_set(S, buf, 1, mino_keyword(S, "refer"));
            gc_valarr_set(S, buf, 2, mino_keyword(S, "all"));
            libspec = mino_vector(S, buf, 3);
        } else if (arg != NULL && arg->type == MINO_VECTOR
                   && arg->as.vec.len >= 1) {
            /* Inspect existing options. */
            for (vi = 1; vi + 1 < arg->as.vec.len; vi += 2) {
                mino_val_t *k = vec_nth(arg, vi);
                mino_val_t *v = vec_nth(arg, vi + 1);
                if (kw_match(k, "refer")) has_refer = 1;
                if (kw_match(k, "only"))  { has_only = 1; only_v = v; }
            }
            if (has_only) {
                /* Replace :only with :refer; pass everything else through. */
                total = arg->as.vec.len;
                tmp   = (mino_val_t **)gc_alloc_typed(
                    S, GC_T_VALARR, total * sizeof(*tmp));
                gc_valarr_set(S, tmp, 0, vec_nth(arg, 0));
                j = 1;
                for (vi = 1; vi + 1 < arg->as.vec.len; vi += 2) {
                    mino_val_t *k = vec_nth(arg, vi);
                    mino_val_t *v = vec_nth(arg, vi + 1);
                    if (kw_match(k, "only")) {
                        gc_valarr_set(S, tmp, j++,
                                      mino_keyword(S, "refer"));
                        gc_valarr_set(S, tmp, j++, only_v);
                    } else {
                        gc_valarr_set(S, tmp, j++, k);
                        gc_valarr_set(S, tmp, j++, v);
                    }
                }
                libspec = mino_vector(S, tmp, total);
            } else if (!has_refer) {
                /* Append :refer :all. */
                total = arg->as.vec.len + 2;
                tmp   = (mino_val_t **)gc_alloc_typed(
                    S, GC_T_VALARR, total * sizeof(*tmp));
                for (vi = 0; vi < arg->as.vec.len; vi++) {
                    gc_valarr_set(S, tmp, vi, vec_nth(arg, vi));
                }
                gc_valarr_set(S, tmp, arg->as.vec.len,
                              mino_keyword(S, "refer"));
                gc_valarr_set(S, tmp, arg->as.vec.len + 1,
                              mino_keyword(S, "all"));
                libspec = mino_vector(S, tmp, total);
            } else {
                /* Already has :refer; pass through. */
                libspec = arg;
            }
        } else {
            return prim_throw_classified(S, "eval/type", "MTY001",
                "use: arg must be a symbol or vector");
        }
        {
            mino_val_t *call_args = mino_cons(S, libspec, mino_nil(S));
            gc_pin(call_args);
            last = prim_require(S, call_args, env);
            gc_unpin(1);
            if (last == NULL) return NULL;
        }
        args = args->as.cons.cdr;
    }
    return last;
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
    /* Walk every env frame from the given env up to root, then also
     * the current namespace chain so primitives interned in mino.core
     * are reachable when the caller env doesn't chain into it. */
    {
        mino_env_t *chains[2];
        size_t      ci;
        chains[0] = env;
        chains[1] = current_ns_env(S);
        for (ci = 0; ci < 2; ci++) {
            for (e = chains[ci]; e != NULL; e = e->parent) {
                size_t i;
                if (ci == 1 && e == chains[0]) continue;
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
    {"use",     prim_use,
     "Loads a module and refers all of its public names by default."},
    {"doc",     prim_doc,
     "Prints the documentation for the named var."},
    {"source",  prim_source,
     "Prints the source code of the named var."},
    {"apropos", prim_apropos,
     "Returns a list of vars whose names match the given pattern."},
};

const size_t k_prims_module_count =
    sizeof(k_prims_module) / sizeof(k_prims_module[0]);
