/*
 * async_scheduler.c -- callback scheduler for async operations.
 */

#include "async_scheduler.h"
#include "async_timer.h"
#include "mino_internal.h"

void async_sched_enqueue(mino_state_t *S, mino_val_t *callback,
                         mino_val_t *value)
{
    sched_entry_t *e = calloc(1, sizeof(*e));
    if (e == NULL) {
        set_eval_diag(S, S->eval_current_form, "internal", "MIN001", "out of memory in scheduler enqueue");
        return;
    }
    e->callback = callback;
    e->value    = value;
    e->cb_ref   = callback ? mino_ref(S, callback) : NULL;
    e->val_ref  = value ? mino_ref(S, value) : NULL;
    e->next     = NULL;

    if (S->async_run_tail) {
        S->async_run_tail->next = e;
    } else {
        S->async_run_head = e;
    }
    S->async_run_tail = e;
}

int async_sched_drain(mino_state_t *S, mino_env_t *env)
{
    int ran = 0;

    /* Check expired timers before draining. */
    async_timers_check(S);

    while (S->async_run_head != NULL) {
        sched_entry_t *e = S->async_run_head;
        mino_val_t *cb  = e->callback;
        mino_val_t *val = e->value;

        S->async_run_head = e->next;
        if (S->async_run_head == NULL)
            S->async_run_tail = NULL;

        if (cb != NULL) {
            mino_val_t *args;
            /* Pin cb and val: once dequeued they may only be in registers,
             * invisible to the conservative stack scanner when mino_cons
             * or mino_call trigger a GC collection. */
            gc_pin(cb);
            gc_pin(val);
            args = mino_cons(S, val ? val : mino_nil(S), NULL);
            mino_call(S, cb, args, env);
            gc_unpin(2);
        }

        if (e->cb_ref)  mino_unref(S, e->cb_ref);
        if (e->val_ref) mino_unref(S, e->val_ref);
        free(e);
        ran = 1;
    }

    return ran;
}

void async_sched_free(mino_state_t *S)
{
    sched_entry_t *e = S->async_run_head;
    while (e != NULL) {
        sched_entry_t *next = e->next;
        if (e->cb_ref)  mino_unref(S, e->cb_ref);
        if (e->val_ref) mino_unref(S, e->val_ref);
        free(e);
        e = next;
    }
    S->async_run_head = NULL;
    S->async_run_tail = NULL;
}

void async_sched_mark(mino_state_t *S)
{
    sched_entry_t *e;
    for (e = S->async_run_head; e != NULL; e = e->next) {
        if (e->callback) gc_mark_interior(S, e->callback);
        if (e->value)    gc_mark_interior(S, e->value);
    }
}
