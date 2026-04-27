/*
 * module.c -- module, require, doc, source, apropos primitives.
 */

#include "prim/internal.h"

static const char *bundled_lib_lookup(mino_state_t *S, const char *name);

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
    const char *bundled_source = NULL;
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
        /* Runtime-ns shortcut: skip the file load when either the
         * file's already in module_cache (loaded earlier) or no file
         * backs the namespace (caller built it at runtime via
         * (ns foo) (def ...) ). Primitive-only env entries don't
         * trigger a skip on first load -- the wrapper file still
         * has to run. */
        if (name_val->as.s.len < 256) {
            char dotbuf[256];
            char shortcut_path[256];
            mino_env_t *e;
            int         path_ok;
            memcpy(dotbuf, name_val->as.s.data, name_val->as.s.len);
            dotbuf[name_val->as.s.len] = '\0';
            e = ns_env_lookup(S, dotbuf);
            path_ok = runtime_module_dotted_to_path(
                          name_val->as.s.data, name_val->as.s.len,
                          shortcut_path, sizeof(shortcut_path)) == 0;
            if (e != NULL && e->len > 0) {
                if (path_ok) {
                    size_t ci;
                    for (ci = 0; ci < S->module_cache_len; ci++) {
                        if (strcmp(S->module_cache[ci].name,
                                   shortcut_path) == 0) {
                            return mino_nil(S);
                        }
                    }
                } else {
                    /* No filesystem path -- runtime-only namespace. */
                    return mino_nil(S);
                }
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
        mino_val_t *rename_map        = NULL;
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
            } else if (kw_match(k, "rename")
                       && v != NULL && v->type == MINO_MAP) {
                rename_map = v;
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
        /* Skip the file load only when the file's already in
         * module_cache (loaded earlier in this state) or when no file
         * backs the namespace (caller built it at runtime via
         * (ns foo) (def ...) ). Pre-installed C primitives populate
         * the env and the var registry at install time, but the
         * wrapper .clj still has to run on first require so its
         * defs/macros become visible to :refer. Mirrors the
         * symbol-form check above. */
        {
            int skip_load = 0;
            if (mod_sym->as.s.len < 256) {
                char dotbuf[256];
                char shortcut_path[256];
                mino_env_t *e;
                int         path_ok;
                memcpy(dotbuf, mod_sym->as.s.data, mod_sym->as.s.len);
                dotbuf[mod_sym->as.s.len] = '\0';
                e = ns_env_lookup(S, dotbuf);
                path_ok = runtime_module_dotted_to_path(
                              mod_sym->as.s.data, mod_sym->as.s.len,
                              shortcut_path, sizeof(shortcut_path)) == 0;
                if (e != NULL && e->len > 0) {
                    if (path_ok) {
                        size_t ci;
                        for (ci = 0; ci < S->module_cache_len; ci++) {
                            if (strcmp(S->module_cache[ci].name,
                                       shortcut_path) == 0) {
                                skip_load = 1;
                                result    = mino_nil(S);
                                break;
                            }
                        }
                    } else {
                        /* No filesystem path -- runtime-only namespace. */
                        skip_load = 1;
                        result    = mino_nil(S);
                    }
                }
            }
            if (!skip_load) {
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
                        const char *bind_name = rbuf;
                        size_t      bind_len  = rn;
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
                        if (rename_map != NULL && rename_map->type == MINO_MAP) {
                            size_t mi;
                            for (mi = 0; mi < rename_map->as.map.len; mi++) {
                                mino_val_t *k = vec_nth(rename_map->as.map.key_order, mi);
                                if (k != NULL && k->type == MINO_SYMBOL
                                    && k->as.s.len == rn
                                    && memcmp(k->as.s.data, rbuf, rn) == 0) {
                                    mino_val_t *renamed = map_get_val(rename_map, k);
                                    if (renamed != NULL && renamed->type == MINO_SYMBOL) {
                                        bind_name = renamed->as.s.data;
                                        bind_len  = renamed->as.s.len;
                                    }
                                    break;
                                }
                            }
                        }
                        if (bind_len < sizeof(rbuf)) {
                            char nbuf[256];
                            memcpy(nbuf, bind_name, bind_len);
                            nbuf[bind_len] = '\0';
                            env_bind(S, target, nbuf, val);
                        }
                    }
                }
            }
            if (refer_all && src != NULL) {
                size_t ri;
                for (ri = 0; ri < src->len; ri++) {
                    const char *bname = src->bindings[ri].name;
                    size_t       blen  = strlen(bname);
                    const char *bind_name = bname;
                    size_t      bind_len  = blen;
                    /* Honor :exclude when paired with :refer :all. */
                    if (exclude_vec != NULL) {
                        size_t  ei;
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
                    if (rename_map != NULL && rename_map->type == MINO_MAP) {
                        size_t mi;
                        for (mi = 0; mi < rename_map->as.map.len; mi++) {
                            mino_val_t *k = vec_nth(rename_map->as.map.key_order, mi);
                            if (k != NULL && k->type == MINO_SYMBOL
                                && k->as.s.len == blen
                                && memcmp(k->as.s.data, bname, blen) == 0) {
                                mino_val_t *renamed = map_get_val(rename_map, k);
                                if (renamed != NULL && renamed->type == MINO_SYMBOL) {
                                    bind_name = renamed->as.s.data;
                                    bind_len  = renamed->as.s.len;
                                }
                                break;
                            }
                        }
                    }
                    if (bind_len < 256) {
                        char nbuf[256];
                        memcpy(nbuf, bind_name, bind_len);
                        nbuf[bind_len] = '\0';
                        env_bind(S, target, nbuf, src->bindings[ri].val);
                    }
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
    /* If the dotted form of name corresponds to a runtime-only ns
     * (a (ns foo) without a backing file) and the namespace already
     * has substantive bindings, treat it as loaded so (require 'foo)
     * doesn't go searching for a foo.clj that was never written to
     * disk. Bindings that came from C primitive installs (e.g. the
     * clojure.string namespace) don't count -- a wrapper file may
     * exist on disk and still need to load the first time.
     *
     * The check fires only when (a) the ns env has bindings AND
     * (b) the resolver can't find a file for this name. Path names
     * use underscores where ns names use dashes, so try both
     * dot-form and dash-form. */
    {
        char dotted[512];
        char dashed[512];
        size_t nl = strlen(name);
        size_t k;
        if (nl < sizeof(dotted)) {
            mino_env_t *e;
            int has_file = 0;
            if (S->module_resolver != NULL) {
                const char *resolved = S->module_resolver(name,
                                          S->module_resolver_ctx);
                if (resolved != NULL) has_file = 1;
            }
            /* A bundled lib is "loadable" too -- treat the same as
             * disk presence so we don't short-circuit to nil based on
             * already-installed C primitives in the ns env. */
            if (!has_file && bundled_lib_lookup(S, name) != NULL) {
                has_file = 1;
            }
            if (!has_file) {
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
    /* Resolve. The bundled-stdlib registry wins over the disk resolver
     * so brew/scoop installs without a lib/ directory still load
     * (require '[clojure.string]) cleanly. A name registered as a
     * bundled lib stays bundled regardless of resolver state, since
     * the embed-time install hook is the explicit opt-in. */
    bundled_source = bundled_lib_lookup(S, name);
    if (bundled_source != NULL) {
        path = NULL;
    } else {
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
        if (bundled_source != NULL) {
            /* Bundled load: eval the in-memory source directly. Set
             * reader_file to a synthetic <bundled name> path so any
             * diagnostics produced during eval point at something
             * meaningful instead of inheriting the caller's file. */
            const char *saved_file = S->reader_file;
            char        synth[256];
            snprintf(synth, sizeof(synth), "<bundled %s>", name);
            S->reader_file = intern_filename(S, synth);
            result         = mino_eval_string(S, bundled_source, env);
            S->reader_file = saved_file;
        } else {
            result = mino_load_file(S, path, env);
        }
        post_ns = S->current_ns;
        S->current_ns = saved_ns;
        /* Re-publish *ns* to track the restored ns. Inside the loaded
         * file, an (ns ...) form will have set the var to the file's
         * declared namespace; the require boundary is not a
         * user-visible switch, so the var follows current_ns back. */
        mino_publish_current_ns(S);
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
         * (e.g. ns foo-bar from file foo_bar.clj), so compare in a
         * canonicalized form where both use dashes.
         *
         * Skip the check when the require argument is a literal path --
         * one ending in a known source extension. (require '[foo.bar])
         * passes a dotted module name, so validation runs; a (require
         * "deps/foo/src/foo/bar.cljc") path-load is a deliberate "load
         * this file" that shouldn't impose a name match. */
        {
            size_t nl = strlen(name);
            int    is_path = 0;
            if ((nl >= 5 && (strcmp(name + nl - 5, ".cljc") == 0
                          || strcmp(name + nl - 5, ".cljs") == 0))
             || (nl >= 4 && strcmp(name + nl - 4, ".clj") == 0)) {
                is_path = 1;
            }
            if (!is_path && post_ns != NULL && saved_ns != NULL
                && strcmp(post_ns, saved_ns) != 0) {
                char        expected[256];
                char        post_canon[256];
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
                            "require: %s %s declared namespace %s, expected %s",
                            (path != NULL ? "file" : "bundled"),
                            (path != NULL ? path  : name),
                            post_ns, expected);
                        set_eval_diag(S, S->eval_current_form,
                            "name", "MNS001", msg);
                        return NULL;
                    }
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

/* Render `e->docstring` plus a trailing capability line when set. The
 * capability surfaces the install-group label the binding belongs to
 * (e.g. "fs", "proc"). User-visible via (doc name) and (apropos). */
static mino_val_t *doc_render_with_capability(mino_state_t *S,
                                              const meta_entry_t *e)
{
    if (e->capability == NULL || e->capability[0] == '\0') {
        if (e->docstring != NULL && e->docstring[0] != '\0') {
            return mino_string(S, e->docstring);
        }
        return mino_nil(S);
    }
    {
        size_t doclen = e->docstring != NULL ? strlen(e->docstring) : 0;
        size_t caplen = strlen(e->capability);
        const char *prefix = "\n  Capability: :";
        size_t plen = strlen(prefix);
        size_t total = doclen + plen + caplen;
        char  *buf = (char *)malloc(total + 1);
        mino_val_t *out;
        if (buf == NULL) {
            return e->docstring != NULL ? mino_string(S, e->docstring)
                                        : mino_nil(S);
        }
        if (doclen > 0) {
            memcpy(buf, e->docstring, doclen);
        }
        memcpy(buf + doclen, prefix, plen);
        memcpy(buf + doclen + plen, e->capability, caplen);
        buf[total] = '\0';
        out = mino_string_n(S, buf, total);
        free(buf);
        return out;
    }
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
    if (e != NULL && (e->docstring != NULL || e->capability != NULL)) {
        return doc_render_with_capability(S, e);
    }
    /* Qualified-name fallback: docstrings register under the bare name,
     * so ns/sym lookups should also try the part after the slash. */
    {
        const char *slash = (n > 1) ? memchr(buf, '/', n) : NULL;
        if (slash != NULL && slash[1] != '\0') {
            e = meta_find(S, slash + 1);
            if (e != NULL && (e->docstring != NULL || e->capability != NULL)) {
                return doc_render_with_capability(S, e);
            }
        }
    }
    /* Fall back to namespace metadata: (ns foo "docstring" ...) puts
     * {:doc "..."} on the namespace; surface it through doc when no
     * named-binding docstring is registered. */
    if (ns_env_lookup(S, buf) != NULL) {
        mino_val_t *meta = ns_env_get_meta(S, buf);
        if (meta != NULL && meta->type == MINO_MAP) {
            mino_val_t *doc_kw = mino_keyword(S, "doc");
            mino_val_t *doc    = map_get_val(meta, doc_kw);
            if (doc != NULL && doc->type == MINO_STRING) {
                return doc;
            }
        }
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
     * the current namespace chain so primitives interned in clojure.core
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

void mino_register_bundled_lib(mino_state_t *S, const char *name,
                                const char *source)
{
    size_t i;
    size_t nlen;
    char  *dup;
    if (S == NULL || name == NULL || source == NULL) return;
    /* Replace if already present. */
    for (i = 0; i < S->bundled_libs_len; i++) {
        if (strcmp(S->bundled_libs[i].name, name) == 0) {
            S->bundled_libs[i].source = source;
            return;
        }
    }
    if (S->bundled_libs_len == S->bundled_libs_cap) {
        size_t new_cap = S->bundled_libs_cap == 0 ? 8
                       : S->bundled_libs_cap * 2;
        bundled_lib_entry_t *nb = (bundled_lib_entry_t *)realloc(
            S->bundled_libs, new_cap * sizeof(*nb));
        if (nb == NULL) return; /* silent: best-effort, caller can re-try */
        S->bundled_libs     = nb;
        S->bundled_libs_cap = new_cap;
    }
    nlen = strlen(name);
    dup  = (char *)malloc(nlen + 1);
    if (dup == NULL) return;
    memcpy(dup, name, nlen + 1);
    S->bundled_libs[S->bundled_libs_len].name   = dup;
    S->bundled_libs[S->bundled_libs_len].source = source;
    S->bundled_libs_len++;
}

/* Compare two namespace names for equality, treating '.' and '/' as
 * the same separator. The symbol form of (require ...) converts the
 * user-facing dotted name to path-style with slashes before reaching
 * the string-form lookup, so bundled libs registered as
 * "clojure.string" still match a "clojure/string" lookup. */
static int ns_name_eq(const char *a, const char *b)
{
    while (*a != '\0' && *b != '\0') {
        char ca = *a, cb = *b;
        if (ca == '/') ca = '.';
        if (cb == '/') cb = '.';
        if (ca != cb) return 0;
        a++; b++;
    }
    return *a == '\0' && *b == '\0';
}

/* Returns the bundled source for `name`, or NULL if not registered. */
static const char *bundled_lib_lookup(mino_state_t *S, const char *name)
{
    size_t i;
    for (i = 0; i < S->bundled_libs_len; i++) {
        if (ns_name_eq(S->bundled_libs[i].name, name)) {
            return S->bundled_libs[i].source;
        }
    }
    return NULL;
}

/* (mino-capability sym) -- return the install-group capability label
 * for the named binding as a keyword (e.g. :fs), or nil when the
 * binding is part of the always-installed core. The label is set at
 * install time by the prim_install_table_with_capability path used by
 * mino_install_io / mino_install_fs / mino_install_proc /
 * mino_install_host / mino_install_async. */
mino_val_t *prim_mino_capability(mino_state_t *S, mino_val_t *args,
                                 mino_env_t *env)
{
    mino_val_t   *name_val;
    char          buf[256];
    size_t        n;
    meta_entry_t *e;
    (void)env;
    if (!mino_is_cons(args) || mino_is_cons(args->as.cons.cdr)) {
        return prim_throw_classified(S, "eval/arity", "MAR001",
            "mino-capability requires one argument");
    }
    name_val = args->as.cons.car;
    if (name_val == NULL || name_val->type != MINO_SYMBOL) {
        return prim_throw_classified(S, "eval/type", "MTY001",
            "mino-capability: argument must be a symbol");
    }
    n = name_val->as.s.len;
    if (n >= sizeof(buf)) {
        return prim_throw_classified(S, "eval/type", "MTY001",
            "mino-capability: name too long");
    }
    memcpy(buf, name_val->as.s.data, n);
    buf[n] = '\0';
    e = meta_find(S, buf);
    if (e == NULL || e->capability == NULL) {
        /* Qualified-name fallback. */
        const char *slash = (n > 1) ? memchr(buf, '/', n) : NULL;
        if (slash != NULL && slash[1] != '\0') {
            e = meta_find(S, slash + 1);
        }
    }
    if (e != NULL && e->capability != NULL && e->capability[0] != '\0') {
        return mino_keyword(S, e->capability);
    }
    return mino_nil(S);
}

const mino_prim_def k_prims_module[] = {
    {"require", prim_require,
     "Loads and evaluates a mino source file."},
    {"use",     prim_use,
     "Loads a module and refers all of its public names by default."},
    {"mino-capability", prim_mino_capability,
     "Return the install-group capability label for the named binding as a keyword, or nil when the binding is part of the always-installed core."},
};

const size_t k_prims_module_count =
    sizeof(k_prims_module) / sizeof(k_prims_module[0]);

/* clojure.repl primitives. Installed into the clojure.repl ns env, not
 * clojure.core, so a fresh user namespace gets these only after an
 * explicit (require '[clojure.repl :refer [doc apropos source]]).
 *
 * The C primitives expose the data accessors (`doc-string`, `source-form`,
 * `apropos`); the user-facing `doc` / `source` macros and the higher-level
 * `dir`, `find-doc`, `pst` live in lib/clojure/repl.clj on top of these. */
const mino_prim_def k_prims_clojure_repl[] = {
    {"doc-string",  prim_doc,
     "Returns the documentation string for the named var, or nil."},
    {"source-form", prim_source,
     "Returns the source form for the named var, or nil."},
    {"apropos",     prim_apropos,
     "Returns a list of vars whose names contain the given substring."},
};

const size_t k_prims_clojure_repl_count =
    sizeof(k_prims_clojure_repl) / sizeof(k_prims_clojure_repl[0]);
