/*
 * mino.c -- evaluator core: dispatch, special forms, apply, destructuring.
 */

#include "mino_internal.h"

int sym_eq(const mino_val_t *v, const char *s)
{
    size_t n;
    if (v == NULL || v->type != MINO_SYMBOL) {
        return 0;
    }
    n = strlen(s);
    return v->as.s.len == n && memcmp(v->as.s.data, s, n) == 0;
}

static int kw_eq(const mino_val_t *v, const char *s)
{
    size_t n;
    if (v == NULL || v->type != MINO_KEYWORD) {
        return 0;
    }
    n = strlen(s);
    return v->as.s.len == n && memcmp(v->as.s.data, s, n) == 0;
}

/*
 * macroexpand1: if `form` is a call whose head resolves to a macro in env,
 * expand it once and return the new form. If not a macro call, return the
 * input unchanged and set *expanded = 0.
 */
mino_val_t *macroexpand1(mino_state_t *S, mino_val_t *form, mino_env_t *env,
                         int *expanded)
{
    char        buf[256];
    size_t      n;
    mino_val_t *head;
    mino_val_t *mac;
    *expanded = 0;
    if (!mino_is_cons(form)) {
        return form;
    }
    head = form->as.cons.car;
    if (head == NULL || head->type != MINO_SYMBOL) {
        return form;
    }
    n = head->as.s.len;
    if (n >= sizeof(buf)) {
        return form;
    }
    memcpy(buf, head->as.s.data, n);
    buf[n] = '\0';
    mac = mino_env_get(env, buf);
    if (mac == NULL || mac->type != MINO_MACRO) {
        return form;
    }
    *expanded = 1;
    return apply_callable(S, mac, form->as.cons.cdr, env);
}

/* Expand repeatedly until `form` is no longer a macro call at the top. */
mino_val_t *macroexpand_all(mino_state_t *S, mino_val_t *form, mino_env_t *env)
{
    for (;;) {
        int         expanded = 0;
        mino_val_t *next     = macroexpand1(S, form, env, &expanded);
        if (next == NULL) {
            return NULL;
        }
        if (!expanded) {
            return form;
        }
        form = next;
    }
}

/*
 * quasiquote_expand: walk `form` as a template. Sublists, subvectors, and
 * submaps are recursed into; (unquote x) evaluates x and uses its value;
 * (unquote-splicing x) evaluates x (expected to yield a list) and splices
 * its elements into the enclosing list.
 */
static mino_val_t *quasiquote_expand(mino_state_t *S, mino_val_t *form,
                                     mino_env_t *env)
{
    if (form != NULL && form->type == MINO_VECTOR) {
        size_t       nn  = form->as.vec.len;
        mino_val_t **tmp;
        size_t       i;
        if (nn == 0) { return form; }
        /* GC_T_VALARR: scratch buffer whose slots the collector traces as
         * mino_val_t*, so partial fills survive allocation mid-loop. */
        tmp = (mino_val_t **)gc_alloc_typed(S, GC_T_VALARR, nn * sizeof(*tmp));
        for (i = 0; i < nn; i++) {
            mino_val_t *e = quasiquote_expand(S, vec_nth(form, i), env);
            if (e == NULL) { return NULL; }
            tmp[i] = e;
        }
        return mino_vector(S, tmp, nn);
    }
    if (form != NULL && form->type == MINO_MAP) {
        size_t        nn = form->as.map.len;
        mino_val_t  **ks;
        mino_val_t  **vs;
        size_t        i;
        if (nn == 0) { return form; }
        ks = (mino_val_t **)gc_alloc_typed(S, GC_T_VALARR, nn * sizeof(*ks));
        vs = (mino_val_t **)gc_alloc_typed(S, GC_T_VALARR, nn * sizeof(*vs));
        for (i = 0; i < nn; i++) {
            mino_val_t *key = vec_nth(form->as.map.key_order, i);
            mino_val_t *val = map_get_val(form, key);
            mino_val_t *kk  = quasiquote_expand(S, key, env);
            mino_val_t *vv;
            if (kk == NULL) { return NULL; }
            vv = quasiquote_expand(S, val, env);
            if (vv == NULL) { return NULL; }
            ks[i] = kk; vs[i] = vv;
        }
        return mino_map(S, ks, vs, nn);
    }
    if (!mino_is_cons(form)) {
        return form;
    }
    {
        mino_val_t *head = form->as.cons.car;
        if (sym_eq(head, "unquote")) {
            mino_val_t *arg = form->as.cons.cdr;
            if (!mino_is_cons(arg)) {
                set_error(S, "unquote requires one argument");
                return NULL;
            }
            return eval_value(S, arg->as.cons.car, env);
        }
        if (sym_eq(head, "unquote-splicing")) {
            set_error(S, "unquote-splicing must appear inside a list");
            return NULL;
        }
    }
    {
        mino_val_t *out  = mino_nil(S);
        mino_val_t *tail = NULL;
        mino_val_t *p    = form;
        while (mino_is_cons(p)) {
            mino_val_t *elem = p->as.cons.car;
            if (mino_is_cons(elem)
                && sym_eq(elem->as.cons.car, "unquote-splicing")) {
                mino_val_t *arg = elem->as.cons.cdr;
                mino_val_t *spliced;
                mino_val_t *sp;
                if (!mino_is_cons(arg)) {
                    set_error(S, "unquote-splicing requires one argument");
                    return NULL;
                }
                spliced = eval_value(S, arg->as.cons.car, env);
                if (spliced == NULL) { return NULL; }
                sp = spliced;
                while (mino_is_cons(sp)) {
                    mino_val_t *cell = mino_cons(S, sp->as.cons.car, mino_nil(S));
                    if (tail == NULL) { out = cell; } else { tail->as.cons.cdr = cell; }
                    tail = cell;
                    sp = sp->as.cons.cdr;
                }
            } else {
                mino_val_t *expanded = quasiquote_expand(S, elem, env);
                mino_val_t *cell;
                if (expanded == NULL) { return NULL; }
                cell = mino_cons(S, expanded, mino_nil(S));
                if (tail == NULL) { out = cell; } else { tail->as.cons.cdr = cell; }
                tail = cell;
            }
            p = p->as.cons.cdr;
        }
        return out;
    }
}

/*
 * Evaluate `form` for its value. Any MINO_RECUR escaping here is a
 * non-tail recur and is rejected. Use plain eval(S, ) in positions where
 * a recur is legitimately in tail position (if branches, implicit-do
 * trailing expression, fn/loop body through the trampoline).
 */
mino_val_t *eval_value(mino_state_t *S, mino_val_t *form, mino_env_t *env)
{
    mino_val_t *v = eval(S, form, env);
    if (v == NULL) {
        return NULL;
    }
    if (v->type == MINO_RECUR) {
        set_error(S, "recur must be in tail position");
        return NULL;
    }
    if (v->type == MINO_TAIL_CALL) {
        set_error(S, "tail call in non-tail position");
        return NULL;
    }
    return v;
}

mino_val_t *eval_implicit_do_impl(mino_state_t *S, mino_val_t *body,
                                  mino_env_t *env, int tail)
{
    if (!mino_is_cons(body)) {
        return mino_nil(S);
    }
    for (;;) {
        mino_val_t *rest = body->as.cons.cdr;
        if (!mino_is_cons(rest)) {
            /* Last expression: tail position, propagate recur/tail-call. */
            return eval_impl(S, body->as.cons.car, env, tail);
        }
        if (eval_value(S, body->as.cons.car, env) == NULL) {
            return NULL;
        }
        body = rest;
    }
}

mino_val_t *eval_implicit_do(mino_state_t *S, mino_val_t *body, mino_env_t *env)
{
    return eval_implicit_do_impl(S, body, env, 0);
}

/*
 * Force a lazy sequence: evaluate the body in the captured environment,
 * cache the result, and release the thunk for GC. Iteratively unwraps
 * nested lazy seqs to avoid stack overflow.
 */
mino_val_t *lazy_force(mino_state_t *S, mino_val_t *v)
{
    if (v->as.lazy.realized) {
        return v->as.lazy.cached;
    }
    {
        mino_val_t *result = eval_implicit_do(S, v->as.lazy.body, v->as.lazy.env);
        if (result == NULL) return NULL;
        /* Iteratively unwrap nested lazy seqs. */
        while (result != NULL && result->type == MINO_LAZY) {
            if (result->as.lazy.realized) {
                result = result->as.lazy.cached;
            } else {
                mino_val_t *inner = eval_implicit_do(S, 
                    result->as.lazy.body, result->as.lazy.env);
                result->as.lazy.cached  = inner;
                result->as.lazy.realized = 1;
                result->as.lazy.body    = NULL;
                result->as.lazy.env     = NULL;
                result = inner;
                if (result == NULL) return NULL;
            }
        }
        v->as.lazy.cached   = result;
        v->as.lazy.realized = 1;
        v->as.lazy.body     = NULL;
        v->as.lazy.env      = NULL;
        return result;
    }
}

mino_val_t *eval_args(mino_state_t *S, mino_val_t *args, mino_env_t *env)
{
    mino_val_t *head = mino_nil(S);
    mino_val_t *tail = NULL;
    while (mino_is_cons(args)) {
        mino_val_t *v = eval_value(S, args->as.cons.car, env);
        mino_val_t *cell;
        if (v == NULL) {
            return NULL;
        }
        gc_pin(v);
        cell = mino_cons(S, v, mino_nil(S));
        gc_unpin(1);
        if (tail == NULL) {
            head = cell;
        } else {
            tail->as.cons.cdr = cell;
        }
        tail = cell;
        args = args->as.cons.cdr;
    }
    return head;
}

/* Helper: bind a single symbol name to a value. */
static int bind_sym(mino_state_t *S, mino_env_t *env, mino_val_t *sym,
                    mino_val_t *val)
{
    char   buf[256];
    size_t n = sym->as.s.len;
    if (n >= sizeof(buf)) {
        set_error(S, "parameter name too long");
        return 0;
    }
    memcpy(buf, sym->as.s.data, n);
    buf[n] = '\0';
    env_bind(S, env, buf, val);
    return 1;
}

/* Forward declaration for recursive destructuring. */
static int bind_form(mino_state_t *S, mino_env_t *env, mino_val_t *pattern,
                     mino_val_t *val, const char *ctx);

/*
 * Destructure a vector pattern against a sequential value (list or vector).
 * Supports positional binding, `&` rest, and `:as` whole-binding.
 */
static int bind_vec_destructure(mino_state_t *S, mino_env_t *env,
                                mino_val_t *pattern, mino_val_t *val,
                                const char *ctx)
{
    size_t plen = pattern->as.vec.len;
    size_t i;
    /* Walk the sequential value into a cons list for uniform access. */
    mino_val_t *args = val;
    if (val != NULL && val->type == MINO_VECTOR) {
        /* Convert vector value to a cons list for positional walk. */
        mino_val_t *lst = mino_nil(S);
        size_t j = val->as.vec.len;
        while (j > 0) {
            j--;
            lst = mino_cons(S, vec_nth(val, j), lst);
        }
        args = lst;
    }
    for (i = 0; i < plen; i++) {
        mino_val_t *p = vec_nth(pattern, i);
        if (sym_eq(p, "&")) {
            /* Rest parameter: next element gets remaining args. */
            if (i + 1 >= plen) {
                set_error(S, "& must be followed by a binding form");
                return 0;
            }
            i++;
            p = vec_nth(pattern, i);
            if (!bind_form(S, env, p, args, ctx)) return 0;
            /* Check for :as after rest. */
            if (i + 1 < plen && kw_eq(vec_nth(pattern, i + 1), "as")) {
                if (i + 2 >= plen) {
                    set_error(S, ":as must be followed by a symbol");
                    return 0;
                }
                i += 2;
                p = vec_nth(pattern, i);
                if (p == NULL || p->type != MINO_SYMBOL) {
                    set_error(S, ":as must be followed by a symbol");
                    return 0;
                }
                if (!bind_sym(S, env, p, val)) return 0;
            }
            if (i + 1 < plen) {
                set_error(S, "unexpected forms after & binding");
                return 0;
            }
            return 1;
        }
        if (kw_eq(p, "as")) {
            /* Whole-collection binding. */
            if (i + 1 >= plen) {
                set_error(S, ":as must be followed by a symbol");
                return 0;
            }
            i++;
            p = vec_nth(pattern, i);
            if (p == NULL || p->type != MINO_SYMBOL) {
                set_error(S, ":as must be followed by a symbol");
                return 0;
            }
            if (!bind_sym(S, env, p, val)) return 0;
            continue;
        }
        /* Normal positional binding. */
        if (!mino_is_cons(args)) {
            /* Bind to nil when value is shorter than pattern. */
            if (!bind_form(S, env, p, mino_nil(S), ctx)) return 0;
        } else {
            if (!bind_form(S, env, p, args->as.cons.car, ctx)) return 0;
            args = args->as.cons.cdr;
        }
    }
    return 1;
}

/*
 * Destructure a map pattern against a map value.
 * Supports :keys [a b], explicit {sym :key} mapping, :or {defaults}, :as sym.
 */
static int bind_map_destructure(mino_state_t *S, mino_env_t *env,
                                mino_val_t *pattern, mino_val_t *val,
                                const char *ctx)
{
    /* Collect :keys, :or, :as, and explicit bindings from the pattern map. */
    mino_val_t *keys_vec  = NULL;
    mino_val_t *or_map    = NULL;
    mino_val_t *as_sym    = NULL;
    size_t i;

    if (val == NULL || val->type != MINO_MAP) {
        val = mino_nil(S);
    }

    /* Iterate pattern map entries by key_order. */
    for (i = 0; i < pattern->as.map.len; i++) {
        mino_val_t *pkey = vec_nth(pattern->as.map.key_order, i);
        mino_val_t *pval = map_get_val(pattern, pkey);
        if (pkey->type == MINO_KEYWORD && pkey->as.s.len == 4
            && memcmp(pkey->as.s.data, "keys", 4) == 0) {
            keys_vec = pval;
        } else if (pkey->type == MINO_KEYWORD && pkey->as.s.len == 2
                   && memcmp(pkey->as.s.data, "or", 2) == 0) {
            or_map = pval;
        } else if (pkey->type == MINO_KEYWORD && pkey->as.s.len == 2
                   && memcmp(pkey->as.s.data, "as", 2) == 0) {
            as_sym = pval;
        } else if (pkey->type == MINO_SYMBOL) {
            /* Explicit binding: {sym :key} — pkey is the symbol to
             * bind, pval is the keyword to look up in val. */
            mino_val_t *found = NULL;
            if (val->type == MINO_MAP) {
                found = map_get_val(val, pval);
            }
            if (found == NULL && or_map != NULL && or_map->type == MINO_MAP) {
                found = map_get_val(or_map, pkey);
            }
            if (found == NULL) found = mino_nil(S);
            if (!bind_form(S, env, pkey, found, ctx)) return 0;
        }
    }

    /* :keys [a b] — look up :a, :b in val. */
    if (keys_vec != NULL && keys_vec->type == MINO_VECTOR) {
        for (i = 0; i < keys_vec->as.vec.len; i++) {
            mino_val_t *ksym = vec_nth(keys_vec, i);
            mino_val_t *lookup_key;
            mino_val_t *found;
            if (ksym == NULL || ksym->type != MINO_SYMBOL) {
                set_error(S, ":keys elements must be symbols");
                return 0;
            }
            lookup_key = mino_keyword_n(S, ksym->as.s.data, ksym->as.s.len);
            found = NULL;
            if (val->type == MINO_MAP) {
                found = map_get_val(val, lookup_key);
            }
            if (found == NULL && or_map != NULL && or_map->type == MINO_MAP) {
                /* :or map uses symbol keys: {a default-val} */
                found = map_get_val(or_map, ksym);
            }
            if (found == NULL) found = mino_nil(S);
            if (!bind_sym(S, env, ksym, found)) return 0;
        }
    }

    /* :as sym — bind whole map. */
    if (as_sym != NULL) {
        if (as_sym->type != MINO_SYMBOL) {
            set_error(S, ":as must be followed by a symbol");
            return 0;
        }
        if (!bind_sym(S, env, as_sym, val)) return 0;
    }

    return 1;
}

/*
 * Recursive destructuring binder. Dispatches on pattern type:
 * - Symbol: bind directly
 * - Vector: positional destructuring
 * - Map: key destructuring
 */
static int bind_form(mino_state_t *S, mino_env_t *env, mino_val_t *pattern,
                     mino_val_t *val, const char *ctx)
{
    if (pattern == NULL || pattern->type == MINO_SYMBOL) {
        return bind_sym(S, env, pattern, val);
    }
    if (pattern->type == MINO_VECTOR) {
        return bind_vec_destructure(S, env, pattern, val, ctx);
    }
    if (pattern->type == MINO_MAP) {
        return bind_map_destructure(S, env, pattern, val, ctx);
    }
    set_error(S, "unsupported binding form (expected symbol, vector, or map)");
    return 0;
}

/*
 * Bind a parameter list (cons list or vector) to a list of argument values.
 * Returns 1 on success, 0 on arity mismatch or error (with error set).
 */
static int bind_params(mino_state_t *S, mino_env_t *env, mino_val_t *params,
                       mino_val_t *args, const char *ctx)
{
    /* Vector params: delegate to vector destructuring. */
    if (params != NULL && params->type == MINO_VECTOR) {
        /* Wrap args list in a temporary vector-like structure?
         * No — bind_vec_destructure already accepts cons lists. */
        return bind_vec_destructure(S, env, params, args, ctx);
    }
    /* Cons-list params: walk in parallel with args. */
    while (mino_is_cons(params)) {
        mino_val_t *name = params->as.cons.car;
        /* `&` marks a rest-parameter. */
        if (sym_eq(name, "&")) {
            params = params->as.cons.cdr;
            if (!mino_is_cons(params)
                || params->as.cons.car == NULL) {
                set_error(S, "& must be followed by a binding form");
                return 0;
            }
            if (mino_is_cons(params->as.cons.cdr)) {
                set_error(S, "& parameter must be last");
                return 0;
            }
            return bind_form(S, env, params->as.cons.car, args, ctx);
        }
        if (!mino_is_cons(args)) {
            char msg[96];
            snprintf(msg, sizeof(msg), "%s arity mismatch", ctx);
            set_error(S, msg);
            return 0;
        }
        if (!bind_form(S, env, name, args->as.cons.car, ctx)) return 0;
        params = params->as.cons.cdr;
        args   = args->as.cons.cdr;
    }
    if (mino_is_cons(args)) {
        char msg[96];
        snprintf(msg, sizeof(msg), "%s arity mismatch", ctx);
        set_error(S, msg);
        return 0;
    }
    return 1;
}

/* --- Evaluator helpers: one per value kind or special-form cluster. --- */

static mino_val_t *eval_symbol(mino_state_t *S, mino_val_t *form, mino_env_t *env)
{
    char buf[256];
    size_t n = form->as.s.len;
    mino_val_t *v;
    if (n >= sizeof(buf)) {
        set_error_at(S, eval_current_form, "symbol name too long");
        return NULL;
    }
    memcpy(buf, form->as.s.data, n);
    buf[n] = '\0';
    v = dyn_lookup(S, buf);
    if (v == NULL) v = mino_env_get(env, buf);
    if (v == NULL) {
        char msg[300];
        snprintf(msg, sizeof(msg), "unbound symbol: %s", buf);
        set_error_at(S, eval_current_form, msg);
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

static mino_val_t *eval_defmacro(mino_state_t *S, mino_val_t *form,
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

static mino_val_t *eval_declare(mino_state_t *S, mino_val_t *form,
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

static mino_val_t *eval_def(mino_state_t *S, mino_val_t *form,
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

static mino_val_t *eval_let(mino_state_t *S, mino_val_t *form,
                            mino_val_t *args, mino_env_t *env, int tail)
{
    mino_val_t *bindings;
    mino_val_t *body;
    mino_env_t *local;
    if (!mino_is_cons(args)) {
        set_error_at(S, form, "let requires a binding form and body");
        return NULL;
    }
    bindings = args->as.cons.car;
    body     = args->as.cons.cdr;
    local = env_child(S, env);
    if (bindings != NULL && bindings->type == MINO_VECTOR) {
        /* Vector binding form: [pattern val pattern val ...] */
        size_t vlen = bindings->as.vec.len;
        size_t vi;
        if (vlen % 2 != 0) {
            set_error_at(S, form, "let vector bindings must have even number of forms");
            return NULL;
        }
        for (vi = 0; vi < vlen; vi += 2) {
            mino_val_t *pat = vec_nth(bindings, vi);
            mino_val_t *val = eval_value(S, vec_nth(bindings, vi + 1), local);
            if (val == NULL) return NULL;
            gc_pin(val);
            if (!bind_form(S, local, pat, val, "let")) {
                gc_unpin(1);
                return NULL;
            }
            gc_unpin(1);
        }
    } else if (mino_is_cons(bindings) || mino_is_nil(bindings)) {
        /* Legacy list binding form: (name val name val ...) */
        while (mino_is_cons(bindings)) {
            mino_val_t *name_form = bindings->as.cons.car;
            mino_val_t *rest_pair = bindings->as.cons.cdr;
            mino_val_t *val;
            if (!mino_is_cons(rest_pair)) {
                set_error_at(S, form, "let binding missing value");
                return NULL;
            }
            val = eval_value(S, rest_pair->as.cons.car, local);
            if (val == NULL) return NULL;
            gc_pin(val);
            if (!bind_form(S, local, name_form, val, "let")) {
                gc_unpin(1);
                return NULL;
            }
            gc_unpin(1);
            bindings = rest_pair->as.cons.cdr;
        }
    } else {
        set_error_at(S, form, "let bindings must be a list or vector");
        return NULL;
    }
    return eval_implicit_do_impl(S, body, local, tail);
}

static mino_val_t *eval_fn(mino_state_t *S, mino_val_t *form,
                           mino_val_t *args, mino_env_t *env)
{
    mino_val_t *fn_name = NULL;
    mino_val_t *params;
    mino_val_t *body;
    mino_val_t *p;
    mino_val_t *fn_val;
    int         multi_arity = 0;
    if (!mino_is_cons(args)) {
        set_error_at(S, form, "fn requires a parameter list");
        return NULL;
    }
    /* Optional name: (fn name (...) body) or (fn name ([x] ...) ([x y] ...)) */
    if (args->as.cons.car != NULL
        && args->as.cons.car->type == MINO_SYMBOL
        && mino_is_cons(args->as.cons.cdr)) {
        mino_val_t *after = args->as.cons.cdr->as.cons.car;
        if (after != NULL
            && (mino_is_cons(after) || mino_is_nil(after)
                || after->type == MINO_VECTOR)) {
            fn_name = args->as.cons.car;
            args    = args->as.cons.cdr;
        }
    }
    params = args->as.cons.car;
    body   = args->as.cons.cdr;
    /* Detect multi-arity: (fn ([x] ...) ([x y] ...))
     * The first arg is a list whose car is a vector or list. */
    if (mino_is_cons(params) && params->as.cons.car != NULL
        && (params->as.cons.car->type == MINO_VECTOR
            || (mino_is_cons(params->as.cons.car)
                || mino_is_nil(params->as.cons.car)))) {
        /* Could be multi-arity OR single-arity with list params.
         * Multi-arity: each clause is (params-vec . body-forms).
         * Disambiguate: if car of first arg is a vector, it's
         * multi-arity. If car is a cons/nil, check if it looks
         * like a params list (all symbols) or an arity clause. */
        if (params->as.cons.car->type == MINO_VECTOR) {
            multi_arity = 1;
        }
    }
    if (multi_arity) {
        /* Multi-arity: args is (([p1] body1...) ([p2] body2...) ...).
         * Store as: params = NULL (sentinel), body = list of
         * (params-vec . body-forms) clauses. */
        mino_val_t *clauses = mino_nil(S);
        mino_val_t *clause_tail = NULL;
        mino_val_t *rest = args;
        while (mino_is_cons(rest)) {
            mino_val_t *clause = rest->as.cons.car;
            mino_val_t *cparams;
            mino_val_t *cbody;
            mino_val_t *cell;
            if (!mino_is_cons(clause)) {
                set_error_at(S, form, "multi-arity clause must be a list");
                return NULL;
            }
            cparams = clause->as.cons.car;
            cbody   = clause->as.cons.cdr;
            if (cparams == NULL
                || (cparams->type != MINO_VECTOR
                    && !mino_is_cons(cparams)
                    && !mino_is_nil(cparams))) {
                set_error_at(S, form, "multi-arity clause must start with a parameter list");
                return NULL;
            }
            cell = mino_cons(S, mino_cons(S, cparams, cbody), mino_nil(S));
            if (clause_tail == NULL) {
                clauses = cell;
            } else {
                clause_tail->as.cons.cdr = cell;
            }
            clause_tail = cell;
            rest = rest->as.cons.cdr;
        }
        params = NULL;
        body   = clauses;
    } else {
        if (!mino_is_cons(params) && !mino_is_nil(params)
            && params->type != MINO_VECTOR) {
            set_error_at(S, form, "fn parameter list must be a list or vector");
            return NULL;
        }
        /* Validate params when given as a cons list. */
        if (mino_is_cons(params) || mino_is_nil(params)) {
            for (p = params; mino_is_cons(p); p = p->as.cons.cdr) {
                mino_val_t *name = p->as.cons.car;
                if (name == NULL || name->type != MINO_SYMBOL) {
                    set_error_at(S, form, "fn parameter must be a symbol");
                    return NULL;
                }
            }
        }
    }
    if (fn_name != NULL) {
        char nbuf[256];
        size_t nlen = fn_name->as.s.len;
        mino_env_t *fn_env;
        if (nlen >= sizeof(nbuf)) {
            set_error_at(S, form, "fn name too long");
            return NULL;
        }
        memcpy(nbuf, fn_name->as.s.data, nlen);
        nbuf[nlen] = '\0';
        fn_env = env_child(S, env);
        fn_val = make_fn(S, params, body, fn_env);
        env_bind(S, fn_env, nbuf, fn_val);
    } else {
        fn_val = make_fn(S, params, body, env);
    }
    return fn_val;
}

static mino_val_t *eval_loop(mino_state_t *S, mino_val_t *form,
                             mino_val_t *args, mino_env_t *env, int tail)
{
    mino_val_t *bindings;
    mino_val_t *body;
    mino_val_t *params      = mino_nil(S);
    mino_val_t *params_tail = NULL;
    mino_env_t *local;
    if (!mino_is_cons(args)) {
        set_error_at(S, form, "loop requires a binding form and body");
        return NULL;
    }
    bindings = args->as.cons.car;
    body     = args->as.cons.cdr;
    local = env_child(S, env);
    if (bindings != NULL && bindings->type == MINO_VECTOR) {
        /* Vector binding form: [name val name val ...] */
        size_t vlen = bindings->as.vec.len;
        size_t vi;
        mino_val_t **ptmp;
        if (vlen % 2 != 0) {
            set_error_at(S, form, "loop vector bindings must have even number of forms");
            return NULL;
        }
        /* Build a params vector of just the name symbols for recur. */
        ptmp = (mino_val_t **)gc_alloc_typed(S, GC_T_VALARR,
                    (vlen / 2) * sizeof(*ptmp));
        for (vi = 0; vi < vlen; vi += 2) {
            mino_val_t *pat = vec_nth(bindings, vi);
            mino_val_t *val = eval_value(S, vec_nth(bindings, vi + 1), local);
            if (val == NULL) return NULL;
            gc_pin(val);
            if (!bind_form(S, local, pat, val, "loop")) {
                gc_unpin(1);
                return NULL;
            }
            gc_unpin(1);
            ptmp[vi / 2] = pat;
        }
        params = mino_vector(S, ptmp, vlen / 2);
    } else if (mino_is_cons(bindings) || mino_is_nil(bindings)) {
        /* Legacy list binding form. */
        while (mino_is_cons(bindings)) {
            mino_val_t *name_form = bindings->as.cons.car;
            mino_val_t *rest_pair = bindings->as.cons.cdr;
            mino_val_t *val;
            mino_val_t *cell;
            if (!mino_is_cons(rest_pair)) {
                set_error_at(S, form, "loop binding missing value");
                return NULL;
            }
            val = eval_value(S, rest_pair->as.cons.car, local);
            if (val == NULL) return NULL;
            gc_pin(val);
            if (!bind_form(S, local, name_form, val, "loop")) {
                gc_unpin(1);
                return NULL;
            }
            gc_unpin(1);
            cell = mino_cons(S, name_form, mino_nil(S));
            if (params_tail == NULL) {
                params = cell;
            } else {
                params_tail->as.cons.cdr = cell;
            }
            params_tail = cell;
            bindings = rest_pair->as.cons.cdr;
        }
    } else {
        set_error_at(S, form, "loop bindings must be a list or vector");
        return NULL;
    }
    for (;;) {
        mino_val_t *result = eval_implicit_do_impl(S, body, local, tail);
        if (result == NULL) {
            return NULL;
        }
        if (result->type != MINO_RECUR) {
            return result;
        }
        if (!bind_params(S, local, params, result->as.recur.args,
                         "recur")) {
            return NULL;
        }
    }
}

static mino_val_t *eval_try(mino_state_t *S, mino_val_t *form,
                            mino_val_t *args, mino_env_t *env)
{
    mino_val_t *body_head = NULL;
    mino_val_t *body_tail = NULL;
    mino_val_t *catch_body = NULL;
    mino_val_t *finally_body = NULL;
    char        var_buf[256];
    int         has_catch   = 0;
    int         has_finally = 0;
    int         saved_try   = try_depth;
    int         saved_call  = call_depth;
    int         saved_trace = trace_added;
    dyn_frame_t *saved_dyn  = dyn_stack;
    volatile int         got_exception = 0;
    volatile mino_val_t *vol_result    = NULL;
    volatile mino_val_t *vol_ex        = NULL;

    /* Partition args into body forms, catch clause, finally clause. */
    {
        mino_val_t *rest = args;
        while (mino_is_cons(rest)) {
            mino_val_t *clause = rest->as.cons.car;
            if (mino_is_cons(clause)
                && sym_eq(clause->as.cons.car, "catch")) {
                /* (catch e handler...) */
                mino_val_t *cv;
                size_t      vl;
                if (!mino_is_cons(clause->as.cons.cdr)) {
                    set_error_at(S, form,
                        "catch requires a binding symbol");
                    return NULL;
                }
                cv = clause->as.cons.cdr->as.cons.car;
                if (cv == NULL || cv->type != MINO_SYMBOL) {
                    set_error_at(S, form,
                        "catch binding must be a symbol");
                    return NULL;
                }
                vl = cv->as.s.len;
                if (vl >= sizeof(var_buf)) {
                    set_error_at(S, form,
                        "catch variable name too long");
                    return NULL;
                }
                memcpy(var_buf, cv->as.s.data, vl);
                var_buf[vl] = '\0';
                catch_body = clause->as.cons.cdr->as.cons.cdr;
                has_catch = 1;
                rest = rest->as.cons.cdr;
                continue;
            }
            if (mino_is_cons(clause)
                && sym_eq(clause->as.cons.car, "finally")) {
                finally_body = clause->as.cons.cdr;
                has_finally = 1;
                rest = rest->as.cons.cdr;
                continue;
            }
            /* Body form -- append to list. */
            {
                mino_val_t *cell = mino_cons(S, clause, mino_nil(S));
                if (body_tail == NULL) {
                    body_head = cell;
                } else {
                    body_tail->as.cons.cdr = cell;
                }
                body_tail = cell;
            }
            rest = rest->as.cons.cdr;
        }
    }

    if (try_depth >= MAX_TRY_DEPTH) {
        set_error_at(S, form, "try nesting too deep");
        return NULL;
    }

    /* Phase 1: evaluate body forms. */
    try_stack[try_depth].exception = NULL;
    if (setjmp(try_stack[try_depth].buf) == 0) {
        mino_val_t *r;
        try_depth++;
        r = eval_implicit_do(S, body_head, env);
        try_depth = saved_try;
        if (r == NULL) {
            /* Fatal runtime error. */
            if (has_finally)
                eval_implicit_do(S, finally_body, env);
            return NULL;
        }
        vol_result = r;
    } else {
        /* longjmp'd from throw in body. */
        vol_ex      = try_stack[saved_try].exception;
        try_depth   = saved_try;
        call_depth  = saved_call;
        trace_added = saved_trace;
        while (dyn_stack != saved_dyn) {
            dyn_frame_t *f = dyn_stack;
            dyn_stack = f->prev;
            dyn_binding_list_free(f->bindings);
        }
        clear_error(S);
        got_exception = 1;
    }

    /* Phase 2: run catch handler if we caught an exception. */
    if (got_exception && has_catch) {
        mino_val_t *ex_val =
            vol_ex ? (mino_val_t *)vol_ex : mino_nil(S);
        mino_env_t *local  = env_child(S, env);
        env_bind(S, local, var_buf, ex_val);

        if (has_finally && try_depth < MAX_TRY_DEPTH) {
            /* Inner try frame catches re-throws from handler
             * so that finally still runs. */
            int         ic = call_depth;
            int         it = trace_added;
            int         is = try_depth; /* save before setjmp */
            dyn_frame_t *id = dyn_stack;
            try_stack[is].exception = NULL;
            if (setjmp(try_stack[is].buf) == 0) {
                mino_val_t *r;
                try_depth++;
                r = eval_implicit_do(S, catch_body, local);
                try_depth = is;
                if (r == NULL) {
                    eval_implicit_do(S, finally_body, env);
                    return NULL;
                }
                vol_result    = r;
                got_exception = 0;
            } else {
                /* Catch handler re-threw. */
                vol_ex      = try_stack[is].exception;
                try_depth   = is;
                call_depth  = ic;
                trace_added = it;
                while (dyn_stack != id) {
                    dyn_frame_t *f = dyn_stack;
                    dyn_stack = f->prev;
                    dyn_binding_list_free(f->bindings);
                }
                clear_error(S);
                /* got_exception stays 1, vol_ex updated. */
            }
        } else {
            /* No finally or nesting limit -- run catch directly. */
            mino_val_t *r =
                eval_implicit_do(S, catch_body, local);
            if (r == NULL) {
                if (has_finally)
                    eval_implicit_do(S, finally_body, env);
                return NULL;
            }
            vol_result    = r;
            got_exception = 0;
        }
    }

    /* Phase 3: run finally unconditionally. */
    if (has_finally) {
        eval_implicit_do(S, finally_body, env);
    }

    /* Phase 4: re-throw if exception was not handled. */
    if (got_exception) {
        mino_val_t *e = (mino_val_t *)vol_ex;
        if (try_depth > 0) {
            try_stack[try_depth - 1].exception = e;
            longjmp(try_stack[try_depth - 1].buf, 1);
        }
        if (e != NULL && e->type == MINO_STRING) {
            char msg[512];
            snprintf(msg, sizeof(msg),
                     "unhandled exception: %.*s",
                     (int)e->as.s.len, e->as.s.data);
            set_error(S, msg);
        } else {
            set_error(S, "unhandled exception");
        }
        return NULL;
    }

    return (mino_val_t *)vol_result;
}

static mino_val_t *eval_binding(mino_state_t *S, mino_val_t *form,
                                mino_val_t *args, mino_env_t *env)
{
    mino_val_t *pairs, *body, *result;
    dyn_frame_t frame;
    dyn_binding_t *bhead = NULL;
    if (!mino_is_cons(args)) {
        set_error_at(S, form, "binding requires a binding list and body");
        return NULL;
    }
    pairs = args->as.cons.car;
    body  = args->as.cons.cdr;
    if (pairs != NULL && pairs->type == MINO_VECTOR) {
        /* Vector binding form: [sym val sym val ...] */
        size_t vlen = pairs->as.vec.len;
        size_t vi;
        if (vlen % 2 != 0) {
            set_error_at(S, form, "binding: odd number of forms in binding vector");
            return NULL;
        }
        for (vi = 0; vi < vlen; vi += 2) {
            mino_val_t *sym_v = vec_nth(pairs, vi);
            mino_val_t *val_form = vec_nth(pairs, vi + 1);
            mino_val_t *val;
            dyn_binding_t *b;
            char nbuf[256];
            size_t nlen;
            if (sym_v == NULL || sym_v->type != MINO_SYMBOL) {
                set_error_at(S, form, "binding: names must be symbols");
                dyn_binding_list_free(bhead);
                return NULL;
            }
            nlen = sym_v->as.s.len;
            if (nlen >= sizeof(nbuf)) {
                set_error_at(S, form, "binding: name too long");
                dyn_binding_list_free(bhead);
                return NULL;
            }
            memcpy(nbuf, sym_v->as.s.data, nlen);
            nbuf[nlen] = '\0';
            val = eval(S, val_form, env);
            if (val == NULL) {
                dyn_binding_list_free(bhead);
                return NULL;
            }
            b = (dyn_binding_t *)malloc(sizeof(*b));
            if (b == NULL) {
                set_error_at(S, form, "binding: out of memory");
                dyn_binding_list_free(bhead);
                return NULL;
            }
            b->name = mino_symbol(S, nbuf)->as.s.data; /* interned */
            b->val  = val;
            b->next = bhead;
            bhead   = b;
        }
    } else if (mino_is_cons(pairs)) {
        /* Legacy list binding form: (sym val sym val ...) */
        while (pairs != NULL && pairs->type == MINO_CONS) {
            mino_val_t *sym_v, *val_form, *val;
            dyn_binding_t *b;
            char nbuf[256];
            size_t nlen;
            sym_v = pairs->as.cons.car;
            if (sym_v == NULL || sym_v->type != MINO_SYMBOL) {
                set_error_at(S, form, "binding: names must be symbols");
                dyn_binding_list_free(bhead);
                return NULL;
            }
            nlen = sym_v->as.s.len;
            if (nlen >= sizeof(nbuf)) {
                set_error_at(S, form, "binding: name too long");
                dyn_binding_list_free(bhead);
                return NULL;
            }
            memcpy(nbuf, sym_v->as.s.data, nlen);
            nbuf[nlen] = '\0';
            pairs = pairs->as.cons.cdr;
            if (pairs == NULL || pairs->type != MINO_CONS) {
                set_error_at(S, form, "binding: odd number of forms in binding list");
                dyn_binding_list_free(bhead);
                return NULL;
            }
            val_form = pairs->as.cons.car;
            pairs    = pairs->as.cons.cdr;
            val = eval(S, val_form, env);
            if (val == NULL) {
                dyn_binding_list_free(bhead);
                return NULL;
            }
            b = (dyn_binding_t *)malloc(sizeof(*b));
            if (b == NULL) {
                set_error_at(S, form, "binding: out of memory");
                dyn_binding_list_free(bhead);
                return NULL;
            }
            b->name = mino_symbol(S, nbuf)->as.s.data; /* interned */
            b->val  = val;
            b->next = bhead;
            bhead   = b;
        }
    } else {
        set_error_at(S, form, "binding requires a binding list and body");
        return NULL;
    }
    /* Push frame. */
    frame.bindings = bhead;
    frame.prev     = dyn_stack;
    dyn_stack      = &frame;
    result = eval_implicit_do(S, body, env);
    /* Pop frame. */
    dyn_stack = frame.prev;
    dyn_binding_list_free(bhead);
    return result;
}

mino_val_t *eval_impl(mino_state_t *S, mino_val_t *form, mino_env_t *env, int tail)
{
    if (limit_exceeded) {
        return NULL;
    }
    if (interrupted) {
        limit_exceeded = 1;
        set_error(S, "interrupted");
        return NULL;
    }
    if (limit_steps > 0 && ++eval_steps > limit_steps) {
        limit_exceeded = 1;
        set_error(S, "step limit exceeded");
        return NULL;
    }
    if (limit_heap > 0 && gc_bytes_alloc > limit_heap) {
        limit_exceeded = 1;
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
        eval_current_form = form;

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
                    return def_val;
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

/* Count elements in a cons list. */
static int list_len(const mino_val_t *lst)
{
    int n = 0;
    while (mino_is_cons(lst)) {
        n++;
        lst = lst->as.cons.cdr;
    }
    return n;
}

/* Count required params (excluding & rest) in a param form. Returns the
 * fixed arity count and sets *has_rest if & is present. */
static int param_arity(const mino_val_t *params, int *has_rest)
{
    int n = 0;
    *has_rest = 0;
    if (params == NULL) return 0;
    if (params->type == MINO_VECTOR) {
        size_t i;
        for (i = 0; i < params->as.vec.len; i++) {
            mino_val_t *p = vec_nth(params, i);
            if (sym_eq(p, "&")) {
                *has_rest = 1;
                return n;
            }
            if (kw_eq(p, "as")) {
                i++; /* skip the :as symbol */
                continue;
            }
            n++;
        }
        return n;
    }
    /* Cons list params. */
    while (mino_is_cons(params)) {
        mino_val_t *p = params->as.cons.car;
        if (sym_eq(p, "&")) {
            *has_rest = 1;
            return n;
        }
        n++;
        params = params->as.cons.cdr;
    }
    return n;
}

/* For a multi-arity fn (params == NULL, body = list of (params . body)
 * clauses), find the clause matching the given arg count. */
static mino_val_t *find_arity_clause(mino_state_t *S, mino_val_t *clauses,
                                     int argc)
{
    (void)S;
    mino_val_t *rest = clauses;
    mino_val_t *variadic_match = NULL;
    while (mino_is_cons(rest)) {
        mino_val_t *clause  = rest->as.cons.car;
        mino_val_t *cparams = clause->as.cons.car;
        int has_rest;
        int fixed = param_arity(cparams, &has_rest);
        if (!has_rest && argc == fixed) return clause;
        if (has_rest && argc >= fixed)  variadic_match = clause;
        rest = rest->as.cons.cdr;
    }
    if (variadic_match) return variadic_match;
    return NULL;
}

/*
 * Invoke `fn` with an already-evaluated argument list. Used both by the
 * evaluator's function-call path and by primitives (e.g. update) that
 * need to call back into user-defined code.
 */
mino_val_t *apply_callable(mino_state_t *S, mino_val_t *fn, mino_val_t *args,
                           mino_env_t *env)
{
    if (fn == NULL) {
        set_error(S, "cannot apply null");
        return NULL;
    }
    if (fn->type == MINO_PRIM) {
        const char *file = NULL;
        int         line = 0;
        mino_val_t *result;
        if (eval_current_form != NULL
            && eval_current_form->type == MINO_CONS) {
            file = eval_current_form->as.cons.file;
            line = eval_current_form->as.cons.line;
        }
        push_frame(S, fn->as.prim.name, file, line);
        result = fn->as.prim.fn(S, args, env);
        if (result == NULL) {
            return NULL; /* leave frame for trace */
        }
        pop_frame(S);
        return result;
    }
    if (fn->type == MINO_FN || fn->type == MINO_MACRO) {
        const char *tag       = fn->type == MINO_MACRO ? "macro" : "fn";
        mino_val_t *cur_params = fn->as.fn.params;
        mino_val_t *cur_body   = fn->as.fn.body;
        mino_env_t *local     = env_child(S, fn->as.fn.env);
        mino_val_t *call_args = args;
        const char *file      = NULL;
        int         line      = 0;
        mino_val_t *result;
        if (eval_current_form != NULL
            && eval_current_form->type == MINO_CONS) {
            file = eval_current_form->as.cons.file;
            line = eval_current_form->as.cons.line;
        }
        push_frame(S, tag, file, line);
        /* Multi-arity dispatch: params == NULL means body is a clause list. */
        if (cur_params == NULL) {
            int argc = list_len(call_args);
            mino_val_t *clause = find_arity_clause(S, cur_body, argc);
            if (clause == NULL) {
                char msg[96];
                snprintf(msg, sizeof(msg), "no matching arity for %d args", argc);
                set_error(S, msg);
                return NULL;
            }
            cur_params = clause->as.cons.car;
            cur_body   = clause->as.cons.cdr;
        }
        for (;;) {
            if (!bind_params(S, local, cur_params, call_args, tag)) {
                return NULL; /* leave frame for trace */
            }
            result = eval_implicit_do_impl(S, cur_body, local, 1);
            if (result == NULL) {
                return NULL; /* leave frame for trace */
            }
            if (result->type == MINO_RECUR) {
                /* Self-recursion: rebind params and loop.
                 * For multi-arity, re-dispatch on new arg count. */
                call_args = result->as.recur.args;
                if (fn->as.fn.params == NULL) {
                    int argc = list_len(call_args);
                    mino_val_t *clause = find_arity_clause(S, fn->as.fn.body, argc);
                    if (clause == NULL) {
                        char msg[96];
                        snprintf(msg, sizeof(msg), "no matching arity for %d args in recur", argc);
                        set_error(S, msg);
                        return NULL;
                    }
                    cur_params = clause->as.cons.car;
                    cur_body   = clause->as.cons.cdr;
                    local      = env_child(S, fn->as.fn.env);
                }
                continue;
            }
            if (result->type == MINO_TAIL_CALL) {
                /* Proper tail call: switch to the target function. */
                fn        = result->as.tail_call.fn;
                call_args = result->as.tail_call.args;
                cur_params = fn->as.fn.params;
                cur_body   = fn->as.fn.body;
                local     = env_child(S, fn->as.fn.env);
                if (cur_params == NULL) {
                    int argc = list_len(call_args);
                    mino_val_t *clause = find_arity_clause(S, cur_body, argc);
                    if (clause == NULL) {
                        char msg[96];
                        snprintf(msg, sizeof(msg), "no matching arity for %d args", argc);
                        set_error(S, msg);
                        return NULL;
                    }
                    cur_params = clause->as.cons.car;
                    cur_body   = clause->as.cons.cdr;
                }
                continue;
            }
            pop_frame(S);
            return result;
        }
    }
    {
        char msg[128];
        snprintf(msg, sizeof(msg), "not a function (got %s)",
                 type_tag_str(fn));
        set_error(S, msg);
    }
    return NULL;
}


