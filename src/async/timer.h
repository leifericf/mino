/*
 * async_timer.h -- deadline-scheduled callbacks.
 *
 * (async-schedule-timer* ms cb) registers cb to run after ms milliseconds.
 * The scheduler drain checks timers cooperatively and enqueues any
 * expired callback on the run queue. This is the one C-side hook the
 * pure-mino (timeout ms) helper uses to arm a close! on a mino channel.
 */

#ifndef ASYNC_TIMER_H
#define ASYNC_TIMER_H

#include "mino_internal.h"

/* Timer queue entry. */
typedef struct timer_entry {
    double               deadline_ms;
    mino_val          *callback;
    mino_ref          *cb_ref;
    struct timer_entry  *next;
} timer_entry_t;

/* Schedule callback to fire after ms milliseconds. Returns 0 on success. */
int async_timer_schedule(mino_state *S, double ms, mino_val *callback);

/* Check and fire any expired timers.
 * Called from the scheduler drain loop. */
void async_timers_check(mino_state *S);

/* Free all timer entries. */
void async_timers_free(mino_state *S);

/* Mark timer values for GC. */
void async_timers_mark(mino_state *S);

#endif /* ASYNC_TIMER_H */
