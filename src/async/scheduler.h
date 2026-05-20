/*
 * async_scheduler.h -- callback scheduler for async operations.
 *
 * The scheduler is a FIFO queue of callback+value pairs that are
 * executed by draining the queue after each top-level eval.
 */

#ifndef ASYNC_SCHEDULER_H
#define ASYNC_SCHEDULER_H

#include "mino_internal.h"

/* Scheduler run-queue entry. */
typedef struct sched_entry {
    mino_val         *callback;
    mino_val         *value;
    mino_ref         *cb_ref;
    mino_ref         *val_ref;
    struct sched_entry *next;
} sched_entry_t;

/* Enqueue a callback to be called with value during the next drain. */
void async_sched_enqueue(mino_state *S, mino_val *callback,
                         mino_val *value);

/* Drain the run queue: dequeue and call each entry.
 * Returns 1 if any entries were executed, 0 if queue was empty. */
int async_sched_drain(mino_state *S, mino_env *env);

/* Free all entries in the run queue without executing them. */
void async_sched_free(mino_state *S);

/* Mark all values in the run queue for GC. */
void async_sched_mark(mino_state *S);

#endif /* ASYNC_SCHEDULER_H */
