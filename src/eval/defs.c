/*
 * defs.c -- def, defmacro, declare, ns special forms.
 */

#include "eval/special_internal.h"

/* --- ns special form helpers ---
 *
 * Module-name path conversion and alias table mutation live in
 * runtime/module.c so this file and prim/module.c share one
 * implementation. */

/* Merge two metadata maps. Returns NEW (or B) if A is nil/non-map.
 * Otherwise rebuilds the union with later writes winning. */
static mino_val_t *ns_meta_merge(mino_state_t *S,
                                 mino_val_t *a, mino_val_t *b)
{
    if (a == NULL || a->type != MINO_MAP) return b;
    if (b == NULL || b->type != MINO_MAP) return a;
    {
        size_t       len = a->as.map.len + b->as.map.len;
        mino_val_t **ks  = (mino_val_t **)gc_alloc_typed(
            S, GC_T_VALARR, len * sizeof(*ks));
        mino_val_t **vs  = (mino_val_t **)gc_alloc_typed(
            S, GC_T_VALARR, len * sizeof(*vs));
        size_t       i, n = 0;
        /* Start with a's entries. */
        for (i = 0; i < a->as.map.len; i++) {
            mino_val_t *k = vec_nth(a->as.map.key_order, i);
            gc_valarr_set(S, ks, n, k);
            gc_valarr_set(S, vs, n, map_get_val(a, k));
            n++;
        }
        /* Overlay b's entries; replace duplicates. */
        for (i = 0; i < b->as.map.len; i++) {
            mino_val_t *k = vec_nth(b->as.map.key_order, i);
            mino_val_t *v = map_get_val(b, k);
            size_t      j;
            int         replaced = 0;
            for (j = 0; j < n; j++) {
                if (mino_eq(ks[j], k)) {
                    gc_valarr_set(S, vs, j, v);
                    replaced = 1;
                    break;
                }
            }
            if (!replaced) {
                gc_valarr_set(S, ks, n, k);
                gc_valarr_set(S, vs, n, v);
                n++;
            }
        }
        return mino_map(S, ks, vs, n);
    }
}

/* True if `name` is bound in the current ns env via :refer from another
 * namespace (env binding exists but no var owned by current_ns). Used by
 * def/declare/defmacro to surface a "already refers to" collision before
 * silently shadowing. Names that already have a var entry for the current
 * namespace are normal redefs and pass through. */
static int refer_collision_check(mino_state_t *S, mino_val_t *form,
                                 const char *name)
{
    mino_env_t    *ns_env;
    env_binding_t *b;
    if (S->current_ns == NULL) return 0;
    /* clojure.core itself "owns" its primitives via env_bind at install
     * time without interning vars; skip the check there so core.clj can
     * def names whose primitive bindings live in the same env. */
    if (strcmp(S->current_ns, "clojure.core") == 0) return 0;
    ns_env = current_ns_env(S);
    if (ns_env == NULL) return 0;
    b = env_find_here(ns_env, name);
    if (b == NULL) return 0;
    if (var_find(S, S->current_ns, name) != NULL) return 0;
    /* Phase 2 interns every primitive into its install-time
     * namespace, so the var_find check above already exempts
     * legitimate shadowing within the home ns (e.g. clojure.string
     * wrappers over clojure.string primitives). A primitive binding
     * that reaches this point was refer'd in from another ns; treat
     * the new def as a real collision. */
    {
        char msg[300];
        snprintf(msg, sizeof(msg),
                 "%s already refers to a var from another namespace",
                 name);
        set_eval_diag(S, form, "name", "MNS001", msg);
    }
    return 1;
}

/* Process a single require spec from within an ns form.
 * spec is either a symbol (bare require) or a vector [mod.name :as alias ...].
 * Attempts to load the module via the resolver and stores aliases.
 * When use_mode is true, default to refer-all (:use semantics).
 * Returns 0 on success, -1 on failure (with a diagnostic set). */
/* True if vec contains a symbol with the given byte name. */
static int sym_vec_contains(mino_val_t *vec, const char *name, size_t namelen)
{
    size_t i;
    if (vec == NULL || vec->type != MINO_VECTOR) return 0;
    for (i = 0; i < vec->as.vec.len; i++) {
        mino_val_t *e = vec_nth(vec, i);
        if (e != NULL && e->type == MINO_SYMBOL
            && e->as.s.len == namelen
            && memcmp(e->as.s.data, name, namelen) == 0) {
            return 1;
        }
    }
    return 0;
}

/* Look up a rename target in a rename map: { old-sym new-sym }. */
static mino_val_t *rename_map_lookup(mino_val_t *m, const char *name,
                                     size_t namelen)
{
    if (m == NULL || m->type != MINO_MAP) return NULL;
    {
        size_t i;
        for (i = 0; i < m->as.map.len; i++) {
            mino_val_t *k = vec_nth(m->as.map.key_order, i);
            if (k != NULL && k->type == MINO_SYMBOL
                && k->as.s.len == namelen
                && memcmp(k->as.s.data, name, namelen) == 0) {
                return map_get_val(m, k);
            }
        }
    }
    return NULL;
}

static int ns_process_require_spec_ex(mino_state_t *S, mino_val_t *spec,
                                      mino_env_t *env, int use_mode)
{
    char pathbuf[256];
    const char *modname;
    size_t      modlen;
    const char *alias_name = NULL;
    size_t       alias_len  = 0;
    mino_val_t  *refer_vec   = NULL;
    mino_val_t  *exclude_vec = NULL;
    mino_val_t  *rename_map  = NULL;
    int          refer_all   = use_mode; /* :use defaults to refer-all */
    int          as_alias_only = 0;

    if (spec->type == MINO_SYMBOL) {
        modname = spec->as.s.data;
        modlen  = spec->as.s.len;
    } else if (spec->type == MINO_VECTOR && spec->as.vec.len >= 1) {
        mino_val_t *first = vec_nth(spec, 0);
        size_t i;
        if (first == NULL || first->type != MINO_SYMBOL) return 0;
        modname = first->as.s.data;
        modlen  = first->as.s.len;
        /* Parse keyword args: :as, :refer, :only, :exclude, :rename */
        for (i = 1; i + 1 < spec->as.vec.len; i += 2) {
            mino_val_t *k = vec_nth(spec, i);
            mino_val_t *v = vec_nth(spec, i + 1);
            if (kw_eq(k, "as") && v->type == MINO_SYMBOL) {
                alias_name = v->as.s.data;
                alias_len  = v->as.s.len;
            }
            if (kw_eq(k, "as-alias") && v->type == MINO_SYMBOL) {
                alias_name    = v->as.s.data;
                alias_len     = v->as.s.len;
                as_alias_only = 1;
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
            if (kw_eq(k, "exclude") && v->type == MINO_VECTOR) {
                exclude_vec = v;
                /* :exclude implies :refer :all unless :refer is set */
                if (refer_vec == NULL) refer_all = 1;
            }
            if (kw_eq(k, "rename") && v->type == MINO_MAP) {
                rename_map = v;
            }
        }
    } else {
        return 0; /* skip unrecognized spec form */
    }

    /* Convert dotted name to path and load. A missing or failing module
     * must surface as an error rather than silently succeeding, so the
     * diagnostic prim_require set is left in place when the call fails.
     * :as-alias skips the load entirely -- the alias is registered
     * regardless of whether the target namespace exists. */
    if (!as_alias_only
        && runtime_module_dotted_to_path(modname, modlen,
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
        if (alias_len >= sizeof(abuf) || modlen >= sizeof(fbuf)) {
            set_eval_diag(S, mino_current_ctx(S)->eval_current_form,
                          "syntax", "MSY001",
                          "ns: alias or module name too long");
            return -1;
        }
        memcpy(abuf, alias_name, alias_len);
        abuf[alias_len] = '\0';
        memcpy(fbuf, modname, modlen);
        fbuf[modlen] = '\0';
        if (runtime_module_add_alias(S, abuf, fbuf) != 0) {
            set_eval_diag(S, mino_current_ctx(S)->eval_current_form,
                          "internal", "MIN001",
                          "ns: out of memory adding alias");
            return -1;
        }
    }

    /* Process :refer — bind referred names into current ns env.
     * Iterate the source ns env so macros come through too. */
    {
        char modbuf[256];
        mino_env_t *target;
        mino_env_t *src;
        if (modlen >= sizeof(modbuf)) {
            set_eval_diag(S, mino_current_ctx(S)->eval_current_form,
                          "syntax", "MSY001",
                          "ns: module name too long");
            return -1;
        }
        target = current_ns_env(S);
        memcpy(modbuf, modname, modlen);
        modbuf[modlen] = '\0';
        src = ns_env_lookup(S, modbuf);
        if (refer_vec != NULL) {
            size_t ri;
            for (ri = 0; ri < refer_vec->as.vec.len; ri++) {
                mino_val_t *rsym = vec_nth(refer_vec, ri);
                if (rsym != NULL && rsym->type == MINO_SYMBOL) {
                    char rbuf[256];
                    size_t rn = rsym->as.s.len;
                    mino_val_t *val = NULL;
                    mino_val_t *renamed;
                    const char *bind_name;
                    size_t bind_len;
                    if (rn >= sizeof(rbuf)) {
                        set_eval_diag(S,
                            mino_current_ctx(S)->eval_current_form,
                            "syntax", "MSY001",
                            "ns: refer name too long");
                        return -1;
                    }
                    memcpy(rbuf, rsym->as.s.data, rn);
                    rbuf[rn] = '\0';
                    if (sym_vec_contains(exclude_vec, rbuf, rn)) {
                        continue;
                    }
                    if (src != NULL) {
                        env_binding_t *b = env_find_here(src, rbuf);
                        if (b != NULL) val = b->val;
                    }
                    if (val == NULL) {
                        mino_val_t *var = var_find(S, modbuf, rbuf);
                        if (var != NULL) val = var->as.var.root;
                    }
                    if (val == NULL) continue;
                    renamed = rename_map_lookup(rename_map, rbuf, rn);
                    if (renamed != NULL && renamed->type == MINO_SYMBOL) {
                        bind_name = renamed->as.s.data;
                        bind_len  = renamed->as.s.len;
                    } else {
                        bind_name = rbuf;
                        bind_len  = rn;
                    }
                    if (bind_len >= sizeof(rbuf)) {
                        set_eval_diag(S,
                            mino_current_ctx(S)->eval_current_form,
                            "syntax", "MSY001",
                            "ns: rename target too long");
                        return -1;
                    }
                    {
                        char nbuf[256];
                        memcpy(nbuf, bind_name, bind_len);
                        nbuf[bind_len] = '\0';
                        env_bind(S, target, nbuf, val);
                    }
                }
            }
        }
        if (refer_all && src != NULL) {
            size_t vi;
            for (vi = 0; vi < src->len; vi++) {
                const char *nm  = src->bindings[vi].name;
                size_t      nl  = strlen(nm);
                mino_val_t *renamed;
                /* Bring in only the source ns's own interned publics, the
                 * way canon (ns-publics 'src) does. Bindings without an
                 * owning var in this ns are transitive refers (e.g.,
                 * clojure.core names that were referred into src) and
                 * must not drag across — otherwise they shadow the
                 * consumer's own clojure.core refers. */
                mino_val_t *var = var_find(S, modbuf, nm);
                if (var == NULL) continue;
                if (var->type == MINO_VAR && var->as.var.is_private) continue;
                if (sym_vec_contains(exclude_vec, nm, nl)) continue;
                renamed = rename_map_lookup(rename_map, nm, nl);
                if (renamed != NULL && renamed->type == MINO_SYMBOL
                    && renamed->as.s.len < 256) {
                    char nbuf[256];
                    memcpy(nbuf, renamed->as.s.data, renamed->as.s.len);
                    nbuf[renamed->as.s.len] = '\0';
                    env_bind(S, target, nbuf, src->bindings[vi].val);
                } else {
                    env_bind(S, target, nm, src->bindings[vi].val);
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
        char buf[256];
        size_t n;
        if (name_form == NULL || name_form->type != MINO_SYMBOL) {
            set_eval_diag(S, form, "syntax", "MSY001",
                          "ns: first arg must be a symbol");
            return NULL;
        }
        n = name_form->as.s.len;
        if (n >= sizeof(buf)) {
            set_eval_diag(S, form, "syntax", "MSY001",
                          "ns: name too long");
            return NULL;
        }
        memcpy(buf, name_form->as.s.data, n);
        buf[n] = '\0';
        S->current_ns = intern_filename(S, buf);
        (void)ns_env_ensure(S, S->current_ns);
        /* Pull together metadata from ^meta on the name, an optional
         * docstring as the second arg, and an optional attr-map as the
         * third (or second when no docstring). */
        {
            mino_val_t *meta = name_form->meta;
            mino_val_t *cur  = args->as.cons.cdr;
            mino_val_t *next = (mino_is_cons(cur)) ? cur->as.cons.car : NULL;
            if (next != NULL && next->type == MINO_STRING) {
                /* Docstring -> {:doc "..."}. Merge with existing meta. */
                mino_val_t *kk = mino_keyword(S, "doc");
                mino_val_t **ks = (mino_val_t **)gc_alloc_typed(
                    S, GC_T_VALARR, sizeof(*ks));
                mino_val_t **vs = (mino_val_t **)gc_alloc_typed(
                    S, GC_T_VALARR, sizeof(*vs));
                gc_valarr_set(S, ks, 0, kk);
                gc_valarr_set(S, vs, 0, next);
                {
                    mino_val_t *doc_map = mino_map(S, ks, vs, 1);
                    meta = ns_meta_merge(S, meta, doc_map);
                }
                cur  = cur->as.cons.cdr;
                next = (mino_is_cons(cur)) ? cur->as.cons.car : NULL;
            }
            if (next != NULL && next->type == MINO_MAP) {
                meta = ns_meta_merge(S, meta, next);
                cur = cur->as.cons.cdr;
            }
            /* Each (ns ...) invocation replaces the namespace's metadata
             * outright; merging only happens between the ^meta, the
             * docstring, and the attribute map within a single call. */
            ns_env_set_meta(S, S->current_ns, meta);
            mino_publish_current_ns(S);
            args = mino_cons(S, name_form, cur);
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
            if (kw_eq(head, "refer-clojure")) {
                /* :refer-clojure :only/:exclude/:rename. Detach parent
                 * (which is clojure.core) and explicitly bring in the
                 * filtered subset, so excluded names are actually hidden
                 * rather than served by the parent chain. */
                mino_val_t *opts = clause->as.cons.cdr;
                mino_val_t *only_vec    = NULL;
                mino_val_t *exclude_vec = NULL;
                mino_val_t *rename_map  = NULL;
                while (mino_is_cons(opts)) {
                    mino_val_t *k = opts->as.cons.car;
                    mino_val_t *v;
                    if (!mino_is_cons(opts->as.cons.cdr)) break;
                    v = opts->as.cons.cdr->as.cons.car;
                    if (kw_eq(k, "only") && v != NULL
                        && v->type == MINO_VECTOR) {
                        only_vec = v;
                    } else if (kw_eq(k, "exclude") && v != NULL
                               && v->type == MINO_VECTOR) {
                        exclude_vec = v;
                    } else if (kw_eq(k, "rename") && v != NULL
                               && v->type == MINO_MAP) {
                        rename_map = v;
                    }
                    opts = opts->as.cons.cdr->as.cons.cdr;
                }
                {
                    mino_env_t *target = current_ns_env(S);
                    mino_env_t *core   = S->mino_core_env;
                    size_t      i;
                    if (target == NULL || core == NULL) {
                        set_eval_diag(S, form, "internal", "MIN001",
                            "ns: refer-clojure missing core env");
                        return NULL;
                    }
                    target->parent = NULL;
                    for (i = 0; i < core->len; i++) {
                        const char *nm = core->bindings[i].name;
                        size_t      nl = strlen(nm);
                        mino_val_t *renamed;
                        mino_val_t *src_var;
                        if (only_vec != NULL
                            && !sym_vec_contains(only_vec, nm, nl)) {
                            continue;
                        }
                        if (sym_vec_contains(exclude_vec, nm, nl)) {
                            continue;
                        }
                        /* Skip privates so refer-clojure mirrors real
                         * Clojure semantics — only public vars come
                         * along. Host-interop stubs like Long/Object
                         * are marked private so libraries that shadow
                         * them with their own defprotocol/def don't
                         * collide with the inherited binding. */
                        src_var = var_find(S, "clojure.core", nm);
                        if (src_var != NULL && src_var->as.var.is_private) {
                            continue;
                        }
                        renamed = rename_map_lookup(rename_map, nm, nl);
                        if (renamed != NULL && renamed->type == MINO_SYMBOL
                            && renamed->as.s.len < 256) {
                            char nbuf[256];
                            memcpy(nbuf, renamed->as.s.data,
                                   renamed->as.s.len);
                            nbuf[renamed->as.s.len] = '\0';
                            env_bind(S, target, nbuf, core->bindings[i].val);
                        } else {
                            env_bind(S, target, nm, core->bindings[i].val);
                        }
                    }
                }
            }
            /* Mino targets pure portable Clojure — :import,
             * :gen-class, :load, and other JVM-only ns clauses are
             * not supported. Throw with a clear message so a file
             * that needs Java interop fails loud at load time. */
            if (kw_eq(head, "import")) {
                set_eval_diag(S, form, "name", "MNS001",
                    "ns: :import is not supported on mino — there are no Java classes to import");
                return NULL;
            }
            if (kw_eq(head, "gen-class")) {
                set_eval_diag(S, form, "name", "MNS001",
                    "ns: :gen-class is not supported on mino — there is no JVM to compile against");
                return NULL;
            }
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
    mac->as.fn.params      = params;
    mac->as.fn.body        = body;
    mac->as.fn.env         = env;
    mac->as.fn.defining_ns = S->current_ns;
    n = name_form->as.s.len;
    if (n >= sizeof(buf)) {
        set_eval_diag(S, form, "syntax", "MSY001", "defmacro name too long");
        return NULL;
    }
    memcpy(buf, name_form->as.s.data, n);
    buf[n] = '\0';
    if (refer_collision_check(S, form, buf)) return NULL;
    {
        int is_priv = 0;
        mino_val_t *m = name_form->meta;
        if (m != NULL && m->type == MINO_MAP) {
            mino_val_t *pk = mino_keyword(S, "private");
            mino_val_t *pv = map_get_val(m, pk);
            if (pv != NULL && mino_is_truthy(pv)) is_priv = 1;
        }
        gc_pin(mac);
        {
            mino_val_t *var = var_intern(S, S->current_ns, buf);
            if (var != NULL) {
                var_set_root(S, var, mac);
                if (is_priv) var->as.var.is_private = 1;
            }
            env_bind(S, current_ns_env(S), buf, mac);
        }
        gc_unpin(1);
        meta_set(S, buf, doc, doc_len, form);
    }
    return mac;
}

mino_val_t *eval_declare(mino_state_t *S, mino_val_t *form,
                         mino_val_t *args, mino_env_t *env, int tail)
{
    mino_val_t *rest = args;
    (void)env;
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
        if (refer_collision_check(S, form, buf)) return NULL;
        /* Intern the var so a later def/declare doesn't look like a
         * cross-namespace refer collision. */
        var_intern(S, S->current_ns, buf);
        env_bind(S, current_ns_env(S), buf, mino_nil(S));
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
    if (refer_collision_check(S, form, buf)) return NULL;
    /* Check for ^:dynamic / ^:private metadata on the name symbol. */
    {
        int is_dynamic = 0;
        int is_priv    = 0;
        mino_val_t *m = name_form->meta;
        if (m != NULL && m->type == MINO_MAP) {
            mino_val_t *dk = mino_keyword(S, "dynamic");
            mino_val_t *dv = map_get_val(m, dk);
            mino_val_t *pk = mino_keyword(S, "private");
            mino_val_t *pv = map_get_val(m, pk);
            if (dv != NULL && mino_is_truthy(dv)) is_dynamic = 1;
            if (pv != NULL && mino_is_truthy(pv)) is_priv    = 1;
        }
        /* (def name) -- declaration only. Var stays unbound unless previously
         * defined; returns the var. */
        if (!mino_is_cons(args->as.cons.cdr)) {
            mino_val_t *var = var_intern(S, S->current_ns, buf);
            if (var != NULL) {
                if (is_dynamic) var->as.var.dynamic = 1;
                if (is_priv)    var->as.var.is_private = 1;
            }
            meta_set(S, buf, NULL, 0, form);
            return var != NULL ? var : mino_nil(S);
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
                if (is_priv)    var->as.var.is_private = 1;
            }
            env_bind(S, current_ns_env(S), buf, value);
            gc_unpin(1);
            meta_set(S, buf, doc, doc_len, form);
            /* Clojure semantics: def returns the var, not the value. */
            return var != NULL ? var : value;
        }
    }
}
