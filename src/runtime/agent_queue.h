/*
 * agent_queue.h -- agent run-queue node and pool-kind enum.
 *
 * One queued action targeting a specific agent lives in the malloc
 * heap (not GC); the mino_val_t pointers it holds are GC roots
 * reached via gc_mark_agent_runq during root marking.
 *
 * Two agent pools live alongside each other: POOLED is the target of
 * `send` (canonical CPU-bound shape), SOLO is the target of
 * `send-off` (canonical IO-bound shape). Each pool has its own FIFO
 * and its own worker thread, but all pools share agent_mu/agent_cv so
 * an await waiter sleeps once and wakes for either pool's progress.
 *
 * mino's per-state eval lock still serializes one action at a time,
 * so the split is architectural -- it preserves enqueue-order within
 * each shape, and gives us a clean seam if we later let SOLO actions
 * yield the eval lock during blocking IO.
 *
 * Internal to the runtime; embedders should only use mino.h.
 */

#ifndef RUNTIME_AGENT_QUEUE_H
#define RUNTIME_AGENT_QUEUE_H

#include "mino_internal.h"

typedef struct agent_action_node {
    mino_val_t                *agent;     /* MINO_AGENT target */
    mino_val_t                *fn;        /* action fn */
    mino_val_t                *extra;     /* extra arg list (nil/cons) */
    mino_val_t                *dyn_snap;  /* dyn-binding snapshot at send */
    mino_env_t                *env;       /* env captured at send time */
    struct agent_action_node  *next;
} agent_action_node_t;

typedef enum {
    AGENT_POOL_POOLED = 0, /* send */
    AGENT_POOL_SOLO   = 1, /* send-off */
    AGENT_POOL_COUNT  = 2
} agent_pool_kind_t;

#endif /* RUNTIME_AGENT_QUEUE_H */
