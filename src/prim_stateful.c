/*
 * prim_stateful.c -- atom primitives: atom, deref, reset!, swap!, atom?.
 *
 * Extracted from prim.c. No behavior change.
 */

#include "prim_internal.h"

mino_val_t *prim_atom(mino_state_t *S, mino_val_t *args, mino_env_t *env)
{
    (void)env;
    if (!mino_is_cons(args) || mino_is_cons(args->as.cons.cdr)) {
        set_error(S, "atom requires one argument");
        return NULL;
    }
    return mino_atom(S, args->as.cons.car);
}

mino_val_t *prim_deref(mino_state_t *S, mino_val_t *args, mino_env_t *env)
{
    mino_val_t *a;
    (void)env;
    if (!mino_is_cons(args) || mino_is_cons(args->as.cons.cdr)) {
        set_error(S, "deref requires one argument");
        return NULL;
    }
    a = args->as.cons.car;
    if (a != NULL && a->type == MINO_ATOM) {
        return a->as.atom.val;
    }
    if (a != NULL && a->type == MINO_REDUCED) {
        return a->as.reduced.val;
    }
    set_error(S, "deref: expected an atom or reduced");
    return NULL;
}

mino_val_t *prim_reset_bang(mino_state_t *S, mino_val_t *args, mino_env_t *env)
{
    mino_val_t *a, *val;
    (void)env;
    if (!mino_is_cons(args) || !mino_is_cons(args->as.cons.cdr)
        || mino_is_cons(args->as.cons.cdr->as.cons.cdr)) {
        set_error(S, "reset! requires two arguments");
        return NULL;
    }
    a   = args->as.cons.car;
    val = args->as.cons.cdr->as.cons.car;
    if (a == NULL || a->type != MINO_ATOM) {
        set_error(S, "reset!: first argument must be an atom");
        return NULL;
    }
    a->as.atom.val = val;
    return val;
}

/* (swap! atom f & args) -- applies (f current-val args...) and sets result. */
mino_val_t *prim_swap_bang(mino_state_t *S, mino_val_t *args, mino_env_t *env)
{
    mino_val_t *a, *fn, *cur, *call_args, *extra, *tail, *result;
    if (!mino_is_cons(args) || !mino_is_cons(args->as.cons.cdr)) {
        set_error(S, "swap! requires at least 2 arguments: atom and function");
        return NULL;
    }
    a  = args->as.cons.car;
    fn = args->as.cons.cdr->as.cons.car;
    if (a == NULL || a->type != MINO_ATOM) {
        set_error(S, "swap!: first argument must be an atom");
        return NULL;
    }
    cur = a->as.atom.val;
    /* Build arg list: (cur extra1 extra2 ...) */
    call_args = mino_nil(S);
    /* Append extra args in reverse then prepend cur. */
    extra = args->as.cons.cdr->as.cons.cdr; /* rest after fn */
    if (extra != NULL && extra->type == MINO_CONS) {
        /* Collect extras into a list. */
        tail = mino_nil(S);
        while (extra != NULL && extra->type == MINO_CONS) {
            tail = mino_cons(S, extra->as.cons.car, tail);
            extra = extra->as.cons.cdr;
        }
        /* Reverse to get correct order. */
        call_args = mino_nil(S);
        while (tail != NULL && tail->type == MINO_CONS) {
            call_args = mino_cons(S, tail->as.cons.car, call_args);
            tail = tail->as.cons.cdr;
        }
    }
    call_args = mino_cons(S, cur, call_args);
    result = mino_call(S, fn, call_args, env);
    if (result == NULL) return NULL;
    a->as.atom.val = result;
    return result;
}

mino_val_t *prim_atom_p(mino_state_t *S, mino_val_t *args, mino_env_t *env)
{
    (void)env;
    if (!mino_is_cons(args) || mino_is_cons(args->as.cons.cdr)) {
        set_error(S, "atom? requires one argument");
        return NULL;
    }
    return mino_is_atom(args->as.cons.car) ? mino_true(S) : mino_false(S);
}
