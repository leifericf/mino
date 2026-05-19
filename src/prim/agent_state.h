/*
 * prim/agent_state.h -- per-state agent subsystem block.
 *
 * Agents are the Clojure send / send-off / await family. The
 * subsystem holds a monotonic id counter, a shutdown flag, the
 * shared mu/cv used by all pools, and the two run-queue pools
 * (POOLED + SOLO).
 *
 * The per-thread agent context (not present in this block) travels
 * on mino_thread_ctx_t.
 *
 * Internal to the runtime; embedders should only use mino.h.
 */

#ifndef PRIM_AGENT_STATE_H
#define PRIM_AGENT_STATE_H

#include "mino_internal.h"
#include "runtime/agent_queue.h"   /* agent_action_node_t, AGENT_POOL_COUNT */

#include <stdint.h>

#if defined(__linux__) && !defined(_XOPEN_SOURCE)
#  define _XOPEN_SOURCE 600
#endif

#if defined(_WIN32) && defined(_MSC_VER)
#  include <windows.h>
#else
#  include <pthread.h>
#endif

typedef struct agent_pool_entry {
    agent_action_node_t *run_head;
    agent_action_node_t *run_tail;
#if defined(_WIN32) && defined(_MSC_VER)
    HANDLE               worker;
#else
    pthread_t            worker;
#endif
    int                  worker_alive;
    int                  worker_pending_join;
} agent_pool_entry_t;

typedef struct agent_subsystem {
    /* Monotonic counter for agent IDs. Mirrors stm.next_ref_id so the
     * agent print form (#agent[ID VAL]) can distinguish two agents
     * that hold the same value. */
    uint64_t             next_id;

    /* Flipped to 1 by (shutdown-agents). After shutdown every send /
     * send-off throws MST008; the worker drains the queue and exits.
     * Idempotent. */
    int                  shutdown;

#if defined(_WIN32) && defined(_MSC_VER)
    CRITICAL_SECTION     mu;
    CONDITION_VARIABLE   cv;
#else
    pthread_mutex_t      mu;
    pthread_cond_t       cv;
#endif

    /* Per-pool state. See runtime/agent_queue.h for the pool-kind
     * enum (POOLED = send, SOLO = send-off). */
    agent_pool_entry_t   pool[AGENT_POOL_COUNT];

    /* mu_inited: has mu/cv been pthread_*_init'd yet (lazy on first
     * send into either pool). */
    int                  mu_inited;
} agent_subsystem_t;

#endif /* PRIM_AGENT_STATE_H */
