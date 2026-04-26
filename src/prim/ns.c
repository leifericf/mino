/*
 * ns.c -- namespace and var introspection primitives.
 *
 * Surface mirrors the Clojure namespace API at the level of detail
 * Babashka/SCI exposes: enough to drive the test suite and ordinary
 * library code without cloning the JVM-only pieces. Each primitive is
 * a thin reader or mutator over the per-ns env table (runtime/ns_env.c)
 * and the var registry (runtime/var.c).
 */

#include "prim/internal.h"

/* --- Argument helpers ----------------------------------------------------- */

static int ns_one_arg(mino_state_t *S, mino_val_t *args, const char *name,
                      mino_val_t **out)
{
    if (!mino_is_cons(args) || mino_is_cons(args->as.cons.cdr)) {
        char msg[96];
        snprintf(msg, sizeof(msg), "%s: expected one argument", name);
        prim_throw_classified(S, "eval/arity", "MAR001", msg);
        return 0;
    }
    *out = args->as.cons.car;
    return 1;
}

/* Coerce a symbol (preferred) or string into a NUL-terminated buffer.
 * Returns 1 on success and writes into buf, 0 on type mismatch. */
static int ns_to_name(mino_state_t *S, mino_val_t *v, char *buf, size_t cap,
                      const char *fn)
{
    size_t n;
    if (v == NULL
        || (v->type != MINO_SYMBOL && v->type != MINO_STRING)) {
        char msg[128];
        snprintf(msg, sizeof(msg),
                 "%s: expected a namespace symbol or string", fn);
        prim_throw_classified(S, "eval/type", "MTY001", msg);
        return 0;
    }
    n = v->as.s.len;
    if (n >= cap) {
        prim_throw_classified(S, "eval/type", "MTY001",
                              "namespace name too long");
        return 0;
    }
    memcpy(buf, v->as.s.data, n);
    buf[n] = '\0';
    return 1;
}

/* --- *ns* dynamic ---------------------------------------------------------
 *
 * Clojure exposes the running namespace through the dynamic var *ns*.
 * mino doesn't have a Namespace type; the current namespace's name as a
 * symbol is the closest analogue and matches what the SCI/Babashka tests
 * expect when comparing equality. */
mino_val_t *prim_star_ns(mino_state_t *S, mino_val_t *args, mino_env_t *env)
{
    (void)args;
    (void)env;
    return mino_symbol(S, S->current_ns != NULL ? S->current_ns : "user");
}

/* --- in-ns ---------------------------------------------------------------- */
mino_val_t *prim_in_ns(mino_state_t *S, mino_val_t *args, mino_env_t *env)
{
    mino_val_t *arg;
    char        buf[256];
    (void)env;
    if (!ns_one_arg(S, args, "in-ns", &arg)) return NULL;
    if (!ns_to_name(S, arg, buf, sizeof(buf), "in-ns")) return NULL;
    (void)ns_env_ensure(S, buf);
    S->current_ns = intern_filename(S, buf);
    return mino_symbol(S, buf);
}

/* --- find-ns / the-ns / create-ns / remove-ns ---------------------------- */
mino_val_t *prim_find_ns(mino_state_t *S, mino_val_t *args, mino_env_t *env)
{
    mino_val_t *arg;
    char        buf[256];
    (void)env;
    if (!ns_one_arg(S, args, "find-ns", &arg)) return NULL;
    if (!ns_to_name(S, arg, buf, sizeof(buf), "find-ns")) return NULL;
    if (ns_env_lookup(S, buf) == NULL) return mino_nil(S);
    return mino_symbol(S, buf);
}

mino_val_t *prim_the_ns(mino_state_t *S, mino_val_t *args, mino_env_t *env)
{
    mino_val_t *arg;
    char        buf[256];
    (void)env;
    if (!ns_one_arg(S, args, "the-ns", &arg)) return NULL;
    if (!ns_to_name(S, arg, buf, sizeof(buf), "the-ns")) return NULL;
    if (ns_env_lookup(S, buf) == NULL) {
        char msg[300];
        snprintf(msg, sizeof(msg), "no namespace: %s", buf);
        return prim_throw_classified(S, "name", "MNS001", msg);
    }
    return mino_symbol(S, buf);
}

mino_val_t *prim_create_ns(mino_state_t *S, mino_val_t *args, mino_env_t *env)
{
    mino_val_t *arg;
    char        buf[256];
    (void)env;
    if (!ns_one_arg(S, args, "create-ns", &arg)) return NULL;
    if (!ns_to_name(S, arg, buf, sizeof(buf), "create-ns")) return NULL;
    (void)ns_env_ensure(S, buf);
    return mino_symbol(S, buf);
}

mino_val_t *prim_remove_ns(mino_state_t *S, mino_val_t *args, mino_env_t *env)
{
    mino_val_t *arg;
    char        buf[256];
    size_t      i;
    (void)env;
    if (!ns_one_arg(S, args, "remove-ns", &arg)) return NULL;
    if (!ns_to_name(S, arg, buf, sizeof(buf), "remove-ns")) return NULL;
    for (i = 0; i < S->ns_env_len; i++) {
        if (strcmp(S->ns_env_table[i].name, buf) == 0) {
            size_t j;
            for (j = i + 1; j < S->ns_env_len; j++) {
                S->ns_env_table[j - 1] = S->ns_env_table[j];
            }
            S->ns_env_len--;
            return mino_nil(S);
        }
    }
    return mino_nil(S);
}

/* --- ns-name -------------------------------------------------------------- */
mino_val_t *prim_ns_name(mino_state_t *S, mino_val_t *args, mino_env_t *env)
{
    mino_val_t *arg;
    char        buf[256];
    (void)env;
    if (!ns_one_arg(S, args, "ns-name", &arg)) return NULL;
    if (!ns_to_name(S, arg, buf, sizeof(buf), "ns-name")) return NULL;
    return mino_symbol(S, buf);
}

/* --- ns-publics / ns-interns / ns-refers / ns-map -------------------------
 *
 * mino doesn't track public/private vs interned/refer'd separately yet
 * (privacy enforcement is a follow-up item), so the four primitives all
 * return the same shape today: a map from symbol to value. The
 * distinction lives in the metadata once privacy lands. */
static mino_val_t *ns_bindings_map(mino_state_t *S, mino_env_t *e)
{
    mino_val_t **ks;
    mino_val_t **vs;
    size_t       i;
    if (e == NULL) return mino_map(S, NULL, NULL, 0);
    if (e->len == 0) return mino_map(S, NULL, NULL, 0);
    ks = (mino_val_t **)gc_alloc_typed(S, GC_T_VALARR,
                                        e->len * sizeof(*ks));
    vs = (mino_val_t **)gc_alloc_typed(S, GC_T_VALARR,
                                        e->len * sizeof(*vs));
    for (i = 0; i < e->len; i++) {
        gc_valarr_set(S, ks, i, mino_symbol(S, e->bindings[i].name));
        gc_valarr_set(S, vs, i, e->bindings[i].val);
    }
    return mino_map(S, ks, vs, e->len);
}

mino_val_t *prim_ns_publics(mino_state_t *S, mino_val_t *args, mino_env_t *env)
{
    mino_val_t *arg;
    char        buf[256];
    (void)env;
    if (!ns_one_arg(S, args, "ns-publics", &arg)) return NULL;
    if (!ns_to_name(S, arg, buf, sizeof(buf), "ns-publics")) return NULL;
    return ns_bindings_map(S, ns_env_lookup(S, buf));
}

mino_val_t *prim_ns_interns(mino_state_t *S, mino_val_t *args, mino_env_t *env)
{
    mino_val_t *arg;
    char        buf[256];
    (void)env;
    if (!ns_one_arg(S, args, "ns-interns", &arg)) return NULL;
    if (!ns_to_name(S, arg, buf, sizeof(buf), "ns-interns")) return NULL;
    return ns_bindings_map(S, ns_env_lookup(S, buf));
}

mino_val_t *prim_ns_refers(mino_state_t *S, mino_val_t *args, mino_env_t *env)
{
    mino_val_t *arg;
    char        buf[256];
    (void)env;
    if (!ns_one_arg(S, args, "ns-refers", &arg)) return NULL;
    if (!ns_to_name(S, arg, buf, sizeof(buf), "ns-refers")) return NULL;
    return ns_bindings_map(S, ns_env_lookup(S, buf));
}

mino_val_t *prim_ns_map(mino_state_t *S, mino_val_t *args, mino_env_t *env)
{
    mino_val_t *arg;
    char        buf[256];
    (void)env;
    if (!ns_one_arg(S, args, "ns-map", &arg)) return NULL;
    if (!ns_to_name(S, arg, buf, sizeof(buf), "ns-map")) return NULL;
    return ns_bindings_map(S, ns_env_lookup(S, buf));
}

/* --- ns-aliases ---------------------------------------------------------- */
mino_val_t *prim_ns_aliases(mino_state_t *S, mino_val_t *args, mino_env_t *env)
{
    mino_val_t *arg;
    char        buf[256];
    mino_val_t **ks;
    mino_val_t **vs;
    size_t       n;
    size_t       i;
    (void)env;
    if (!ns_one_arg(S, args, "ns-aliases", &arg)) return NULL;
    if (!ns_to_name(S, arg, buf, sizeof(buf), "ns-aliases")) return NULL;
    /* Aliases are tracked at state level, not per-ns, so every ns sees
     * the same alias set. Refining this is a follow-up. */
    n = S->ns_alias_len;
    if (n == 0) return mino_map(S, NULL, NULL, 0);
    ks = (mino_val_t **)gc_alloc_typed(S, GC_T_VALARR, n * sizeof(*ks));
    vs = (mino_val_t **)gc_alloc_typed(S, GC_T_VALARR, n * sizeof(*vs));
    for (i = 0; i < n; i++) {
        gc_valarr_set(S, ks, i, mino_symbol(S, S->ns_aliases[i].alias));
        gc_valarr_set(S, vs, i, mino_symbol(S, S->ns_aliases[i].full_name));
    }
    return mino_map(S, ks, vs, n);
}

/* --- refer ----------------------------------------------------------------
 *
 * (refer 'ns) bring all of ns's publics into the current namespace.
 * Mirrors Clojure's clojure.core/refer for the bare and :refer :all
 * forms. Future passes can grow it to honor :only, :exclude, :rename. */
mino_val_t *prim_refer(mino_state_t *S, mino_val_t *args, mino_env_t *env)
{
    mino_val_t *ns_arg;
    char        ns_buf[256];
    mino_env_t *src;
    mino_env_t *dst;
    size_t      i;
    (void)env;
    if (!mino_is_cons(args)) {
        return prim_throw_classified(S, "eval/arity", "MAR001",
            "refer requires a namespace symbol");
    }
    ns_arg = args->as.cons.car;
    if (!ns_to_name(S, ns_arg, ns_buf, sizeof(ns_buf), "refer")) return NULL;
    src = ns_env_lookup(S, ns_buf);
    if (src == NULL) {
        char msg[300];
        snprintf(msg, sizeof(msg), "refer: no namespace: %s", ns_buf);
        return prim_throw_classified(S, "name", "MNS001", msg);
    }
    dst = current_ns_env(S);
    if (dst == NULL) return mino_nil(S);
    for (i = 0; i < src->len; i++) {
        env_bind(S, dst, src->bindings[i].name, src->bindings[i].val);
    }
    return mino_nil(S);
}

/* --- alias / ns-unalias --------------------------------------------------- */
mino_val_t *prim_alias(mino_state_t *S, mino_val_t *args, mino_env_t *env)
{
    mino_val_t *a;
    mino_val_t *t;
    char        abuf[256];
    char        tbuf[256];
    (void)env;
    if (!mino_is_cons(args) || !mino_is_cons(args->as.cons.cdr)
        || mino_is_cons(args->as.cons.cdr->as.cons.cdr)) {
        return prim_throw_classified(S, "eval/arity", "MAR001",
            "alias: expected (alias alias-sym ns-sym)");
    }
    a = args->as.cons.car;
    t = args->as.cons.cdr->as.cons.car;
    if (!ns_to_name(S, a, abuf, sizeof(abuf), "alias")) return NULL;
    if (!ns_to_name(S, t, tbuf, sizeof(tbuf), "alias")) return NULL;
    runtime_module_add_alias(S, abuf, tbuf);
    return mino_nil(S);
}

mino_val_t *prim_ns_unalias(mino_state_t *S, mino_val_t *args,
                             mino_env_t *env)
{
    mino_val_t *ns_arg;
    mino_val_t *alias_arg;
    char        ns_buf[256];
    char        a_buf[256];
    size_t      i;
    (void)env;
    if (!mino_is_cons(args) || !mino_is_cons(args->as.cons.cdr)
        || mino_is_cons(args->as.cons.cdr->as.cons.cdr)) {
        return prim_throw_classified(S, "eval/arity", "MAR001",
            "ns-unalias: expected (ns-unalias ns-sym alias-sym)");
    }
    ns_arg    = args->as.cons.car;
    alias_arg = args->as.cons.cdr->as.cons.car;
    if (!ns_to_name(S, ns_arg, ns_buf, sizeof(ns_buf), "ns-unalias"))
        return NULL;
    if (!ns_to_name(S, alias_arg, a_buf, sizeof(a_buf), "ns-unalias"))
        return NULL;
    for (i = 0; i < S->ns_alias_len; i++) {
        if (strcmp(S->ns_aliases[i].alias, a_buf) == 0) {
            size_t j;
            free(S->ns_aliases[i].alias);
            free(S->ns_aliases[i].full_name);
            for (j = i + 1; j < S->ns_alias_len; j++) {
                S->ns_aliases[j - 1] = S->ns_aliases[j];
            }
            S->ns_alias_len--;
            break;
        }
    }
    return mino_nil(S);
}

/* --- ns-unmap ------------------------------------------------------------ */
mino_val_t *prim_ns_unmap(mino_state_t *S, mino_val_t *args, mino_env_t *env)
{
    mino_val_t *ns_arg;
    mino_val_t *sym_arg;
    char        ns_buf[256];
    char        s_buf[256];
    mino_env_t *e;
    size_t      i;
    (void)env;
    if (!mino_is_cons(args) || !mino_is_cons(args->as.cons.cdr)
        || mino_is_cons(args->as.cons.cdr->as.cons.cdr)) {
        return prim_throw_classified(S, "eval/arity", "MAR001",
            "ns-unmap: expected (ns-unmap ns-sym sym)");
    }
    ns_arg  = args->as.cons.car;
    sym_arg = args->as.cons.cdr->as.cons.car;
    if (!ns_to_name(S, ns_arg, ns_buf, sizeof(ns_buf), "ns-unmap"))
        return NULL;
    if (!ns_to_name(S, sym_arg, s_buf, sizeof(s_buf), "ns-unmap"))
        return NULL;
    e = ns_env_lookup(S, ns_buf);
    if (e == NULL) return mino_nil(S);
    /* Find the binding and rebind to nil. We don't physically delete
     * to keep the bindings array stable; ns-publics callers can filter
     * on val != nil if they need true "absence" semantics. */
    for (i = 0; i < e->len; i++) {
        if (strcmp(e->bindings[i].name, s_buf) == 0) {
            /* Compact: shift remaining entries down. */
            size_t j;
            for (j = i + 1; j < e->len; j++) {
                e->bindings[j - 1] = e->bindings[j];
            }
            e->len--;
            break;
        }
    }
    return mino_nil(S);
}

/* --- all-ns / loaded-libs ------------------------------------------------ */
mino_val_t *prim_all_ns(mino_state_t *S, mino_val_t *args, mino_env_t *env)
{
    mino_val_t **tmp;
    size_t       i;
    (void)args;
    (void)env;
    if (S->ns_env_len == 0) return mino_vector(S, NULL, 0);
    tmp = (mino_val_t **)gc_alloc_typed(S, GC_T_VALARR,
                                          S->ns_env_len * sizeof(*tmp));
    for (i = 0; i < S->ns_env_len; i++) {
        gc_valarr_set(S, tmp, i,
                      mino_symbol(S, S->ns_env_table[i].name));
    }
    return mino_vector(S, tmp, S->ns_env_len);
}

mino_val_t *prim_loaded_libs(mino_state_t *S, mino_val_t *args,
                              mino_env_t *env)
{
    mino_val_t **tmp;
    size_t       i;
    (void)args;
    (void)env;
    if (S->module_cache_len == 0) return mino_vector(S, NULL, 0);
    tmp = (mino_val_t **)gc_alloc_typed(S, GC_T_VALARR,
                                          S->module_cache_len * sizeof(*tmp));
    for (i = 0; i < S->module_cache_len; i++) {
        gc_valarr_set(S, tmp, i,
                      mino_symbol(S, S->module_cache[i].name));
    }
    return mino_vector(S, tmp, S->module_cache_len);
}

/* --- find-var / ns-resolve / requiring-resolve --------------------------- */
static mino_val_t *resolve_in_ns(mino_state_t *S, const char *ns_name,
                                  const char *sym_name)
{
    mino_val_t *var;
    mino_env_t *e;
    var = var_find(S, ns_name, sym_name);
    if (var != NULL) return var;
    e = ns_env_lookup(S, ns_name);
    if (e != NULL) {
        env_binding_t *b = env_find_here(e, sym_name);
        if (b != NULL) {
            /* Promote env binding into a var so callers can deref it. */
            mino_val_t *promoted = var_intern(S, ns_name, sym_name);
            if (promoted != NULL) {
                var_set_root(S, promoted, b->val);
                return promoted;
            }
        }
    }
    return NULL;
}

mino_val_t *prim_find_var(mino_state_t *S, mino_val_t *args, mino_env_t *env)
{
    mino_val_t *arg;
    const char *data;
    size_t      n;
    const char *slash;
    char        ns_buf[256];
    char        sym_buf[256];
    mino_val_t *var;
    (void)env;
    if (!ns_one_arg(S, args, "find-var", &arg)) return NULL;
    if (arg == NULL || arg->type != MINO_SYMBOL) {
        return prim_throw_classified(S, "eval/type", "MTY001",
            "find-var: expected a qualified symbol");
    }
    data  = arg->as.s.data;
    n     = arg->as.s.len;
    slash = (n > 1) ? memchr(data, '/', n) : NULL;
    if (slash == NULL) {
        return prim_throw_classified(S, "eval/type", "MTY001",
            "find-var: symbol must be qualified (ns/name)");
    }
    {
        size_t ns_len  = (size_t)(slash - data);
        size_t sym_len = n - ns_len - 1;
        if (ns_len >= sizeof(ns_buf) || sym_len >= sizeof(sym_buf)) {
            return prim_throw_classified(S, "eval/type", "MTY001",
                "find-var: symbol too long");
        }
        memcpy(ns_buf, data, ns_len);
        ns_buf[ns_len] = '\0';
        memcpy(sym_buf, slash + 1, sym_len);
        sym_buf[sym_len] = '\0';
    }
    var = resolve_in_ns(S, ns_buf, sym_buf);
    return var != NULL ? var : mino_nil(S);
}

mino_val_t *prim_ns_resolve(mino_state_t *S, mino_val_t *args,
                             mino_env_t *env)
{
    mino_val_t *ns_arg;
    mino_val_t *sym_arg;
    char        ns_buf[256];
    char        sym_buf[256];
    mino_val_t *var;
    (void)env;
    if (!mino_is_cons(args) || !mino_is_cons(args->as.cons.cdr)
        || mino_is_cons(args->as.cons.cdr->as.cons.cdr)) {
        return prim_throw_classified(S, "eval/arity", "MAR001",
            "ns-resolve: expected (ns-resolve ns sym)");
    }
    ns_arg  = args->as.cons.car;
    sym_arg = args->as.cons.cdr->as.cons.car;
    if (!ns_to_name(S, ns_arg, ns_buf, sizeof(ns_buf), "ns-resolve"))
        return NULL;
    if (sym_arg == NULL || sym_arg->type != MINO_SYMBOL
        || sym_arg->as.s.len >= sizeof(sym_buf)) {
        return prim_throw_classified(S, "eval/type", "MTY001",
            "ns-resolve: second arg must be a symbol");
    }
    /* Qualified arg sym/foo wins over the ns context. */
    {
        const char *slash = (sym_arg->as.s.len > 1)
            ? memchr(sym_arg->as.s.data, '/', sym_arg->as.s.len) : NULL;
        if (slash != NULL) {
            size_t qlen = (size_t)(slash - sym_arg->as.s.data);
            size_t plen = sym_arg->as.s.len - qlen - 1;
            if (qlen >= sizeof(ns_buf) || plen >= sizeof(sym_buf)) {
                return prim_throw_classified(S, "eval/type", "MTY001",
                    "ns-resolve: symbol too long");
            }
            memcpy(ns_buf, sym_arg->as.s.data, qlen);
            ns_buf[qlen] = '\0';
            memcpy(sym_buf, slash + 1, plen);
            sym_buf[plen] = '\0';
        } else {
            memcpy(sym_buf, sym_arg->as.s.data, sym_arg->as.s.len);
            sym_buf[sym_arg->as.s.len] = '\0';
        }
    }
    var = resolve_in_ns(S, ns_buf, sym_buf);
    return var != NULL ? var : mino_nil(S);
}

/* requiring-resolve = (require ns) then (resolve sym).  Argument is a
 * single qualified symbol like 'foo.bar/baz; if foo.bar isn't loaded we
 * load it lazily and then resolve. */
mino_val_t *prim_requiring_resolve(mino_state_t *S, mino_val_t *args,
                                    mino_env_t *env)
{
    mino_val_t *arg;
    const char *data;
    size_t      n;
    const char *slash;
    char        ns_buf[256];
    char        sym_buf[256];
    if (!ns_one_arg(S, args, "requiring-resolve", &arg)) return NULL;
    if (arg == NULL || arg->type != MINO_SYMBOL) {
        return prim_throw_classified(S, "eval/type", "MTY001",
            "requiring-resolve: expected a qualified symbol");
    }
    data  = arg->as.s.data;
    n     = arg->as.s.len;
    slash = (n > 1) ? memchr(data, '/', n) : NULL;
    if (slash == NULL) {
        return prim_throw_classified(S, "eval/type", "MTY001",
            "requiring-resolve: symbol must be qualified");
    }
    {
        size_t ns_len  = (size_t)(slash - data);
        size_t sym_len = n - ns_len - 1;
        if (ns_len >= sizeof(ns_buf) || sym_len >= sizeof(sym_buf)) {
            return prim_throw_classified(S, "eval/type", "MTY001",
                "requiring-resolve: symbol too long");
        }
        memcpy(ns_buf, data, ns_len);
        ns_buf[ns_len] = '\0';
        memcpy(sym_buf, slash + 1, sym_len);
        sym_buf[sym_len] = '\0';
    }
    if (ns_env_lookup(S, ns_buf) == NULL) {
        mino_val_t *req_args = mino_cons(S, mino_symbol(S, ns_buf),
                                         mino_nil(S));
        gc_pin(req_args);
        if (prim_require(S, req_args, env) == NULL) {
            gc_unpin(1);
            return NULL;
        }
        gc_unpin(1);
    }
    {
        mino_val_t *var = resolve_in_ns(S, ns_buf, sym_buf);
        return var != NULL ? var : mino_nil(S);
    }
}

/* --- intern / var-get / var-set / var? / bound? -------------------------- */
mino_val_t *prim_intern(mino_state_t *S, mino_val_t *args, mino_env_t *env)
{
    size_t      argc;
    mino_val_t *ns_arg;
    mino_val_t *sym_arg;
    mino_val_t *val_arg = NULL;
    char        ns_buf[256];
    char        s_buf[256];
    mino_val_t *var;
    mino_env_t *target;
    (void)env;
    arg_count(S, args, &argc);
    if (argc != 2 && argc != 3) {
        return prim_throw_classified(S, "eval/arity", "MAR001",
            "intern: expected (intern ns sym) or (intern ns sym val)");
    }
    ns_arg  = args->as.cons.car;
    sym_arg = args->as.cons.cdr->as.cons.car;
    if (argc == 3) val_arg = args->as.cons.cdr->as.cons.cdr->as.cons.car;
    if (!ns_to_name(S, ns_arg, ns_buf, sizeof(ns_buf), "intern")) return NULL;
    if (sym_arg == NULL || sym_arg->type != MINO_SYMBOL
        || sym_arg->as.s.len >= sizeof(s_buf)) {
        return prim_throw_classified(S, "eval/type", "MTY001",
            "intern: second arg must be a symbol");
    }
    memcpy(s_buf, sym_arg->as.s.data, sym_arg->as.s.len);
    s_buf[sym_arg->as.s.len] = '\0';
    var = var_intern(S, ns_buf, s_buf);
    if (var == NULL) return NULL;
    if (val_arg != NULL) {
        var_set_root(S, var, val_arg);
    }
    /* Make the intern visible to unqualified resolution by also
     * binding into the target ns env. */
    target = ns_env_ensure(S, ns_buf);
    if (target != NULL) {
        env_bind(S, target, s_buf,
                 val_arg != NULL ? val_arg : var->as.var.root);
    }
    return var;
}

mino_val_t *prim_var_get(mino_state_t *S, mino_val_t *args, mino_env_t *env)
{
    mino_val_t *arg;
    (void)env;
    if (!ns_one_arg(S, args, "var-get", &arg)) return NULL;
    if (arg == NULL || arg->type != MINO_VAR) {
        return prim_throw_classified(S, "eval/type", "MTY001",
            "var-get: expected a var");
    }
    return arg->as.var.root != NULL ? arg->as.var.root : mino_nil(S);
}

mino_val_t *prim_var_set(mino_state_t *S, mino_val_t *args, mino_env_t *env)
{
    mino_val_t *var_arg;
    mino_val_t *val_arg;
    (void)env;
    if (!mino_is_cons(args) || !mino_is_cons(args->as.cons.cdr)
        || mino_is_cons(args->as.cons.cdr->as.cons.cdr)) {
        return prim_throw_classified(S, "eval/arity", "MAR001",
            "var-set: expected (var-set var val)");
    }
    var_arg = args->as.cons.car;
    val_arg = args->as.cons.cdr->as.cons.car;
    if (var_arg == NULL || var_arg->type != MINO_VAR) {
        return prim_throw_classified(S, "eval/type", "MTY001",
            "var-set: expected a var");
    }
    var_set_root(S, var_arg, val_arg);
    return val_arg;
}

mino_val_t *prim_alter_var_root(mino_state_t *S, mino_val_t *args,
                                 mino_env_t *env)
{
    mino_val_t *var_arg;
    mino_val_t *fn_arg;
    mino_val_t *rest_args;
    mino_val_t *call_args;
    mino_val_t *new_val;
    if (!mino_is_cons(args) || !mino_is_cons(args->as.cons.cdr)) {
        return prim_throw_classified(S, "eval/arity", "MAR001",
            "alter-var-root: expected (alter-var-root var f & args)");
    }
    var_arg   = args->as.cons.car;
    fn_arg    = args->as.cons.cdr->as.cons.car;
    rest_args = args->as.cons.cdr->as.cons.cdr;
    if (var_arg == NULL || var_arg->type != MINO_VAR) {
        return prim_throw_classified(S, "eval/type", "MTY001",
            "alter-var-root: first arg must be a var");
    }
    /* (apply f current-root rest-args) */
    call_args = mino_cons(S, var_arg->as.var.root != NULL
                              ? var_arg->as.var.root : mino_nil(S),
                          rest_args);
    gc_pin(call_args);
    new_val = apply_callable(S, fn_arg, call_args, env);
    gc_unpin(1);
    if (new_val == NULL) return NULL;
    var_set_root(S, var_arg, new_val);
    return new_val;
}

/* --- Registration --------------------------------------------------------- */
const mino_prim_def k_prims_ns[] = {
    {"in-ns",          prim_in_ns,
     "Set the current namespace, creating it if necessary."},
    {"find-ns",        prim_find_ns,
     "Return the namespace symbol if it exists, else nil."},
    {"the-ns",         prim_the_ns,
     "Return the namespace symbol or throw if not found."},
    {"create-ns",      prim_create_ns,
     "Ensure the namespace exists and return its symbol."},
    {"remove-ns",      prim_remove_ns,
     "Remove a namespace from the runtime."},
    {"ns-name",        prim_ns_name,
     "Return the symbol name of a namespace."},
    {"ns-publics",     prim_ns_publics,
     "Return the public bindings of a namespace as a map."},
    {"ns-interns",     prim_ns_interns,
     "Return the interned bindings of a namespace as a map."},
    {"ns-refers",      prim_ns_refers,
     "Return the refer'd bindings of a namespace as a map."},
    {"ns-map",         prim_ns_map,
     "Return all bindings visible in a namespace as a map."},
    {"ns-aliases",     prim_ns_aliases,
     "Return the alias map of a namespace."},
    {"alias",          prim_alias,
     "Add an alias to a namespace."},
    {"ns-unalias",     prim_ns_unalias,
     "Remove an alias from a namespace."},
    {"ns-unmap",       prim_ns_unmap,
     "Remove a binding from a namespace."},
    {"refer",          prim_refer,
     "Bring all publics of a namespace into the current namespace."},
    {"all-ns",         prim_all_ns,
     "Return a vector of all known namespace symbols."},
    {"loaded-libs",    prim_loaded_libs,
     "Return a vector of names that have been required."},
    {"find-var",       prim_find_var,
     "Return the var named by a qualified symbol, or nil."},
    {"ns-resolve",     prim_ns_resolve,
     "Resolve a symbol to a var in the given namespace."},
    {"requiring-resolve", prim_requiring_resolve,
     "Require the namespace if needed, then resolve a qualified symbol."},
    {"intern",         prim_intern,
     "Intern a value into a namespace by name."},
    {"var-get",        prim_var_get,
     "Return the root value of a var."},
    {"var-set",        prim_var_set,
     "Set the root value of a var."},
    {"alter-var-root", prim_alter_var_root,
     "Apply a function to a var's root and store the result."},
};

const size_t k_prims_ns_count =
    sizeof(k_prims_ns) / sizeof(k_prims_ns[0]);
