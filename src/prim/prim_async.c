/*
 * prim_async.c -- minimal C surface for the async scheduler.
 *
 * Channels, buffers, and alts live in lib/core/channel.mino. The three
 * things that still need to touch C are:
 *
 *   1. The scheduler run queue. Callbacks for any pending channel op or
 *      timer go through this queue so the mino-level code and host
 *      embedders share a single drain surface.
 *   2. Deadline timers. Arming a callback to fire "N ms from now"
 *      needs host nanosecond time and a priority queue; both are small
 *      and naturally C-shaped.
 *   3. The (drain! / drain-loop!) public API for host embedders that
 *      drive the event loop from outside mino.
 *
 * That's it: four primitives.
 */

#include "runtime_internal.h"
#include "prim_internal.h"
#include "async_scheduler.h"
#include "async_timer.h"

static mino_val_t *prim_sched_enqueue(mino_state_t *S, mino_val_t *args,
                                      mino_env_t *env)
{
    mino_val_t *cb, *val;
    (void)env;

    if (args == NULL || args->type != MINO_CONS ||
        args->as.cons.cdr == NULL) {
        set_eval_diag(S, S->eval_current_form, "eval/arity", "MAR001",
                      "async-sched-enqueue* requires callback and value");
        return NULL;
    }
    cb  = args->as.cons.car;
    val = args->as.cons.cdr->as.cons.car;

    if (cb != NULL && cb->type == MINO_NIL) cb = NULL;
    if (cb != NULL) async_sched_enqueue(S, cb, val);
    return mino_nil(S);
}

static mino_val_t *prim_timer_schedule(mino_state_t *S, mino_val_t *args,
                                       mino_env_t *env)
{
    double ms;
    mino_val_t *cb;
    (void)env;

    if (args == NULL || args->type != MINO_CONS ||
        args->as.cons.cdr == NULL) {
        set_eval_diag(S, S->eval_current_form, "eval/arity", "MAR001",
                      "async-schedule-timer* requires ms and callback");
        return NULL;
    }
    if (!as_double(args->as.cons.car, &ms)) {
        set_eval_diag(S, S->eval_current_form, "eval/type", "MTY001",
                      "async-schedule-timer* first argument must be a number");
        return NULL;
    }
    if (ms < 0) ms = 0;
    cb = args->as.cons.cdr->as.cons.car;
    if (cb != NULL && cb->type == MINO_NIL) cb = NULL;

    if (async_timer_schedule(S, ms, cb) != 0) return NULL;
    return mino_nil(S);
}

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
        set_eval_diag(S, S->eval_current_form, "eval/arity", "MAR001",
                      "drain-loop! requires a done-check thunk");
        return NULL;
    }
    done_thunk = args->as.cons.car;
    gc_pin(done_thunk);

    for (;;) {
        int progress = async_sched_drain(S, env);

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

void mino_install_async(mino_state_t *S, mino_env_t *env)
{
    DEF_PRIM(env, "async-sched-enqueue*", prim_sched_enqueue,
             "Enqueue a callback on the async scheduler run queue.");
    DEF_PRIM(env, "async-schedule-timer*", prim_timer_schedule,
             "Schedule a callback to fire after ms milliseconds.");
    DEF_PRIM(env, "drain!", prim_drain,
             "Drain the async run queue once.");
    DEF_PRIM(env, "drain-loop!", prim_drain_loop,
             "Drain until done-thunk returns truthy or no progress.");
}
