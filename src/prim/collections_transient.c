/*
 * collections_transient.c -- transient primitives (thin wrappers
 * around the C kernel in src/collections/transient.c). The kernel
 * enforces the validity-bit invariant and write-barrier discipline;
 * the primitives just marshal args. Carved out of prim/collections.c.
 *
 * Internal to the prim subsystem; embedders should only use mino.h.
 */

#include "prim/internal.h"

#include <stdio.h>
#include <string.h>

/* ------------------------------------------------------------------------- */
/* Transient primitives                                                       */
/*                                                                            */
/* Thin wrappers around the C kernel in transient.c. The kernel enforces     */
/* the validity-bit invariant and write-barrier discipline; the primitives   */
/* just marshal args. Supported inner types are vector, map, and set.        */
/* ------------------------------------------------------------------------- */

mino_val *prim_transient(mino_state *S, mino_val *args, mino_env *env)
{
    size_t n;
    (void)env;
    arg_count(S, args, &n);
    if (n != 1) {
        return prim_throw_classified(S, "eval/arity", "MAR001",
            "transient requires one argument");
    }
    return mino_transient(S, args->as.cons.car);
}

mino_val *prim_persistent_bang(mino_state *S, mino_val *args, mino_env *env)
{
    size_t n;
    (void)env;
    arg_count(S, args, &n);
    if (n != 1) {
        return prim_throw_classified(S, "eval/arity", "MAR001",
            "persistent! requires one argument");
    }
    return mino_persistent(S, args->as.cons.car);
}

mino_val *prim_assoc_bang(mino_state *S, mino_val *args, mino_env *env)
{
    /* Clojure: (assoc! tcoll k v & kvs) -- extra args supplied as
     * key/value pairs and assoc'd left-to-right. Unlike assoc, assoc!
     * accepts a trailing odd-out key (treats its value as nil); this
     * is the documented Clojure JVM behaviour the conformance suite
     * exercises. The final transient is returned. */
    size_t      n;
    mino_val *t;
    mino_val *p;
    (void)env;
    arg_count(S, args, &n);
    if (n < 3) {
        return prim_throw_classified(S, "eval/arity", "MAR001",
            "assoc! requires at least three arguments");
    }
    t = args->as.cons.car;
    p = args->as.cons.cdr;
    while (mino_is_cons(p)) {
        mino_val *k = p->as.cons.car;
        mino_val *v;
        if (mino_is_cons(p->as.cons.cdr)) {
            v = p->as.cons.cdr->as.cons.car;
            p = p->as.cons.cdr->as.cons.cdr;
        } else {
            /* Odd-out key: pair value defaults to nil. */
            v = mino_nil(S);
            p = p->as.cons.cdr;
        }
        t = mino_assoc_bang(S, t, k, v);
        if (t == NULL) return NULL;
    }
    return t;
}

mino_val *prim_conj_bang(mino_state *S, mino_val *args, mino_env *env)
{
    /* Clojure: (conj!)              -> (transient [])
     *          (conj! tcoll)        -> tcoll unchanged
     *          (conj! tcoll x)      -> tcoll with x added
     *          (conj! tcoll x & xs) -> additional values conj'd left-
     *                                  to-right; final transient is
     *                                  returned. */
    mino_val *t;
    mino_val *p;
    (void)env;
    if (!mino_is_cons(args)) {
        return mino_transient(S, mino_vector(S, NULL, 0));
    }
    t = args->as.cons.car;
    p = args->as.cons.cdr;
    while (mino_is_cons(p)) {
        t = mino_conj_bang(S, t, p->as.cons.car);
        if (t == NULL) return NULL;
        p = p->as.cons.cdr;
    }
    return t;
}

mino_val *prim_dissoc_bang(mino_state *S, mino_val *args, mino_env *env)
{
    /* Clojure: (dissoc! tcoll k & ks) -- additional keys removed
     * left-to-right; the final transient is returned. */
    size_t      n;
    mino_val *t;
    mino_val *p;
    (void)env;
    arg_count(S, args, &n);
    if (n < 2) {
        return prim_throw_classified(S, "eval/arity", "MAR001",
            "dissoc! requires at least a transient and a key");
    }
    t = args->as.cons.car;
    p = args->as.cons.cdr;
    while (mino_is_cons(p)) {
        t = mino_dissoc_bang(S, t, p->as.cons.car);
        if (t == NULL) return NULL;
        p = p->as.cons.cdr;
    }
    return t;
}

mino_val *prim_disj_bang(mino_state *S, mino_val *args, mino_env *env)
{
    /* Clojure: (disj! tcoll k & ks) -- additional keys removed
     * left-to-right; the final transient is returned. */
    size_t      n;
    mino_val *t;
    mino_val *p;
    (void)env;
    arg_count(S, args, &n);
    if (n < 2) {
        return prim_throw_classified(S, "eval/arity", "MAR001",
            "disj! requires at least a transient and a key");
    }
    t = args->as.cons.car;
    p = args->as.cons.cdr;
    while (mino_is_cons(p)) {
        t = mino_disj_bang(S, t, p->as.cons.car);
        if (t == NULL) return NULL;
        p = p->as.cons.cdr;
    }
    return t;
}

mino_val *prim_pop_bang(mino_state *S, mino_val *args, mino_env *env)
{
    size_t n;
    (void)env;
    arg_count(S, args, &n);
    if (n != 1) {
        return prim_throw_classified(S, "eval/arity", "MAR001",
            "pop! requires one argument");
    }
    return mino_pop_bang(S, args->as.cons.car);
}

mino_val *prim_transient_p(mino_state *S, mino_val *args, mino_env *env)
{
    size_t n;
    (void)env;
    arg_count(S, args, &n);
    if (n != 1) {
        return prim_throw_classified(S, "eval/arity", "MAR001",
            "transient? requires one argument");
    }
    return mino_is_transient(args->as.cons.car) ? mino_true(S) : mino_false(S);
}
