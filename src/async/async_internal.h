/*
 * async_internal.h -- async subsystem umbrella header.
 *
 * Internal to the runtime; embedders should only use mino.h.
 *
 * Aggregates the per-component async headers so other subsystems can
 * pull in scheduler + timer types with one include.
 */

#ifndef ASYNC_INTERNAL_H
#define ASYNC_INTERNAL_H

#include "async_scheduler.h"
#include "async_timer.h"

#endif /* ASYNC_INTERNAL_H */
