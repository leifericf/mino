/*
 * ref_publish.h -- shared validator and watch dispatch for the
 * reference types (atom, var, ref, agent, store).
 *
 * Every reference type publishes a state change the same way: run the
 * validator against the candidate value, apply the type's own swap
 * (slot write + write barrier, owned by the caller), then notify the
 * watch table with (key ref old new). Only the failure surface
 * differs per type -- atoms, vars, and stores raise immediately, the
 * STM commit aborts the transaction, agents enter their fail state --
 * so the policy parameter names that difference once instead of five
 * hand-copied validate + notify loops.
 *
 * Body lives in state/ref_publish.c. Internal to the runtime;
 * embedders should only use mino.h.
 */

#ifndef RUNTIME_REF_PUBLISH_H
#define RUNTIME_REF_PUBLISH_H

#include "mino_internal.h"

/* How a validator rejection or a watch throw surfaces. */
typedef enum {
    REF_FAIL_THROW,   /* atom / var / store: raise immediately; a
                       * failing watch stops dispatch of later watches */
    REF_FAIL_CAPTURE  /* ref (tx commit) / agent: run user code under
                       * pcall and report the payload through *out_ex;
                       * watch dispatch continues past a throw */
} ref_fail_policy_t;

/* Run validator (NULL = accept) against candidate. Returns 1 when
 * accepted. Under REF_FAIL_THROW, 0 means rejected with the throw
 * already raised (the validator's own throw propagated, or the
 * classified MCT001 "Invalid reference state" on a falsy return).
 * Under REF_FAIL_CAPTURE, 0 means the validator threw and *out_ex
 * holds the payload; -1 means it returned falsy without throwing
 * (*out_ex left NULL) and the caller owns the surfacing. */
int ref_validate(mino_state *S, mino_val *validator,
                 mino_val *candidate, mino_env *env,
                 ref_fail_policy_t policy, mino_val **out_ex);

/* Dispatch the watch table (a map, registration-ordered) with
 * (key ref old_val new_val) per entry. Returns 0 when every watch ran
 * clean. Under REF_FAIL_THROW, -1 means a watch failed, dispatch
 * stopped there, and the throw is already propagating. Under
 * REF_FAIL_CAPTURE, every watch runs; -1 means at least one threw and
 * *out_ex holds the first thrown payload. */
int ref_notify(mino_state *S, mino_val *ref, mino_val *watches,
               mino_val *old_val, mino_val *new_val, mino_env *env,
               ref_fail_policy_t policy, mino_val **out_ex);

#endif /* RUNTIME_REF_PUBLISH_H */
