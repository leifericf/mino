/*
 * prim_async.c -- async channel primitives exposed to the mino runtime.
 */

#include "prim_internal.h"
#include "async_buffer.h"
#include "async_channel.h"
#include "async_scheduler.h"
#include "async_select.h"

/* ------------------------------------------------------------------ */
/* Buffer constructors                                                */
/* ------------------------------------------------------------------ */

static mino_val_t *prim_buf_fixed(mino_state_t *S, mino_val_t *args,
                                  mino_env_t *env)
{
    long long n;
    mino_async_buf_t *buf;
    (void)env;
    if (args == NULL || args->type != MINO_CONS) {
        set_error(S, "buffer requires a capacity argument");
        return NULL;
    }
    if (!as_long(args->as.cons.car, &n) || n <= 0) {
        set_error(S, "buffer capacity must be a positive integer");
        return NULL;
    }
    buf = async_buf_create(S, ASYNC_BUF_FIXED, (size_t)n);
    if (buf == NULL) {
        set_error(S, "out of memory creating buffer");
        return NULL;
    }
    return mino_handle(S, buf, "async/buf");
}

static mino_val_t *prim_buf_dropping(mino_state_t *S, mino_val_t *args,
                                     mino_env_t *env)
{
    long long n;
    mino_async_buf_t *buf;
    (void)env;
    if (args == NULL || args->type != MINO_CONS) {
        set_error(S, "dropping-buffer requires a capacity argument");
        return NULL;
    }
    if (!as_long(args->as.cons.car, &n) || n <= 0) {
        set_error(S, "buffer capacity must be a positive integer");
        return NULL;
    }
    buf = async_buf_create(S, ASYNC_BUF_DROPPING, (size_t)n);
    if (buf == NULL) {
        set_error(S, "out of memory creating buffer");
        return NULL;
    }
    return mino_handle(S, buf, "async/buf");
}

static mino_val_t *prim_buf_sliding(mino_state_t *S, mino_val_t *args,
                                    mino_env_t *env)
{
    long long n;
    mino_async_buf_t *buf;
    (void)env;
    if (args == NULL || args->type != MINO_CONS) {
        set_error(S, "sliding-buffer requires a capacity argument");
        return NULL;
    }
    if (!as_long(args->as.cons.car, &n) || n <= 0) {
        set_error(S, "buffer capacity must be a positive integer");
        return NULL;
    }
    buf = async_buf_create(S, ASYNC_BUF_SLIDING, (size_t)n);
    if (buf == NULL) {
        set_error(S, "out of memory creating buffer");
        return NULL;
    }
    return mino_handle(S, buf, "async/buf");
}

static mino_val_t *prim_buf_promise(mino_state_t *S, mino_val_t *args,
                                    mino_env_t *env)
{
    mino_async_buf_t *buf;
    (void)env;
    (void)args;
    buf = async_buf_create(S, ASYNC_BUF_PROMISE, 1);
    if (buf == NULL) {
        set_error(S, "out of memory creating promise buffer");
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
            set_error(S, "chan* expects a buffer handle or nil");
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
        set_error(S, "chan-put* requires channel and value");
        return NULL;
    }
    ch_val = args->as.cons.car;
    val    = args->as.cons.cdr->as.cons.car;

    ch = async_chan_get(ch_val);
    if (ch == NULL) {
        set_error(S, "chan-put* first argument must be a channel");
        return NULL;
    }
    if (val->type == MINO_NIL) {
        return prim_throw_error(S, "cannot put nil on a channel");
    }

    /* Optional callback. */
    if (args->as.cons.cdr->as.cons.cdr != NULL &&
        args->as.cons.cdr->as.cons.cdr->type == MINO_CONS) {
        cb = args->as.cons.cdr->as.cons.cdr->as.cons.car;
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
        set_error(S, "chan-take* requires a channel argument");
        return NULL;
    }
    ch_val = args->as.cons.car;

    ch = async_chan_get(ch_val);
    if (ch == NULL) {
        set_error(S, "chan-take* first argument must be a channel");
        return NULL;
    }

    /* Optional callback. */
    if (args->as.cons.cdr != NULL && args->as.cons.cdr->type == MINO_CONS) {
        cb = args->as.cons.cdr->as.cons.car;
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
        set_error(S, "chan-close* requires a channel argument");
        return NULL;
    }
    ch_val = args->as.cons.car;

    ch = async_chan_get(ch_val);
    if (ch == NULL) {
        set_error(S, "chan-close* argument must be a channel");
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
        set_error(S, "chan-closed?* requires a channel argument");
        return NULL;
    }
    ch_val = args->as.cons.car;

    ch = async_chan_get(ch_val);
    if (ch == NULL) {
        set_error(S, "chan-closed?* argument must be a channel");
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
        set_error(S, "offer!* requires channel and value");
        return NULL;
    }
    ch_val = args->as.cons.car;
    val    = args->as.cons.cdr->as.cons.car;

    ch = async_chan_get(ch_val);
    if (ch == NULL) {
        set_error(S, "offer!* first argument must be a channel");
        return NULL;
    }
    if (val->type == MINO_NIL) {
        return prim_throw_error(S, "cannot put nil on a channel");
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
        set_error(S, "poll!* requires a channel argument");
        return NULL;
    }
    ch_val = args->as.cons.car;

    ch = async_chan_get(ch_val);
    if (ch == NULL) {
        set_error(S, "poll!* argument must be a channel");
        return NULL;
    }

    return async_chan_poll(S, ch);
}

/* ------------------------------------------------------------------ */
/* Channel predicate                                                  */
/* ------------------------------------------------------------------ */

static mino_val_t *prim_chan_p(mino_state_t *S, mino_val_t *args,
                              mino_env_t *env)
{
    (void)env;
    if (args == NULL || args->type != MINO_CONS) {
        set_error(S, "chan?* requires one argument");
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
        set_error(S, "alts* requires ops, opts, and callback");
        return NULL;
    }
    ops = args->as.cons.car;

    if (args->as.cons.cdr == NULL || args->as.cons.cdr->type != MINO_CONS) {
        set_error(S, "alts* requires opts argument");
        return NULL;
    }
    opts = args->as.cons.cdr->as.cons.car;

    if (args->as.cons.cdr->as.cons.cdr == NULL ||
        args->as.cons.cdr->as.cons.cdr->type != MINO_CONS) {
        set_error(S, "alts* requires callback argument");
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
/* Scheduler primitives                                               */
/* ------------------------------------------------------------------ */

static mino_val_t *prim_drain(mino_state_t *S, mino_val_t *args,
                              mino_env_t *env)
{
    (void)args;
    async_sched_drain(S, env);
    return mino_nil(S);
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
    DEF_PRIM(env, "alts*",        prim_alts,           "Alts arbitration primitive.");

    /* Scheduler */
    DEF_PRIM(env, "drain!",       prim_drain,          "Drain the async run queue.");
}
