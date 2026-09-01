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
void mino_throw_capture_site(mino_state *S);
mino_val *normalize_exception(mino_state *S, mino_val *ex_val);

/* control.c -- classed catch dispatch (ADR 32, ADR 37). The table maps
 * a source-compatibility class-name token to the diagnostic :mino/kind
 * strings its clause accepts; kinds[0] == NULL marks a catch-all.
 * mino_catch_class_index matches a symbol or keyword name (":default"
 * for the keyword) on the tail after the last dot, and returns -1 for
 * names outside the table.
 *
 * ADR 37: a keyword catch class that is NOT a table alias is not an
 * error -- it is mino's native, open dispatch on a diagnostic's
 * :mino/kind. The parser records it as MINO_CATCH_CLASS_KIND and stashes
 * the keyword; both tiers match it by equality via
 * mino_catch_kind_matches. The class-name table stays frozen: symbols
 * are the compat surface (unknown symbol = error), keywords are data. */
typedef struct mino_catch_class {
    const char *name;
    const char *kinds[3];
} mino_catch_class_t;

/* Sentinels stored in a clause's catch-class slot. Non-negative values
 * index mino_catch_classes; these two are the special cases. Mirrored in
 * eval/bc/internal.h for the bytecode tier. */
#ifndef MINO_CATCH_CLASS_ANY
#define MINO_CATCH_CLASS_ANY  (-1) /* bare (catch e ...): matches anything */
#define MINO_CATCH_CLASS_KIND (-2) /* (catch :kw e ...): match :mino/kind == kw */
#endif

extern const mino_catch_class_t mino_catch_classes[];
int mino_catch_class_index(const char *name);
int mino_catch_class_matches(mino_state *S, int class_idx, mino_val *diag);
/* True iff diag is a map whose :mino/kind equals kind_kw (ADR 37). */
int mino_catch_kind_matches(mino_state *S, mino_val *kind_kw, mino_val *diag);

/* fn.c */
mino_val *eval_fn(mino_state *S, mino_val *form,
                    mino_val *args, mino_env *env, int tail);
mino_val *build_multi_arity_clauses(mino_state *S, mino_val *form,
                                      mino_val *arity_list,
                                      const char *diag_code,
                                      const char *label);
/* Rewrite a single arity body so a leading {:pre [...] :post [...]} map
 * (only when the body has more than one form) enforces its conditions,
 * sharing defn's exact rewrite shape. Returns body unchanged when no
 * condition map applies; NULL on OOM. */
mino_val *fn_rewrite_prepost_body(mino_state *S, mino_val *body);

/* eval/special_registry.c */
int eval_try_special_form(mino_state *S, mino_val *form,
                          mino_val *head, mino_val *args,
                          mino_env *env, int tail,
                          mino_val **out);

/* special_host.c: host-interop syntax sugar (.method, .-field, new, Foo.) */
int eval_try_host_syntax(mino_state *S, mino_val *form,
                         mino_val *head, mino_val *args,
                         mino_env *env, mino_val **out);

/* Registers the public C special forms (fn, let, loop, lazy-seq,
 * binding, declare, defmacro, ns, when, and, or) as clojure.core
 * vars + env bindings so they appear in ns-publics / resolve / doc.
 * Idempotent per name. Must run after the clojure.core ns env exists. */
void eval_special_register_vars(mino_state *S);

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
