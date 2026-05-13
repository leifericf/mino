/*
 * collections/builders.c -- thin, embedder-friendly builders that wrap
 * transients with explicit names.
 *
 * Builders give C embedders an obvious idiom for constructing a
 * collection piece-by-piece without having to think about persistent
 * / transient semantics. Internally they are transients rooted via
 * mino_ref so a GC fired mid-build doesn't reclaim the in-flight
 * collection. _finish unroots and returns the persistent value.
 *
 * Use mino_vector / mino_map / mino_set when the elements are already
 * sitting in C arrays; use builders when the elements are produced one
 * at a time.
 */

#include "runtime/internal.h"

struct mino_vec_builder { mino_state_t *S; mino_ref_t *ref; };
struct mino_map_builder { mino_state_t *S; mino_ref_t *ref; };
struct mino_set_builder { mino_state_t *S; mino_ref_t *ref; };

mino_vec_builder_t *mino_vector_builder_new(mino_state_t *S)
{
    mino_vec_builder_t *b;
    mino_val_t *seed;
    if (S == NULL) return NULL;
    b = (mino_vec_builder_t *)malloc(sizeof(*b));
    if (b == NULL) return NULL;
    seed = mino_transient(S, mino_vector(S, NULL, 0));
    if (seed == NULL) { free(b); return NULL; }
    b->S   = S;
    b->ref = mino_ref(S, seed);
    return b;
}

void mino_vector_builder_push(mino_vec_builder_t *b, mino_val_t *v)
{
    mino_val_t *cur, *next;
    if (b == NULL) return;
    cur  = mino_deref(b->ref);
    next = mino_conj_bang(b->S, cur, v);
    if (next != NULL && next != cur) {
        mino_unref(b->S, b->ref);
        b->ref = mino_ref(b->S, next);
    }
}

mino_val_t *mino_vector_builder_finish(mino_vec_builder_t *b)
{
    mino_val_t *out;
    if (b == NULL) return NULL;
    out = mino_persistent(b->S, mino_deref(b->ref));
    mino_unref(b->S, b->ref);
    free(b);
    return out;
}

mino_map_builder_t *mino_map_builder_new(mino_state_t *S)
{
    mino_map_builder_t *b;
    mino_val_t *seed;
    if (S == NULL) return NULL;
    b = (mino_map_builder_t *)malloc(sizeof(*b));
    if (b == NULL) return NULL;
    seed = mino_transient(S, mino_map(S, NULL, NULL, 0));
    if (seed == NULL) { free(b); return NULL; }
    b->S   = S;
    b->ref = mino_ref(S, seed);
    return b;
}

void mino_map_builder_put(mino_map_builder_t *b, mino_val_t *k, mino_val_t *v)
{
    mino_val_t *cur, *next;
    if (b == NULL) return;
    cur  = mino_deref(b->ref);
    next = mino_assoc_bang(b->S, cur, k, v);
    if (next != NULL && next != cur) {
        mino_unref(b->S, b->ref);
        b->ref = mino_ref(b->S, next);
    }
}

mino_val_t *mino_map_builder_finish(mino_map_builder_t *b)
{
    mino_val_t *out;
    if (b == NULL) return NULL;
    out = mino_persistent(b->S, mino_deref(b->ref));
    mino_unref(b->S, b->ref);
    free(b);
    return out;
}

mino_set_builder_t *mino_set_builder_new(mino_state_t *S)
{
    mino_set_builder_t *b;
    mino_val_t *seed;
    if (S == NULL) return NULL;
    b = (mino_set_builder_t *)malloc(sizeof(*b));
    if (b == NULL) return NULL;
    seed = mino_transient(S, mino_set(S, NULL, 0));
    if (seed == NULL) { free(b); return NULL; }
    b->S   = S;
    b->ref = mino_ref(S, seed);
    return b;
}

void mino_set_builder_add(mino_set_builder_t *b, mino_val_t *v)
{
    mino_val_t *cur, *next;
    if (b == NULL) return;
    cur  = mino_deref(b->ref);
    next = mino_conj_bang(b->S, cur, v);
    if (next != NULL && next != cur) {
        mino_unref(b->S, b->ref);
        b->ref = mino_ref(b->S, next);
    }
}

mino_val_t *mino_set_builder_finish(mino_set_builder_t *b)
{
    mino_val_t *out;
    if (b == NULL) return NULL;
    out = mino_persistent(b->S, mino_deref(b->ref));
    mino_unref(b->S, b->ref);
    free(b);
    return out;
}
