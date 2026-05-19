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

#include "runtime/internal.h"
#include "prim/internal.h"
#include "collections/internal.h"  /* vec_conj1_owned / assoc1_owned / pop_owned */

#include <stdio.h>
#include <stdlib.h>

/* ------------------------------------------------------------------------- */
/* Small helpers                                                             */
/* ------------------------------------------------------------------------- */

static mino_val_t *transient_error(mino_state_t *S, const char *msg)
{
    return prim_throw_classified(S, "eval/type", "MTY001", msg);
}

static void transient_set_current(mino_state_t *S, mino_val_t *t,
                                  mino_val_t *new_inner);

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
    return v != NULL && mino_type_of(v) == MINO_TRANSIENT;
}

size_t mino_transient_count(const mino_val_t *t)
{
    if (!mino_is_transient(t) || !t->as.transient.valid) {
        return 0;
    }
    {
        const mino_val_t *c = t->as.transient.current;
        if (c == NULL) return 0;
        switch (mino_type_of(c)) {
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
        || (mino_type_of(coll) != MINO_VECTOR
            && mino_type_of(coll) != MINO_MAP
            && mino_type_of(coll) != MINO_SET)) {
        return transient_error(S,
            "transient!: expected a persistent vector, map, or set");
    }
    t = alloc_val(S, MINO_TRANSIENT);
    t->as.transient.current  = coll;
    t->as.transient.valid    = 1;
    /* Mint a fresh monotonic owner ID. The pre-increment skips 0
     * (reserved for "not owned by any transient"). On the rare
     * 32-bit wraparound we leave owner_id at 0 so vec_*_owned's
     * `node->owner == owner_id` check stays false-on-NULL and the
     * transient falls back to a path-copy wrapper instead of
     * spuriously claiming nodes whose IDs collided with a long-dead
     * transient. */
    if (S->ns_vars.transient_owner_next == 0xFFFFFFFFu) {
        t->as.transient.owner_id = 0;
    } else {
        t->as.transient.owner_id = (uintptr_t)(++S->ns_vars.transient_owner_next);
    }
    return t;
}

/* MINO_COLL_SIZE_STATS=1 tri-state sniff. Returns 1 when enabled. */
static int coll_size_stats_enabled(mino_state_t *S)
{
    if (S->coll_size_stats_enabled == 0) {
        const char *e = getenv("MINO_COLL_SIZE_STATS");
        S->coll_size_stats_enabled = (e != NULL && e[0] != '\0' && e[0] != '0')
                                     ? 1 : -1;
    }
    return S->coll_size_stats_enabled == 1;
}

/* Tick the size histogram for one finalized collection. kind: 0=vec,
 * 1=map, 2=set. bucket = clamp(floor(log2(size+1)), 0..31). */
static void coll_size_tick(mino_state_t *S, int kind, size_t size)
{
    unsigned bucket;
    size_t n;
    if (!coll_size_stats_enabled(S)) return;
    if (kind < 0 || kind > 2) return;
    bucket = 0u;
    n = size + 1u;
    while ((n >> 1) > 0u && bucket < 31u) {
        n >>= 1;
        bucket++;
    }
    S->coll_size_hist[kind][bucket]++;
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
    /* Seal against further mutation. The store to current routes
     * through the barrier so SATB sees the outgoing inner during a
     * mid-cycle MAJOR_MARK; the caller's stack slot roots the return
     * value for everything after. */
    t->as.transient.valid = 0;
    transient_set_current(S, t, NULL);
    /* Instrumentation: tick the collection-size histogram for the
     * finalized inner. The kind / size lookups are env-gated so the
     * default path is one branch + return. */
    if (out != NULL) {
        switch (mino_type_of(out)) {
        case MINO_VECTOR: coll_size_tick(S, 0, out->as.vec.len); break;
        case MINO_MAP:    coll_size_tick(S, 1, out->as.map.len); break;
        case MINO_SET:    coll_size_tick(S, 2, out->as.set.len); break;
        default: break;
        }
    }
    return out;
}

/* ------------------------------------------------------------------------- */
/* Mutators                                                                  */
/* ------------------------------------------------------------------------- */

/* Update the transient's inner pointer. Must route through the write
 * barrier: a long batch loop can promote t to OLD, after which a
 * direct store of a YOUNG persistent result would install an
 * OLD->YOUNG edge that the remset never hears about. */
static void transient_set_current(mino_state_t *S, mino_val_t *t,
                                  mino_val_t *new_inner)
{
    gc_write_barrier(S, t, t->as.transient.current, new_inner);
    t->as.transient.current = new_inner;
}

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
        || (mino_type_of(inner) != MINO_VECTOR && mino_type_of(inner) != MINO_MAP)) {
        return transient_error(S,
            "assoc!: transient must wrap a vector or map");
    }
    /* Vector branch: owner-tagged in-place edit on tail / trie spine.
     * owner_id == 0 means the 32-bit owner space has wrapped; we fall
     * through to the wrapper-style path so the owner==0 mismatch
     * never spuriously claims a persistent node. */
    if (mino_type_of(inner) == MINO_VECTOR
        && t->as.transient.owner_id != 0) {
        if (!mino_val_int_p(key)) {
            return transient_error(S,
                "assoc!: vector key must be an integer index");
        }
        {
            long long idx = mino_val_int_get(key);
            if (idx < 0 || (size_t)idx > inner->as.vec.len) {
                return transient_error(S, "assoc!: index out of range");
            }
            result = vec_assoc1_owned(S, inner, (size_t)idx, val, t->as.transient.owner_id);
            if (result == NULL) return NULL;
            transient_set_current(S, t, result);
            return t;
        }
    }
    /* Map branch: owner-tagged HAMT walk. The first assoc! against a
     * fresh transient clones spine + slots arrays once; subsequent
     * assoc!'s with owner-matching nodes mutate slot pointers in place
     * (and key_order via vec_conj1_owned when the key is new).
     * owner_id == 0 wraparound falls through to the persistent path. */
    if (t->as.transient.owner_id != 0) {
        result = mino_map_assoc1_owned(S, inner, key, val,
                                         t->as.transient.owner_id);
        if (result == NULL) return NULL;
        transient_set_current(S, t, result);
        return t;
    }
    args = cons1(S, inner,
                 cons1(S, key, cons1(S, val, mino_nil(S))));
    result = prim_assoc(S, args, NULL);
    if (result == NULL) return NULL;
    transient_set_current(S, t, result);
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
        || (mino_type_of(inner) != MINO_VECTOR
            && mino_type_of(inner) != MINO_MAP
            && mino_type_of(inner) != MINO_SET)) {
        return transient_error(S,
            "conj!: transient must wrap a vector, map, or set");
    }
    /* Vector branch: owner-tagged tail / trie edit. The first conj!
     * against a fresh transient clones the tail (owner mismatch);
     * subsequent conj!'s within the same 32-slot chunk are a single
     * slot write + count bump with no allocation. owner_id == 0
     * means the 32-bit ID space wrapped; fall through to the
     * wrapper path so owner==0 nodes are not spuriously claimed. */
    if (mino_type_of(inner) == MINO_VECTOR
        && t->as.transient.owner_id != 0) {
        result = vec_conj1_owned(S, inner, val, t->as.transient.owner_id);
        if (result == NULL) return NULL;
        transient_set_current(S, t, result);
        return t;
    }
    /* Set branch: owner-tagged HAMT + key_order. conj! on a map
     * transient with a 2-element pair value falls through to
     * mino_assoc_bang via prim_conj's wrapper-style dispatch --
     * routing here would force a fast/slow split for that uncommon
     * shape. */
    if (mino_type_of(inner) == MINO_SET
        && t->as.transient.owner_id != 0) {
        result = set_conj1_owned(S, inner, val, t->as.transient.owner_id);
        if (result == NULL) return NULL;
        transient_set_current(S, t, result);
        return t;
    }
    /* Map branch (conj! with a [k v] pair) and owner_id == 0 fallback:
     * keep the wrapper-style dispatch. */
    args = cons1(S, inner, cons1(S, val, mino_nil(S)));
    result = prim_conj(S, args, NULL);
    if (result == NULL) return NULL;
    transient_set_current(S, t, result);
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
    if (inner == NULL || mino_type_of(inner) != MINO_MAP) {
        return transient_error(S, "dissoc!: transient must wrap a map");
    }
    if (t->as.transient.owner_id != 0) {
        result = mino_map_dissoc1_owned(S, inner, key,
                                          t->as.transient.owner_id);
        if (result == NULL) return NULL;
        transient_set_current(S, t, result);
        return t;
    }
    args = cons1(S, inner, cons1(S, key, mino_nil(S)));
    result = prim_dissoc(S, args, NULL);
    if (result == NULL) return NULL;
    transient_set_current(S, t, result);
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
    if (inner == NULL || mino_type_of(inner) != MINO_SET) {
        return transient_error(S, "disj!: transient must wrap a set");
    }
    if (t->as.transient.owner_id != 0) {
        result = set_disj1_owned(S, inner, key, t->as.transient.owner_id);
        if (result == NULL) return NULL;
        transient_set_current(S, t, result);
        return t;
    }
    args = cons1(S, inner, cons1(S, key, mino_nil(S)));
    result = prim_disj(S, args, NULL);
    if (result == NULL) return NULL;
    transient_set_current(S, t, result);
    return t;
}

mino_val_t *mino_pop_bang(mino_state_t *S, mino_val_t *t)
{
    mino_val_t *inner;
    mino_val_t *result;
    mino_val_t *args;
    if (!require_valid_transient(S, t, "pop!")) return NULL;
    inner = t->as.transient.current;
    if (inner == NULL || mino_type_of(inner) != MINO_VECTOR) {
        return transient_error(S, "pop!: transient must wrap a vector");
    }
    if (inner->as.vec.len == 0) {
        return transient_error(S, "pop!: empty vector");
    }
    if (t->as.transient.owner_id != 0) {
        result = vec_pop_owned(S, inner, t->as.transient.owner_id);
        if (result == NULL) return NULL;
        (void)args;
        transient_set_current(S, t, result);
        return t;
    }
    /* owner_id == 0 means the 32-bit ID space wrapped; fall back to
     * the persistent wrapper path so owner==0 nodes are not
     * spuriously claimed. */
    args = cons1(S, inner, mino_nil(S));
    result = prim_pop(S, args, NULL);
    if (result == NULL) return NULL;
    transient_set_current(S, t, result);
    return t;
}
