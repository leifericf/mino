/*
 * async/state.h -- per-state async subsystem block: scheduler run
 * queue and deadline timer queue.
 *
 * Internal to the runtime; embedders should only use mino.h.
 */

#ifndef ASYNC_STATE_H
#define ASYNC_STATE_H

#include "async/scheduler.h"      /* sched_entry_t */
#include "async/timer.h"          /* timer_entry_t */

typedef struct async_state {
    /* Async scheduler run queue. */
    sched_entry_t  *run_head;
    sched_entry_t  *run_tail;

    /* Async deadline-sorted timer queue. */
    timer_entry_t  *timers;
} async_state_t;

#endif /* ASYNC_STATE_H */
