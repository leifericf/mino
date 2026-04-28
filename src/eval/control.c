/*
 * control.c -- try/catch/finally special form.
 */

#include "eval/special_internal.h"

mino_val_t *eval_try(mino_state_t *S, mino_val_t *form,
                     mino_val_t *args, mino_env_t *env, int tail)
{
    mino_val_t *body_head = NULL;
    mino_val_t *body_tail = NULL;
    mino_val_t *catch_body = NULL;
    mino_val_t *finally_body = NULL;
    char        var_buf[256];
    (void)tail;
    int         has_catch   = 0;
    int         has_finally = 0;
    int         saved_try   = S->ctx->try_depth;
    int         saved_call  = S->ctx->call_depth;
    int         saved_trace = S->ctx->trace_added;
    dyn_frame_t *saved_dyn  = S->ctx->dyn_stack;
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
                    set_eval_diag(S, form, "syntax", "MSY001",
                        "catch requires a binding symbol");
                    return NULL;
                }
                cv = clause->as.cons.cdr->as.cons.car;
                if (cv == NULL || cv->type != MINO_SYMBOL) {
                    set_eval_diag(S, form, "syntax", "MSY001",
                        "catch binding must be a symbol");
                    return NULL;
                }
                vl = cv->as.s.len;
                if (vl >= sizeof(var_buf)) {
                    set_eval_diag(S, form, "syntax", "MSY001",
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
                    mino_cons_cdr_set(S, body_tail, cell);
                }
                body_tail = cell;
            }
            rest = rest->as.cons.cdr;
        }
    }

    if (S->ctx->try_depth >= MAX_TRY_DEPTH) {
        set_eval_diag(S, form, "limit", "MLM002", "try nesting too deep");
        return NULL;
    }

    /* Phase 1: evaluate body forms. */
    S->ctx->try_stack[S->ctx->try_depth].exception      = NULL;
    S->ctx->try_stack[S->ctx->try_depth].saved_ns       = S->current_ns;
    S->ctx->try_stack[S->ctx->try_depth].saved_ambient  = S->fn_ambient_ns;
    S->ctx->try_stack[S->ctx->try_depth].saved_load_len = S->load_stack_len;
    if (setjmp(S->ctx->try_stack[S->ctx->try_depth].buf) == 0) {
        mino_val_t *r;
        S->ctx->try_depth++;
        r = eval_implicit_do(S, body_head, env);
        S->ctx->try_depth = saved_try;
        if (r == NULL) {
            /* Fatal runtime error. */
            if (has_finally)
                eval_implicit_do(S, finally_body, env);
            return NULL;
        }
        vol_result = r;
    } else {
        /* longjmp'd from throw in body. Restore current_ns and ambient
         * since the throw bypassed any per-fn restore on its way up. */
        vol_ex      = S->ctx->try_stack[saved_try].exception;
        S->current_ns    = S->ctx->try_stack[saved_try].saved_ns;
        S->fn_ambient_ns = S->ctx->try_stack[saved_try].saved_ambient;
        load_stack_truncate(S, S->ctx->try_stack[saved_try].saved_load_len);
        S->ctx->try_depth   = saved_try;
        S->ctx->call_depth  = saved_call;
        S->ctx->trace_added = saved_trace;
        while (S->ctx->dyn_stack != saved_dyn) {
            dyn_frame_t *f = S->ctx->dyn_stack;
            S->ctx->dyn_stack = f->prev;
            dyn_binding_list_free(f->bindings);
        }
        clear_error(S);
        got_exception = 1;
    }

    /* Phase 2: run catch handler if we caught an exception. */
    if (got_exception && has_catch) {
        mino_val_t *ex_val =
            vol_ex ? (mino_val_t *)vol_ex : mino_nil(S);

        /* Normalize: wrap non-diagnostic values into a diagnostic map
         * so catch handlers always receive a structured error value. */
        if (ex_val->type != MINO_MAP
            || map_get_val(ex_val, mino_keyword(S, "mino/kind")) == NULL) {
            mino_val_t *keys[5], *vals[5];
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
            ex_val = mino_map(S, keys, vals, 5);
        }

        {
        mino_env_t *local  = env_child(S, env);
        env_bind(S, local, var_buf, ex_val);

        if (has_finally && S->ctx->try_depth < MAX_TRY_DEPTH) {
            /* Inner try frame catches re-throws from handler
             * so that finally still runs. */
            int         ic = S->ctx->call_depth;
            int         it = S->ctx->trace_added;
            int         is = S->ctx->try_depth; /* save before setjmp */
            dyn_frame_t *id = S->ctx->dyn_stack;
            S->ctx->try_stack[is].exception      = NULL;
            S->ctx->try_stack[is].saved_ns       = S->current_ns;
            S->ctx->try_stack[is].saved_ambient  = S->fn_ambient_ns;
            S->ctx->try_stack[is].saved_load_len = S->load_stack_len;
            if (setjmp(S->ctx->try_stack[is].buf) == 0) {
                mino_val_t *r;
                S->ctx->try_depth++;
                r = eval_implicit_do(S, catch_body, local);
                S->ctx->try_depth = is;
                if (r == NULL) {
                    eval_implicit_do(S, finally_body, env);
                    return NULL;
                }
                vol_result    = r;
                got_exception = 0;
            } else {
                /* Catch handler re-threw. */
                vol_ex      = S->ctx->try_stack[is].exception;
                S->current_ns    = S->ctx->try_stack[is].saved_ns;
                S->fn_ambient_ns = S->ctx->try_stack[is].saved_ambient;
                load_stack_truncate(S, S->ctx->try_stack[is].saved_load_len);
                S->ctx->try_depth   = is;
                S->ctx->call_depth  = ic;
                S->ctx->trace_added = it;
                while (S->ctx->dyn_stack != id) {
                    dyn_frame_t *f = S->ctx->dyn_stack;
                    S->ctx->dyn_stack = f->prev;
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
        } /* end normalization block */
    }

    /* Phase 3: run finally unconditionally. */
    if (has_finally) {
        eval_implicit_do(S, finally_body, env);
    }

    /* Phase 4: re-throw if exception was not handled. */
    if (got_exception) {
        mino_val_t *e = (mino_val_t *)vol_ex;
        if (S->ctx->try_depth > 0) {
            S->ctx->try_stack[S->ctx->try_depth - 1].exception = e;
            longjmp(S->ctx->try_stack[S->ctx->try_depth - 1].buf, 1);
        }
        if (e != NULL && e->type == MINO_STRING) {
            char msg[512];
            snprintf(msg, sizeof(msg),
                     "unhandled exception: %.*s",
                     (int)e->as.s.len, e->as.s.data);
            set_eval_diag(S, S->ctx->eval_current_form, "user", "MUS001", msg);
        } else {
            set_eval_diag(S, S->ctx->eval_current_form, "internal", "MIN001", "unhandled exception");
        }
        return NULL;
    }

    return (mino_val_t *)vol_result;
}
