/*
 * fn.c -- fn special form, arity dispatch, apply_callable.
 */

#include "eval/special_internal.h"
#include "eval/bc/internal.h"

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
            || (mino_type_of(cparams) != MINO_VECTOR
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
            mino_cons_cdr_set(S, clause_tail, cell);
        }
        clause_tail = cell;
        rest = rest->as.cons.cdr;
    }
    return clauses;
}

mino_val_t *eval_fn(mino_state_t *S, mino_val_t *form,
                    mino_val_t *args, mino_env_t *env, int tail)
{
    mino_val_t *fn_name = NULL;
    mino_val_t *params;
    mino_val_t *body;
    mino_val_t *p;
    mino_val_t *fn_val;
    (void)tail;
    int         multi_arity = 0;
    if (!mino_is_cons(args)) {
        set_eval_diag(S, form, "syntax", "MSY002", "fn requires a parameter list");
        return NULL;
    }
    /* Optional name: (fn name (...) body) or (fn name ([x] ...) ([x y] ...)) */
    if (args->as.cons.car != NULL
        && mino_type_of(args->as.cons.car) == MINO_SYMBOL
        && mino_is_cons(args->as.cons.cdr)) {
        mino_val_t *after = args->as.cons.cdr->as.cons.car;
        if (after != NULL
            && (mino_is_cons(after) || mino_is_nil(after)
                || mino_type_of(after) == MINO_VECTOR)) {
            fn_name = args->as.cons.car;
            args    = args->as.cons.cdr;
        }
    }
    params = args->as.cons.car;
    body   = args->as.cons.cdr;
    /* Detect multi-arity: (fn ([x] ...) ([x y] ...))
     * The first arg is a list whose car is a vector or list. */
    if (mino_is_cons(params) && params->as.cons.car != NULL
        && (mino_type_of(params->as.cons.car) == MINO_VECTOR
            || (mino_is_cons(params->as.cons.car)
                || mino_is_nil(params->as.cons.car)))) {
        /* Could be multi-arity OR single-arity with list params.
         * Multi-arity: each clause is (params-vec . body-forms).
         * Disambiguate: if car of first arg is a vector, it's
         * multi-arity. If car is a cons/nil, check if it looks
         * like a params list (all symbols) or an arity clause. */
        if (mino_type_of(params->as.cons.car) == MINO_VECTOR) {
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
            && mino_type_of(params) != MINO_VECTOR) {
            set_eval_diag(S, form, "syntax", "MSY002", "fn parameter list must be a list or vector");
            return NULL;
        }
        /* Validate params when given as a cons list. */
        if (mino_is_cons(params) || mino_is_nil(params)) {
            for (p = params; mino_is_cons(p); p = p->as.cons.cdr) {
                mino_val_t *name = p->as.cons.car;
                if (name == NULL || mino_type_of(name) != MINO_SYMBOL) {
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
    if (mino_type_of(params) == MINO_VECTOR) {
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
/* dispatch_multi_arity -- when a fn was defined with multiple arity
 * clauses, fn->as.fn.params is NULL and fn->as.fn.body is a clause
 * list. Pick the clause matching the current argc, update *params and
 * *body to the chosen clause's pair, and return 0. On no-match, set the
 * diagnostic with the supplied context tag (e.g. "" for the call entry,
 * " in recur" for a recur backward branch) and return -1. */
static int dispatch_multi_arity(mino_state_t *S, mino_val_t *clauses,
                                mino_val_t *call_args, const char *ctx_suffix,
                                mino_val_t **out_params,
                                mino_val_t **out_body)
{
    int         argc   = list_len(call_args);
    mino_val_t *clause = find_arity_clause(S, clauses, argc);
    if (clause == NULL) {
        char        msg[256];
        char        name_buf[128] = {0};
        mino_val_t *cur = mino_current_ctx(S)->eval_current_form;
        /* Name the callee from the in-progress (callee args...) cons
         * so the user sees which fn / macro mismatched. */
        if (cur != NULL && mino_is_cons(cur)) {
            mino_val_t *head = cur->as.cons.car;
            if (head != NULL && mino_type_of(head) == MINO_SYMBOL
                && head->as.s.len > 0
                && head->as.s.len < sizeof(name_buf) - 4) {
                snprintf(name_buf, sizeof(name_buf), " `%.*s`",
                         (int)head->as.s.len, head->as.s.data);
            }
        }
        snprintf(msg, sizeof(msg),
                 "no matching arity%s for %d args%s",
                 name_buf, argc, ctx_suffix);
        set_eval_diag(S, mino_current_ctx(S)->eval_current_form,
                      "eval/arity", "MAR002", msg);
        return -1;
    }
    *out_params = clause->as.cons.car;
    *out_body   = clause->as.cons.cdr;
    return 0;
}

mino_val_t *apply_callable(mino_state_t *S, mino_val_t *fn, mino_val_t *args,
                           mino_env_t *env)
{
    if (fn == NULL) {
        set_eval_diag(S, mino_current_ctx(S)->eval_current_form, "eval/type", "MTY002", "cannot apply null");
        return NULL;
    }
    if (mino_type_of(fn) == MINO_VAR) {
        if (!fn->as.var.bound) {
            char msg[256];
            snprintf(msg, sizeof(msg), "var is unbound: %s/%s",
                     fn->as.var.ns ? fn->as.var.ns : "?",
                     fn->as.var.sym ? fn->as.var.sym : "?");
            set_eval_diag(S, mino_current_ctx(S)->eval_current_form, "eval/type", "MTY002", msg);
            return NULL;
        }
        fn = fn->as.var.root;
        if (fn == NULL) {
            set_eval_diag(S, mino_current_ctx(S)->eval_current_form, "eval/type", "MTY002",
                "var has nil root");
            return NULL;
        }
    }
    if (mino_type_of(fn) == MINO_PRIM) {
        const char *file = NULL;
        int         line = 0;
        int         col  = 0;
        mino_val_t *result;
        if (mino_current_ctx(S)->eval_current_form != NULL
            && mino_type_of(mino_current_ctx(S)->eval_current_form) == MINO_CONS) {
            file = mino_current_ctx(S)->eval_current_form->as.cons.file;
            line = mino_current_ctx(S)->eval_current_form->as.cons.line;
            col  = mino_current_ctx(S)->eval_current_form->as.cons.column;
        }
        push_frame(S, fn->as.prim.name, file, line, col);
        if (fn->as.prim.fn2 != NULL) {
            /* argv ABI: walk the cons spine into a stack scratch array.
             * Conservative stack scan keeps the slots rooted across
             * any GC the prim itself triggers. Spillover beyond the
             * scratch capacity falls through to a heap VALARR so
             * variadic argv-prims still work; the common 1-3 arg case
             * never touches the heap. */
            mino_val_t  *scratch[16];
            mino_val_t **argv = scratch;
            int          argc = 0;
            int          cap  = (int)(sizeof(scratch) / sizeof(scratch[0]));
            mino_val_t  *cur  = args;
            mino_val_t **heap = NULL;
            while (mino_is_cons(cur)) {
                if (argc == cap) {
                    int new_cap = cap * 2;
                    mino_val_t **grown = (mino_val_t **)gc_alloc_typed(
                        S, GC_T_VALARR, (size_t)new_cap * sizeof(*grown));
                    memcpy(grown, argv, (size_t)argc * sizeof(*argv));
                    argv = grown;
                    cap  = new_cap;
                    heap = grown;
                }
                argv[argc++] = cur->as.cons.car;
                cur = cur->as.cons.cdr;
            }
            result = fn->as.prim.fn2(S, argv, argc, env);
            (void)heap;
        } else {
            result = fn->as.prim.fn(S, args, env);
        }
        if (result == NULL) {
            return NULL; /* leave frame for trace */
        }
        pop_frame(S);
        return result;
    }
    if (mino_type_of(fn) == MINO_FN || mino_type_of(fn) == MINO_MACRO) {
        const char *tag       = mino_type_of(fn) == MINO_MACRO ? "macro" : "fn";
        /* Lazy compile-on-first-call. Macros stay tree-walked; their
         * call frequency is low and the bc compiler's macro-body
         * handling lives in Phase 2. Plain fns get one compile attempt
         * the first time they're invoked; the attempt either populates
         * fn->as.fn.bc with a runnable program or leaves the
         * declined sentinel so the next call skips the retry. */
        if (mino_type_of(fn) == MINO_FN && fn->as.fn.bc == NULL) {
            (void)mino_bc_compile_fn(S, fn);
        }
        /* Stale-fold check. Literal-arg pure-fn fold caches a folded
         * result in the const pool; if the compiled fn observed any
         * such fold (has_folds) AND the global IC generation has
         * bumped since the compile (a `def` / `ns-unmap` / `set!` of
         * SOME var has fired in between), one of the deps may now
         * resolve differently. Drop the bc back to NULL so the next
         * compile observes the current bindings. The recompile fires
         * lazily on the very next call, so we don't pay the cost on
         * fns that aren't reached again. */
        if (mino_type_of(fn) == MINO_FN
            && fn->as.fn.bc != NULL
            && fn->as.fn.bc != &mino_bc_declined
            && fn->as.fn.bc->has_folds
            && fn->as.fn.bc->compile_ic_gen != S->ic_gen) {
            fn->as.fn.bc = NULL;
            (void)mino_bc_compile_fn(S, fn);
        }
        mino_bc_check_require(S, fn);
        if (mino_type_of(fn) == MINO_FN && MINO_BC_RUNNABLE(fn)) {
            /* argv ABI: walk the cons spine into a stack scratch array.
             * The slots are kept alive across any GC the body triggers
             * because the conservative stack scan covers this frame
             * AND the bc register stack is a GC root once mino_bc_run
             * copies the values in. */
            mino_val_t  *scratch[16];
            mino_val_t **argv = scratch;
            int          argc = 0;
            int          cap  = (int)(sizeof(scratch) / sizeof(scratch[0]));
            mino_val_t  *cur  = args;
            const char  *file = NULL;
            int          line = 0;
            int          col  = 0;
            const char  *saved_ns      = S->current_ns;
            const char  *saved_ambient = S->fn_ambient_ns;
            mino_val_t  *result;
            while (mino_is_cons(cur)) {
                if (argc == cap) {
                    int new_cap = cap * 2;
                    mino_val_t **grown = (mino_val_t **)gc_alloc_typed(
                        S, GC_T_VALARR, (size_t)new_cap * sizeof(*grown));
                    if (grown == NULL) return NULL;
                    memcpy(grown, argv, (size_t)argc * sizeof(*argv));
                    argv = grown;
                    cap  = new_cap;
                }
                argv[argc++] = cur->as.cons.car;
                cur = cur->as.cons.cdr;
            }
            if (fn->as.fn.defining_ns != NULL) {
                S->current_ns    = fn->as.fn.defining_ns;
                S->fn_ambient_ns = fn->as.fn.defining_ns;
            }
            if (mino_current_ctx(S)->eval_current_form != NULL
                && mino_type_of(mino_current_ctx(S)->eval_current_form) == MINO_CONS) {
                file = mino_current_ctx(S)->eval_current_form->as.cons.file;
                line = mino_current_ctx(S)->eval_current_form->as.cons.line;
                col  = mino_current_ctx(S)->eval_current_form->as.cons.column;
            }
            push_frame(S, tag, file, line, col);
            /* Trampoline loop: re-enter mino_bc_run as long as the body
             * returns a MINO_TAIL_CALL sentinel. When the tail-call
             * target is itself a bc-compatible MINO_FN, we stay in
             * the bc world. When it isn't, we hand off to
             * apply_callable so a non-bc callee runs through the
             * regular dispatch. This keeps tail recursion flat across
             * compiled fns without growing the C stack. */
            for (;;) {
                result = mino_bc_run(S, fn, argv, argc, fn->as.fn.env);
                if (result == NULL) {
                    S->current_ns    = saved_ns;
                    S->fn_ambient_ns = saved_ambient;
                    return NULL;
                }
                if (mino_type_of(result) != MINO_TAIL_CALL) break;
                mino_val_t *next_fn   = result->as.tail_call.fn;
                mino_val_t *next_args = result->as.tail_call.args;
                /* Lazy compile the new target if it's a fresh MINO_FN. */
                if (next_fn != NULL && mino_type_of(next_fn) == MINO_FN
                    && next_fn->as.fn.bc == NULL) {
                    (void)mino_bc_compile_fn(S, next_fn);
                }
                if (next_fn != NULL && mino_type_of(next_fn) == MINO_FN
                    && MINO_BC_RUNNABLE(next_fn)
                    && next_fn->as.fn.params != NULL) {
                    /* Rebuild argv from the new args list. */
                    argc = 0;
                    cur  = next_args;
                    while (mino_is_cons(cur)) {
                        if (argc == cap) {
                            int new_cap = cap * 2;
                            mino_val_t **grown = (mino_val_t **)gc_alloc_typed(
                                S, GC_T_VALARR,
                                (size_t)new_cap * sizeof(*grown));
                            if (grown == NULL) {
                                S->current_ns    = saved_ns;
                                S->fn_ambient_ns = saved_ambient;
                                return NULL;
                            }
                            memcpy(grown, argv,
                                   (size_t)argc * sizeof(*argv));
                            argv = grown;
                            cap  = new_cap;
                        }
                        argv[argc++] = cur->as.cons.car;
                        cur = cur->as.cons.cdr;
                    }
                    fn = next_fn;
                    if (fn->as.fn.defining_ns != NULL) {
                        S->current_ns    = fn->as.fn.defining_ns;
                        S->fn_ambient_ns = fn->as.fn.defining_ns;
                    }
                    continue;
                }
                /* Non-bc target: pop our frame and hand off. */
                pop_frame(S);
                S->current_ns    = saved_ns;
                S->fn_ambient_ns = saved_ambient;
                return apply_callable(S, next_fn, next_args, env);
            }
            pop_frame(S);
            S->current_ns    = saved_ns;
            S->fn_ambient_ns = saved_ambient;
            return result;
        }
        mino_val_t *cur_params = fn->as.fn.params;
        mino_val_t *cur_body   = fn->as.fn.body;
        mino_env_t *local     = env_child(S, fn->as.fn.env);
        mino_val_t *call_args = args;
        const char *file      = NULL;
        int         line      = 0;
        int         col       = 0;
        const char *saved_ns      = S->current_ns;
        const char *saved_ambient = S->fn_ambient_ns;
        mino_val_t *result;
        if (mino_type_of(fn) == MINO_MACRO) {
            env_bind(S, local, "&env", mino_nil(S));
        }
        /* Closures resolve free unqualified vars in the namespace they
         * were created in. Swap current_ns for the body so def/etc land
         * in the right place by default; record the same ns as the
         * "ambient" so eval_symbol can still find these bindings even
         * after the body itself mutates current_ns via (ns ...) or
         * (in-ns ...).
         *
         * Macros are different: their body runs at the *caller's*
         * macroexpansion context, so *ns* and (resolve ...) must see
         * the caller's namespace. Set only fn_ambient_ns for macros so
         * helper-symbol lookups fall back to the defining ns without
         * shifting current_ns. */
        if (fn->as.fn.defining_ns != NULL) {
            if (mino_type_of(fn) == MINO_MACRO) {
                S->fn_ambient_ns = fn->as.fn.defining_ns;
            } else {
                S->current_ns    = fn->as.fn.defining_ns;
                S->fn_ambient_ns = fn->as.fn.defining_ns;
            }
        }
        if (mino_current_ctx(S)->eval_current_form != NULL
            && mino_type_of(mino_current_ctx(S)->eval_current_form) == MINO_CONS) {
            file = mino_current_ctx(S)->eval_current_form->as.cons.file;
            line = mino_current_ctx(S)->eval_current_form->as.cons.line;
            col  = mino_current_ctx(S)->eval_current_form->as.cons.column;
        }
        push_frame(S, tag, file, line, col);
        /* Multi-arity dispatch: params == NULL means body is a clause list. */
        if (cur_params == NULL
            && dispatch_multi_arity(S, cur_body, call_args, "",
                                    &cur_params, &cur_body) != 0) {
            S->current_ns    = saved_ns;
            S->fn_ambient_ns = saved_ambient;
            return NULL;
        }
        for (;;) {
            int simple_path = 0;
            if (cur_params != NULL && cur_params == fn->as.fn.params) {
                /* Lazy shape detection on the per-fn cache. dispatch_multi_arity
                 * yields per-clause params that are not the cached one; those
                 * walks fall through to the general bind_params. */
                if (fn->as.fn.shape == 0) {
                    fn->as.fn.shape =
                        fn_params_simple_shape(cur_params) ? 1 : -1;
                }
                simple_path = (fn->as.fn.shape == 1);
            }
            if (simple_path) {
                if (!bind_simple_params(S, local, cur_params, call_args, tag)) {
                    S->current_ns    = saved_ns;
                    S->fn_ambient_ns = saved_ambient;
                    return NULL;
                }
            } else if (!bind_params(S, local, cur_params, call_args, tag)) {
                S->current_ns    = saved_ns;
                S->fn_ambient_ns = saved_ambient;
                return NULL; /* leave frame for trace */
            }
            result = eval_implicit_do_impl(S, cur_body, local, 1);
            if (result == NULL) {
                S->current_ns    = saved_ns;
                S->fn_ambient_ns = saved_ambient;
                return NULL; /* leave frame for trace */
            }
            if (mino_type_of(result) == MINO_RECUR) {
                /* Self-recursion: rebind params and loop.
                 * For multi-arity, re-dispatch on new arg count. */
                call_args = result->as.recur.args;
                if (fn->as.fn.params == NULL) {
                    mino_val_t *prev_params = cur_params;
                    if (dispatch_multi_arity(S, fn->as.fn.body, call_args,
                                             " in recur",
                                             &cur_params, &cur_body) != 0) {
                        S->current_ns = saved_ns;
                        return NULL;
                    }
                    /* Reuse `local` only when the recur lands on the
                     * same clause: the binding slots line up with the
                     * existing env and bind_params will overwrite them
                     * in place. A different clause may have a
                     * different param shape (different arity, rest
                     * arg, destructuring), so allocate a fresh env. */
                    if (cur_params != prev_params)
                        local = env_child(S, fn->as.fn.env);
                }
                /* Safepoint poll at the fn-self-recur backward
                 * branch — same rationale as the loop trampoline in
                 * eval/bindings.c: tight tail-recursive bodies skip
                 * eval_impl entry between iterations. */
                mino_safepoint_poll(S);
                continue;
            }
            if (mino_type_of(result) == MINO_TAIL_CALL) {
                /* Proper tail call: switch to the target function. */
                mino_val_t *new_fn = result->as.tail_call.fn;
                call_args = result->as.tail_call.args;
                if (new_fn == fn && fn->as.fn.params != NULL) {
                    /* Self tail call, single-arity: reuse local env; the
                     * loop's bind_params will update the existing param
                     * slots in place without allocating a fresh frame. */
                    continue;
                }
                fn        = new_fn;
                cur_params = fn->as.fn.params;
                cur_body   = fn->as.fn.body;
                local     = env_child(S, fn->as.fn.env);
                /* Tail-call to a different fn: switch to its defining ns
                 * so its body's free vars resolve correctly. */
                if (mino_type_of(fn) == MINO_FN && fn->as.fn.defining_ns != NULL) {
                    S->current_ns    = fn->as.fn.defining_ns;
                    S->fn_ambient_ns = fn->as.fn.defining_ns;
                }
                if (cur_params == NULL
                    && dispatch_multi_arity(S, cur_body, call_args, "",
                                            &cur_params, &cur_body) != 0) {
                    S->current_ns = saved_ns;
                    return NULL;
                }
                continue;
            }
            pop_frame(S);
            S->current_ns    = saved_ns;
            S->fn_ambient_ns = saved_ambient;
            return result;
        }
    }
    return apply_non_fn_callable(S, fn, args, mino_current_ctx(S)->eval_current_form);
}

/* Helper: build a cons-spine list from argv[0..argc). Used by the slow
 * paths of apply_callable_argv that need to delegate back to
 * apply_callable's cons-list-based dispatch. Returns NULL on alloc
 * failure. */
static mino_val_t *argv_to_cons(mino_state_t *S, mino_val_t **argv, int argc)
{
    mino_val_t *list = mino_nil(S);
    if (list == NULL) return NULL;
    for (int i = argc - 1; i >= 0; i--) {
        mino_val_t *cell = mino_cons(S, argv[i], list);
        if (cell == NULL) return NULL;
        list = cell;
    }
    return list;
}

mino_val_t *apply_callable_argv(mino_state_t *S, mino_val_t *fn,
                                mino_val_t **argv, int argc,
                                mino_env_t *env)
{
    if (fn == NULL) {
        set_eval_diag(S, mino_current_ctx(S)->eval_current_form,
                      "eval/type", "MTY002", "cannot apply null");
        return NULL;
    }
    /* Var deref. A var bound to a callable just unwraps; matches
     * apply_callable's first action. */
    if (mino_type_of(fn) == MINO_VAR) {
        if (!fn->as.var.bound) {
            char msg[256];
            snprintf(msg, sizeof(msg), "var is unbound: %s/%s",
                     fn->as.var.ns ? fn->as.var.ns : "?",
                     fn->as.var.sym ? fn->as.var.sym : "?");
            set_eval_diag(S, mino_current_ctx(S)->eval_current_form,
                          "eval/type", "MTY002", msg);
            return NULL;
        }
        fn = fn->as.var.root;
        if (fn == NULL) {
            set_eval_diag(S, mino_current_ctx(S)->eval_current_form,
                          "eval/type", "MTY002", "var has nil root");
            return NULL;
        }
    }

    /* Fast path #1: argv-ABI prim. The hot case for arithmetic, IO,
     * and collection prims after the install_stdlib migration. No
     * cons-spine traffic. */
    if (mino_type_of(fn) == MINO_PRIM && fn->as.prim.fn2 != NULL) {
        const char *file = NULL;
        int         line = 0;
        int         col  = 0;
        mino_val_t *result;
        if (mino_current_ctx(S)->eval_current_form != NULL
            && mino_type_of(mino_current_ctx(S)->eval_current_form) == MINO_CONS) {
            file = mino_current_ctx(S)->eval_current_form->as.cons.file;
            line = mino_current_ctx(S)->eval_current_form->as.cons.line;
            col  = mino_current_ctx(S)->eval_current_form->as.cons.column;
        }
        push_frame(S, fn->as.prim.name, file, line, col);
        result = fn->as.prim.fn2(S, argv, argc, env);
        if (result == NULL) return NULL; /* leave frame for trace */
        pop_frame(S);
        return result;
    }

    /* Fast path #2: bc-runnable FN. Lazy compile, staleness check,
     * trampoline through mino_bc_run. argv passes through; the
     * tail-call sentinel still uses a cons-format args list, walked
     * back into argv inside the trampoline. */
    if (mino_type_of(fn) == MINO_FN) {
        if (fn->as.fn.bc == NULL) {
            (void)mino_bc_compile_fn(S, fn);
        }
        if (fn->as.fn.bc != NULL
            && fn->as.fn.bc != &mino_bc_declined
            && fn->as.fn.bc->has_folds
            && fn->as.fn.bc->compile_ic_gen != S->ic_gen) {
            fn->as.fn.bc = NULL;
            (void)mino_bc_compile_fn(S, fn);
        }
        mino_bc_check_require(S, fn);
        if (MINO_BC_RUNNABLE(fn)) {
            mino_val_t **call_argv = argv;
            int          call_argc = argc;
            int          cap       = argc;
            int          heap      = 0;
            const char  *file      = NULL;
            int          line      = 0;
            int          col       = 0;
            const char  *saved_ns      = S->current_ns;
            const char  *saved_ambient = S->fn_ambient_ns;
            mino_val_t  *result;
            if (fn->as.fn.defining_ns != NULL) {
                S->current_ns    = fn->as.fn.defining_ns;
                S->fn_ambient_ns = fn->as.fn.defining_ns;
            }
            if (mino_current_ctx(S)->eval_current_form != NULL
                && mino_type_of(mino_current_ctx(S)->eval_current_form) == MINO_CONS) {
                file = mino_current_ctx(S)->eval_current_form->as.cons.file;
                line = mino_current_ctx(S)->eval_current_form->as.cons.line;
                col  = mino_current_ctx(S)->eval_current_form->as.cons.column;
            }
            push_frame(S, "fn", file, line, col);
            for (;;) {
                result = mino_bc_run(S, fn, call_argv, call_argc,
                                     fn->as.fn.env);
                if (result == NULL) {
                    S->current_ns    = saved_ns;
                    S->fn_ambient_ns = saved_ambient;
                    return NULL;
                }
                if (mino_type_of(result) != MINO_TAIL_CALL) break;
                mino_val_t *next_fn   = result->as.tail_call.fn;
                mino_val_t *next_args = result->as.tail_call.args;
                if (next_fn != NULL && mino_type_of(next_fn) == MINO_FN
                    && next_fn->as.fn.bc == NULL) {
                    (void)mino_bc_compile_fn(S, next_fn);
                }
                if (next_fn != NULL && mino_type_of(next_fn) == MINO_FN
                    && MINO_BC_RUNNABLE(next_fn)
                    && next_fn->as.fn.params != NULL) {
                    /* Walk the tail-call cons back into argv. Allocate
                     * a heap buffer if the current cap isn't enough
                     * (or if we were still pointing at the immutable
                     * caller-supplied argv slice from OP_CALL, which
                     * we must not write to). */
                    int new_argc = 0;
                    mino_val_t *cur = next_args;
                    while (mino_is_cons(cur)) {
                        if (new_argc >= cap || !heap) {
                            int new_cap = (new_argc + 1) < (cap * 2)
                                ? (cap * 2) : (new_argc + 8);
                            mino_val_t **grown = (mino_val_t **)gc_alloc_typed(
                                S, GC_T_VALARR,
                                (size_t)new_cap * sizeof(*grown));
                            if (grown == NULL) {
                                S->current_ns    = saved_ns;
                                S->fn_ambient_ns = saved_ambient;
                                return NULL;
                            }
                            memcpy(grown, call_argv,
                                   (size_t)new_argc * sizeof(*grown));
                            call_argv = grown;
                            cap       = new_cap;
                            heap      = 1;
                        }
                        call_argv[new_argc++] = cur->as.cons.car;
                        cur = cur->as.cons.cdr;
                    }
                    call_argc = new_argc;
                    fn        = next_fn;
                    if (fn->as.fn.defining_ns != NULL) {
                        S->current_ns    = fn->as.fn.defining_ns;
                        S->fn_ambient_ns = fn->as.fn.defining_ns;
                    }
                    continue;
                }
                /* Non-bc target: hand off via cons-form apply_callable
                 * since the tail-call args are already in cons form. */
                pop_frame(S);
                S->current_ns    = saved_ns;
                S->fn_ambient_ns = saved_ambient;
                return apply_callable(S, next_fn, next_args, env);
            }
            pop_frame(S);
            S->current_ns    = saved_ns;
            S->fn_ambient_ns = saved_ambient;
            return result;
        }
        /* Tree-walker fallback (compile declined): build cons, delegate
         * to apply_callable for full multi-arity dispatch. */
    }

    /* Slow paths: PRIM with fn1 ABI, MINO_FN tree-walker, MINO_MACRO,
     * non-fn callables. Build the cons-spine and delegate. */
    {
        mino_val_t *args = argv_to_cons(S, argv, argc);
        if (args == NULL) return NULL;
        return apply_callable(S, fn, args, env);
    }
}

mino_val_t *apply_non_fn_callable(mino_state_t *S, mino_val_t *fn,
                                  mino_val_t *args, const mino_val_t *form)
{
    int         nargs = 0;
    mino_val_t *tmp;
    for (tmp = args; mino_is_cons(tmp); tmp = tmp->as.cons.cdr)
        nargs++;

    /* Transients delegate the read interface to their persistent view.
     * (t-vec idx), (t-map :k), (t-set v) all behave identically to the
     * persistent original until persistent! is called -- matching
     * Clojure's read-only-on-transient contract. */
    if (mino_type_of(fn) == MINO_TRANSIENT) {
        if (!fn->as.transient.valid) {
            set_eval_diag(S, form, "eval/state", "MST001",
                "transient is no longer valid");
            return NULL;
        }
        fn = fn->as.transient.current;
        if (fn == NULL || mino_type_of(fn) == MINO_NIL) {
            set_eval_diag(S, form, "eval/type", "MTY002",
                "transient has no underlying collection");
            return NULL;
        }
    }

    if (mino_type_of(fn) == MINO_KEYWORD) {
        /* (:k m) => (get m :k); (:k m default) => (get m :k default). */
        if (nargs < 1 || nargs > 2) {
            set_eval_diag(S, form, "eval/arity", "MAR001",
                "keyword as function takes 1 or 2 arguments");
            return NULL;
        }
        {
            mino_val_t *coll    = args->as.cons.car;
            mino_val_t *def_val = nargs == 2
                ? args->as.cons.cdr->as.cons.car
                : mino_nil(S);
            if (coll != NULL && mino_type_of(coll) == MINO_TRANSIENT) {
                if (!coll->as.transient.valid) {
                    set_eval_diag(S, form, "eval/state", "MST001",
                        "transient is no longer valid");
                    return NULL;
                }
                coll = coll->as.transient.current;
                if (coll == NULL || mino_type_of(coll) == MINO_NIL) return def_val;
            }
            if (coll != NULL && mino_type_of(coll) == MINO_MAP) {
                mino_val_t *v = map_get_val(coll, fn);
                return v == NULL ? def_val : v;
            }
            if (coll != NULL && mino_type_of(coll) == MINO_SORTED_MAP) {
                mino_val_t *v = rb_get(S, coll->as.sorted.root, fn,
                                        coll->as.sorted.comparator);
                return v == NULL ? def_val : v;
            }
            if (coll != NULL && mino_type_of(coll) == MINO_RECORD) {
                int idx = record_field_index(coll, fn);
                if (idx >= 0) return coll->as.record.vals[idx];
                if (coll->as.record.ext != NULL) {
                    mino_val_t *v = map_get_val(coll->as.record.ext, fn);
                    if (v != NULL) return v;
                }
                return def_val;
            }
            if (coll != NULL && mino_type_of(coll) == MINO_SET) {
                /* (:k #{:k :other}) returns :k if present, else default.
                 * Mirrors Clojure's keyword-as-fn behaviour against a
                 * set: the set acts as a "is this key present?" check
                 * and the keyword is its own value. */
                uint32_t h = hash_val(fn);
                if (hamt_get(coll->as.set.root, fn, h, 0u) != NULL)
                    return fn;
                return def_val;
            }
            if (coll != NULL && mino_type_of(coll) == MINO_SORTED_SET) {
                if (rb_contains(S, coll->as.sorted.root, fn,
                                coll->as.sorted.comparator))
                    return fn;
                return def_val;
            }
            return def_val;
        }
    }
    if (mino_type_of(fn) == MINO_MAP) {
        /* ({:a 1} :k) => (get {:a 1} :k). */
        if (nargs < 1 || nargs > 2) {
            set_eval_diag(S, form, "eval/arity", "MAR001",
                "map as function takes 1 or 2 arguments");
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
    if (mino_type_of(fn) == MINO_RECORD) {
        /* (record :key) and (record :key default) -- same lookup
         * surface as map. Goes through record_field_index (declared
         * fields) and falls back to ext lookup before returning the
         * default. */
        if (nargs < 1 || nargs > 2) {
            set_eval_diag(S, form, "eval/arity", "MAR001",
                "record as function takes 1 or 2 arguments");
            return NULL;
        }
        {
            mino_val_t *key     = args->as.cons.car;
            mino_val_t *def_val = nargs == 2
                ? args->as.cons.cdr->as.cons.car
                : mino_nil(S);
            int idx = record_field_index(fn, key);
            if (idx >= 0) return fn->as.record.vals[idx];
            if (fn->as.record.ext != NULL) {
                mino_val_t *v = map_get_val(fn->as.record.ext, key);
                if (v != NULL) return v;
            }
            return def_val;
        }
    }
    if (mino_type_of(fn) == MINO_VECTOR) {
        /* ([1 2 3] 0) => (nth [1 2 3] 0). */
        if (nargs != 1) {
            set_eval_diag(S, form, "eval/arity", "MAR001",
                "vector as function takes 1 argument");
            return NULL;
        }
        {
            mino_val_t *idx = args->as.cons.car;
            long long i;
            if (idx == NULL || !mino_val_int_p(idx)) {
                set_eval_diag(S, form, "eval/type", "MTY001",
                    "vector index must be an integer");
                return NULL;
            }
            i = mino_val_int_get(idx);
            if (i < 0 || (size_t)i >= fn->as.vec.len) {
                set_eval_diag(S, form, "eval/bounds", "MBD001",
                    "vector index out of bounds");
                return NULL;
            }
            return vec_nth(fn, (size_t)i);
        }
    }
    if (mino_type_of(fn) == MINO_SET) {
        /* (#{:a :b} :a) => :a or nil. */
        if (nargs != 1) {
            set_eval_diag(S, form, "eval/arity", "MAR001",
                "set as function takes 1 argument");
            return NULL;
        }
        {
            mino_val_t *key = args->as.cons.car;
            uint32_t h = hash_val(key);
            return hamt_get(fn->as.set.root, key, h, 0u) != NULL
                ? key : mino_nil(S);
        }
    }
    if (mino_type_of(fn) == MINO_SORTED_MAP) {
        if (nargs < 1 || nargs > 2) {
            set_eval_diag(S, form, "eval/arity", "MAR001",
                "sorted-map as function takes 1 or 2 arguments");
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
    if (mino_type_of(fn) == MINO_SORTED_SET) {
        if (nargs != 1) {
            set_eval_diag(S, form, "eval/arity", "MAR001",
                "sorted-set as function takes 1 argument");
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
        set_eval_diag(S, form, "eval/type", "MTY002", msg);
    }
    return NULL;
}
