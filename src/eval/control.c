/*
 * control.c -- try/catch/finally special form.
 */

#include "eval/special_internal.h"
#include "eval/bc/internal.h"  /* mino_bc_fn_t + mino_bc_source_lookup */

/* try_clauses_t -- the partitioned shape of a (try body... [catch e ...]
 * [finally ...]) form. body_head is a freshly-built cons list of the
 * body forms (so eval_implicit_do can walk it); catch_body and
 * finally_body are tails of the original args list (no copy). */
typedef struct {
    mino_val *body_head;
    mino_val *catch_body;
    mino_val *finally_body;
    int         has_catch;
    int         has_finally;
    char        catch_var[256];
} try_clauses_t;

/* partition_try_clauses -- walk args once, classifying each top-level
 * form as a body, catch, or finally clause, and emitting a try_clauses_t.
 * Returns 0 on success, -1 on a syntax error with the diagnostic
 * already set. */
static int partition_try_clauses(mino_state *S, mino_val *form,
                                 mino_val *args, try_clauses_t *out)
{
    mino_val *body_tail = NULL;
    mino_val *rest      = args;

    out->body_head    = NULL;
    out->catch_body   = NULL;
    out->finally_body = NULL;
    out->has_catch    = 0;
    out->has_finally  = 0;
    out->catch_var[0] = '\0';

    while (mino_is_cons(rest)) {
        mino_val *clause = rest->as.cons.car;
        if (mino_is_cons(clause)
            && sym_eq(clause->as.cons.car, "catch")) {
            /* (catch e handler...)
             * DEVIATION: JVM Clojure requires (catch ExceptionType e handler...)
             * and filters by type. mino omits the type argument and catches ALL
             * exceptions unconditionally. This is intentional: mino has a single
             * exception kind (a diagnostic map) rather than a class hierarchy, so
             * type-based dispatch has no meaning here. The type argument is
             * silently absent from the syntax. */
            mino_val *cv;
            size_t      vl;
            if (!mino_is_cons(clause->as.cons.cdr)) {
                set_eval_diag(S, form, "syntax", "MSY001",
                    "catch requires a binding symbol");
                return -1;
            }
            cv = clause->as.cons.cdr->as.cons.car;
            if (cv == NULL || mino_type_of(cv) != MINO_SYMBOL) {
                set_eval_diag(S, form, "syntax", "MSY001",
                    "catch binding must be a symbol");
                return -1;
            }
            vl = cv->as.s.len;
            if (vl >= sizeof(out->catch_var)) {
                set_eval_diag(S, form, "syntax", "MSY001",
                    "catch variable name too long");
                return -1;
            }
            memcpy(out->catch_var, cv->as.s.data, vl);
            out->catch_var[vl] = '\0';
            out->catch_body = clause->as.cons.cdr->as.cons.cdr;
            out->has_catch  = 1;
            rest = rest->as.cons.cdr;
            continue;
        }
        if (mino_is_cons(clause)
            && sym_eq(clause->as.cons.car, "finally")) {
            out->finally_body = clause->as.cons.cdr;
            out->has_finally  = 1;
            rest = rest->as.cons.cdr;
            continue;
        }
        /* Body form -- append to list. */
        {
            mino_val *cell = mino_cons(S, clause, mino_nil(S));
            if (body_tail == NULL) {
                out->body_head = cell;
            } else {
                mino_cons_cdr_set(S, body_tail, cell);
            }
            body_tail = cell;
        }
        rest = rest->as.cons.cdr;
    }
    return 0;
}

/* normalize_exception -- ensure the value passed to a catch handler is
 * a structured diagnostic map (has :mino/kind). Strings, plain values,
 * and partial maps are wrapped; already-diagnostic maps pass through.
 * Shared with the bytecode VM's OP_PUSHCATCH landing pad so a caught
 * exception arrives at the BC handler in the same shape as the tree-
 * walker's catch binding.
 *
 * Attaches :mino/location when the current frame knows where it is:
 * BC frames consult `bc_current_pc` via the source-map side table
 * (the inner instruction position); tree-walker frames fall back to
 * `eval_current_form`. This brings user-throw catch values in line
 * with system-throw catch values, which already carry the field. */
/* Capture the throw site into the ctx side channel before a user
 * throw unwinds. The landing pads rewind the BC cursor to its
 * frame-entry value, so normalize_exception could not derive the
 * position after the longjmp; both throw paths (prim_throw and the
 * VM's OP_THROW) call this right before jumping. */
void mino_throw_capture_site(mino_state *S)
{
    mino_thread_ctx_t *ctx = mino_current_ctx(S);
    const char *loc_file = NULL;
    int         loc_line = 0;
    int         loc_col  = 0;
    if (ctx->bc_current_bc != NULL) {
        (void)mino_bc_source_lookup(ctx->bc_current_bc,
                                    ctx->bc_current_pc,
                                    &loc_file, &loc_line, &loc_col);
    }
    if (loc_file == NULL || loc_line <= 0) {
        const mino_val *form = ctx->eval_current_form;
        if (form != NULL && mino_is_cons(form)
            && form->as.cons.file != NULL && form->as.cons.line > 0) {
            loc_file = form->as.cons.file;
            loc_line = form->as.cons.line;
            loc_col  = form->as.cons.column;
        }
    }
    ctx->throw_loc_file = loc_file;
    ctx->throw_loc_line = loc_line;
    ctx->throw_loc_col  = loc_col;
}

mino_val *normalize_exception(mino_state *S, mino_val *ex_val)
{
    mino_val *keys[6], *vals[6];
    mino_val *result;
    size_t n;
    const char *loc_file = NULL;
    int loc_line = 0;
    int loc_col  = 0;
    if (mino_type_of(ex_val) == MINO_MAP
        && map_get_val(ex_val, mino_keyword(S, "mino/kind")) != NULL) {
        mino_current_ctx(S)->throw_loc_file = NULL;
        mino_current_ctx(S)->throw_loc_line = 0;
        mino_current_ctx(S)->throw_loc_col  = 0;
        return ex_val;
    }
    keys[0] = mino_keyword(S, "mino/kind");
    vals[0] = mino_keyword(S, "user");
    keys[1] = mino_keyword(S, "mino/code");
    vals[1] = mino_string(S, "MUS001");
    keys[2] = mino_keyword(S, "mino/phase");
    vals[2] = mino_keyword(S, "eval");
    keys[3] = mino_keyword(S, "mino/message");
    if (mino_type_of(ex_val) == MINO_STRING) {
        vals[3] = ex_val;
    } else if (mino_type_of(ex_val) == MINO_MAP) {
        mino_val *msg_val = map_get_val(ex_val,
            mino_keyword(S, "message"));
        vals[3] = (msg_val != NULL && mino_type_of(msg_val) == MINO_STRING)
            ? msg_val : mino_string(S, "uncaught exception");
    } else if (ex_val != NULL) {
        /* Non-string, non-map payload (keyword, symbol, vector,
         * record, ...): print the value into the diagnostic message
         * so embedders can see what was thrown. Phase 6 of the
         * embedder UX cycle called this out. */
        char buf[384];
        char msg[512];
        int  w = mino_print_to_buf(S, ex_val, buf, sizeof(buf));
        if (w > 0) {
            snprintf(msg, sizeof(msg), "uncaught exception: %s", buf);
            vals[3] = mino_string(S, msg);
        } else {
            vals[3] = mino_string(S, "uncaught exception");
        }
    } else {
        vals[3] = mino_string(S, "uncaught exception");
    }
    keys[4] = mino_keyword(S, "mino/data");
    vals[4] = ex_val;
    n = 5;
    /* Source location: prefer the inner BC PC (the throw site inside
     * a compiled fn body) over eval_current_form (the outer call
     * site). Without this, a caught user-throw map would have no
     * location, and a BC-fn-body throw caught from outside would
     * blame the caller's line rather than the (throw ...) form. */
    {
        mino_thread_ctx_t *ctx = mino_current_ctx(S);
        if (ctx->throw_loc_line > 0 && ctx->throw_loc_file != NULL) {
            /* prim_throw captured the throw site before the landing
             * pad rewound the BC cursor; prefer it. */
            loc_file = ctx->throw_loc_file;
            loc_line = ctx->throw_loc_line;
            loc_col  = ctx->throw_loc_col;
        } else {
            const mino_bc_fn_t *cur_bc = ctx->bc_current_bc;
            size_t              cur_pc = ctx->bc_current_pc;
            if (cur_bc != NULL) {
                (void)mino_bc_source_lookup(cur_bc, cur_pc,
                                            &loc_file, &loc_line, &loc_col);
            }
            if (loc_file == NULL || loc_line <= 0) {
                const mino_val *form = ctx->eval_current_form;
                if (form != NULL && mino_is_cons(form)
                    && form->as.cons.file != NULL && form->as.cons.line > 0) {
                    loc_file = form->as.cons.file;
                    loc_line = form->as.cons.line;
                    loc_col  = form->as.cons.column;
                }
            }
        }
        ctx->throw_loc_file = NULL;
        ctx->throw_loc_line = 0;
        ctx->throw_loc_col  = 0;
    }
    if (loc_file != NULL && loc_line > 0) {
        mino_val *lkeys[3], *lvals[3];
        lkeys[0] = mino_keyword(S, "file");
        lvals[0] = mino_string(S, loc_file);
        lkeys[1] = mino_keyword(S, "line");
        lvals[1] = mino_int(S, loc_line);
        lkeys[2] = mino_keyword(S, "column");
        lvals[2] = mino_int(S, loc_col);
        keys[n] = mino_keyword(S, "mino/location");
        vals[n] = mino_map(S, lkeys, lvals, 3);
        n++;
    }
    result = mino_map(S, keys, vals, n);
    /* Carry metadata from the thrown value onto the diagnostic so
     * (meta caught) round-trips through catch. ex-info already
     * piggybacks cause chains through metadata; rich-error-info
     * patterns rely on it too. Guard with MINO_IS_PTR -- tagged
     * primitives (ints, bools) cannot carry meta and dereferencing
     * them as `mino_val *` would segfault. */
    if (result != NULL && MINO_IS_PTR(ex_val) && ex_val->meta != NULL) {
        result->meta = ex_val->meta;
    }
    return result;
}

mino_val *eval_try(mino_state *S, mino_val *form,
                     mino_val *args, mino_env *env, int tail)
{
    try_clauses_t clauses;
    int           saved_try;
    int           saved_call;
    int           saved_trace;
    int           saved_gc_save;
    dyn_frame_t  *saved_dyn;
    volatile int       got_exception = 0;
    mino_val * volatile vol_result = NULL;
    mino_val * volatile vol_ex     = NULL;
    (void)tail;

    if (partition_try_clauses(S, form, args, &clauses) != 0) {
        return NULL;
    }
    saved_try   = mino_current_ctx(S)->try_depth;
    saved_call  = mino_current_ctx(S)->call_depth;
    saved_trace = mino_current_ctx(S)->trace_added;
    saved_gc_save = mino_current_ctx(S)->gc_save_len;
    saved_dyn   = mino_current_ctx(S)->dyn_stack;
    /* Snapshot bc_top so a longjmp that unwinds through bc_run
     * frames (intermediate fns called by the body) doesn't leave
     * their register windows stranded in [0, bc_top). The catch
     * arm below pops down to this value and zeroes the freed
     * slots, matching what the bypassed bc_pop_window would have
     * done on a normal return. Without this, leaked register
     * slots root their contents as GC roots until the outermost
     * mino_bc_run returns. */
    size_t saved_bc_top = S->bc.bc_top;

    if (mino_current_ctx(S)->try_depth >= MAX_TRY_DEPTH) {
        set_eval_diag(S, form, "limit", "MLM002", "try nesting too deep");
        return NULL;
    }

    /* Body: evaluate inside a setjmp landing pad so a throw lands here. */
    mino_current_ctx(S)->try_stack[mino_current_ctx(S)->try_depth].exception      = NULL;
    mino_current_ctx(S)->try_stack[mino_current_ctx(S)->try_depth].saved_ns       = S->ns_vars.current_ns;
    mino_current_ctx(S)->try_stack[mino_current_ctx(S)->try_depth].saved_ambient  = S->ns_vars.fn_ambient_ns;
    mino_current_ctx(S)->try_stack[mino_current_ctx(S)->try_depth].saved_load_len = S->module.load_stack_len;
    mino_current_ctx(S)->try_stack[mino_current_ctx(S)->try_depth].saved_lazy_len = mino_current_ctx(S)->lazy_inflight_len;
    mino_current_ctx(S)->try_stack[mino_current_ctx(S)->try_depth].saved_bc_cursor =     mino_current_ctx(S)->bc_current_bc;
    mino_current_ctx(S)->try_stack[mino_current_ctx(S)->try_depth].saved_bc_cursor_pc =     mino_current_ctx(S)->bc_current_pc;
    if (setjmp(mino_current_ctx(S)->try_stack[mino_current_ctx(S)->try_depth].buf) == 0) {
        mino_val *r;
        mino_current_ctx(S)->try_depth++;
        r = eval_implicit_do(S, clauses.body_head, env);
        mino_current_ctx(S)->try_depth = saved_try;
        if (r == NULL) {
            /* Fatal runtime error. */
            if (clauses.has_finally)
                eval_implicit_do(S, clauses.finally_body, env);
            return NULL;
        }
        if (mino_type_of(r) == MINO_RECUR) {
            /* A recur target cannot be re-entered across the try
             * frame -- the unwind machinery would be skipped. */
            set_eval_diag(S, form, "syntax", "MSY001",
                          "cannot recur across try");
            if (clauses.has_finally)
                eval_implicit_do(S, clauses.finally_body, env);
            return NULL;
        }
        vol_result = r;
    } else {
        /* longjmp'd from throw in body. Restore current_ns and ambient
         * since the throw bypassed any per-fn restore on its way up. */
        vol_ex      = mino_current_ctx(S)->try_stack[saved_try].exception;
        S->ns_vars.current_ns    = mino_current_ctx(S)->try_stack[saved_try].saved_ns;
        S->ns_vars.fn_ambient_ns = mino_current_ctx(S)->try_stack[saved_try].saved_ambient;
        load_stack_truncate(S, mino_current_ctx(S)->try_stack[saved_try].saved_load_len);
        mino_lazy_inflight_unwind(S, mino_current_ctx(S)->try_stack[saved_try].saved_lazy_len);
        mino_current_ctx(S)->bc_current_bc = mino_current_ctx(S)->try_stack[saved_try].saved_bc_cursor;
        mino_current_ctx(S)->bc_current_pc = mino_current_ctx(S)->try_stack[saved_try].saved_bc_cursor_pc;
        mino_current_ctx(S)->try_depth   = saved_try;
        mino_current_ctx(S)->call_depth  = saved_call;
        mino_current_ctx(S)->trace_added = saved_trace;
        /* The throw longjmp'd past the gc_unpin calls in the abandoned
         * frames; restore the save stack to its try-entry depth so those
         * transient pins don't leak (the exception is rooted separately
         * via try_stack[].exception). */
        mino_current_ctx(S)->gc_save_len = saved_gc_save;
        while (mino_current_ctx(S)->dyn_stack != saved_dyn) {
            dyn_frame_t *f = mino_current_ctx(S)->dyn_stack;
            mino_current_ctx(S)->dyn_stack = f->prev;
            dyn_binding_list_free(f->bindings);
            /* Mirror eval_binding's normal-path free(frame); the
             * frame is malloc'd in bindings.c (heap-allocated so
             * the pointer survives a body throw) and must be
             * reclaimed here on the unwind path. */
            free(f);
        }
        while (S->bc.bc_top > saved_bc_top) {
            S->bc.bc_top--;
            S->bc.bc_regs[S->bc.bc_top] = NULL;
        }
        clear_error(S);
        got_exception = 1;
    }

    /* Catch: run the handler if the body threw. */
    if (got_exception && clauses.has_catch) {
        mino_val *ex_val = normalize_exception(S,
            vol_ex ? (mino_val *)vol_ex : mino_nil(S));
        mino_env *local  = env_child(S, env);
        env_bind(S, local, clauses.catch_var, ex_val);

        if (clauses.has_finally) {
            /* Inner try frame catches re-throws from the catch handler
             * so finally still runs. The slot at try_stack[try_depth]
             * is always available here: the entry guard at the top of
             * eval_try rejects any call with try_depth >= MAX_TRY_DEPTH
             * BEFORE the body runs, and the longjmp-unwind path
             * restored try_depth to that pre-entry value, so there is
             * room for one more push. */
            int         ic = mino_current_ctx(S)->call_depth;
            int         it = mino_current_ctx(S)->trace_added;
            int         is = mino_current_ctx(S)->try_depth; /* save before setjmp */
            dyn_frame_t *id = mino_current_ctx(S)->dyn_stack;
            size_t      ibt = S->bc.bc_top; /* bc_top before catch-handler */
            mino_current_ctx(S)->try_stack[is].exception      = NULL;
            mino_current_ctx(S)->try_stack[is].saved_ns       = S->ns_vars.current_ns;
            mino_current_ctx(S)->try_stack[is].saved_ambient  = S->ns_vars.fn_ambient_ns;
            mino_current_ctx(S)->try_stack[is].saved_load_len = S->module.load_stack_len;
            mino_current_ctx(S)->try_stack[is].saved_lazy_len = mino_current_ctx(S)->lazy_inflight_len;
            mino_current_ctx(S)->try_stack[is].saved_bc_cursor    = mino_current_ctx(S)->bc_current_bc;
            mino_current_ctx(S)->try_stack[is].saved_bc_cursor_pc = mino_current_ctx(S)->bc_current_pc;
            if (setjmp(mino_current_ctx(S)->try_stack[is].buf) == 0) {
                mino_val *r;
                mino_current_ctx(S)->try_depth++;
                r = eval_implicit_do(S, clauses.catch_body, local);
                mino_current_ctx(S)->try_depth = is;
                if (r == NULL) {
                    eval_implicit_do(S, clauses.finally_body, env);
                    return NULL;
                }
                if (mino_type_of(r) == MINO_RECUR) {
                    set_eval_diag(S, form, "syntax", "MSY001",
                                  "cannot recur across try");
                    eval_implicit_do(S, clauses.finally_body, env);
                    return NULL;
                }
                vol_result    = r;
                got_exception = 0;
            } else {
                /* Catch handler re-threw. */
                vol_ex      = mino_current_ctx(S)->try_stack[is].exception;
                S->ns_vars.current_ns    = mino_current_ctx(S)->try_stack[is].saved_ns;
                S->ns_vars.fn_ambient_ns = mino_current_ctx(S)->try_stack[is].saved_ambient;
                load_stack_truncate(S, mino_current_ctx(S)->try_stack[is].saved_load_len);
                mino_lazy_inflight_unwind(S, mino_current_ctx(S)->try_stack[is].saved_lazy_len);
                mino_current_ctx(S)->bc_current_bc = mino_current_ctx(S)->try_stack[is].saved_bc_cursor;
                mino_current_ctx(S)->bc_current_pc = mino_current_ctx(S)->try_stack[is].saved_bc_cursor_pc;
                mino_current_ctx(S)->try_depth   = is;
                mino_current_ctx(S)->call_depth  = ic;
                mino_current_ctx(S)->trace_added = it;
                while (mino_current_ctx(S)->dyn_stack != id) {
                    dyn_frame_t *f = mino_current_ctx(S)->dyn_stack;
                    mino_current_ctx(S)->dyn_stack = f->prev;
                    dyn_binding_list_free(f->bindings);
                    free(f);
                }
                while (S->bc.bc_top > ibt) {
                    S->bc.bc_top--;
                    S->bc.bc_regs[S->bc.bc_top] = NULL;
                }
                clear_error(S);
                /* got_exception stays 1, vol_ex updated. */
            }
        } else {
            /* No finally: run catch directly. A re-throw inside the
             * handler longjmps straight to the enclosing try frame,
             * which is exactly the contract -- there is no finally on
             * this frame to run on the unwind. */
            mino_val *r =
                eval_implicit_do(S, clauses.catch_body, local);
            if (r == NULL) {
                return NULL;
            }
            if (mino_type_of(r) == MINO_RECUR) {
                set_eval_diag(S, form, "syntax", "MSY001",
                              "cannot recur across try");
                return NULL;
            }
            vol_result    = r;
            got_exception = 0;
        }
    }

    /* Finally: run unconditionally before propagating any unhandled throw. */
    if (clauses.has_finally) {
        eval_implicit_do(S, clauses.finally_body, env);
    }

    /* Rethrow: if no handler matched, propagate to the enclosing try. */
    if (got_exception) {
        mino_val *e = (mino_val *)vol_ex;
        if (mino_current_ctx(S)->try_depth > 0) {
            mino_current_ctx(S)->try_stack[mino_current_ctx(S)->try_depth - 1].exception = e;
            longjmp(mino_current_ctx(S)->try_stack[mino_current_ctx(S)->try_depth - 1].buf, 1);
        }
        if (e != NULL && mino_type_of(e) == MINO_STRING) {
            char msg[512];
            snprintf(msg, sizeof(msg),
                     "unhandled exception: %.*s",
                     (int)e->as.s.len, e->as.s.data);
            set_eval_diag(S, mino_current_ctx(S)->eval_current_form, "user", "MUS001", msg);
        } else {
            set_eval_diag(S, mino_current_ctx(S)->eval_current_form, "internal", "MIN001", "unhandled exception");
        }
        return NULL;
    }

    return (mino_val *)vol_result;
}
