/*
 * eval_internal.h -- evaluator core, macroexpansion, reader, printer hooks.
 *
 * Internal to the runtime; embedders should only use mino.h.
 */

#ifndef EVAL_INTERNAL_H
#define EVAL_INTERNAL_H

#include "mino.h"

#include <setjmp.h>
#include <stdio.h>

/* ------------------------------------------------------------------------- */
/* Exception handling                                                        */
/* ------------------------------------------------------------------------- */

#define MAX_TRY_DEPTH 64

typedef struct {
    jmp_buf     buf;
    mino_val_t *exception;
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
/* eval_special.c: dispatch, special forms, destructuring, apply             */
/*                                                                           */
/* All return GC-owned values (NULL on error).                               */
/* ------------------------------------------------------------------------- */

mino_val_t *eval_impl(mino_state_t *S, mino_val_t *form, mino_env_t *env,
                      int tail);
mino_val_t *eval(mino_state_t *S, mino_val_t *form, mino_env_t *env);
mino_val_t *apply_callable(mino_state_t *S, mino_val_t *fn, mino_val_t *args,
                           mino_env_t *env);

/* ------------------------------------------------------------------------- */
/* print.c                                                                   */
/* ------------------------------------------------------------------------- */

void print_val(mino_state_t *S, FILE *out, const mino_val_t *v, int readably);

/* ------------------------------------------------------------------------- */
/* read.c                                                                    */
/* ------------------------------------------------------------------------- */

const char *intern_filename(mino_state_t *S, const char *name);  /* interned for state's lifetime */

#endif /* EVAL_INTERNAL_H */
