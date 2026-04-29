/*
 * special.c -- eval dispatch (eval_impl), literal evaluation, and the
 *              top-level eval entry point.
 *
 * Special-form implementations live in domain-specific files:
 *   defs.c      -- def, defmacro, declare
 *   bindings.c  -- let, loop, binding, destructuring
 *   control.c   -- try/catch/finally
 *   fn.c        -- fn, apply_callable, arity dispatch
 */

#include "eval/special_internal.h"

/* --- Evaluator helpers: one per value kind. --- */

/* Look up an alias in the current namespace's alias table; returns the
 * full module name or NULL if not found. Aliases are scoped per-ns:
 * the same alias name can resolve to different targets in different
 * namespaces. */
static const char *alias_resolve(mino_state_t *S, const char *alias)
{
    size_t i;
    const char *cur = S->current_ns != NULL ? S->current_ns : "user";
    for (i = 0; i < S->ns_alias_len; i++) {
        if (S->ns_aliases[i].owning_ns != NULL
            && strcmp(S->ns_aliases[i].owning_ns, cur) == 0
            && strcmp(S->ns_aliases[i].alias, alias) == 0) {
            return S->ns_aliases[i].full_name;
        }
    }
    return NULL;
}

/* Resolve an ns/name qualified symbol. The caller has located the '/'
 * separator; this helper handles the literal-binding fast path, alias
 * resolution, var lookup with private-access check, and the ns-env
 * fallback for primitives. On a miss it sets a diagnostic that names
 * the most likely cause (missing var, missing ns, missing alias). */
static mino_val_t *eval_qualified_symbol(mino_state_t *S, mino_env_t *env,
                                         const char *data, size_t n,
                                         const char *slash)
{
    char        ns_buf[256];
    const char *sym_name    = slash + 1;
    size_t      ns_len      = (size_t)(slash - data);
    const char *resolved_ns;
    mino_env_t *target_env;
    mino_val_t *var;
    mino_val_t *v;
    char        msg[300];
    int         is_alias;
    (void)n;

    /* Try full name as a literal env binding first (e.g. "host/new"). */
    v = mino_env_get(env, data);
    if (v != NULL) return v;

    if (ns_len >= sizeof(ns_buf)) {
        set_eval_diag(S, mino_current_ctx(S)->eval_current_form,
            "syntax", "MSY001", "symbol name too long");
        return NULL;
    }
    memcpy(ns_buf, data, ns_len);
    ns_buf[ns_len] = '\0';

    /* Resolve alias to full module name. */
    resolved_ns = alias_resolve(S, ns_buf);
    if (resolved_ns == NULL) resolved_ns = ns_buf;

    /* Look up in var registry by resolved namespace + name. */
    var = var_find(S, resolved_ns, sym_name);
    if (var != NULL) {
        /* Cross-ns access of a private var is rejected. Same-ns access
         * is fine since callers within a namespace are not outsiders. */
        if (var->as.var.is_private
            && (S->current_ns == NULL
                || strcmp(S->current_ns, resolved_ns) != 0)) {
            snprintf(msg, sizeof(msg),
                "var %s/%s is private", resolved_ns, sym_name);
            set_eval_diag(S, mino_current_ctx(S)->eval_current_form,
                "name", "MNS001", msg);
            return NULL;
        }
        return var->as.var.root;
    }

    /* Primitives live in the ns env but aren't interned as vars, so a
     * var_find miss falls back to the ns env's own bindings. */
    target_env = ns_env_lookup(S, resolved_ns);
    if (target_env != NULL) {
        env_binding_t *b = env_find_here(target_env, sym_name);
        if (b != NULL) return b->val;
    }

    is_alias = (resolved_ns != ns_buf);
    if (target_env != NULL) {
        snprintf(msg, sizeof(msg),
            "no var %s in namespace %s", sym_name, resolved_ns);
    } else if (is_alias) {
        /* alias_resolve gave us a name but no ns env exists: the alias
         * points at an unloaded namespace. */
        snprintf(msg, sizeof(msg),
            "no such namespace: %s", resolved_ns);
    } else {
        /* Neither an alias nor a loaded namespace -- most likely the
         * user meant an alias that isn't set up. */
        const char *cur =
            (S->current_ns != NULL) ? S->current_ns : "user";
        snprintf(msg, sizeof(msg),
            "no such alias: %s in namespace %s", ns_buf, cur);
    }
    set_eval_diag(S, mino_current_ctx(S)->eval_current_form,
        "name", "MNS001", msg);
    return NULL;
}

static mino_val_t *eval_symbol(mino_state_t *S, mino_val_t *form, mino_env_t *env)
{
    size_t n = form->as.s.len;
    const char *data = form->as.s.data;  /* null-terminated (dup_n adds \0) */
    mino_val_t *v;
    const char *slash;

    /* Check for namespace-qualified symbol (e.g. t/is, clojure.core/+).
     * Single-char "/" is the division function, not a qualified symbol. */
    slash = (n > 1) ? memchr(data, '/', n) : NULL;
    if (slash != NULL) {
        return eval_qualified_symbol(S, env, data, n, slash);
    }

    /* *ns* derefs to the current namespace symbol. Once mino_install_core
     * runs, *ns* is interned as a dynamic var in clojure.core (see
     * runtime/ns_env.c:mino_publish_current_ns); this fast path stays
     * for embedders that read *ns* before installation completes and
     * for reading the running ns without going through the var registry.
     * The symbol carries the namespace's metadata so (meta *ns*) works. */
    if (n == 4 && memcmp(data, "*ns*", 4) == 0) {
        if (mino_env_get(env, data) == NULL) {
            const char *cur = S->current_ns != NULL ? S->current_ns : "user";
            mino_val_t *sym = mino_symbol(S, cur);
            mino_val_t *meta = ns_env_get_meta(S, cur);
            if (meta != NULL && sym != NULL) {
                mino_val_t *copy = alloc_val(S, sym->type);
                copy->as   = sym->as;
                copy->meta = meta;
                return copy;
            }
            return sym;
        }
    }

    /* Unqualified: dynamic → lexical → current-ns env → fn ambient ns. */
    v = (mino_current_ctx(S)->dyn_stack != NULL) ? dyn_lookup(S, data) : NULL;
    if (v == NULL) v = mino_env_get(env, data);
    if (v == NULL) {
        mino_env_t *ns_env = current_ns_env(S);
        if (ns_env != NULL) v = mino_env_get(ns_env, data);
    }
    if (v == NULL && S->fn_ambient_ns != NULL
        && S->fn_ambient_ns != S->current_ns
        && (S->current_ns == NULL
            || strcmp(S->fn_ambient_ns, S->current_ns) != 0)) {
        mino_env_t *amb = ns_env_lookup(S, S->fn_ambient_ns);
        if (amb != NULL) v = mino_env_get(amb, data);
    }
    if (v == NULL) {
        char msg[300];
        snprintf(msg, sizeof(msg), "unbound symbol: %s", data);
        set_eval_diag(S, mino_current_ctx(S)->eval_current_form, "name", "MNS001", msg);
        return NULL;
    }
    return v;
}

/* A form evaluates to itself when all of its children would also evaluate
 * to themselves — leaf primitives (int, string, etc.) always, and nested
 * vectors/maps/sets/lazies only when they contain no symbols or calls. For
 * these, collection literals can return the AST form directly instead of
 * rebuilding it, since mino's data structures are immutable. */
static int is_eval_constant(mino_val_t *v)
{
    if (v == NULL) return 1;
    switch (v->type) {
    case MINO_NIL: case MINO_BOOL: case MINO_INT: case MINO_FLOAT:
    case MINO_STRING: case MINO_KEYWORD:
        return 1;
    default:
        return 0;
    }
}

static mino_val_t *eval_vector_literal(mino_state_t *S, mino_val_t *form,
                                       mino_env_t *env)
{
    size_t i;
    size_t n = form->as.vec.len;
    mino_val_t **tmp;
    if (n == 0) {
        return form;
    }
    /* Fast path: all elements are self-evaluating, so the literal already
     * equals its own result. Skip per-element eval and collection rebuild. */
    for (i = 0; i < n; i++) {
        if (!is_eval_constant(vec_nth(form, i))) break;
    }
    if (i == n) {
        return form;
    }
    tmp = (mino_val_t **)gc_alloc_typed(S, GC_T_VALARR, n * sizeof(*tmp));
    for (i = 0; i < n; i++) {
        mino_val_t *ev = eval_value(S, vec_nth(form, i), env);
        if (ev == NULL) {
            return NULL;
        }
        gc_valarr_set(S, tmp, i, ev);
    }
    {
        mino_val_t *result = mino_vector(S, tmp, n);
        if (form->meta != NULL) {
            result->meta = form->meta;
        }
        return result;
    }
}

static mino_val_t *eval_map_literal(mino_state_t *S, mino_val_t *form,
                                    mino_env_t *env)
{
    size_t i;
    size_t n = form->as.map.len;
    mino_val_t **ks;
    mino_val_t **vs;
    if (n == 0) {
        return form;
    }
    /* Fast path: every key and value is self-evaluating. */
    for (i = 0; i < n; i++) {
        mino_val_t *form_key = vec_nth(form->as.map.key_order, i);
        if (!is_eval_constant(form_key)) break;
        if (!is_eval_constant(map_get_val(form, form_key))) break;
    }
    if (i == n) {
        return form;
    }
    ks = (mino_val_t **)gc_alloc_typed(S, GC_T_VALARR, n * sizeof(*ks));
    vs = (mino_val_t **)gc_alloc_typed(S, GC_T_VALARR, n * sizeof(*vs));
    for (i = 0; i < n; i++) {
        mino_val_t *form_key = vec_nth(form->as.map.key_order, i);
        mino_val_t *form_val = map_get_val(form, form_key);
        mino_val_t *k = eval_value(S, form_key, env);
        mino_val_t *v;
        if (k == NULL) { return NULL; }
        v = eval_value(S, form_val, env);
        if (v == NULL) { return NULL; }
        gc_valarr_set(S, ks, i, k);
        gc_valarr_set(S, vs, i, v);
    }
    {
        mino_val_t *result = mino_map(S, ks, vs, n);
        if (form->meta != NULL) {
            result->meta = form->meta;
        }
        return result;
    }
}

static mino_val_t *eval_set_literal(mino_state_t *S, mino_val_t *form,
                                    mino_env_t *env)
{
    size_t i;
    size_t n = form->as.set.len;
    mino_val_t **tmp;
    if (n == 0) {
        return form;
    }
    /* Fast path: every element is self-evaluating. */
    for (i = 0; i < n; i++) {
        if (!is_eval_constant(vec_nth(form->as.set.key_order, i))) break;
    }
    if (i == n) {
        return form;
    }
    tmp = (mino_val_t **)gc_alloc_typed(S, GC_T_VALARR, n * sizeof(*tmp));
    for (i = 0; i < n; i++) {
        mino_val_t *ev = eval_value(S, vec_nth(form->as.set.key_order, i), env);
        if (ev == NULL) {
            return NULL;
        }
        gc_valarr_set(S, tmp, i, ev);
    }
    {
        mino_val_t *result = mino_set(S, tmp, n);
        if (form->meta != NULL) {
            result->meta = form->meta;
        }
        return result;
    }
}

static void sf_init(mino_state_t *S)
{
    S->sf_quote            = mino_symbol(S, "quote");
    S->sf_quasiquote       = mino_symbol(S, "quasiquote");
    S->sf_unquote          = mino_symbol(S, "unquote");
    S->sf_unquote_splicing = mino_symbol(S, "unquote-splicing");
    S->sf_defmacro         = mino_symbol(S, "defmacro");
    S->sf_declare          = mino_symbol(S, "declare");
    S->sf_ns               = mino_symbol(S, "ns");
    S->sf_var              = mino_symbol(S, "var");
    S->sf_def              = mino_symbol(S, "def");
    S->sf_if               = mino_symbol(S, "if");
    S->sf_do               = mino_symbol(S, "do");
    S->sf_let              = mino_symbol(S, "let");
    S->sf_let_star         = mino_symbol(S, "let*");
    S->sf_fn               = mino_symbol(S, "fn");
    S->sf_fn_star          = mino_symbol(S, "fn*");
    S->sf_recur            = mino_symbol(S, "recur");
    S->sf_loop             = mino_symbol(S, "loop");
    S->sf_loop_star        = mino_symbol(S, "loop*");
    S->sf_try              = mino_symbol(S, "try");
    S->sf_binding          = mino_symbol(S, "binding");
    S->sf_lazy_seq         = mino_symbol(S, "lazy-seq");
    S->sf_new              = mino_symbol(S, "new");
    S->sf_when             = mino_symbol(S, "when");
    S->sf_and              = mino_symbol(S, "and");
    S->sf_or               = mino_symbol(S, "or");
    S->sf_initialized      = 1;
}

/* --- Main eval dispatch -------------------------------------------------- */

/*
 * Host-syntax sugar dispatch. Recognises the four interop shapes
 * and rewrites them into the corresponding host/... primitive call:
 *
 *   (.method target args...)         -> (host/call target :method args...)
 *   (.-field target)                 -> (host/get  target :field)
 *   (new TypeName args...)           -> (host/new :TypeName args...)
 *   (TypeName/staticMethod args...)  -> (host/static-call :TypeName :staticMethod args...)
 *
 * Returns 1 with `*out` set when the form was a host shape (success
 * or error path; *out reflects the call result, NULL on diag).
 * Returns 0 when the form is not a host shape, leaving *out untouched.
 */
static int eval_try_host_syntax(mino_state_t *S, mino_val_t *form,
                                mino_val_t *head, mino_val_t *args,
                                mino_env_t *env, mino_val_t **out)
{
    const char *hname;
    size_t      hlen;
    if (head == NULL || head->type != MINO_SYMBOL || head->as.s.len == 0) {
        return 0;
    }
    hname = head->as.s.data;
    hlen  = head->as.s.len;

    /* (.method target args...) -> (host/call target :method args...)
     * (.-field target)         -> (host/get  target :field) */
    if (hname[0] == '.' && hlen > 1) {
        int         is_getter = (hlen > 2 && hname[1] == '-');
        const char *member    = hname + (is_getter ? 2 : 1);
        mino_val_t *kw        = mino_keyword(S, member);
        gc_pin(kw);
        if (is_getter) {
            mino_val_t *target_form;
            mino_val_t *target_val;
            mino_val_t *prim;
            if (!mino_is_cons(args)) {
                gc_unpin(1);
                set_eval_diag(S, form, "syntax", "MSY001",
                              ".-field requires a target");
                *out = NULL;
                return 1;
            }
            target_form = args->as.cons.car;
            target_val  = eval_value(S, target_form, env);
            if (target_val == NULL) { gc_unpin(1); *out = NULL; return 1; }
            gc_pin(target_val);
            prim = mino_env_get(env, "host/get");
            if (prim == NULL) {
                gc_unpin(2);
                set_eval_diag(S, form, "name", "MNS001", "host/get not bound");
                *out = NULL;
                return 1;
            }
            {
                mino_val_t *a = mino_cons(S, kw, mino_nil(S));
                gc_pin(a);
                a = mino_cons(S, target_val, a);
                gc_unpin(3);
                *out = apply_callable(S, prim, a, env);
                return 1;
            }
        } else {
            mino_val_t *target_form;
            mino_val_t *target_val;
            mino_val_t *prim;
            mino_val_t *rest;
            mino_val_t *evaled_rest;
            if (!mino_is_cons(args)) {
                gc_unpin(1);
                set_eval_diag(S, form, "syntax", "MSY001",
                              ".method requires a target");
                *out = NULL;
                return 1;
            }
            target_form = args->as.cons.car;
            rest        = args->as.cons.cdr;
            target_val  = eval_value(S, target_form, env);
            if (target_val == NULL) { gc_unpin(1); *out = NULL; return 1; }
            gc_pin(target_val);
            evaled_rest = eval_args(S, rest, env);
            if (evaled_rest == NULL && mino_is_cons(rest)) {
                gc_unpin(2); *out = NULL; return 1;
            }
            gc_pin(evaled_rest);
            prim = mino_env_get(env, "host/call");
            if (prim == NULL) {
                gc_unpin(3);
                set_eval_diag(S, form, "name", "MNS001",
                              "host/call not bound");
                *out = NULL;
                return 1;
            }
            {
                mino_val_t *a = mino_cons(S, kw, evaled_rest);
                gc_pin(a);
                a = mino_cons(S, target_val, a);
                gc_unpin(4);
                *out = apply_callable(S, prim, a, env);
                return 1;
            }
        }
    }

    /* (new TypeName args...) -> (host/new :TypeName args...) */
    if (hlen == 3 && memcmp(hname, "new", 3) == 0 && mino_is_cons(args)) {
        mino_val_t *type_sym = args->as.cons.car;
        if (type_sym != NULL && type_sym->type == MINO_SYMBOL) {
            mino_val_t *kw   = mino_keyword(S, type_sym->as.s.data);
            mino_val_t *rest = args->as.cons.cdr;
            mino_val_t *evaled_rest;
            mino_val_t *prim;
            gc_pin(kw);
            evaled_rest = eval_args(S, rest, env);
            if (evaled_rest == NULL && mino_is_cons(rest)) {
                gc_unpin(1); *out = NULL; return 1;
            }
            gc_pin(evaled_rest);
            prim = mino_env_get(env, "host/new");
            if (prim == NULL) {
                gc_unpin(2);
                set_eval_diag(S, form, "name", "MNS001",
                              "host/new not bound");
                *out = NULL;
                return 1;
            }
            {
                mino_val_t *a = mino_cons(S, kw, evaled_rest);
                gc_unpin(2);
                *out = apply_callable(S, prim, a, env);
                return 1;
            }
        }
    }

    /* (TypeName/staticMethod args...)
     * -> (host/static-call :TypeName :staticMethod args...)
     * Only if the namespace part matches a registered host type
     * and the full name is not a literal env binding. */
    {
        const char *sl = (hlen > 1) ? memchr(hname, '/', hlen) : NULL;
        if (sl != NULL) {
            char        tbuf[256];
            size_t      tlen  = (size_t)(sl - hname);
            const char *mname = sl + 1;
            if (tlen < sizeof(tbuf) && tlen > 0 && *mname != '\0'
                && mino_env_get(env, hname) == NULL) {
                host_type_t *ht;
                memcpy(tbuf, hname, tlen);
                tbuf[tlen] = '\0';
                ht = host_type_find(S, tbuf);
                if (ht != NULL) {
                    mino_val_t *tkw = mino_keyword(S, tbuf);
                    mino_val_t *mkw = mino_keyword(S, mname);
                    mino_val_t *evaled_rest;
                    mino_val_t *prim;
                    gc_pin(tkw);
                    gc_pin(mkw);
                    evaled_rest = eval_args(S, args, env);
                    if (evaled_rest == NULL && mino_is_cons(args)) {
                        gc_unpin(2); *out = NULL; return 1;
                    }
                    gc_pin(evaled_rest);
                    prim = mino_env_get(env, "host/static-call");
                    if (prim == NULL) {
                        gc_unpin(3);
                        set_eval_diag(S, form, "name", "MNS001",
                                      "host/static-call not bound");
                        *out = NULL;
                        return 1;
                    }
                    {
                        mino_val_t *a = mino_cons(S, mkw, evaled_rest);
                        gc_pin(a);
                        a = mino_cons(S, tkw, a);
                        gc_unpin(4);
                        *out = apply_callable(S, prim, a, env);
                        return 1;
                    }
                }
            }
        }
    }
    return 0;
}

/*
 * Apply a regular (non-special-form) call. Resolves the head into
 * a callable, evaluates the args, and dispatches by callable kind:
 * PRIM/FN go through apply_callable (with a tail-call sentinel for
 * FN in tail position); MACRO expands and re-evaluates the result;
 * other callables (keyword, map, vector, set, sorted-map,
 * sorted-set) flow through apply_non_fn_callable so the two
 * dispatch entries cannot drift.
 */
static mino_val_t *eval_apply_regular_call(mino_state_t *S, mino_val_t *form,
                                           mino_val_t *head, mino_val_t *args,
                                           mino_env_t *env, int tail)
{
    mino_val_t *fn = eval_value(S, head, env);
    mino_val_t *evaled;
    if (fn == NULL) {
        return NULL;
    }
    /* Pin fn: eval_args allocates, and the conservative stack
     * scanner may miss fn if the compiler keeps it in a register. */
    gc_pin(fn);
    /* Fast path for the dominant PRIM/FN case, checked first so
     * non-function callables (keyword/map/vector/etc.) don't delay
     * typical calls. */
    if (fn->type == MINO_PRIM || fn->type == MINO_FN) {
        evaled = eval_args(S, args, env);
        gc_unpin(1);
        if (evaled == NULL && mino_last_error(S) != NULL) {
            return NULL;
        }
        if (tail && fn->type == MINO_FN) {
            S->tail_call_sentinel.as.tail_call.fn   = fn;
            S->tail_call_sentinel.as.tail_call.args = evaled;
            return &S->tail_call_sentinel;
        }
        return apply_callable(S, fn, evaled, env);
    }
    if (fn->type == MINO_MACRO) {
        /* Expand with unevaluated args; re-eval the resulting form in
         * the caller's environment. */
        mino_val_t *expanded = apply_callable(S, fn, args, env);
        gc_unpin(1);
        if (expanded == NULL) {
            return NULL;
        }
        return eval_impl(S, expanded, env, tail);
    }
    /* Non-fn callables: keyword, map, vector, set, sorted-map,
     * sorted-set. */
    gc_unpin(1);
    evaled = eval_args(S, args, env);
    if (evaled == NULL && mino_last_error(S) != NULL) {
        return NULL;
    }
    return apply_non_fn_callable(S, fn, evaled, form);
}

/*
 * Per-step gate: bail out before doing real work if a host limit
 * (steps, heap), an interrupt, or a sticky limit-exceeded flag is
 * in effect. Sets the appropriate eval diagnostic and the
 * limit_exceeded latch so the rest of eval_impl observes a single
 * source of truth. Returns 0 if eval should bail (with diag set
 * when applicable), 1 to proceed.
 *
 * The safepoint poll for major-GC STW also folds in here — eval_impl
 * entry is the densest legitimate safepoint site, so one branch
 * covers both the cancel/limit path and the GC park request. The
 * poll is a single predictably-not-taken read on the single-threaded
 * fast path.
 */
static int eval_check_limits(mino_state_t *S)
{
    if (mino_current_ctx(S)->limit_exceeded) {
        return 0;
    }
    if (mino_current_ctx(S)->interrupted) {
        mino_current_ctx(S)->limit_exceeded = 1;
        set_eval_diag(S, mino_current_ctx(S)->eval_current_form, "limit", "MLM001",
                      "mino_current_ctx(S)->interrupted");
        return 0;
    }
    mino_safepoint_poll(S);
    if (S->limit_steps > 0 && ++mino_current_ctx(S)->eval_steps > S->limit_steps) {
        mino_current_ctx(S)->limit_exceeded = 1;
        set_eval_diag(S, mino_current_ctx(S)->eval_current_form, "limit", "MLM001",
                      "step limit exceeded");
        return 0;
    }
    if (S->limit_heap > 0 && S->gc_bytes_alloc > S->limit_heap) {
        mino_current_ctx(S)->limit_exceeded = 1;
        set_eval_diag(S, mino_current_ctx(S)->eval_current_form, "limit", "MLM001",
                      "heap limit exceeded");
        return 0;
    }
    return 1;
}

mino_val_t *eval_impl(mino_state_t *S, mino_val_t *form, mino_env_t *env, int tail)
{
    if (!S->sf_initialized) {
        sf_init(S);
    }
    if (!eval_check_limits(S)) {
        return NULL;
    }
    if (form == NULL) {
        return mino_nil(S);
    }
    switch (form->type) {
    case MINO_NIL:
    case MINO_EMPTY_LIST:
    case MINO_BOOL:
    case MINO_INT:
    case MINO_FLOAT:
    case MINO_CHAR:
    case MINO_STRING:
    case MINO_KEYWORD:
    case MINO_PRIM:
    case MINO_FN:
    case MINO_MACRO:
    case MINO_HANDLE:
    case MINO_ATOM:
    case MINO_VOLATILE:
    case MINO_LAZY:
    case MINO_CHUNK:
    case MINO_CHUNKED_CONS:
    case MINO_RECUR:
    case MINO_TAIL_CALL:
    case MINO_REDUCED:
    case MINO_VAR:
    case MINO_TRANSIENT:
    case MINO_SORTED_MAP:
    case MINO_SORTED_SET:
    case MINO_BIGINT:
    case MINO_RATIO:
    case MINO_BIGDEC:
    case MINO_TYPE:
    case MINO_RECORD:
    case MINO_FUTURE:
        return form;
    case MINO_SYMBOL:
        return eval_symbol(S, form, env);
    case MINO_VECTOR:
        return eval_vector_literal(S, form, env);
    case MINO_MAP:
        return eval_map_literal(S, form, env);
    case MINO_SET:
        return eval_set_literal(S, form, env);
    case MINO_CONS: {
        mino_val_t *head = form->as.cons.car;
        mino_val_t *args = form->as.cons.cdr;
        mino_val_t *host_result;
        mino_current_ctx(S)->eval_current_form = form;

        if (eval_try_host_syntax(S, form, head, args, env, &host_result)) {
            return host_result;
        }

        /* Special forms run through the data-table dispatch in
         * eval/special_registry.c. */
        {
            mino_val_t *sf_result;
            if (eval_try_special_form(S, form, head, args, env, tail,
                                      &sf_result)) {
                return sf_result;
            }
        }

        /* Function or macro application. */
        return eval_apply_regular_call(S, form, head, args, env, tail);
    }
    }
    set_eval_diag(S, mino_current_ctx(S)->eval_current_form, "internal", "MIN001", "eval: unknown value type");
    return NULL;
}

mino_val_t *eval(mino_state_t *S, mino_val_t *form, mino_env_t *env)
{
    return eval_impl(S, form, env, 0);
}
