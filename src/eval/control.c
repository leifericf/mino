/*
 * control.c -- try/catch/finally special form.
 */

#include "eval/special_internal.h"
#include "eval/bc/internal.h"  /* mino_bc_fn_t + mino_bc_source_lookup */

/* catch-class table (ADR 32): maps a JVM exception class name to the
 * diagnostic :mino/kind strings its clause accepts. kinds[0] == NULL
 * marks a catch-all. Shared C data: the bytecode tier's OP_CATCH_MATCH
 * and compile-time class validation read the same table. The type and
 * accessors are declared in eval/special_internal.h and
 * eval/bc/internal.h. */
const mino_catch_class_t mino_catch_classes[] = {
    {":default",                    {NULL}},
    {"Throwable",                   {NULL}},
    {"Exception",                   {NULL}},
    {"Object",                      {NULL}},
    {"ExceptionInfo",               {"user", NULL}},
    {"Error",                       {"internal", NULL}},
    {"ClassCastException",          {"eval/type", NULL}},
    {"ArithmeticException",         {"eval/type", NULL}},
    {"NullPointerException",        {"eval/type", NULL}},
    {"NumberFormatException",       {"eval/type", NULL}},
    {"IllegalArgumentException",   {"eval/arity", "eval/contract", NULL}},
    {"UnsupportedOperationException", {"eval/contract", NULL}},
    {"IndexOutOfBoundsException",   {"eval/bounds", NULL}},
    {"StringIndexOutOfBoundsException", {"eval/bounds", NULL}},
    {"IllegalStateException",       {"eval/state", NULL}},
};

int mino_catch_class_index(const char *name)
{
    size_t n = sizeof(mino_catch_classes) / sizeof(mino_catch_classes[0]);
    /* Qualified names (clojure.lang.ExceptionInfo) match on the tail
     * after the last dot, which also covers the simple name. */
    const char *tail = strrchr(name, '.');
    if (tail != NULL) {
        tail++;
    } else {
        tail = name;
    }
    for (size_t i = 0; i < n; i++) {
        if (strcmp(mino_catch_classes[i].name, tail) == 0) {
            return (int)i;
        }
    }
    return -1;
}

int mino_catch_class_matches(mino_state *S, int class_idx, mino_val *diag)
{
    const mino_catch_class_t *cc;
    mino_val *kind;
    if (class_idx < 0 || class_idx >= (int)(sizeof(mino_catch_classes)
                                            / sizeof(mino_catch_classes[0]))) {
        return 0;
    }
    cc = &mino_catch_classes[class_idx];
    if (cc->kinds[0] == NULL) {
        return 1;
    }
    kind = (diag != NULL && mino_type_of(diag) == MINO_MAP)
        ? map_get_val(diag, mino_keyword(S, "mino/kind"))
        : NULL;
    for (int i = 0; i < 3 && cc->kinds[i] != NULL; i++) {
        if (kw_eq(kind, cc->kinds[i])) {
            return 1;
        }
    }
    return 0;
}

static int unknown_catch_class(mino_state *S, mino_val *form,
                               const char *name)
{
    char ebuf[384];
    snprintf(ebuf, sizeof(ebuf), "unknown catch class: %s", name);
    set_eval_diag(S, form, "syntax", "MSY004", ebuf);
    return -1;
}

#define MAX_CATCH_CLAUSES 8

/* try_clauses_t -- the partitioned shape of a (try body... [catch ...]
 * [finally ...]) form. body_head is a freshly-built cons list of the
 * body forms (so eval_implicit_do can walk it); catch_body and
 * finally_body are tails of the original args list (no copy).
 * catch_class[i] is -1 for a bare catch-all clause, else an index into
 * mino_catch_classes. */
typedef struct {
    mino_val *body_head;
    mino_val *finally_body;
    int         has_finally;
    int         n_catch;
    mino_val *catch_body[MAX_CATCH_CLAUSES];
    char        catch_var[MAX_CATCH_CLAUSES][256];
    int         catch_class[MAX_CATCH_CLAUSES];
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
    out->finally_body = NULL;
    out->has_finally  = 0;
    out->n_catch      = 0;

    while (mino_is_cons(rest)) {
        mino_val *clause = rest->as.cons.car;
        if (mino_is_cons(clause)
            && sym_eq(clause->as.cons.car, "catch")) {
            /* (catch e handler...) is the bare catch-all;
             * (catch Class e handler...) is classed and dispatches on
             * the diagnostic's :mino/kind (ADR 32). Two leading
             * symbols is classed even when the first would also work
             * as a binding name, mirroring the JVM grammar. */
            mino_val *tail = clause->as.cons.cdr;
            mino_val *cv;
            mino_val *body;
            int        cls = -1;
            char       clsbuf[300];
            if (!mino_is_cons(tail)) {
                set_eval_diag(S, form, "syntax", "MSY001",
                    "catch requires a binding symbol");
                return -1;
            }
            cv = tail->as.cons.car;
            if (mino_type_of(cv) == MINO_KEYWORD) {
                size_t kl = cv->as.s.len;
                if (kl + 2 > sizeof(clsbuf)) {
                    set_eval_diag(S, form, "syntax", "MSY001",
                        "catch class name too long");
                    return -1;
                }
                clsbuf[0] = ':';
                memcpy(clsbuf + 1, cv->as.s.data, kl);
                clsbuf[kl + 1] = '\0';
                cls = mino_catch_class_index(clsbuf);
                if (cls < 0) {
                    return unknown_catch_class(S, form, clsbuf);
                }
                if (!mino_is_cons(tail->as.cons.cdr)
                    || (tail->as.cons.cdr->as.cons.car == NULL
                        || mino_type_of(tail->as.cons.cdr->as.cons.car)
                           != MINO_SYMBOL)) {
                    set_eval_diag(S, form, "syntax", "MSY001",
                        "catch binding must be a symbol");
                    return -1;
                }
                cv   = tail->as.cons.cdr->as.cons.car;
                body = tail->as.cons.cdr->as.cons.cdr;
            } else if (cv != NULL && mino_type_of(cv) == MINO_SYMBOL) {
                mino_val *second = mino_is_cons(tail->as.cons.cdr)
                    ? tail->as.cons.cdr->as.cons.car : NULL;
                if (second != NULL && mino_type_of(second) == MINO_SYMBOL) {
                    size_t cl = cv->as.s.len;
                    if (cl >= sizeof(clsbuf)) {
                        set_eval_diag(S, form, "syntax", "MSY001",
                            "catch class name too long");
                        return -1;
                    }
                    memcpy(clsbuf, cv->as.s.data, cl);
                    clsbuf[cl] = '\0';
                    cls = mino_catch_class_index(clsbuf);
                    if (cls < 0) {
                        return unknown_catch_class(S, form, clsbuf);
                    }
                    cv   = second;
                    body = tail->as.cons.cdr->as.cons.cdr;
                } else {
                    body = tail->as.cons.cdr;
                }
            } else {
                set_eval_diag(S, form, "syntax", "MSY001",
                    "catch binding must be a symbol");
                return -1;
            }
            if (out->n_catch >= MAX_CATCH_CLAUSES) {
                set_eval_diag(S, form, "syntax", "MSY001",
                    "too many catch clauses");
                return -1;
            }
            {
                size_t vl = cv->as.s.len;
                if (vl >= sizeof(out->catch_var[0])) {
                    set_eval_diag(S, form, "syntax", "MSY001",
                        "catch variable name too long");
                    return -1;
                }
                memcpy(out->catch_var[out->n_catch], cv->as.s.data, vl);
                out->catch_var[out->n_catch][vl] = '\0';
            }
            out->catch_class[out->n_catch] = cls;
            out->catch_body[out->n_catch]  = body;
            out->n_catch++;
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
    mino_current_ctx(S)->try_stack[mino_current_ctx(S)->try_depth].saved_jit_invoke_depth = mino_current_ctx(S)->jit_invoke_depth;
    mino_current_ctx(S)->try_stack[mino_current_ctx(S)->try_depth].saved_gc_depth = mino_current_ctx(S)->gc_depth;
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
        mino_current_ctx(S)->jit_invoke_depth = mino_current_ctx(S)->try_stack[saved_try].saved_jit_invoke_depth;
        mino_current_ctx(S)->gc_depth = mino_current_ctx(S)->try_stack[saved_try].saved_gc_depth;
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
            dyn_frame_restore_ns(S, f);
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

    /* Catch: run the first matching handler if the body threw. A
     * classed clause whose class does not accept the diagnostic's
     * :mino/kind is skipped; a bare clause accepts everything. When
     * no clause matches, got_exception stays set so finally still
     * runs and the throw propagates below. ex_val and matched are
     * assigned before any setjmp in this function and never after,
     * so they stay valid across the handler's longjmp paths. */
    int         matched = -1;
    mino_val *ex_val  = NULL;
    if (got_exception && clauses.n_catch > 0) {
        ex_val = normalize_exception(S,
            vol_ex ? (mino_val *)vol_ex : mino_nil(S));
        for (int i = 0; i < clauses.n_catch; i++) {
            if (clauses.catch_class[i] < 0
                || mino_catch_class_matches(S, clauses.catch_class[i],
                                            ex_val)) {
                matched = i;
                break;
            }
        }
    }
    if (got_exception && matched >= 0) {
        mino_env *local  = env_child(S, env);
        env_bind(S, local, clauses.catch_var[matched], ex_val);

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
            mino_current_ctx(S)->try_stack[is].saved_gc_depth = mino_current_ctx(S)->gc_depth;
            if (setjmp(mino_current_ctx(S)->try_stack[is].buf) == 0) {
                mino_val *r;
                mino_current_ctx(S)->try_depth++;
                r = eval_implicit_do(S, clauses.catch_body[matched], local);
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
                mino_current_ctx(S)->gc_depth = mino_current_ctx(S)->try_stack[is].saved_gc_depth;
                mino_current_ctx(S)->try_depth   = is;
                mino_current_ctx(S)->call_depth  = ic;
                mino_current_ctx(S)->trace_added = it;
                while (mino_current_ctx(S)->dyn_stack != id) {
                    dyn_frame_t *f = mino_current_ctx(S)->dyn_stack;
                    mino_current_ctx(S)->dyn_stack = f->prev;
                    dyn_frame_restore_ns(S, f);
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
                eval_implicit_do(S, clauses.catch_body[matched], local);
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
