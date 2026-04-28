/*
 * control.c -- try/catch/finally special form.
 */

#include "eval/special_internal.h"

/* try_clauses_t -- the partitioned shape of a (try body... [catch e ...]
 * [finally ...]) form. body_head is a freshly-built cons list of the
 * body forms (so eval_implicit_do can walk it); catch_body and
 * finally_body are tails of the original args list (no copy). */
typedef struct {
    mino_val_t *body_head;
    mino_val_t *catch_body;
    mino_val_t *finally_body;
    int         has_catch;
    int         has_finally;
    char        catch_var[256];
} try_clauses_t;

/* partition_try_clauses -- walk args once, classifying each top-level
 * form as a body, catch, or finally clause, and emitting a try_clauses_t.
 * Returns 0 on success, -1 on a syntax error with the diagnostic
 * already set. */
static int partition_try_clauses(mino_state_t *S, mino_val_t *form,
                                 mino_val_t *args, try_clauses_t *out)
{
    mino_val_t *body_tail = NULL;
    mino_val_t *rest      = args;

    out->body_head    = NULL;
    out->catch_body   = NULL;
    out->finally_body = NULL;
    out->has_catch    = 0;
    out->has_finally  = 0;
    out->catch_var[0] = '\0';

    while (mino_is_cons(rest)) {
        mino_val_t *clause = rest->as.cons.car;
        if (mino_is_cons(clause)
            && sym_eq(clause->as.cons.car, "catch")) {
            /* (catch e handler...) */
            mino_val_t *cv;
            size_t      vl;
            if (!mino_is_cons(clause->as.cons.cdr)) {
                set_eval_diag(S, form, "syntax", "MSY001",
                    "catch requires a binding symbol");
                return -1;
            }
            cv = clause->as.cons.cdr->as.cons.car;
            if (cv == NULL || cv->type != MINO_SYMBOL) {
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
            mino_val_t *cell = mino_cons(S, clause, mino_nil(S));
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
 * and partial maps are wrapped; already-diagnostic maps pass through. */
static mino_val_t *normalize_exception(mino_state_t *S, mino_val_t *ex_val)
{
    mino_val_t *keys[5], *vals[5];
    if (ex_val->type == MINO_MAP
        && map_get_val(ex_val, mino_keyword(S, "mino/kind")) != NULL) {
        return ex_val;
    }
    keys[0] = mino_keyword(S, "mino/kind");
    vals[0] = mino_keyword(S, "user");
    keys[1] = mino_keyword(S, "mino/code");
    vals[1] = mino_string(S, "MUS001");
    keys[2] = mino_keyword(S, "mino/phase");
    vals[2] = mino_keyword(S, "eval");
    keys[3] = mino_keyword(S, "mino/message");
    if (ex_val->type == MINO_STRING) {
        vals[3] = ex_val;
    } else if (ex_val->type == MINO_MAP) {
        mino_val_t *msg_val = map_get_val(ex_val,
            mino_keyword(S, "message"));
        vals[3] = (msg_val != NULL && msg_val->type == MINO_STRING)
            ? msg_val : mino_string(S, "uncaught exception");
    } else {
        vals[3] = mino_string(S, "uncaught exception");
    }
    keys[4] = mino_keyword(S, "mino/data");
    vals[4] = ex_val;
    return mino_map(S, keys, vals, 5);
}

mino_val_t *eval_try(mino_state_t *S, mino_val_t *form,
                     mino_val_t *args, mino_env_t *env, int tail)
{
    try_clauses_t clauses;
    int           saved_try;
    int           saved_call;
    int           saved_trace;
    dyn_frame_t  *saved_dyn;
    volatile int         got_exception = 0;
    volatile mino_val_t *vol_result    = NULL;
    volatile mino_val_t *vol_ex        = NULL;
    (void)tail;

    if (partition_try_clauses(S, form, args, &clauses) != 0) {
        return NULL;
    }
    saved_try   = mino_current_ctx(S)->try_depth;
    saved_call  = mino_current_ctx(S)->call_depth;
    saved_trace = mino_current_ctx(S)->trace_added;
    saved_dyn   = mino_current_ctx(S)->dyn_stack;

    if (mino_current_ctx(S)->try_depth >= MAX_TRY_DEPTH) {
        set_eval_diag(S, form, "limit", "MLM002", "try nesting too deep");
        return NULL;
    }

    /* Phase 1: evaluate body forms. */
    mino_current_ctx(S)->try_stack[mino_current_ctx(S)->try_depth].exception      = NULL;
    mino_current_ctx(S)->try_stack[mino_current_ctx(S)->try_depth].saved_ns       = S->current_ns;
    mino_current_ctx(S)->try_stack[mino_current_ctx(S)->try_depth].saved_ambient  = S->fn_ambient_ns;
    mino_current_ctx(S)->try_stack[mino_current_ctx(S)->try_depth].saved_load_len = S->load_stack_len;
    if (setjmp(mino_current_ctx(S)->try_stack[mino_current_ctx(S)->try_depth].buf) == 0) {
        mino_val_t *r;
        mino_current_ctx(S)->try_depth++;
        r = eval_implicit_do(S, clauses.body_head, env);
        mino_current_ctx(S)->try_depth = saved_try;
        if (r == NULL) {
            /* Fatal runtime error. */
            if (clauses.has_finally)
                eval_implicit_do(S, clauses.finally_body, env);
            return NULL;
        }
        vol_result = r;
    } else {
        /* longjmp'd from throw in body. Restore current_ns and ambient
         * since the throw bypassed any per-fn restore on its way up. */
        vol_ex      = mino_current_ctx(S)->try_stack[saved_try].exception;
        S->current_ns    = mino_current_ctx(S)->try_stack[saved_try].saved_ns;
        S->fn_ambient_ns = mino_current_ctx(S)->try_stack[saved_try].saved_ambient;
        load_stack_truncate(S, mino_current_ctx(S)->try_stack[saved_try].saved_load_len);
        mino_current_ctx(S)->try_depth   = saved_try;
        mino_current_ctx(S)->call_depth  = saved_call;
        mino_current_ctx(S)->trace_added = saved_trace;
        while (mino_current_ctx(S)->dyn_stack != saved_dyn) {
            dyn_frame_t *f = mino_current_ctx(S)->dyn_stack;
            mino_current_ctx(S)->dyn_stack = f->prev;
            dyn_binding_list_free(f->bindings);
        }
        clear_error(S);
        got_exception = 1;
    }

    /* Phase 2: run catch handler if we caught an exception. */
    if (got_exception && clauses.has_catch) {
        mino_val_t *ex_val = normalize_exception(S,
            vol_ex ? (mino_val_t *)vol_ex : mino_nil(S));
        mino_env_t *local  = env_child(S, env);
        env_bind(S, local, clauses.catch_var, ex_val);

        if (clauses.has_finally
            && mino_current_ctx(S)->try_depth < MAX_TRY_DEPTH) {
            /* Inner try frame catches re-throws from handler
             * so that finally still runs. */
            int         ic = mino_current_ctx(S)->call_depth;
            int         it = mino_current_ctx(S)->trace_added;
            int         is = mino_current_ctx(S)->try_depth; /* save before setjmp */
            dyn_frame_t *id = mino_current_ctx(S)->dyn_stack;
            mino_current_ctx(S)->try_stack[is].exception      = NULL;
            mino_current_ctx(S)->try_stack[is].saved_ns       = S->current_ns;
            mino_current_ctx(S)->try_stack[is].saved_ambient  = S->fn_ambient_ns;
            mino_current_ctx(S)->try_stack[is].saved_load_len = S->load_stack_len;
            if (setjmp(mino_current_ctx(S)->try_stack[is].buf) == 0) {
                mino_val_t *r;
                mino_current_ctx(S)->try_depth++;
                r = eval_implicit_do(S, clauses.catch_body, local);
                mino_current_ctx(S)->try_depth = is;
                if (r == NULL) {
                    eval_implicit_do(S, clauses.finally_body, env);
                    return NULL;
                }
                vol_result    = r;
                got_exception = 0;
            } else {
                /* Catch handler re-threw. */
                vol_ex      = mino_current_ctx(S)->try_stack[is].exception;
                S->current_ns    = mino_current_ctx(S)->try_stack[is].saved_ns;
                S->fn_ambient_ns = mino_current_ctx(S)->try_stack[is].saved_ambient;
                load_stack_truncate(S, mino_current_ctx(S)->try_stack[is].saved_load_len);
                mino_current_ctx(S)->try_depth   = is;
                mino_current_ctx(S)->call_depth  = ic;
                mino_current_ctx(S)->trace_added = it;
                while (mino_current_ctx(S)->dyn_stack != id) {
                    dyn_frame_t *f = mino_current_ctx(S)->dyn_stack;
                    mino_current_ctx(S)->dyn_stack = f->prev;
                    dyn_binding_list_free(f->bindings);
                }
                clear_error(S);
                /* got_exception stays 1, vol_ex updated. */
            }
        } else {
            /* No finally or nesting limit -- run catch directly. */
            mino_val_t *r =
                eval_implicit_do(S, clauses.catch_body, local);
            if (r == NULL) {
                if (clauses.has_finally)
                    eval_implicit_do(S, clauses.finally_body, env);
                return NULL;
            }
            vol_result    = r;
            got_exception = 0;
        }
    }

    /* Phase 3: run finally unconditionally. */
    if (clauses.has_finally) {
        eval_implicit_do(S, clauses.finally_body, env);
    }

    /* Phase 4: re-throw if exception was not handled. */
    if (got_exception) {
        mino_val_t *e = (mino_val_t *)vol_ex;
        if (mino_current_ctx(S)->try_depth > 0) {
            mino_current_ctx(S)->try_stack[mino_current_ctx(S)->try_depth - 1].exception = e;
            longjmp(mino_current_ctx(S)->try_stack[mino_current_ctx(S)->try_depth - 1].buf, 1);
        }
        if (e != NULL && e->type == MINO_STRING) {
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

    return (mino_val_t *)vol_result;
}
