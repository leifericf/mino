/*
 * prim_async.c -- async channel primitives exposed to the mino runtime.
 */

#include "prim_internal.h"
#include "async_buffer.h"
#include "async_channel.h"
#include "async_scheduler.h"
#include "async_select.h"
#include "async_timer.h"

/* ------------------------------------------------------------------ */
/* Buffer constructors                                                */
/* ------------------------------------------------------------------ */

static mino_val_t *buf_ctor(mino_state_t *S, mino_val_t *args, int kind)
{
    long long n;
    mino_async_buf_t *buf;
    if (args == NULL || args->type != MINO_CONS) {
        set_eval_diag(S, S->eval_current_form, "eval/arity", "MAR001", "buffer requires a capacity argument");
        return NULL;
    }
    if (!as_long(args->as.cons.car, &n) || n <= 0) {
        set_eval_diag(S, S->eval_current_form, "eval/type", "MTY001", "buffer capacity must be a positive integer");
        return NULL;
    }
    buf = async_buf_create(S, kind, (size_t)n);
    if (buf == NULL) {
        set_eval_diag(S, S->eval_current_form, "internal", "MIN001", "out of memory creating buffer");
        return NULL;
    }
    return mino_handle(S, buf, "async/buf");
}

static mino_val_t *prim_buf_fixed(mino_state_t *S, mino_val_t *args,
                                  mino_env_t *env)
{
    (void)env;
    return buf_ctor(S, args, ASYNC_BUF_FIXED);
}

static mino_val_t *prim_buf_dropping(mino_state_t *S, mino_val_t *args,
                                     mino_env_t *env)
{
    (void)env;
    return buf_ctor(S, args, ASYNC_BUF_DROPPING);
}

static mino_val_t *prim_buf_sliding(mino_state_t *S, mino_val_t *args,
                                    mino_env_t *env)
{
    (void)env;
    return buf_ctor(S, args, ASYNC_BUF_SLIDING);
}

static mino_val_t *prim_buf_promise(mino_state_t *S, mino_val_t *args,
                                    mino_env_t *env)
{
    mino_async_buf_t *buf;
    (void)env;
    (void)args;
    buf = async_buf_create(S, ASYNC_BUF_PROMISE, 1);
    if (buf == NULL) {
        set_eval_diag(S, S->eval_current_form, "internal", "MIN001", "out of memory creating promise buffer");
        return NULL;
    }
    return mino_handle(S, buf, "async/buf");
}

/* ------------------------------------------------------------------ */
/* Channel primitives                                                 */
/* ------------------------------------------------------------------ */

static mino_val_t *prim_chan(mino_state_t *S, mino_val_t *args,
                            mino_env_t *env)
{
    mino_async_buf_t *buf = NULL;
    (void)env;

    /* (chan*) -- unbuffered */
    /* (chan* buf-handle) -- buffered */
    if (args != NULL && args->type == MINO_CONS) {
        mino_val_t *bv = args->as.cons.car;
        if (bv->type == MINO_HANDLE &&
            strcmp(mino_handle_tag(bv), "async/buf") == 0) {
            buf = (mino_async_buf_t *)mino_handle_ptr(bv);
        } else if (bv->type != MINO_NIL) {
            set_eval_diag(S, S->eval_current_form, "eval/type", "MTY001", "chan* expects a buffer handle or nil");
            return NULL;
        }
    }

    return async_chan_create(S, buf);
}

static mino_val_t *prim_chan_put(mino_state_t *S, mino_val_t *args,
                                mino_env_t *env)
{
    mino_val_t *ch_val, *val, *cb = NULL;
    mino_async_chan_t *ch;
    (void)env;

    if (args == NULL || args->type != MINO_CONS ||
        args->as.cons.cdr == NULL) {
        set_eval_diag(S, S->eval_current_form, "eval/arity", "MAR001", "chan-put* requires channel and value");
        return NULL;
    }
    ch_val = args->as.cons.car;
    val    = args->as.cons.cdr->as.cons.car;

    ch = async_chan_get(ch_val);
    if (ch == NULL) {
        set_eval_diag(S, S->eval_current_form, "eval/type", "MTY001", "chan-put* first argument must be a channel");
        return NULL;
    }
    if (val->type == MINO_NIL) {
        return prim_throw_classified(S, "eval/contract", "MCT001", "cannot put nil on a channel");
    }

    /* Optional callback. */
    if (args->as.cons.cdr->as.cons.cdr != NULL &&
        args->as.cons.cdr->as.cons.cdr->type == MINO_CONS) {
        cb = args->as.cons.cdr->as.cons.cdr->as.cons.car;
        if (cb != NULL && cb->type == MINO_NIL) cb = NULL;
    }

    async_chan_put(S, ch, val, cb);
    return mino_nil(S);
}

static mino_val_t *prim_chan_take(mino_state_t *S, mino_val_t *args,
                                 mino_env_t *env)
{
    mino_val_t *ch_val, *cb = NULL;
    mino_async_chan_t *ch;
    (void)env;

    if (args == NULL || args->type != MINO_CONS) {
        set_eval_diag(S, S->eval_current_form, "eval/arity", "MAR001", "chan-take* requires a channel argument");
        return NULL;
    }
    ch_val = args->as.cons.car;

    ch = async_chan_get(ch_val);
    if (ch == NULL) {
        set_eval_diag(S, S->eval_current_form, "eval/type", "MTY001", "chan-take* first argument must be a channel");
        return NULL;
    }

    /* Optional callback. */
    if (args->as.cons.cdr != NULL && args->as.cons.cdr->type == MINO_CONS) {
        cb = args->as.cons.cdr->as.cons.car;
        if (cb != NULL && cb->type == MINO_NIL) cb = NULL;
    }

    async_chan_take(S, ch, cb);
    return mino_nil(S);
}

static mino_val_t *prim_chan_close(mino_state_t *S, mino_val_t *args,
                                  mino_env_t *env)
{
    mino_val_t *ch_val;
    mino_async_chan_t *ch;
    (void)env;

    if (args == NULL || args->type != MINO_CONS) {
        set_eval_diag(S, S->eval_current_form, "eval/arity", "MAR001", "chan-close* requires a channel argument");
        return NULL;
    }
    ch_val = args->as.cons.car;

    ch = async_chan_get(ch_val);
    if (ch == NULL) {
        set_eval_diag(S, S->eval_current_form, "eval/type", "MTY001", "chan-close* argument must be a channel");
        return NULL;
    }

    async_chan_close(S, ch);
    return mino_nil(S);
}

static mino_val_t *prim_chan_closed_p(mino_state_t *S, mino_val_t *args,
                                     mino_env_t *env)
{
    mino_val_t *ch_val;
    mino_async_chan_t *ch;
    (void)env;

    if (args == NULL || args->type != MINO_CONS) {
        set_eval_diag(S, S->eval_current_form, "eval/arity", "MAR001", "chan-closed?* requires a channel argument");
        return NULL;
    }
    ch_val = args->as.cons.car;

    ch = async_chan_get(ch_val);
    if (ch == NULL) {
        set_eval_diag(S, S->eval_current_form, "eval/type", "MTY001", "chan-closed?* argument must be a channel");
        return NULL;
    }

    return async_chan_closed(ch) ? mino_true(S) : mino_false(S);
}

static mino_val_t *prim_chan_offer(mino_state_t *S, mino_val_t *args,
                                  mino_env_t *env)
{
    mino_val_t *ch_val, *val;
    mino_async_chan_t *ch;
    (void)env;

    if (args == NULL || args->type != MINO_CONS ||
        args->as.cons.cdr == NULL) {
        set_eval_diag(S, S->eval_current_form, "eval/arity", "MAR001", "offer!* requires channel and value");
        return NULL;
    }
    ch_val = args->as.cons.car;
    val    = args->as.cons.cdr->as.cons.car;

    ch = async_chan_get(ch_val);
    if (ch == NULL) {
        set_eval_diag(S, S->eval_current_form, "eval/type", "MTY001", "offer!* first argument must be a channel");
        return NULL;
    }
    if (val->type == MINO_NIL) {
        return prim_throw_classified(S, "eval/contract", "MCT001", "cannot put nil on a channel");
    }

    return async_chan_offer(S, ch, val) ? mino_true(S) : mino_false(S);
}

static mino_val_t *prim_chan_poll(mino_state_t *S, mino_val_t *args,
                                 mino_env_t *env)
{
    mino_val_t *ch_val;
    mino_async_chan_t *ch;
    (void)env;

    if (args == NULL || args->type != MINO_CONS) {
        set_eval_diag(S, S->eval_current_form, "eval/arity", "MAR001", "poll!* requires a channel argument");
        return NULL;
    }
    ch_val = args->as.cons.car;

    ch = async_chan_get(ch_val);
    if (ch == NULL) {
        set_eval_diag(S, S->eval_current_form, "eval/type", "MTY001", "poll!* argument must be a channel");
        return NULL;
    }

    return async_chan_poll(S, ch);
}

/* ------------------------------------------------------------------ */
/* Transducer support primitives                                      */
/* ------------------------------------------------------------------ */

/* (chan-set-xform* ch rf ex-handler)
 * Set the transducer reducing function and exception handler on a channel.
 * Both rf and ex-handler may be nil. */
static mino_val_t *prim_chan_set_xform(mino_state_t *S, mino_val_t *args,
                                       mino_env_t *env)
{
    mino_val_t *ch_val, *xform, *ex_handler = NULL;
    mino_async_chan_t *ch;
    (void)env;

    if (args == NULL || args->type != MINO_CONS ||
        args->as.cons.cdr == NULL) {
        set_eval_diag(S, S->eval_current_form, "eval/arity", "MAR001", "chan-set-xform* requires channel and rf");
        return NULL;
    }
    ch_val = args->as.cons.car;
    xform  = args->as.cons.cdr->as.cons.car;

    ch = async_chan_get(ch_val);
    if (ch == NULL) {
        set_eval_diag(S, S->eval_current_form, "eval/type", "MTY001", "chan-set-xform* first argument must be a channel");
        return NULL;
    }

    if (xform->type == MINO_NIL) xform = NULL;

    /* Optional ex-handler. */
    if (args->as.cons.cdr->as.cons.cdr != NULL &&
        args->as.cons.cdr->as.cons.cdr->type == MINO_CONS) {
        ex_handler = args->as.cons.cdr->as.cons.cdr->as.cons.car;
        if (ex_handler->type == MINO_NIL) ex_handler = NULL;
    }

    async_chan_set_xform(S, ch, xform, ex_handler);
    return mino_nil(S);
}

/* (chan-buf-add* ch val)
 * Add a value directly to the channel's buffer, bypassing the
 * transducer. Used as the reducing function's step operation. */
static mino_val_t *prim_chan_buf_add(mino_state_t *S, mino_val_t *args,
                                     mino_env_t *env)
{
    mino_val_t *ch_val, *val;
    mino_async_chan_t *ch;
    (void)env;

    if (args == NULL || args->type != MINO_CONS ||
        args->as.cons.cdr == NULL) {
        set_eval_diag(S, S->eval_current_form, "eval/arity", "MAR001", "chan-buf-add* requires channel and value");
        return NULL;
    }
    ch_val = args->as.cons.car;
    val    = args->as.cons.cdr->as.cons.car;

    ch = async_chan_get(ch_val);
    if (ch == NULL) {
        set_eval_diag(S, S->eval_current_form, "eval/type", "MTY001", "chan-buf-add* first argument must be a channel");
        return NULL;
    }

    async_chan_buf_add(S, ch, val);
    return mino_nil(S);
}

/* ------------------------------------------------------------------ */
/* Channel predicate                                                  */
/* ------------------------------------------------------------------ */

static mino_val_t *prim_chan_p(mino_state_t *S, mino_val_t *args,
                              mino_env_t *env)
{
    (void)env;
    if (args == NULL || args->type != MINO_CONS) {
        set_eval_diag(S, S->eval_current_form, "eval/arity", "MAR001", "chan?* requires one argument");
        return NULL;
    }
    return async_chan_get(args->as.cons.car) != NULL
        ? mino_true(S) : mino_false(S);
}

/* ------------------------------------------------------------------ */
/* Alts primitive                                                     */
/* ------------------------------------------------------------------ */

static mino_val_t *prim_alts(mino_state_t *S, mino_val_t *args,
                             mino_env_t *env)
{
    mino_val_t *ops, *opts, *callback;
    int result;

    if (args == NULL || args->type != MINO_CONS) {
        set_eval_diag(S, S->eval_current_form, "eval/arity", "MAR001", "alts* requires ops, opts, and callback");
        return NULL;
    }
    ops = args->as.cons.car;

    if (args->as.cons.cdr == NULL || args->as.cons.cdr->type != MINO_CONS) {
        set_eval_diag(S, S->eval_current_form, "eval/arity", "MAR001", "alts* requires opts argument");
        return NULL;
    }
    opts = args->as.cons.cdr->as.cons.car;

    if (args->as.cons.cdr->as.cons.cdr == NULL ||
        args->as.cons.cdr->as.cons.cdr->type != MINO_CONS) {
        set_eval_diag(S, S->eval_current_form, "eval/arity", "MAR001", "alts* requires callback argument");
        return NULL;
    }
    callback = args->as.cons.cdr->as.cons.cdr->as.cons.car;

    result = async_do_alts(S, env, ops, opts, callback);
    /* Return the result code as an integer:
     *  1 = completed immediately
     *  0 = error (already set)
     * -1 = no immediate completion, needs pending setup */
    return mino_int(S, result);
}

/* ------------------------------------------------------------------ */
/* Timer primitive                                                    */
/* ------------------------------------------------------------------ */

static mino_val_t *prim_timeout(mino_state_t *S, mino_val_t *args,
                                mino_env_t *env)
{
    double ms;
    (void)env;

    if (args == NULL || args->type != MINO_CONS) {
        set_eval_diag(S, S->eval_current_form, "eval/arity", "MAR001", "timeout* requires a millisecond argument");
        return NULL;
    }
    if (!as_double(args->as.cons.car, &ms)) {
        set_eval_diag(S, S->eval_current_form, "eval/type", "MTY001", "timeout* argument must be a number");
        return NULL;
    }
    if (ms < 0) ms = 0;

    return async_timeout(S, ms);
}

/* ------------------------------------------------------------------ */
/* Scheduler primitives                                               */
/* ------------------------------------------------------------------ */

static mino_val_t *prim_drain(mino_state_t *S, mino_val_t *args,
                              mino_env_t *env)
{
    (void)args;
    async_sched_drain(S, env);
    return mino_nil(S);
}

/* (drain-loop! done-thunk)
 * Repeatedly drains the scheduler and checks timers until either:
 *   (a) (done-thunk) returns truthy, or
 *   (b) a full pass produces no progress (no callbacks ran, no timers
 *       fired) -- meaning further draining cannot help.
 * Returns true if done-thunk returned truthy, false if no progress. */
static mino_val_t *prim_drain_loop(mino_state_t *S, mino_val_t *args,
                                   mino_env_t *env)
{
    mino_val_t *done_thunk, *result;

    if (args == NULL || args->type != MINO_CONS) {
        set_eval_diag(S, S->eval_current_form, "eval/arity", "MAR001", "drain-loop! requires a done-check thunk");
        return NULL;
    }
    done_thunk = args->as.cons.car;
    gc_pin(done_thunk);

    for (;;) {
        int progress = async_sched_drain(S, env);

        /* Check if done. */
        result = mino_call(S, done_thunk, NULL, env);
        if (result != NULL && result->type != MINO_NIL &&
            !(result->type == MINO_BOOL && !result->as.b)) {
            gc_unpin(1);
            return mino_true(S);
        }

        if (!progress) {
            gc_unpin(1);
            return mino_false(S);
        }
    }
}

/* ------------------------------------------------------------------ */
/* Registration                                                       */
/* ------------------------------------------------------------------ */

void mino_install_async(mino_state_t *S, mino_env_t *env)
{
    /* Buffer constructors */
    DEF_PRIM(env, "buf-fixed*",    prim_buf_fixed,    "Create a fixed-size buffer.");
    DEF_PRIM(env, "buf-dropping*", prim_buf_dropping,  "Create a dropping buffer.");
    DEF_PRIM(env, "buf-sliding*",  prim_buf_sliding,   "Create a sliding buffer.");
    DEF_PRIM(env, "buf-promise*",  prim_buf_promise,   "Create a promise buffer.");

    /* Channel operations */
    DEF_PRIM(env, "chan*",         prim_chan,           "Create a channel.");
    DEF_PRIM(env, "chan?*",        prim_chan_p,         "Check if value is a channel.");
    DEF_PRIM(env, "chan-put*",     prim_chan_put,       "Put a value on a channel.");
    DEF_PRIM(env, "chan-take*",    prim_chan_take,      "Take a value from a channel.");
    DEF_PRIM(env, "chan-close*",   prim_chan_close,     "Close a channel.");
    DEF_PRIM(env, "chan-closed?*", prim_chan_closed_p,  "Check if a channel is closed.");
    DEF_PRIM(env, "offer!*",      prim_chan_offer,     "Non-blocking put on a channel.");
    DEF_PRIM(env, "poll!*",       prim_chan_poll,       "Non-blocking take from a channel.");
    DEF_PRIM(env, "chan-set-xform*", prim_chan_set_xform, "Set transducer on channel.");
    DEF_PRIM(env, "chan-buf-add*",   prim_chan_buf_add,   "Raw buffer add (for xform rf).");
    DEF_PRIM(env, "alts*",        prim_alts,           "Alts arbitration primitive.");
    DEF_PRIM(env, "timeout*",     prim_timeout,        "Create a timeout channel.");

    /* Scheduler */
    DEF_PRIM(env, "drain!",       prim_drain,          "Drain the async run queue.");
    DEF_PRIM(env, "drain-loop!",  prim_drain_loop,     "Drain until done or no progress.");
}
