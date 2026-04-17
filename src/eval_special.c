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
    char buf[256];
    size_t n = form->as.s.len;
    mino_val_t *v;
    const char *slash;
    if (n >= sizeof(buf)) {
        set_error_at(S, S->eval_current_form, "symbol name too long");
        return NULL;
    }
    memcpy(buf, form->as.s.data, n);
    buf[n] = '\0';

    /* Check for namespace-qualified symbol (e.g. t/is, clojure.core/+).
     * Single-char "/" is the division function, not a qualified symbol. */
    slash = (n > 1) ? strchr(buf, '/') : NULL;
    if (slash != NULL) {
        char ns_buf[256];
        const char *sym_name = slash + 1;
        size_t ns_len = (size_t)(slash - buf);
        const char *resolved_ns;
        mino_val_t *var;

        memcpy(ns_buf, buf, ns_len);
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
            snprintf(msg, sizeof(msg), "unbound symbol: %s", buf);
            set_error_at(S, S->eval_current_form, msg);
            return NULL;
        }
    }

    v = dyn_lookup(S, buf);
    if (v == NULL) v = mino_env_get(env, buf);
    if (v == NULL) {
        char msg[300];
        snprintf(msg, sizeof(msg), "unbound symbol: %s", buf);
        set_error_at(S, S->eval_current_form, msg);
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

/* --- Main eval dispatch -------------------------------------------------- */

mino_val_t *eval_impl(mino_state_t *S, mino_val_t *form, mino_env_t *env, int tail)
{
    if (S->limit_exceeded) {
        return NULL;
    }
    if (S->interrupted) {
        S->limit_exceeded = 1;
        set_error(S, "S->interrupted");
        return NULL;
    }
    if (S->limit_steps > 0 && ++S->eval_steps > S->limit_steps) {
        S->limit_exceeded = 1;
        set_error(S, "step limit exceeded");
        return NULL;
    }
    if (S->limit_heap > 0 && S->gc_bytes_alloc > S->limit_heap) {
        S->limit_exceeded = 1;
        set_error(S, "heap limit exceeded");
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

        /* Special forms. */
        if (sym_eq(head, "quote")) {
            if (!mino_is_cons(args)) {
                set_error_at(S, form, "quote requires one argument");
                return NULL;
            }
            return args->as.cons.car;
        }
        if (sym_eq(head, "quasiquote")) {
            if (!mino_is_cons(args)) {
                set_error_at(S, form, "quasiquote requires one argument");
                return NULL;
            }
            return quasiquote_expand(S, args->as.cons.car, env);
        }
        if (sym_eq(head, "unquote")
            || sym_eq(head, "unquote-splicing")) {
            set_error_at(S, form, "unquote outside of quasiquote");
            return NULL;
        }
        if (sym_eq(head, "defmacro")) {
            return eval_defmacro(S, form, args, env);
        }
        if (sym_eq(head, "declare")) {
            return eval_declare(S, form, args, env);
        }
        if (sym_eq(head, "ns")) {
            return eval_ns(S, form, args, env);
        }
        if (sym_eq(head, "var")) {
            mino_val_t *sym_arg;
            mino_val_t *var;
            char vbuf[256];
            size_t vn;
            if (!mino_is_cons(args) || mino_is_cons(args->as.cons.cdr)) {
                set_error_at(S, form, "var requires exactly one argument");
                return NULL;
            }
            sym_arg = args->as.cons.car;
            if (sym_arg->type != MINO_SYMBOL) {
                set_error_at(S, form, "var requires a symbol argument");
                return NULL;
            }
            vn = sym_arg->as.s.len;
            if (vn >= sizeof(vbuf)) {
                set_error_at(S, form, "var: symbol name too long");
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
                set_error_at(S, form, msg);
                return NULL;
            }
        }
        if (sym_eq(head, "def")) {
            return eval_def(S, form, args, env);
        }
        if (sym_eq(head, "if")) {
            mino_val_t *cond_form;
            mino_val_t *then_form;
            mino_val_t *else_form = mino_nil(S);
            mino_val_t *cond;
            if (!mino_is_cons(args) || !mino_is_cons(args->as.cons.cdr)) {
                set_error_at(S, form, "if requires a condition and a then-branch");
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
        if (sym_eq(head, "do")) {
            return eval_implicit_do_impl(S, args, env, tail);
        }
        if (sym_eq(head, "let") || sym_eq(head, "let*")) {
            return eval_let(S, form, args, env, tail);
        }
        if (sym_eq(head, "fn") || sym_eq(head, "fn*")) {
            return eval_fn(S, form, args, env);
        }
        if (sym_eq(head, "recur")) {
            mino_val_t *evaled = eval_args(S, args, env);
            mino_val_t *r;
            if (evaled == NULL && mino_last_error(S) != NULL) {
                return NULL;
            }
            r = alloc_val(S, MINO_RECUR);
            r->as.recur.args = evaled;
            return r;
        }
        if (sym_eq(head, "loop") || sym_eq(head, "loop*")) {
            return eval_loop(S, form, args, env, tail);
        }
        if (sym_eq(head, "try")) {
            return eval_try(S, form, args, env);
        }
        if (sym_eq(head, "binding")) {
            return eval_binding(S, form, args, env);
        }

        if (sym_eq(head, "lazy-seq")) {
            mino_val_t *lz = alloc_val(S, MINO_LAZY);
            lz->as.lazy.body     = args;
            lz->as.lazy.env      = env;
            lz->as.lazy.cached   = NULL;
            lz->as.lazy.realized = 0;
            return lz;
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
                    set_error_at(S, form,
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
                    set_error_at(S, form,
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
                    set_error_at(S, form,
                        "vector as function takes 1 argument");
                    return NULL;
                }
                {
                    mino_val_t *idx = evaled->as.cons.car;
                    long long i;
                    if (idx == NULL || idx->type != MINO_INT) {
                        set_error_at(S, form,
                            "vector index must be an integer");
                        return NULL;
                    }
                    i = idx->as.i;
                    if (i < 0 || (size_t)i >= vec->as.vec.len) {
                        set_error_at(S, form,
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
                    set_error_at(S, form,
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
                    set_error_at(S, form,
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
                    set_error_at(S, form,
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
            if (fn->type != MINO_PRIM && fn->type != MINO_FN) {
                gc_unpin(1);
                {
                    char msg[128];
                    snprintf(msg, sizeof(msg), "not a function (got %s)",
                             type_tag_str(fn));
                    set_error_at(S, form, msg);
                }
                return NULL;
            }
            evaled = eval_args(S, args, env);
            gc_unpin(1);
            if (evaled == NULL && mino_last_error(S) != NULL) {
                return NULL;
            }
            /* Proper tail calls: in tail position, return a trampoline
             * sentinel instead of growing the C stack. */
            if (tail && fn->type == MINO_FN) {
                mino_val_t *tc = alloc_val(S, MINO_TAIL_CALL);
                tc->as.tail_call.fn   = fn;
                tc->as.tail_call.args = evaled;
                return tc;
            }
            return apply_callable(S, fn, evaled, env);
        }
    }
    }
    set_error(S, "eval: unknown value type");
    return NULL;
}

mino_val_t *eval(mino_state_t *S, mino_val_t *form, mino_env_t *env)
{
    return eval_impl(S, form, env, 0);
}
