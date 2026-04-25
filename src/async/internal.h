/*
 * async_internal.h -- async subsystem umbrella header.
 *
 * Internal to the runtime; embedders should only use mino.h.
 *
 * Aggregates the per-component async headers so other subsystems can
 * pull in scheduler + timer types with one include.
 *
 * Error classes emitted (see diag/diag_contract.h):
 *
 *   MINO_ERR_RECOVERABLE -- scheduler.c, timer.c, prim/async.c.
 *      Arity / type errors on async primitives (drain!,
 *      async-sched-enqueue*, async-schedule-timer*) reach
 *      prim_throw_classified.  No CORRUPT or HOST paths -- the
 *      async surface is a thin layer over a cooperative run-queue
 *      that never escapes to the host.
 */

#ifndef ASYNC_INTERNAL_H
#define ASYNC_INTERNAL_H

#include "async/scheduler.h"
#include "async/timer.h"

#endif /* ASYNC_INTERNAL_H */
