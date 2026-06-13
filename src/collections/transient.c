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
 * The owned-path mutators (owner_id != 0) call collection-layer helpers
 * directly: vec_conj1_owned / vec_assoc1_owned / vec_pop_owned,
 * mino_map_assoc1_owned / mino_map_dissoc1_owned, and
 * set_conj1_owned / set_disj1_owned (all in collections/internal.h).
 *
 * The owner_id == 0 fallback paths (32-bit ID counter wrapped) use the
 * persistent collection helpers: vec_conj1, vec_assoc1, vec_pop,
 * mino_map_assoc1, mino_map_dissoc1, set_conj1_owned(..., 0).
 * Passing owner = 0 to the _owned variants is safe: no node can have
 * owner == 0, so all walks degrade to path-copy (persistent) behaviour.
 *
 * Error reporting uses set_eval_diag (available via runtime/internal.h),
 * which longjmps to the active try frame when try_depth > 0, matching
 * the semantics previously provided by prim_throw_classified.
 */

#include "runtime/internal.h"
#include "collections/internal.h"

#include <stdio.h>
#include <stdlib.h>

/* Portability shim for the owner-ID CAS loop. Mirrors the tc_load / tc_add
 * pattern in src/runtime/host_threads.h. */
#if defined(_MSC_VER)
#include <windows.h>
static uint32_t transient_atomic_load_u32(uint32_t volatile *p)
{
    return (uint32_t)InterlockedCompareExchange((LONG volatile *)p, 0, 0);
}
static int transient_cas_u32(uint32_t volatile *p,
                              uint32_t *expected, uint32_t desired)
{
    uint32_t old = (uint32_t)InterlockedCompareExchange(
        (LONG volatile *)p, (LONG)desired, (LONG)*expected);
    if (old == *expected) return 1;
    *expected = old;
    return 0;
}
#else
static uint32_t transient_atomic_load_u32(uint32_t volatile *p)
{
    return __atomic_load_n(p, __ATOMIC_RELAXED);
}
static int transient_cas_u32(uint32_t volatile *p,
                              uint32_t *expected, uint32_t desired)
{
    return __atomic_compare_exchange_n(p, expected, desired, 0,
                                       __ATOMIC_RELAXED, __ATOMIC_RELAXED);
}
#endif

/* ------------------------------------------------------------------------- */
/* Small helpers                                                             */
/* ------------------------------------------------------------------------- */

static mino_val *transient_error(mino_state *S, const char *msg)
{
    set_eval_diag(S, NULL, "eval/type", "MTY001", msg);
    return NULL;
}

static void transient_set_current(mino_state *S, mino_val *t,
                                  mino_val *new_inner);


/* ------------------------------------------------------------------------- */
/* Predicates and accessors                                                  */
/* ------------------------------------------------------------------------- */

int mino_is_transient(const mino_val *v)
{
    return v != NULL && mino_type_of(v) == MINO_TRANSIENT;
}

size_t mino_transient_count(const mino_val *t)
{
    if (!mino_is_transient(t) || !t->as.transient.valid) {
        return 0;
    }
    {
        const mino_val *c = t->as.transient.current;
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

mino_val *mino_transient(mino_state *S, mino_val *coll)
{
    mino_val *t;
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
     * 32-bit wraparound the counter sticks at 0xFFFFFFFFu and every
     * subsequent mint takes owner_id = 0 so vec_*_owned's
     * `node->owner == owner_id` check stays false-on-NULL and the
     * transient falls back to a path-copy wrapper instead of
     * spuriously claiming nodes whose IDs collided with a long-dead
     * transient.
     *
     * The mint is a CAS loop so a state shared by host worker
     * threads -- (future ...) bodies, embedder threads holding their
     * own state_lock window, future refactors that elide the global
     * state_lock around a transient site -- never publishes the same
     * owner_id to two transients. Without that, hnode_ensure_owned
     * would see a stale `node->owner == owner_id` match and edit a
     * persistent subtree in place, silently breaking immutability
     * for the original collection. RELAXED ordering is sufficient:
     * the owner_id is opaque except to the owner check, which
     * compares for identity, not happens-before. */
    {
        uint32_t prev = transient_atomic_load_u32(
            (uint32_t volatile *)&S->ns_vars.transient_owner_next);
        for (;;) {
            uint32_t next;
            if (prev == 0xFFFFFFFFu) {
                t->as.transient.owner_id = 0;
                break;
            }
            next = prev + 1u;
            if (transient_cas_u32(
                    (uint32_t volatile *)&S->ns_vars.transient_owner_next,
                    &prev, next)) {
                t->as.transient.owner_id = (uintptr_t)next;
                break;
            }
            /* CAS failed: prev now holds the observed counter; retry. */
        }
    }
    return t;
}

/* MINO_COLL_SIZE_STATS=1 tri-state sniff. Returns 1 when enabled. */
static int coll_size_stats_enabled(mino_state *S)
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
static void coll_size_tick(mino_state *S, int kind, size_t size)
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

mino_val *mino_persistent(mino_state *S, mino_val *t)
{
    mino_val *out;
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
static void transient_set_current(mino_state *S, mino_val *t,
                                  mino_val *new_inner)
{
    gc_write_barrier(S, t, t->as.transient.current, new_inner);
    t->as.transient.current = new_inner;
}

/* Guard shared by every *_bang. Returns 1 when t is a live transient,
 * 0 after installing a classified error (the caller propagates NULL). */
static int require_valid_transient(mino_state *S, const mino_val *t,
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

mino_val *mino_assoc_bang(mino_state *S, mino_val *t,
                            mino_val *key, mino_val *val)
{
    mino_val *inner;
    mino_val *result;
    if (!require_valid_transient(S, t, "assoc!")) return NULL;
    inner = t->as.transient.current;
    if (inner == NULL
        || (mino_type_of(inner) != MINO_VECTOR && mino_type_of(inner) != MINO_MAP)) {
        return transient_error(S,
            "assoc!: transient must wrap a vector or map");
    }
    /* Vector branch: owner-tagged in-place edit on tail / trie spine.
     * owner_id == 0 means the 32-bit owner space has wrapped; we fall
     * through to the persistent-path below so owner==0 never spuriously
     * claims a persistent node. */
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
    if (mino_type_of(inner) == MINO_MAP && t->as.transient.owner_id != 0) {
        result = mino_map_assoc1_owned(S, inner, key, val,
                                         t->as.transient.owner_id);
        if (result == NULL) return NULL;
        transient_set_current(S, t, result);
        return t;
    }
    /* owner_id == 0 fallback: use persistent helpers directly.
     * Both VEC and MAP can reach here on 32-bit ID counter wraparound. */
    if (mino_type_of(inner) == MINO_VECTOR) {
        long long idx;
        if (!mino_val_int_p(key)) {
            return transient_error(S,
                "assoc!: vector key must be an integer index");
        }
        idx = mino_val_int_get(key);
        if (idx < 0 || (size_t)idx > inner->as.vec.len) {
            return transient_error(S, "assoc!: index out of range");
        }
        result = vec_assoc1(S, inner, (size_t)idx, val);
    } else {
        result = mino_map_assoc1(S, inner, key, val);
    }
    if (result == NULL) return NULL;
    transient_set_current(S, t, result);
    return t;
}

mino_val *mino_conj_bang(mino_state *S, mino_val *t,
                           mino_val *val)
{
    mino_val *inner;
    mino_val *result;
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
     * persistent path so owner==0 nodes are not spuriously claimed. */
    if (mino_type_of(inner) == MINO_VECTOR
        && t->as.transient.owner_id != 0) {
        result = vec_conj1_owned(S, inner, val, t->as.transient.owner_id);
        if (result == NULL) return NULL;
        transient_set_current(S, t, result);
        return t;
    }
    /* Set branch: owner-tagged HAMT + key_order. */
    if (mino_type_of(inner) == MINO_SET
        && t->as.transient.owner_id != 0) {
        result = set_conj1_owned(S, inner, val, t->as.transient.owner_id);
        if (result == NULL) return NULL;
        transient_set_current(S, t, result);
        return t;
    }
    /* Map branch: extract [k v] pair or map-entry and call assoc.
     * Uses the owned variant when owner_id != 0 for in-place HAMT edits;
     * falls back to persistent mino_map_assoc1 when owner_id == 0. */
    if (mino_type_of(inner) == MINO_MAP) {
        mino_val *k;
        mino_val *v;
        if (mino_type_of(val) == MINO_VECTOR && val->as.vec.len == 2) {
            k = vec_nth(val, 0);
            v = vec_nth(val, 1);
        } else if (mino_type_of(val) == MINO_MAP_ENTRY) {
            k = val->as.map_entry.k;
            v = val->as.map_entry.v;
        } else {
            return transient_error(S,
                "conj!: map transient requires a 2-element vector or map entry");
        }
        result = (t->as.transient.owner_id != 0)
            ? mino_map_assoc1_owned(S, inner, k, v,
                                      t->as.transient.owner_id)
            : mino_map_assoc1(S, inner, k, v);
        if (result == NULL) return NULL;
        transient_set_current(S, t, result);
        return t;
    }
    /* VEC or SET with owner_id == 0 (32-bit ID counter wrapped):
     * use persistent helpers; owner==0 in _owned is safe (path-copy). */
    if (mino_type_of(inner) == MINO_VECTOR) {
        result = vec_conj1(S, inner, val);
    } else {
        /* SET */
        result = set_conj1_owned(S, inner, val, 0);
    }
    if (result == NULL) return NULL;
    transient_set_current(S, t, result);
    return t;
}

mino_val *mino_dissoc_bang(mino_state *S, mino_val *t,
                             mino_val *key)
{
    mino_val *inner;
    mino_val *result;
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
    /* owner_id == 0 fallback: use persistent helper directly. */
    result = mino_map_dissoc1(S, inner, key);
    if (result == NULL) return NULL;
    transient_set_current(S, t, result);
    return t;
}

mino_val *mino_disj_bang(mino_state *S, mino_val *t,
                           mino_val *key)
{
    mino_val *inner;
    mino_val *result;
    if (!require_valid_transient(S, t, "disj!")) return NULL;
    inner = t->as.transient.current;
    if (inner == NULL || mino_type_of(inner) != MINO_SET) {
        return transient_error(S, "disj!: transient must wrap a set");
    }
    /* Owned and owner_id==0 fallback both use set_disj1_owned.
     * With owner==0, hamt_dissoc_owned degrades to path-copy (no node
     * has owner==0), giving persistent semantics safely. */
    result = set_disj1_owned(S, inner, key, t->as.transient.owner_id);
    if (result == NULL) return NULL;
    transient_set_current(S, t, result);
    return t;
}

mino_val *mino_pop_bang(mino_state *S, mino_val *t)
{
    mino_val *inner;
    mino_val *result;
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
        transient_set_current(S, t, result);
        return t;
    }
    /* owner_id == 0 means the 32-bit ID space wrapped; use the
     * persistent vec_pop so owner==0 nodes are not spuriously claimed. */
    result = vec_pop(S, inner);
    if (result == NULL) return NULL;
    transient_set_current(S, t, result);
    return t;
}
