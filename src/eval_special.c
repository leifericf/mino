/*
 * eval_special.c -- eval dispatch (eval_impl), literal evaluation, and the
 *                   top-level eval entry point.
 *
 * Special-form implementations live in domain-specific files:
 *   eval_special_defs.c      -- def, defmacro, declare
 *   eval_special_bindings.c  -- let, loop, binding, destructuring
 *   eval_special_control.c   -- try/catch/finally
 *   eval_special_fn.c        -- fn, apply_callable, arity dispatch
 */

#include "eval_special_internal.h"

/* --- Evaluator helpers: one per value kind. --- */

/* Look up an alias in the ns_aliases table; returns the full module name
 * or NULL if not found. */
static const char *alias_resolve(mino_state_t *S, const char *alias)
{
    size_t i;
    for (i = 0; i < S->ns_alias_len; i++) {
        if (strcmp(S->ns_aliases[i].alias, alias) == 0)
            return S->ns_aliases[i].full_name;
    }
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
        char ns_buf[256];
        const char *sym_name = slash + 1;
        size_t ns_len = (size_t)(slash - data);
        const char *resolved_ns;
        mino_val_t *var;

        /* Try full name as a literal env binding first (e.g. "host/new"). */
        v = mino_env_get(env, data);
        if (v != NULL) return v;

        if (ns_len >= sizeof(ns_buf)) {
            set_eval_diag(S, S->eval_current_form, "syntax", "MSY001",
                "symbol name too long");
            return NULL;
        }
        memcpy(ns_buf, data, ns_len);
        ns_buf[ns_len] = '\0';

        /* Resolve alias to full module name. */
        resolved_ns = alias_resolve(S, ns_buf);
        if (resolved_ns == NULL) resolved_ns = ns_buf;

        /* Look up in var registry by resolved namespace + name. */
        var = var_find(S, resolved_ns, sym_name);
        if (var != NULL) return var->as.var.root;

        /* Also try looking up the bare name in root env as fallback. */
        v = mino_env_get(env, sym_name);
        if (v != NULL) return v;

        {
            char msg[300];
            snprintf(msg, sizeof(msg), "unbound symbol: %s", data);
            set_eval_diag(S, S->eval_current_form, "name", "MNS001", msg);
            return NULL;
        }
    }

    v = (S->dyn_stack != NULL) ? dyn_lookup(S, data) : NULL;
    if (v == NULL) v = mino_env_get(env, data);
    if (v == NULL) {
        char msg[300];
        snprintf(msg, sizeof(msg), "unbound symbol: %s", data);
        set_eval_diag(S, S->eval_current_form, "name", "MNS001", msg);
        return NULL;
    }
    return v;
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
    tmp = (mino_val_t **)gc_alloc_typed(S, GC_T_VALARR, n * sizeof(*tmp));
    for (i = 0; i < n; i++) {
        mino_val_t *ev = eval_value(S, vec_nth(form, i), env);
        if (ev == NULL) {
            return NULL;
        }
        tmp[i] = ev;
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
        ks[i] = k;
        vs[i] = v;
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
    tmp = (mino_val_t **)gc_alloc_typed(S, GC_T_VALARR, n * sizeof(*tmp));
    for (i = 0; i < n; i++) {
        mino_val_t *ev = eval_value(S, vec_nth(form->as.set.key_order, i), env);
        if (ev == NULL) {
            return NULL;
        }
        tmp[i] = ev;
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

/* Special-form match: pointer-eq against the interned cached symbol is the
 * fast path. Symbols that carry metadata are fresh copies (reader clones them
 * before attaching meta) and need a content-based fallback. */
#define HEAD_IS(cached, lit) \
    (head == (cached) || \
     (head != NULL && head->meta != NULL && head->type == MINO_SYMBOL && \
      head->as.s.len == sizeof(lit) - 1 && \
      memcmp(head->as.s.data, (lit), sizeof(lit) - 1) == 0))

mino_val_t *eval_impl(mino_state_t *S, mino_val_t *form, mino_env_t *env, int tail)
{
    if (!S->sf_initialized) {
        sf_init(S);
    }
    if (S->limit_exceeded) {
        return NULL;
    }
    if (S->interrupted) {
        S->limit_exceeded = 1;
        set_eval_diag(S, S->eval_current_form, "limit", "MLM001", "S->interrupted");
        return NULL;
    }
    if (S->limit_steps > 0 && ++S->eval_steps > S->limit_steps) {
        S->limit_exceeded = 1;
        set_eval_diag(S, S->eval_current_form, "limit", "MLM001", "step limit exceeded");
        return NULL;
    }
    if (S->limit_heap > 0 && S->gc_bytes_alloc > S->limit_heap) {
        S->limit_exceeded = 1;
        set_eval_diag(S, S->eval_current_form, "limit", "MLM001", "heap limit exceeded");
        return NULL;
    }
    if (form == NULL) {
        return mino_nil(S);
    }
    switch (form->type) {
    case MINO_NIL:
    case MINO_BOOL:
    case MINO_INT:
    case MINO_FLOAT:
    case MINO_STRING:
    case MINO_KEYWORD:
    case MINO_PRIM:
    case MINO_FN:
    case MINO_MACRO:
    case MINO_HANDLE:
    case MINO_ATOM:
    case MINO_LAZY:
    case MINO_RECUR:
    case MINO_TAIL_CALL:
    case MINO_REDUCED:
    case MINO_VAR:
    case MINO_SORTED_MAP:
    case MINO_SORTED_SET:
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
        S->eval_current_form = form;

        /* Interop syntax desugaring. */
        if (head != NULL && head->type == MINO_SYMBOL
            && head->as.s.len > 0) {
            const char *hname = head->as.s.data;
            size_t      hlen  = head->as.s.len;

            /* (.method target args...) -> (host/call target :method args...)
             * (.-field target)         -> (host/get  target :field) */
            if (hname[0] == '.' && hlen > 1) {
                int is_getter = (hlen > 2 && hname[1] == '-');
                const char *member = hname + (is_getter ? 2 : 1);
                mino_val_t *kw = mino_keyword(S, member);
                gc_pin(kw);
                if (is_getter) {
                    /* (host/get target :field) */
                    mino_val_t *target_form;
                    mino_val_t *target_val;
                    mino_val_t *prim;
                    if (!mino_is_cons(args)) {
                        gc_unpin(1);
                        set_eval_diag(S, form, "syntax", "MSY001", ".-field requires a target");
                        return NULL;
                    }
                    target_form = args->as.cons.car;
                    target_val = eval_value(S, target_form, env);
                    if (target_val == NULL) { gc_unpin(1); return NULL; }
                    gc_pin(target_val);
                    prim = mino_env_get(env, "host/get");
                    if (prim == NULL) {
                        gc_unpin(2);
                        set_eval_diag(S, form, "name", "MNS001", "host/get not bound");
                        return NULL;
                    }
                    {
                        mino_val_t *a = mino_cons(S, kw, mino_nil(S));
                        gc_pin(a);
                        a = mino_cons(S, target_val, a);
                        gc_unpin(3);
                        return apply_callable(S, prim, a, env);
                    }
                } else {
                    /* (host/call target :method args...) */
                    mino_val_t *target_form;
                    mino_val_t *target_val;
                    mino_val_t *prim;
                    mino_val_t *rest;
                    mino_val_t *evaled_rest;
                    if (!mino_is_cons(args)) {
                        gc_unpin(1);
                        set_eval_diag(S, form, "syntax", "MSY001", ".method requires a target");
                        return NULL;
                    }
                    target_form = args->as.cons.car;
                    rest = args->as.cons.cdr;
                    target_val = eval_value(S, target_form, env);
                    if (target_val == NULL) { gc_unpin(1); return NULL; }
                    gc_pin(target_val);
                    evaled_rest = eval_args(S, rest, env);
                    if (evaled_rest == NULL && mino_is_cons(rest)) {
                        gc_unpin(2); return NULL;
                    }
                    gc_pin(evaled_rest);
                    prim = mino_env_get(env, "host/call");
                    if (prim == NULL) {
                        gc_unpin(3);
                        set_eval_diag(S, form, "name", "MNS001", "host/call not bound");
                        return NULL;
                    }
                    {
                        mino_val_t *a = mino_cons(S, kw, evaled_rest);
                        gc_pin(a);
                        a = mino_cons(S, target_val, a);
                        gc_unpin(4);
                        return apply_callable(S, prim, a, env);
                    }
                }
            }

            /* (new TypeName args...) -> (host/new :TypeName args...) */
            if (hlen == 3 && memcmp(hname, "new", 3) == 0
                && mino_is_cons(args)) {
                mino_val_t *type_sym = args->as.cons.car;
                if (type_sym != NULL && type_sym->type == MINO_SYMBOL) {
                    mino_val_t *kw = mino_keyword(S, type_sym->as.s.data);
                    mino_val_t *rest = args->as.cons.cdr;
                    mino_val_t *evaled_rest;
                    mino_val_t *prim;
                    gc_pin(kw);
                    evaled_rest = eval_args(S, rest, env);
                    if (evaled_rest == NULL && mino_is_cons(rest)) {
                        gc_unpin(1); return NULL;
                    }
                    gc_pin(evaled_rest);
                    prim = mino_env_get(env, "host/new");
                    if (prim == NULL) {
                        gc_unpin(2);
                        set_eval_diag(S, form, "name", "MNS001", "host/new not bound");
                        return NULL;
                    }
                    {
                        mino_val_t *a = mino_cons(S, kw, evaled_rest);
                        gc_unpin(2);
                        return apply_callable(S, prim, a, env);
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
                    char tbuf[256];
                    size_t tlen = (size_t)(sl - hname);
                    const char *mname = sl + 1;
                    if (tlen < sizeof(tbuf) && tlen > 0
                        && *mname != '\0'
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
                                gc_unpin(2); return NULL;
                            }
                            gc_pin(evaled_rest);
                            prim = mino_env_get(env, "host/static-call");
                            if (prim == NULL) {
                                gc_unpin(3);
                                set_eval_diag(S, form, "name", "MNS001",
                                             "host/static-call not bound");
                                return NULL;
                            }
                            {
                                mino_val_t *a = mino_cons(S, mkw,
                                                          evaled_rest);
                                gc_pin(a);
                                a = mino_cons(S, tkw, a);
                                gc_unpin(4);
                                return apply_callable(S, prim, a, env);
                            }
                        }
                    }
                }
            }
        }

        /* Special forms. */
        if (HEAD_IS(S->sf_quote, "quote")) {
            if (!mino_is_cons(args)) {
                set_eval_diag(S, form, "syntax", "MSY001", "quote requires one argument");
                return NULL;
            }
            return args->as.cons.car;
        }
        if (HEAD_IS(S->sf_quasiquote, "quasiquote")) {
            if (!mino_is_cons(args)) {
                set_eval_diag(S, form, "syntax", "MSY001", "quasiquote requires one argument");
                return NULL;
            }
            return quasiquote_expand(S, args->as.cons.car, env);
        }
        if (HEAD_IS(S->sf_unquote, "unquote")
            || HEAD_IS(S->sf_unquote_splicing, "unquote-splicing")) {
            set_eval_diag(S, form, "syntax", "MSY001", "unquote outside of quasiquote");
            return NULL;
        }
        if (HEAD_IS(S->sf_defmacro, "defmacro")) {
            return eval_defmacro(S, form, args, env);
        }
        if (HEAD_IS(S->sf_declare, "declare")) {
            return eval_declare(S, form, args, env);
        }
        if (HEAD_IS(S->sf_ns, "ns")) {
            return eval_ns(S, form, args, env);
        }
        if (HEAD_IS(S->sf_var, "var")) {
            mino_val_t *sym_arg;
            mino_val_t *var;
            char vbuf[256];
            size_t vn;
            if (!mino_is_cons(args) || mino_is_cons(args->as.cons.cdr)) {
                set_eval_diag(S, form, "syntax", "MSY001", "var requires exactly one argument");
                return NULL;
            }
            sym_arg = args->as.cons.car;
            if (sym_arg->type != MINO_SYMBOL) {
                set_eval_diag(S, form, "syntax", "MSY001", "var requires a symbol argument");
                return NULL;
            }
            vn = sym_arg->as.s.len;
            if (vn >= sizeof(vbuf)) {
                set_eval_diag(S, form, "syntax", "MSY001", "var: symbol name too long");
                return NULL;
            }
            memcpy(vbuf, sym_arg->as.s.data, vn);
            vbuf[vn] = '\0';
            /* Try qualified (ns/name) lookup. */
            {
                const char *sl = memchr(vbuf, '/', vn);
                if (sl != NULL && vn > 1) {
                    char ns_buf[256];
                    size_t ns_len = (size_t)(sl - vbuf);
                    const char *name = sl + 1;
                    memcpy(ns_buf, vbuf, ns_len);
                    ns_buf[ns_len] = '\0';
                    var = var_find(S, ns_buf, name);
                    if (var != NULL) return var;
                }
            }
            /* Unqualified: try current ns, then "user", then scan all. */
            var = var_find(S, S->current_ns, vbuf);
            if (var == NULL) var = var_find(S, "user", vbuf);
            if (var == NULL) {
                /* Fallback: search by name across all namespaces. */
                size_t vi;
                for (vi = 0; vi < S->var_registry_len; vi++) {
                    if (strcmp(S->var_registry[vi].name, vbuf) == 0) {
                        var = S->var_registry[vi].var;
                        break;
                    }
                }
            }
            if (var != NULL) return var;
            {
                char msg[300];
                snprintf(msg, sizeof(msg), "var: unbound symbol: %s", vbuf);
                set_eval_diag(S, form, "name", "MNS001", msg);
                return NULL;
            }
        }
        if (HEAD_IS(S->sf_def, "def")) {
            return eval_def(S, form, args, env);
        }
        if (HEAD_IS(S->sf_if, "if")) {
            mino_val_t *cond_form;
            mino_val_t *then_form;
            mino_val_t *else_form = mino_nil(S);
            mino_val_t *cond;
            if (!mino_is_cons(args) || !mino_is_cons(args->as.cons.cdr)) {
                set_eval_diag(S, form, "syntax", "MSY001", "if requires a condition and a then-branch");
                return NULL;
            }
            cond_form = args->as.cons.car;
            then_form = args->as.cons.cdr->as.cons.car;
            if (mino_is_cons(args->as.cons.cdr->as.cons.cdr)) {
                else_form = args->as.cons.cdr->as.cons.cdr->as.cons.car;
            }
            cond = eval_value(S, cond_form, env);
            if (cond == NULL) {
                return NULL;
            }
            /* Branch is tail position: propagate recur/tail-call. */
            return eval_impl(S, mino_is_truthy(cond) ? then_form : else_form,
                             env, tail);
        }
        if (HEAD_IS(S->sf_do, "do")) {
            return eval_implicit_do_impl(S, args, env, tail);
        }
        if (HEAD_IS(S->sf_let, "let") || HEAD_IS(S->sf_let_star, "let*")) {
            return eval_let(S, form, args, env, tail);
        }
        if (HEAD_IS(S->sf_fn, "fn") || HEAD_IS(S->sf_fn_star, "fn*")) {
            return eval_fn(S, form, args, env);
        }
        if (HEAD_IS(S->sf_recur, "recur")) {
            mino_val_t *evaled = eval_args(S, args, env);
            if (evaled == NULL && mino_last_error(S) != NULL) {
                return NULL;
            }
            S->recur_sentinel.as.recur.args = evaled;
            return &S->recur_sentinel;
        }
        if (HEAD_IS(S->sf_loop, "loop") || HEAD_IS(S->sf_loop_star, "loop*")) {
            return eval_loop(S, form, args, env, tail);
        }
        if (HEAD_IS(S->sf_try, "try")) {
            return eval_try(S, form, args, env);
        }
        if (HEAD_IS(S->sf_binding, "binding")) {
            return eval_binding(S, form, args, env);
        }

        if (HEAD_IS(S->sf_lazy_seq, "lazy-seq")) {
            mino_val_t *lz = alloc_val(S, MINO_LAZY);
            lz->as.lazy.body     = args;
            lz->as.lazy.env      = env;
            lz->as.lazy.cached   = NULL;
            lz->as.lazy.realized = 0;
            return lz;
        }

        /* when, and, or inlined from their core.mino macro definitions to
         * avoid per-invocation macro-expansion overhead. The macros remain
         * defined so macroexpand still returns the expected expansion. */
        if (HEAD_IS(S->sf_when, "when")) {
            mino_val_t *cond;
            if (!mino_is_cons(args)) {
                set_eval_diag(S, form, "syntax", "MSY001",
                    "when requires a condition");
                return NULL;
            }
            cond = eval_value(S, args->as.cons.car, env);
            if (cond == NULL) return NULL;
            if (!mino_is_truthy(cond)) return mino_nil(S);
            return eval_implicit_do_impl(S, args->as.cons.cdr, env, tail);
        }
        if (HEAD_IS(S->sf_and, "and")) {
            mino_val_t *result = &S->true_singleton;
            while (mino_is_cons(args)) {
                mino_val_t *rest = args->as.cons.cdr;
                /* Last clause is tail position. */
                if (!mino_is_cons(rest)) {
                    return eval_impl(S, args->as.cons.car, env, tail);
                }
                result = eval_value(S, args->as.cons.car, env);
                if (result == NULL) return NULL;
                if (!mino_is_truthy(result)) return result;
                args = rest;
            }
            return result;
        }
        if (HEAD_IS(S->sf_or, "or")) {
            mino_val_t *result = mino_nil(S);
            while (mino_is_cons(args)) {
                mino_val_t *rest = args->as.cons.cdr;
                if (!mino_is_cons(rest)) {
                    return eval_impl(S, args->as.cons.car, env, tail);
                }
                result = eval_value(S, args->as.cons.car, env);
                if (result == NULL) return NULL;
                if (mino_is_truthy(result)) return result;
                args = rest;
            }
            return result;
        }

        /* Function or macro application. */
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
                /* Expand with unevaluated args; re-eval the resulting form
                 * in the caller's environment. */
                mino_val_t *expanded = apply_callable(S, fn, args, env);
                gc_unpin(1);
                if (expanded == NULL) {
                    return NULL;
                }
                return eval_impl(S, expanded, env, tail);
            }
            if (fn->type == MINO_KEYWORD) {
                /* Callable keywords: (:k m) => (get m :k),
                 *                    (:k m default) => (get m :k default). */
                mino_val_t *kw = fn;
                int         nargs = 0;
                mino_val_t *tmp;
                gc_unpin(1);
                evaled = eval_args(S, args, env);
                if (evaled == NULL && mino_last_error(S) != NULL)
                    return NULL;
                for (tmp = evaled; mino_is_cons(tmp); tmp = tmp->as.cons.cdr)
                    nargs++;
                if (nargs < 1 || nargs > 2) {
                    set_eval_diag(S, form, "eval/arity", "MAR001",
                        "keyword as function takes 1 or 2 arguments");
                    return NULL;
                }
                {
                    mino_val_t *coll    = evaled->as.cons.car;
                    mino_val_t *def_val = nargs == 2
                        ? evaled->as.cons.cdr->as.cons.car
                        : mino_nil(S);
                    if (coll != NULL && coll->type == MINO_MAP) {
                        mino_val_t *v = map_get_val(coll, kw);
                        return v == NULL ? def_val : v;
                    }
                    if (coll != NULL && coll->type == MINO_SORTED_MAP) {
                        mino_val_t *v = rb_get(S, coll->as.sorted.root, kw,
                                                coll->as.sorted.comparator);
                        return v == NULL ? def_val : v;
                    }
                    return def_val;
                }
            }
            if (fn->type == MINO_MAP) {
                /* Callable maps: ({:a 1} :k) => (get {:a 1} :k),
                 *                ({:a 1} :k d) => (get {:a 1} :k d). */
                mino_val_t *m = fn;
                int         nargs = 0;
                mino_val_t *tmp;
                gc_unpin(1);
                evaled = eval_args(S, args, env);
                if (evaled == NULL && mino_last_error(S) != NULL)
                    return NULL;
                for (tmp = evaled; mino_is_cons(tmp); tmp = tmp->as.cons.cdr)
                    nargs++;
                if (nargs < 1 || nargs > 2) {
                    set_eval_diag(S, form, "eval/arity", "MAR001",
                        "map as function takes 1 or 2 arguments");
                    return NULL;
                }
                {
                    mino_val_t *key     = evaled->as.cons.car;
                    mino_val_t *def_val = nargs == 2
                        ? evaled->as.cons.cdr->as.cons.car
                        : mino_nil(S);
                    mino_val_t *v = map_get_val(m, key);
                    return v == NULL ? def_val : v;
                }
            }
            if (fn->type == MINO_VECTOR) {
                /* Callable vectors: ([1 2 3] 0) => (nth [1 2 3] 0). */
                mino_val_t *vec = fn;
                int         nargs = 0;
                mino_val_t *tmp;
                gc_unpin(1);
                evaled = eval_args(S, args, env);
                if (evaled == NULL && mino_last_error(S) != NULL)
                    return NULL;
                for (tmp = evaled; mino_is_cons(tmp); tmp = tmp->as.cons.cdr)
                    nargs++;
                if (nargs != 1) {
                    set_eval_diag(S, form, "eval/arity", "MAR001",
                        "vector as function takes 1 argument");
                    return NULL;
                }
                {
                    mino_val_t *idx = evaled->as.cons.car;
                    long long i;
                    if (idx == NULL || idx->type != MINO_INT) {
                        set_eval_diag(S, form, "eval/type", "MTY002",
                            "vector index must be an integer");
                        return NULL;
                    }
                    i = idx->as.i;
                    if (i < 0 || (size_t)i >= vec->as.vec.len) {
                        set_eval_diag(S, form, "eval/type", "MTY002",
                            "vector index out of bounds");
                        return NULL;
                    }
                    return vec_nth(vec, (size_t)i);
                }
            }
            if (fn->type == MINO_SET) {
                /* Callable sets: (#{:a :b} :a) => :a or nil. */
                mino_val_t *s = fn;
                int         nargs = 0;
                mino_val_t *tmp;
                gc_unpin(1);
                evaled = eval_args(S, args, env);
                if (evaled == NULL && mino_last_error(S) != NULL)
                    return NULL;
                for (tmp = evaled; mino_is_cons(tmp); tmp = tmp->as.cons.cdr)
                    nargs++;
                if (nargs != 1) {
                    set_eval_diag(S, form, "eval/arity", "MAR001",
                        "set as function takes 1 argument");
                    return NULL;
                }
                {
                    mino_val_t *key = evaled->as.cons.car;
                    uint32_t h = hash_val(key);
                    return hamt_get(s->as.set.root, key, h, 0u) != NULL
                        ? key : mino_nil(S);
                }
            }
            if (fn->type == MINO_SORTED_MAP) {
                /* Callable sorted maps: (sm :k) => (get sm :k). */
                mino_val_t *m = fn;
                int         nargs = 0;
                mino_val_t *tmp;
                gc_unpin(1);
                evaled = eval_args(S, args, env);
                if (evaled == NULL && mino_last_error(S) != NULL)
                    return NULL;
                for (tmp = evaled; mino_is_cons(tmp); tmp = tmp->as.cons.cdr)
                    nargs++;
                if (nargs < 1 || nargs > 2) {
                    set_eval_diag(S, form, "eval/arity", "MAR001",
                        "sorted-map as function takes 1 or 2 arguments");
                    return NULL;
                }
                {
                    mino_val_t *key     = evaled->as.cons.car;
                    mino_val_t *def_val = nargs == 2
                        ? evaled->as.cons.cdr->as.cons.car
                        : mino_nil(S);
                    mino_val_t *v = rb_get(S, m->as.sorted.root, key,
                                            m->as.sorted.comparator);
                    return v == NULL ? def_val : v;
                }
            }
            if (fn->type == MINO_SORTED_SET) {
                /* Callable sorted sets: (ss :a) => :a or nil. */
                mino_val_t *s = fn;
                int         nargs = 0;
                mino_val_t *tmp;
                gc_unpin(1);
                evaled = eval_args(S, args, env);
                if (evaled == NULL && mino_last_error(S) != NULL)
                    return NULL;
                for (tmp = evaled; mino_is_cons(tmp); tmp = tmp->as.cons.cdr)
                    nargs++;
                if (nargs != 1) {
                    set_eval_diag(S, form, "eval/arity", "MAR001",
                        "sorted-set as function takes 1 argument");
                    return NULL;
                }
                {
                    mino_val_t *key = evaled->as.cons.car;
                    return rb_contains(S, s->as.sorted.root, key,
                                        s->as.sorted.comparator)
                        ? key : mino_nil(S);
                }
            }
            gc_unpin(1);
            {
                char msg[128];
                snprintf(msg, sizeof(msg), "not a function (got %s)",
                         type_tag_str(fn));
                set_eval_diag(S, form, "eval/type", "MTY002", msg);
            }
            return NULL;
        }
    }
    }
    set_eval_diag(S, S->eval_current_form, "internal", "MIN001", "eval: unknown value type");
    return NULL;
}

mino_val_t *eval(mino_state_t *S, mino_val_t *form, mino_env_t *env)
{
    return eval_impl(S, form, env, 0);
}
