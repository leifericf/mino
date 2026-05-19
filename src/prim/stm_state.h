/*
 * prim/stm_state.h -- per-state STM (software transactional memory)
 * subsystem block.
 *
 * stm_subsystem_t holds the global commit lock plus the monotonic
 * ref-id counter used by tx-ref construction. The per-thread
 * transaction state (tx_state_t) lives in runtime/stm_state.h
 * because it travels on mino_thread_ctx_t, not here.
 *
 * Internal to the runtime; embedders should only use mino.h.
 */

#ifndef PRIM_STM_STATE_H
#define PRIM_STM_STATE_H

#include "mino_internal.h"

#include <stdint.h>

#if defined(__linux__) && !defined(_XOPEN_SOURCE)
#  define _XOPEN_SOURCE 600
#endif

#if !(defined(_WIN32) && defined(_MSC_VER))
#  include <pthread.h>
#endif

typedef struct stm_subsystem {
    /* Global STM commit lock. Held only across the commit phase of a
     * transaction (read-set validation + writes + version bumps);
     * watch dispatch runs outside it. Lazy-initialized on the first
     * call to mino_install_stm so embedders that never opt into STM
     * pay only the bool tracking cost. Coarse-grained on purpose:
     * mino's typical embedded workload is single-digit refs and a
     * handful of worker threads, where contention from a single
     * mutex is far cheaper than the per-ref pthread_mutex_t
     * alternative. */
#if defined(_WIN32) && defined(_MSC_VER)
    void           *commit_lock;   /* CRITICAL_SECTION; lazy */
#else
    pthread_mutex_t commit_lock;
#endif
    int             lock_inited;

    /* Monotonic counter for tx-ref IDs. Refs get a stable identity
     * value at construction; not currently used for lock ordering
     * (single global commit lock) but kept reserved for a future
     * fine-grained lock matrix. */
    uint64_t        next_ref_id;
} stm_subsystem_t;

#endif /* PRIM_STM_STATE_H */
