/*
 * special_internal.h -- shared declarations for the eval/ special-form
 * translation units (special.c, defs.c, bindings.c, control.c, fn.c,
 * special_registry.c).
 *
 * Not part of the public API. Each .c in the family includes this for
 * cross-domain function access within the evaluator's special-form
 * layer.
 */

#ifndef EVAL_SPECIAL_INTERNAL_H
#define EVAL_SPECIAL_INTERNAL_H

#include "runtime/internal.h"

/* Portable fall-through marker for intentional switch fall-through. A
 * plain comment satisfies GCC but not clang's -Wimplicit-fallthrough
 * (clang ignores the comment), which the pinned-zig lint lane enables.
 * The statement attribute is understood by clang and GCC >= 7; it
 * degrades to a no-op elsewhere. Use as a statement: `MINO_FALLTHROUGH;` */
#if defined(__clang__) || (defined(__GNUC__) && __GNUC__ >= 7)
#  define MINO_FALLTHROUGH __attribute__((fallthrough))
#else
#  define MINO_FALLTHROUGH ((void)0)
#endif

/* prim/module.c: needed by eval_ns for require delegation. */
mino_val *prim_require(mino_state *S, mino_val *args, mino_env *env);

/* bindings.c: destructuring and binding helpers. */
int kw_eq(const mino_val *v, const char *s);
int bind_params(mino_state *S, mino_env *env, mino_val *params,
                mino_val *args, const char *ctx);
/* Closure-shape pre-compile helpers. fn_params_simple_shape returns 1
 * iff `params` is a vector of plain interned symbols with no
 * destructure / no &-rest / no :as. bind_simple_params binds such a
 * vector to args without going through bind_form's dispatch tower. */
int fn_params_simple_shape(mino_val *params);
int bind_simple_params(mino_state *S, mino_env *env,
                       mino_val *params, mino_val *args, const char *ctx);

/*
 * Special-form handler signature. Every entry in the special-form
 * registry table (eval/special_registry.c) takes (S, form, args, env,
 * tail). Handlers that don't need `tail` accept it and ignore it; the
 * uniform shape is what makes the data-table dispatch work.
 */
typedef mino_val *(*special_fn)(mino_state *S, mino_val *form,
                                   mino_val *args, mino_env *env,
                                   int tail);

/* defs.c */
mino_val *eval_defmacro(mino_state *S, mino_val *form,
                          mino_val *args, mino_env *env, int tail);
mino_val *eval_declare(mino_state *S, mino_val *form,
                         mino_val *args, mino_env *env, int tail);
mino_val *eval_def(mino_state *S, mino_val *form,
                     mino_val *args, mino_env *env, int tail);

mino_val *eval_ns(mino_state *S, mino_val *form,
                    mino_val *args, mino_env *env, int tail);

/* bindings.c */
mino_val *eval_let(mino_state *S, mino_val *form,
                     mino_val *args, mino_env *env, int tail);
mino_val *eval_letfn_star(mino_state *S, mino_val *form,
                            mino_val *args, mino_env *env, int tail);
mino_val *eval_loop(mino_state *S, mino_val *form,
                      mino_val *args, mino_env *env, int tail);
mino_val *eval_binding(mino_state *S, mino_val *form,
                         mino_val *args, mino_env *env, int tail);

/* control.c */
mino_val *eval_try(mino_state *S, mino_val *form,
                     mino_val *args, mino_env *env, int tail);
mino_val *normalize_exception(mino_state *S, mino_val *ex_val);

/* fn.c */
mino_val *eval_fn(mino_state *S, mino_val *form,
                    mino_val *args, mino_env *env, int tail);
mino_val *build_multi_arity_clauses(mino_state *S, mino_val *form,
                                      mino_val *arity_list,
                                      const char *diag_code,
                                      const char *label);

/* eval/special_registry.c */
int eval_try_special_form(mino_state *S, mino_val *form,
                          mino_val *head, mino_val *args,
                          mino_env *env, int tail,
                          mino_val **out);
mino_val *build_multi_arity_clauses(mino_state *S, mino_val *form,
                                      mino_val *arity_list,
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
mino_val *apply_non_fn_callable(mino_state *S, mino_val *fn,
                                  mino_val *args, const mino_val *form);

#endif /* EVAL_SPECIAL_INTERNAL_H */
