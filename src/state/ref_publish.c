/*
 * ref_publish.c -- shared validator and watch dispatch for the
 * reference types (atom, var, ref, agent, store).
 *
 * The caller owns the swap itself (slot write + write barrier +
 * per-type bookkeeping); this module owns the two steps around it
 * that run user code, so the validate and notify semantics cannot
 * drift between reference types. See runtime/ref_publish.h for the
 * policy contract.
 */

#include "runtime/internal.h"
#include "runtime/ref_publish.h"

int ref_validate(mino_state *S, mino_val *validator,
                 mino_val *candidate, mino_env *env,
                 ref_fail_policy_t policy, mino_val **out_ex)
{
    mino_val *vargs;
    mino_val *result = NULL;
    if (out_ex != NULL) *out_ex = NULL;
    if (validator == NULL) return 1;
    vargs = mino_cons(S, candidate, mino_nil(S));
    if (policy == REF_FAIL_CAPTURE) {
        mino_val *thrown = NULL;
        if (mino_pcall(S, validator, vargs, env, &result, &thrown) != 0) {
            if (out_ex != NULL) *out_ex = thrown;
            return 0;
        }
        if (result == NULL) return 0; /* defensive; shouldn't happen */
        return mino_is_truthy(result) ? 1 : -1;
    }
    result = mino_call(S, validator, vargs, env);
    if (result == NULL) return 0; /* validator threw */
    if (!mino_is_truthy(result)) {
        throw_classified(S, "eval/contract", "MCT001",
                              "Invalid reference state");
        return 0;
    }
    return 1;
}

int ref_notify(mino_state *S, mino_val *ref, mino_val *watches,
               mino_val *old_val, mino_val *new_val, mino_env *env,
               ref_fail_policy_t policy, mino_val **out_ex)
{
    size_t      i, n;
    mino_val *first_thrown = NULL;
    if (out_ex != NULL) *out_ex = NULL;
    if (watches == NULL || mino_type_of(watches) != MINO_MAP
        || watches->as.map.len == 0) {
        return 0;
    }
    /* Pin what user code could otherwise unroot mid-dispatch: a watch
     * that calls remove-watch detaches the map from the ref, and
     * old_val is no longer reachable from the published slot. */
    gc_pin(watches);
    gc_pin(old_val);
    gc_pin(new_val);
    n = watches->as.map.len;
    for (i = 0; i < n; i++) {
        mino_val *key = vec_nth(watches->as.map.key_order, i);
        mino_val *fn  = map_get_val(watches, key);
        mino_val *wargs;
        if (fn == NULL) continue;
        {
            mino_val *tmp = mino_cons(S, new_val, mino_nil(S)); gc_pin(tmp);
            tmp = mino_cons(S, old_val, tmp);                   gc_unpin(1); gc_pin(tmp);
            tmp = mino_cons(S, ref, tmp);                       gc_unpin(1); gc_pin(tmp);
            wargs = mino_cons(S, key, tmp);                     gc_unpin(1);
        }
        if (policy == REF_FAIL_CAPTURE) {
            mino_val *result = NULL;
            mino_val *thrown = NULL;
            if (mino_pcall(S, fn, wargs, env, &result, &thrown) != 0
                && thrown != NULL && first_thrown == NULL) {
                first_thrown = thrown;
                gc_pin(first_thrown);
            }
        } else {
            if (mino_call(S, fn, wargs, env) == NULL) {
                gc_unpin(3);
                return -1;
            }
        }
    }
    if (first_thrown != NULL) {
        gc_unpin(4);
        if (out_ex != NULL) *out_ex = first_thrown;
        return -1;
    }
    gc_unpin(3);
    return 0;
}
