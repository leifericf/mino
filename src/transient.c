/*
 * transient.c -- C-level batch-mutation wrapper around persistent
 * vectors, maps, and sets.
 *
 * The public surface exposes mino_transient / mino_persistent and the
 * *_bang mutators declared in mino.h. This file is the embedders'
 * fast path: use it to build up a large persistent collection in a
 * loop without the (new-vec-per-step) allocation storm that the
 * fully-persistent ops would cause.
 *
 * The current implementation wraps the standard persistent operations
 * -- each mutator calls the matching prim_* entry point and updates
 * the wrapper's inner pointer. This is correct and complete, but it
 * still materialises an intermediate persistent value per step, so
 * the performance win is modest. A follow-up can replace the
 * wrapper with in-place mutation on edit-owned trie nodes (Clojure's
 * real transient path). The C API stays identical.
 */

#include "mino_internal.h"
#include "prim_internal.h"

#include <stdio.h>

/* ------------------------------------------------------------------------- */
/* Small helpers                                                             */
/* ------------------------------------------------------------------------- */

static mino_val_t *transient_error(mino_state_t *S, const char *msg)
{
    return prim_throw_classified(S, "eval/type", "MTY001", msg);
}

/* Build a one-link cons list (car . cdr). Callers pre-root any
 * values that must survive across the allocation. */
static mino_val_t *cons1(mino_state_t *S, mino_val_t *car, mino_val_t *cdr)
{
    return mino_cons(S, car, cdr);
}

/* ------------------------------------------------------------------------- */
/* Predicates and accessors                                                  */
/* ------------------------------------------------------------------------- */

int mino_is_transient(const mino_val_t *v)
{
    return v != NULL && v->type == MINO_TRANSIENT;
}

size_t mino_transient_count(const mino_val_t *t)
{
    if (!mino_is_transient(t) || !t->as.transient.valid) {
        return 0;
    }
    {
        const mino_val_t *c = t->as.transient.current;
        if (c == NULL) return 0;
        switch (c->type) {
        case MINO_VECTOR: return c->as.vec.len;
        case MINO_MAP:    return c->as.map.len;
        case MINO_SET:    return c->as.set.len;
        default:          return 0;
        }
    }
}

/* ------------------------------------------------------------------------- */
/* Transient creation and sealing                                            */
/* ------------------------------------------------------------------------- */

mino_val_t *mino_transient(mino_state_t *S, mino_val_t *coll)
{
    mino_val_t *t;
    if (coll == NULL
        || (coll->type != MINO_VECTOR
            && coll->type != MINO_MAP
            && coll->type != MINO_SET)) {
        return transient_error(S,
            "transient!: expected a persistent vector, map, or set");
    }
    t = alloc_val(S, MINO_TRANSIENT);
    t->as.transient.current = coll;
    t->as.transient.valid   = 1;
    return t;
}

mino_val_t *mino_persistent(mino_state_t *S, mino_val_t *t)
{
    mino_val_t *out;
    if (!mino_is_transient(t)) {
        return transient_error(S, "persistent!: expected a transient");
    }
    if (!t->as.transient.valid) {
        return transient_error(S,
            "persistent!: transient already sealed");
    }
    out = t->as.transient.current;
    /* Seal against further mutation. The GC can reclaim the transient
     * once the caller drops its reference; the persistent value it
     * returned is unaffected. */
    t->as.transient.valid   = 0;
    t->as.transient.current = NULL;
    return out;
}

/* ------------------------------------------------------------------------- */
/* Mutators                                                                  */
/* ------------------------------------------------------------------------- */

/* Guard shared by every *_bang. Returns 1 when t is a live transient,
 * 0 after installing a classified error (the caller propagates NULL). */
static int require_valid_transient(mino_state_t *S, const mino_val_t *t,
                                   const char *name)
{
    char buf[96];
    if (!mino_is_transient(t)) {
        snprintf(buf, sizeof(buf), "%s: expected a transient", name);
        (void)transient_error(S, buf);
        return 0;
    }
    if (!t->as.transient.valid) {
        snprintf(buf, sizeof(buf),
                 "%s: transient already sealed by persistent!", name);
        (void)transient_error(S, buf);
        return 0;
    }
    return 1;
}

mino_val_t *mino_assoc_bang(mino_state_t *S, mino_val_t *t,
                            mino_val_t *key, mino_val_t *val)
{
    mino_val_t *inner;
    mino_val_t *result;
    mino_val_t *args;
    if (!require_valid_transient(S, t, "assoc!")) return NULL;
    inner = t->as.transient.current;
    if (inner == NULL
        || (inner->type != MINO_VECTOR && inner->type != MINO_MAP)) {
        return transient_error(S,
            "assoc!: transient must wrap a vector or map");
    }
    /* Build (inner key val) and dispatch to the persistent op. */
    args = cons1(S, inner,
                 cons1(S, key, cons1(S, val, mino_nil(S))));
    result = prim_assoc(S, args, NULL);
    if (result == NULL) return NULL;
    t->as.transient.current = result;
    return t;
}

mino_val_t *mino_conj_bang(mino_state_t *S, mino_val_t *t,
                           mino_val_t *val)
{
    mino_val_t *inner;
    mino_val_t *result;
    mino_val_t *args;
    if (!require_valid_transient(S, t, "conj!")) return NULL;
    inner = t->as.transient.current;
    if (inner == NULL
        || (inner->type != MINO_VECTOR
            && inner->type != MINO_MAP
            && inner->type != MINO_SET)) {
        return transient_error(S,
            "conj!: transient must wrap a vector, map, or set");
    }
    args = cons1(S, inner, cons1(S, val, mino_nil(S)));
    result = prim_conj(S, args, NULL);
    if (result == NULL) return NULL;
    t->as.transient.current = result;
    return t;
}

mino_val_t *mino_dissoc_bang(mino_state_t *S, mino_val_t *t,
                             mino_val_t *key)
{
    mino_val_t *inner;
    mino_val_t *result;
    mino_val_t *args;
    if (!require_valid_transient(S, t, "dissoc!")) return NULL;
    inner = t->as.transient.current;
    if (inner == NULL || inner->type != MINO_MAP) {
        return transient_error(S, "dissoc!: transient must wrap a map");
    }
    args = cons1(S, inner, cons1(S, key, mino_nil(S)));
    result = prim_dissoc(S, args, NULL);
    if (result == NULL) return NULL;
    t->as.transient.current = result;
    return t;
}

mino_val_t *mino_disj_bang(mino_state_t *S, mino_val_t *t,
                           mino_val_t *key)
{
    mino_val_t *inner;
    mino_val_t *result;
    mino_val_t *args;
    if (!require_valid_transient(S, t, "disj!")) return NULL;
    inner = t->as.transient.current;
    if (inner == NULL || inner->type != MINO_SET) {
        return transient_error(S, "disj!: transient must wrap a set");
    }
    args = cons1(S, inner, cons1(S, key, mino_nil(S)));
    result = prim_disj(S, args, NULL);
    if (result == NULL) return NULL;
    t->as.transient.current = result;
    return t;
}

mino_val_t *mino_pop_bang(mino_state_t *S, mino_val_t *t)
{
    mino_val_t *inner;
    mino_val_t *result;
    mino_val_t *args;
    if (!require_valid_transient(S, t, "pop!")) return NULL;
    inner = t->as.transient.current;
    if (inner == NULL || inner->type != MINO_VECTOR) {
        return transient_error(S, "pop!: transient must wrap a vector");
    }
    args = cons1(S, inner, mino_nil(S));
    result = prim_pop(S, args, NULL);
    if (result == NULL) return NULL;
    t->as.transient.current = result;
    return t;
}
