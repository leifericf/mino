/*
 * stm_state.h -- STM transaction state types.
 *
 * Per-thread state for a running `dosync` invocation. tx_ref_state_t is
 * the node tracking what a transaction has done with a single ref since
 * it opened: snapshot version captured on first read, the kind of
 * pending mutation (alter vs. logged commute), the tentative value or
 * commute log, and the next-pointer for the linked list owned by
 * tx_state_t. The list itself is GC-traced via gc_mark_roots so
 * tentative values stay reachable during a transaction's lifetime.
 *
 * Internal to the runtime; embedders should only use mino.h.
 */

#ifndef RUNTIME_STM_STATE_H
#define RUNTIME_STM_STATE_H

#include "mino_internal.h"

#include <stdint.h>

typedef enum {
    TX_STATE_ALTER       = 0,
    TX_STATE_COMMUTE_LOG = 1
} tx_ref_state_kind_t;

typedef struct tx_ref_state {
    mino_val          *ref;              /* MINO_TX_REF this state tracks */
    uint64_t             snapshot_version; /* version at first read */
    int                  read;             /* 1 if this tx read the ref */
    tx_ref_state_kind_t  kind;
    mino_val          *tentative;        /* in-tx value; NULL if no write */
    mino_val          *commute_log;      /* MINO_CONS of (fn args...); or NULL */
    /* Populated by tx_commit when a write is applied: the ref's value
     * before the commit (for watch dispatch's `old` arg) and the new
     * committed value (`new` arg). Both NULL if no write happened on
     * this ref. */
    mino_val          *committed_old;
    mino_val          *committed_new;
    struct tx_ref_state *next;
} tx_ref_state_t;

typedef struct tx_state {
    int                  depth;            /* outermost dosync = 1 */
    tx_ref_state_t      *refs_head;        /* per-ref state chain */
    int                  retry_count;
    int                  try_depth_at_start; /* try-stack snapshot for retry */
    /* Non-zero while tx_commit is walking the write set and invoking
     * user code (commute log replay, validators). Mutating ops
     * (alter / ref-set / commute) re-entered through that user code
     * cannot be honoured -- the iterator has already moved past the
     * affected ref node, so the new tentative would be silently lost.
     * Throw MST002 instead. */
    int                  in_commit;
    /* Set by tx_commit to the validator's thrown exception (if any)
     * so dosync_run can re-throw the original payload instead of a
     * generic MCT001 message. NULL when the validator returned falsy
     * without throwing or when no validator ran. */
    mino_val          *validator_thrown_ex;
    /* (send / send-off) called from inside the transaction body
     * appends (agent fn extra) triples to this cons list (LIFO --
     * head holds the most recent send) instead of dispatching
     * synchronously. JVM canon: pending sends fire only on
     * successful commit, are silently discarded on retry or abort
     * so a body that's run multiple times sends the action once. */
    mino_val          *pending_sends;
} tx_state_t;

#endif /* RUNTIME_STM_STATE_H */
