/*
 * eval_internal.h -- evaluator core, macroexpansion, reader, printer hooks.
 *
 * Internal to the runtime; embedders should only use mino.h.
 *
 * Error classes emitted (see diag/diag_contract.h):
 *
 *   MINO_ERR_RECOVERABLE -- the dominant path.  Type errors, arity
 *      mismatches, undefined symbols, and any error raised by user
 *      code surface here through set_eval_diag plus a longjmp into
 *      the active try frame (or a propagating NULL when no try frame
 *      is on the stack).  Diagnostic kinds: :eval/..., :type/...,
 *      :arity/..., :user/...
 *   MINO_ERR_HOST -- step-limit and heap-limit hits set a host-visible
 *      diagnostic and return NULL without unwinding.  Diagnostic
 *      kinds: :limit/steps, :limit/heap.
 *   MINO_ERR_RECOVERABLE -- read.c reader errors set a diagnostic and
 *      return NULL; mino_read's caller surfaces them via
 *      mino_last_error.
 */

#ifndef EVAL_INTERNAL_H
#define EVAL_INTERNAL_H

#include "mino_internal.h"
#include "runtime/thread_ctx.h"  /* MAX_TRY_DEPTH + try_frame_t (the per-ctx try-stack types) */

#include <stdio.h>

/* ------------------------------------------------------------------------- */
/* mino.c: evaluator core helpers                                            */
/*                                                                           */
/* All eval/expand functions return GC-owned values (NULL on error).         */
/* ------------------------------------------------------------------------- */

int         sym_eq(const mino_val *v, const char *s);        /* pure */
mino_val *eval_value(mino_state *S, mino_val *form, mino_env *env);
mino_val *eval_implicit_do(mino_state *S, mino_val *body,
                             mino_env *env);
mino_val *eval_implicit_do_impl(mino_state *S, mino_val *body,
                                  mino_env *env, int tail);
mino_val *lazy_force(mino_state *S, mino_val *v);       /* mutates lazy cache */
mino_val *eval_args(mino_state *S, mino_val *args, mino_env *env);
mino_val *macroexpand1(mino_state *S, mino_val *form, mino_env *env,
                         int *expanded);
mino_val *macroexpand_all(mino_state *S, mino_val *form,
                            mino_env *env);
mino_val *quasiquote_expand(mino_state *S, mino_val *form,
                              mino_env *env);

/* ------------------------------------------------------------------------- */
/* special.c: dispatch, special forms, destructuring, apply                  */
/*                                                                           */
/* All return GC-owned values (NULL on error).                               */
/* ------------------------------------------------------------------------- */

mino_val *eval_impl(mino_state *S, mino_val *form, mino_env *env,
                      int tail);
mino_val *eval(mino_state *S, mino_val *form, mino_env *env);
mino_val *apply_callable(mino_state *S, mino_val *fn, mino_val *args,
                           mino_env *env);
/* special_registry.c: true when `name`/`len` is one of the special-form
 * spellings the registry dispatches (let, fn, loop, if, do, quote, ...).
 * Syntax-quote consults it so these names stay bare and special-form
 * recognition keeps working even after they gain clojure.core bindings. */
int eval_is_special_form_name(const char *name, size_t len);
/* special_registry.c: true when `name`/`len` is one of the eleven public
 * macro-family forms (fn, let, loop, lazy-seq, binding, declare, defmacro,
 * ns, when, and, or) -- the special forms that canonical Clojure exposes as
 * clojure.core macros. Syntax-quote qualifies these to clojure.core/X (the
 * true special forms if/do/def/quote/... stay bare), and the registry
 * accepts the clojure.core/-qualified spelling for exactly this set. */
int eval_is_public_form_name(const char *name, size_t len);
/* True for the reader's meta-flagged record values (tagged-literal
 * fallback, preserved reader conditionals); they self-evaluate. */
int mino_is_reader_record(mino_state *S, mino_val *form);
/* argv ABI variant: invoke `fn` with a slice of `argc` pointers from
 * `argv`. Skips the cons-spine build+walk used by callers that already
 * have their args in argv form (BC's OP_CALL, in particular).
 *
 * Fast paths handled directly: MINO_PRIM with fn2, MINO_FN bc-runnable.
 * Slow paths (PRIM-fn1, MINO_FN tree-walker, MINO_MACRO, non-fn
 * callables) build a cons list internally and delegate to
 * apply_callable to reuse its trampoline and multi-arity dispatch. */
mino_val *apply_callable_argv(mino_state *S, mino_val *fn,
                                mino_val **argv, int argc,
                                mino_env *env);

/* JIT-only fast entry into apply_callable_argv's bc-fn branch. Skips
 * the dispatch switch and reuses the shared bc-fn invocation core.
 * Defensive: returns to apply_callable_argv if the callee's shape
 * has drifted from what the IC slot captured. */
mino_val *mino_apply_known_bc_fn_argv(mino_state *S, mino_val *fn,
                                        mino_val **argv, int argc,
                                        mino_env *env);

/* fn_lazy_safe_rest -- predicate used by prim_apply to decide whether
 * the final-arg collection can be spliced into the args spine
 * (preserving a lazy tail) rather than materialized.
 *
 * Returns 1 only when fn (var-unwrapped) is a single-arity MINO_FN
 * whose params include `& rest`. Multi-arity fns are excluded because
 * dispatch_multi_arity counts args via list_len, which would not
 * terminate on an infinite lazy tail. Anything that isn't an FN
 * with rest-args (MINO_PRIM, fixed-arity FN, MACRO, keywords-as-fn,
 * ...) is rejected so the caller falls back to eager materialization. */
int fn_lazy_safe_rest(mino_val *fn);

/* ------------------------------------------------------------------------- */
/* print.c / print_dynvars.c                                                 */
/* ------------------------------------------------------------------------- */

void print_val(mino_state *S, FILE *out, const mino_val *v, int readably);

/* Print dynvar plumbing. Top-level print entrypoints (pr / prn / print
 * / println / pr-str) call resolve to snapshot *print-length* and
 * *print-level* / *print-readably* / *print-meta* / *print-dup* /
 * *print-namespace-maps* / *flush-on-newline* from the current
 * binding stack into the state's cached fields, do the print, then
 * call restore with the saved values. Both helpers are no-ops when
 * env is NULL or the dynvars are unset.
 *
 * The cached values mean the per-collection printers read a single
 * int field instead of walking the binding stack per nested value.
 *
 * Declared here (not prim/internal.h) because the implementations
 * live in eval/print_dynvars.c. */
typedef struct {
    int length;
    int level;
    int readably;
    int meta;
    int dup;
    int ns_maps;
    int flush_nl;
} print_dynvars_saved_t;

void print_dynvars_resolve(mino_state *S, mino_env *env,
                           print_dynvars_saved_t *saved);
void print_dynvars_restore(mino_state *S, const print_dynvars_saved_t *saved);

/* ------------------------------------------------------------------------- */
/* bindings_destr.c                                                          */
/* ------------------------------------------------------------------------- */

/* Declared here (not prim/internal.h) because the implementation lives in
 * eval/bindings_destr.c. Registered as a primitive by prim/reflection.c's
 * table; eval/bc/compile.c also calls it at compile time. */
mino_val *prim_destructure(mino_state *S, mino_val *args, mino_env *env);

/* ------------------------------------------------------------------------- */
/* read.c                                                                    */
/* ------------------------------------------------------------------------- */

const char *intern_filename(mino_state *S, const char *name);  /* interned for state's lifetime */

#endif /* EVAL_INTERNAL_H */
