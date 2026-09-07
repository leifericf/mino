/*
 * async_scheduler.c -- callback scheduler for async operations.
 */

#include "async/scheduler.h"
#include "async/timer.h"
#include "runtime/internal.h"

/* Root walker: pin scheduler run-queue callbacks/values and timer
 * channel payloads. Registered with the GC at state init so gc/
 * enumerates async roots without including async internals. */
static void async_roots_mark(mino_state *S)
{
    sched_entry_t *e;
    for (e = S->async.run_head; e != NULL; e = e->next) {
        gc_mark_interior(S, e->callback);
        gc_mark_interior(S, e->value);
    }
    async_timers_mark(S);
}

void mino_async_register_gc_handlers(mino_state *S)
{
    gc_register_root_walker(S, async_roots_mark);
}

void async_sched_enqueue(mino_state *S, mino_val *callback,
                         mino_val *value)
{
    sched_entry_t *e = calloc(1, sizeof(*e));
    if (e == NULL) {
        set_eval_diag(S, mino_current_ctx(S)->eval_current_form, "internal", "MIN001", "out of memory in scheduler enqueue");
        return;
    }
    e->callback = callback;
    e->value    = value;
    e->cb_ref   = callback ? mino_root_new(S, callback) : NULL;
    e->val_ref  = value ? mino_root_new(S, value) : NULL;
    e->next     = NULL;

    if (S->async.run_tail) {
        S->async.run_tail->next = e;
    } else {
        S->async.run_head = e;
    }
    S->async.run_tail = e;
}

int async_sched_drain(mino_state *S, mino_env *env)
{
    int ran = 0;

    /* Check expired timers before draining. */
    async_timers_check(S);

    while (S->async.run_head != NULL) {
        sched_entry_t *e = S->async.run_head;
        mino_val *cb       = e->callback;
        mino_val *val      = e->value;
        mino_val *thrown_ex = NULL;
        int threw = 0;

        S->async.run_head = e->next;
        if (S->async.run_head == NULL)
            S->async.run_tail = NULL;

        if (cb != NULL) {
            mino_val *args;
            mino_val *result = NULL;
            int       pinned = 0;

            /* Pin cb and val so they survive the mino_cons allocation.
             * Guard the pin count to avoid aborting in sanitizer builds
             * when gc_save is at capacity: if the save array is full the
             * refs (e->cb_ref / e->val_ref) already root these values, so
             * skipping the pins is safe. */
            if (mino_current_ctx(S)->gc_save_len + 2 <= GC_SAVE_MAX) {
                gc_pin(cb);
                gc_pin(val);
                pinned = 1;
            }
            args = mino_cons(S, val ? val : mino_nil(S), NULL);
            /* Use pcall so a throw cannot skip the cleanup below. */
            if (mino_pcall(S, cb, args, env, &result, &thrown_ex) != 0) {
                threw = 1;
            }
            if (pinned) gc_unpin(2);
        }

        if (e->cb_ref)  mino_unroot(S, e->cb_ref);
        if (e->val_ref) mino_unroot(S, e->val_ref);
        free(e);
        ran = 1;

        /* Re-raise: callers that wrap async_sched_drain in a try frame
         * (or propagate upward) still see the exception; we just
         * guarantee the entry is freed before the longjmp. */
        if (threw) {
            mino_throw(S, thrown_ex);
        }
    }

    return ran;
}

