/*
 * async_timer.h -- timeout channels for async operations.
 *
 * (timeout ms) returns a channel that closes after ms milliseconds.
 * Cooperative: timers are checked during scheduler drain.
 */

#ifndef ASYNC_TIMER_H
#define ASYNC_TIMER_H

#include "mino.h"

/* Timer queue entry. */
typedef struct timer_entry {
    double               deadline_ms;
    mino_val_t          *chan_handle;  /* the timeout channel */
    mino_ref_t          *chan_ref;
    struct timer_entry  *next;
} timer_entry_t;

/* Create a timeout channel that closes after ms milliseconds.
 * Returns a MINO_HANDLE channel value. */
mino_val_t *async_timeout(mino_state_t *S, double ms);

/* Check and fire any expired timers.
 * Called from the scheduler drain loop. */
void async_timers_check(mino_state_t *S);

/* Free all timer entries. */
void async_timers_free(mino_state_t *S);

/* Mark timer values for GC. */
void async_timers_mark(mino_state_t *S);

#endif /* ASYNC_TIMER_H */
