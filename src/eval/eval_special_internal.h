/*
 * eval_special_internal.h -- shared declarations for eval_special_*.c files.
 *
 * Not part of the public API. Each eval_special_*.c includes this for
 * cross-domain function access within the evaluator's special-form layer.
 */

#ifndef EVAL_SPECIAL_INTERNAL_H
#define EVAL_SPECIAL_INTERNAL_H

#include "mino_internal.h"

/* prim_module.c: needed by eval_ns for require delegation. */
mino_val_t *prim_require(mino_state_t *S, mino_val_t *args, mino_env_t *env);

/* eval_special_bindings.c: destructuring and binding helpers. */
int kw_eq(const mino_val_t *v, const char *s);
int bind_params(mino_state_t *S, mino_env_t *env, mino_val_t *params,
                mino_val_t *args, const char *ctx);

/* eval_special_defs.c */
mino_val_t *eval_defmacro(mino_state_t *S, mino_val_t *form,
                          mino_val_t *args, mino_env_t *env);
mino_val_t *eval_declare(mino_state_t *S, mino_val_t *form,
                         mino_val_t *args, mino_env_t *env);
mino_val_t *eval_def(mino_state_t *S, mino_val_t *form,
                     mino_val_t *args, mino_env_t *env);

mino_val_t *eval_ns(mino_state_t *S, mino_val_t *form,
                    mino_val_t *args, mino_env_t *env);

/* eval_special_bindings.c */
mino_val_t *eval_let(mino_state_t *S, mino_val_t *form,
                     mino_val_t *args, mino_env_t *env, int tail);
mino_val_t *eval_loop(mino_state_t *S, mino_val_t *form,
                      mino_val_t *args, mino_env_t *env, int tail);
mino_val_t *eval_binding(mino_state_t *S, mino_val_t *form,
                         mino_val_t *args, mino_env_t *env);

/* eval_special_control.c */
mino_val_t *eval_try(mino_state_t *S, mino_val_t *form,
                     mino_val_t *args, mino_env_t *env);

/* eval_special_fn.c */
mino_val_t *eval_fn(mino_state_t *S, mino_val_t *form,
                    mino_val_t *args, mino_env_t *env);
mino_val_t *build_multi_arity_clauses(mino_state_t *S, mino_val_t *form,
                                      mino_val_t *arity_list,
                                      const char *diag_code,
                                      const char *label);

/*
 * Resolve a non-fn callable value (keyword, map, vector, set, sorted
 * map, sorted set) against an already-evaluated argument list.
 *
 * FN must not be MINO_PRIM, MINO_FN, or MINO_MACRO; those are callers'
 * business. If FN is none of the recognized non-fn callable types,
 * raises a "not a function" type diagnostic (MTY002) against FORM.
 * All diagnostics are posted with set_eval_diag against FORM.
 *
 * Returns the call result on success, NULL on error.
 */
mino_val_t *apply_non_fn_callable(mino_state_t *S, mino_val_t *fn,
                                  mino_val_t *args, mino_val_t *form);

#endif /* EVAL_SPECIAL_INTERNAL_H */
