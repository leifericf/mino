/*
 * eval_special_fn.c -- fn special form, arity dispatch, apply_callable.
 *
 * Extracted from eval_special.c. No behavior change.
 */

#include "eval_special_internal.h"

/* Build the clause list for a multi-arity fn or defmacro.
 * `arity_list` is the cons list of arity clauses: (([p] b...) ([p q] b...) ...).
 * Returns the clause list on success, NULL on error. */
mino_val_t *build_multi_arity_clauses(mino_state_t *S, mino_val_t *form,
                                      mino_val_t *arity_list,
                                      const char *diag_code,
                                      const char *label)
{
    mino_val_t *clauses     = mino_nil(S);
    mino_val_t *clause_tail = NULL;
    mino_val_t *rest        = arity_list;
    while (mino_is_cons(rest)) {
        mino_val_t *clause = rest->as.cons.car;
        mino_val_t *cparams;
        mino_val_t *cbody;
        mino_val_t *cell;
        char        msg[128];
        if (!mino_is_cons(clause)) {
            snprintf(msg, sizeof(msg),
                     "multi-arity %s clause must be a list", label);
            set_eval_diag(S, form, "syntax", diag_code, msg);
            return NULL;
        }
        cparams = clause->as.cons.car;
        cbody   = clause->as.cons.cdr;
        if (cparams == NULL
            || (cparams->type != MINO_VECTOR
                && !mino_is_cons(cparams)
                && !mino_is_nil(cparams))) {
            snprintf(msg, sizeof(msg),
                     "multi-arity %s clause must start with a parameter list",
                     label);
            set_eval_diag(S, form, "syntax", diag_code, msg);
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
    return clauses;
}

mino_val_t *eval_fn(mino_state_t *S, mino_val_t *form,
                    mino_val_t *args, mino_env_t *env)
{
    mino_val_t *fn_name = NULL;
    mino_val_t *params;
    mino_val_t *body;
    mino_val_t *p;
    mino_val_t *fn_val;
    int         multi_arity = 0;
    if (!mino_is_cons(args)) {
        set_eval_diag(S, form, "syntax", "MSY002", "fn requires a parameter list");
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
        mino_val_t *clauses = build_multi_arity_clauses(
            S, form, args, "MSY002", "fn");
        if (clauses == NULL) { return NULL; }
        params = NULL;
        body   = clauses;
    } else {
        if (!mino_is_cons(params) && !mino_is_nil(params)
            && params->type != MINO_VECTOR) {
            set_eval_diag(S, form, "syntax", "MSY002", "fn parameter list must be a list or vector");
            return NULL;
        }
        /* Validate params when given as a cons list. */
        if (mino_is_cons(params) || mino_is_nil(params)) {
            for (p = params; mino_is_cons(p); p = p->as.cons.cdr) {
                mino_val_t *name = p->as.cons.car;
                if (name == NULL || name->type != MINO_SYMBOL) {
                    set_eval_diag(S, form, "syntax", "MSY002", "fn parameter must be a symbol");
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
            set_eval_diag(S, form, "syntax", "MSY002", "fn name too long");
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

/* --- Arity dispatch helpers ---------------------------------------------- */

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
        set_eval_diag(S, S->eval_current_form, "eval/type", "MTY002", "cannot apply null");
        return NULL;
    }
    if (fn->type == MINO_PRIM) {
        const char *file = NULL;
        int         line = 0;
        int         col  = 0;
        mino_val_t *result;
        if (S->eval_current_form != NULL
            && S->eval_current_form->type == MINO_CONS) {
            file = S->eval_current_form->as.cons.file;
            line = S->eval_current_form->as.cons.line;
            col  = S->eval_current_form->as.cons.column;
        }
        push_frame(S, fn->as.prim.name, file, line, col);
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
        int         col       = 0;
        mino_val_t *result;
        if (S->eval_current_form != NULL
            && S->eval_current_form->type == MINO_CONS) {
            file = S->eval_current_form->as.cons.file;
            line = S->eval_current_form->as.cons.line;
            col  = S->eval_current_form->as.cons.column;
        }
        push_frame(S, tag, file, line, col);
        /* Multi-arity dispatch: params == NULL means body is a clause list. */
        if (cur_params == NULL) {
            int argc = list_len(call_args);
            mino_val_t *clause = find_arity_clause(S, cur_body, argc);
            if (clause == NULL) {
                char msg[96];
                snprintf(msg, sizeof(msg), "no matching arity for %d args", argc);
                set_eval_diag(S, S->eval_current_form, "eval/arity", "MAR002", msg);
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
                        set_eval_diag(S, S->eval_current_form, "eval/arity", "MAR002", msg);
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
                        set_eval_diag(S, S->eval_current_form, "eval/arity", "MAR002", msg);
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
    if (fn->type == MINO_KEYWORD) {
        /* Keyword as function in higher-order context: (:k m) => (get m :k). */
        int         nargs = 0;
        mino_val_t *tmp;
        for (tmp = args; mino_is_cons(tmp); tmp = tmp->as.cons.cdr)
            nargs++;
        if (nargs < 1 || nargs > 2) {
            set_eval_diag(S, S->eval_current_form, "eval/arity", "MAR001", "keyword as function takes 1 or 2 arguments");
            return NULL;
        }
        {
            mino_val_t *coll    = args->as.cons.car;
            mino_val_t *def_val = nargs == 2
                ? args->as.cons.cdr->as.cons.car
                : mino_nil(S);
            if (coll != NULL && coll->type == MINO_MAP) {
                mino_val_t *v = map_get_val(coll, fn);
                return v == NULL ? def_val : v;
            }
            if (coll != NULL && coll->type == MINO_SORTED_MAP) {
                mino_val_t *v = rb_get(S, coll->as.sorted.root, fn,
                                        coll->as.sorted.comparator);
                return v == NULL ? def_val : v;
            }
            return def_val;
        }
    }
    if (fn->type == MINO_MAP) {
        /* Map as function in higher-order context: ({:a 1} :k). */
        int         nargs = 0;
        mino_val_t *tmp;
        for (tmp = args; mino_is_cons(tmp); tmp = tmp->as.cons.cdr)
            nargs++;
        if (nargs < 1 || nargs > 2) {
            set_eval_diag(S, S->eval_current_form, "eval/arity", "MAR001", "map as function takes 1 or 2 arguments");
            return NULL;
        }
        {
            mino_val_t *key     = args->as.cons.car;
            mino_val_t *def_val = nargs == 2
                ? args->as.cons.cdr->as.cons.car
                : mino_nil(S);
            mino_val_t *v = map_get_val(fn, key);
            return v == NULL ? def_val : v;
        }
    }
    if (fn->type == MINO_VECTOR) {
        /* Vector as function in higher-order context: ([1 2 3] 0). */
        int         nargs = 0;
        mino_val_t *tmp;
        for (tmp = args; mino_is_cons(tmp); tmp = tmp->as.cons.cdr)
            nargs++;
        if (nargs != 1) {
            set_eval_diag(S, S->eval_current_form, "eval/arity", "MAR001", "vector as function takes 1 argument");
            return NULL;
        }
        {
            mino_val_t *idx = args->as.cons.car;
            long long i;
            if (idx == NULL || idx->type != MINO_INT) {
                set_eval_diag(S, S->eval_current_form, "eval/type", "MTY001", "vector index must be an integer");
                return NULL;
            }
            i = idx->as.i;
            if (i < 0 || (size_t)i >= fn->as.vec.len) {
                set_eval_diag(S, S->eval_current_form, "eval/bounds", "MBD001", "vector index out of bounds");
                return NULL;
            }
            return vec_nth(fn, (size_t)i);
        }
    }
    if (fn->type == MINO_SET) {
        /* Set as function in higher-order context: (#{:a :b} :a) => :a. */
        int         nargs = 0;
        mino_val_t *tmp;
        for (tmp = args; mino_is_cons(tmp); tmp = tmp->as.cons.cdr)
            nargs++;
        if (nargs != 1) {
            set_eval_diag(S, S->eval_current_form, "eval/arity", "MAR001", "set as function takes 1 argument");
            return NULL;
        }
        {
            mino_val_t *key = args->as.cons.car;
            uint32_t h = hash_val(key);
            return hamt_get(fn->as.set.root, key, h, 0u) != NULL
                ? key : mino_nil(S);
        }
    }
    if (fn->type == MINO_SORTED_MAP) {
        int         nargs = 0;
        mino_val_t *tmp;
        for (tmp = args; mino_is_cons(tmp); tmp = tmp->as.cons.cdr)
            nargs++;
        if (nargs < 1 || nargs > 2) {
            set_eval_diag(S, S->eval_current_form, "eval/arity", "MAR001", "sorted-map as function takes 1 or 2 arguments");
            return NULL;
        }
        {
            mino_val_t *key     = args->as.cons.car;
            mino_val_t *def_val = nargs == 2
                ? args->as.cons.cdr->as.cons.car
                : mino_nil(S);
            mino_val_t *v = rb_get(S, fn->as.sorted.root, key,
                                    fn->as.sorted.comparator);
            return v == NULL ? def_val : v;
        }
    }
    if (fn->type == MINO_SORTED_SET) {
        int         nargs = 0;
        mino_val_t *tmp;
        for (tmp = args; mino_is_cons(tmp); tmp = tmp->as.cons.cdr)
            nargs++;
        if (nargs != 1) {
            set_eval_diag(S, S->eval_current_form, "eval/arity", "MAR001", "sorted-set as function takes 1 argument");
            return NULL;
        }
        {
            mino_val_t *key = args->as.cons.car;
            return rb_contains(S, fn->as.sorted.root, key,
                                fn->as.sorted.comparator)
                ? key : mino_nil(S);
        }
    }
    {
        char msg[128];
        snprintf(msg, sizeof(msg), "not a function (got %s)",
                 type_tag_str(fn));
        set_eval_diag(S, S->eval_current_form, "eval/type", "MTY002", msg);
    }
    return NULL;
}
