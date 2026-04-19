/*
 * async_select.h -- alts arbitration for async channels.
 *
 * Implements the alts! operation: given a vector of channel operations,
 * exactly one commits and delivers its result.
 */

#ifndef ASYNC_SELECT_H
#define ASYNC_SELECT_H

#include "mino.h"

/* Perform alts operation.
 * ops is a vector of channel operations:
 *   - a channel value means "take from this channel"
 *   - a vector [ch val] means "put val on ch"
 * opts is a map with optional :priority and :default keys.
 * callback is called with [val ch] on completion.
 *
 * Returns 1 on success (immediate or pending registered),
 * 0 on error. */
int async_do_alts(mino_state_t *S, mino_env_t *env,
                  mino_val_t *ops, mino_val_t *opts,
                  mino_val_t *callback);

#endif /* ASYNC_SELECT_H */
