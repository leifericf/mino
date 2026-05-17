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

#include <setjmp.h>
#include <stdio.h>

/* ------------------------------------------------------------------------- */
/* Exception handling                                                        */
/* ------------------------------------------------------------------------- */

#define MAX_TRY_DEPTH 64

typedef struct {
    jmp_buf     buf;
    mino_val_t *exception;
    const char *saved_ns;       /* current_ns at try-frame entry; restored on catch */
    const char *saved_ambient;  /* fn_ambient_ns at try-frame entry */
    size_t      saved_load_len; /* require load-stack depth at frame entry */
} try_frame_t;

/* ------------------------------------------------------------------------- */
/* mino.c: evaluator core helpers                                            */
/*                                                                           */
/* All eval/expand functions return GC-owned values (NULL on error).         */
/* ------------------------------------------------------------------------- */

int         sym_eq(const mino_val_t *v, const char *s);        /* pure */
mino_val_t *eval_value(mino_state_t *S, mino_val_t *form, mino_env_t *env);
mino_val_t *eval_implicit_do(mino_state_t *S, mino_val_t *body,
                             mino_env_t *env);
mino_val_t *eval_implicit_do_impl(mino_state_t *S, mino_val_t *body,
                                  mino_env_t *env, int tail);
mino_val_t *lazy_force(mino_state_t *S, mino_val_t *v);       /* mutates lazy cache */
mino_val_t *eval_args(mino_state_t *S, mino_val_t *args, mino_env_t *env);
mino_val_t *macroexpand1(mino_state_t *S, mino_val_t *form, mino_env_t *env,
                         int *expanded);
mino_val_t *macroexpand_all(mino_state_t *S, mino_val_t *form,
                            mino_env_t *env);
mino_val_t *quasiquote_expand(mino_state_t *S, mino_val_t *form,
                              mino_env_t *env);

/* ------------------------------------------------------------------------- */
/* special.c: dispatch, special forms, destructuring, apply                  */
/*                                                                           */
/* All return GC-owned values (NULL on error).                               */
/* ------------------------------------------------------------------------- */

mino_val_t *eval_impl(mino_state_t *S, mino_val_t *form, mino_env_t *env,
                      int tail);
mino_val_t *eval(mino_state_t *S, mino_val_t *form, mino_env_t *env);
mino_val_t *apply_callable(mino_state_t *S, mino_val_t *fn, mino_val_t *args,
                           mino_env_t *env);
/* argv ABI variant: invoke `fn` with a slice of `argc` pointers from
 * `argv`. Skips the cons-spine build+walk used by callers that already
 * have their args in argv form (BC's OP_CALL, in particular).
 *
 * Fast paths handled directly: MINO_PRIM with fn2, MINO_FN bc-runnable.
 * Slow paths (PRIM-fn1, MINO_FN tree-walker, MINO_MACRO, non-fn
 * callables) build a cons list internally and delegate to
 * apply_callable to reuse its trampoline and multi-arity dispatch. */
mino_val_t *apply_callable_argv(mino_state_t *S, mino_val_t *fn,
                                mino_val_t **argv, int argc,
                                mino_env_t *env);

/* JIT-only fast entry into apply_callable_argv's bc-fn branch. Skips
 * the dispatch switch and reuses the shared bc-fn invocation core.
 * Defensive: returns to apply_callable_argv if the callee's shape
 * has drifted from what the IC slot captured. */
mino_val_t *mino_apply_known_bc_fn_argv(mino_state_t *S, mino_val_t *fn,
                                        mino_val_t **argv, int argc,
                                        mino_env_t *env);

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
int fn_lazy_safe_rest(mino_val_t *fn);

/* ------------------------------------------------------------------------- */
/* print.c                                                                   */
/* ------------------------------------------------------------------------- */

void print_val(mino_state_t *S, FILE *out, const mino_val_t *v, int readably);

/* ------------------------------------------------------------------------- */
/* read.c                                                                    */
/* ------------------------------------------------------------------------- */

const char *intern_filename(mino_state_t *S, const char *name);  /* interned for state's lifetime */

#endif /* EVAL_INTERNAL_H */
