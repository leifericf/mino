/*
 * async_scheduler.h -- callback scheduler for async operations.
 *
 * The scheduler is a FIFO queue of callback+value pairs that are
 * executed by draining the queue after each top-level eval.
 */

#ifndef ASYNC_SCHEDULER_H
#define ASYNC_SCHEDULER_H

#include "mino.h"

/* Scheduler run-queue entry. */
typedef struct sched_entry {
    mino_val_t         *callback;
    mino_val_t         *value;
    mino_ref_t         *cb_ref;
    mino_ref_t         *val_ref;
    struct sched_entry *next;
} sched_entry_t;

/* Enqueue a callback to be called with value during the next drain. */
void async_sched_enqueue(mino_state_t *S, mino_val_t *callback,
                         mino_val_t *value);

/* Drain the run queue: dequeue and call each entry.
 * Returns 1 if any entries were executed, 0 if queue was empty. */
int async_sched_drain(mino_state_t *S, mino_env_t *env);

/* Free all entries in the run queue without executing them. */
void async_sched_free(mino_state_t *S);

/* Mark all values in the run queue for GC. */
void async_sched_mark(mino_state_t *S);

#endif /* ASYNC_SCHEDULER_H */
