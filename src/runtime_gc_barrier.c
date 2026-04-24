/*
 * runtime_gc_barrier.c -- write barrier, remembered set, and SATB push.
 *
 * The barrier handles two tasks at every mutation of a GC-managed slot:
 *
 *  1. Remembered set. When the store creates an OLD -> YOUNG edge, the
 *     OLD container is appended to the remembered set so the next minor
 *     cycle can reach the YOUNG target. Duplicates are deduped via
 *     gc_hdr_t::dirty; the remset never grows past one entry per live
 *     OLD container per epoch. This runs in every phase.
 *
 *  2. SATB push. While the major collector is in MAJOR_MARK, the
 *     value being overwritten (old_value) must survive this cycle
 *     even if the mutator breaks its only surviving link. The barrier
 *     pushes old_value onto the mark stack; gc_major_step will trace
 *     it on the next slice.
 *
 * Singletons -- nil, true, false, small-int cache, recur/tail-call
 * sentinels -- live inside mino_state_t and are not GC-managed. The
 * state-embedded check drops them on both paths so header arithmetic
 * on a singleton cannot corrupt neighbouring state fields.
 *
 * -------------------------------------------------------------------
 * Barrier mutation-site matrix
 * -------------------------------------------------------------------
 *
 * Every in-place mutation of a GC-managed slot on a post-initialisation
 * container must route through gc_write_barrier (directly or via a
 * typed helper below). Fresh allocations initialising fields for the
 * first time are exempt -- the container is young, all fields start as
 * GC nullish, and SATB on uninitialised memory would be a use-after-
 * read. If you add a new mutation site, add it to this table in the
 * same commit.
 *
 *   Type         Slot(s)                   Helper / direct call
 *   -----------------------------------------------------------------
 *   MINO_CONS    cdr                       mino_cons_cdr_set (helper)
 *                car                       -- immutable after construction --
 *   MINO_VAR     root                      gc_write_barrier (runtime_var.c)
 *   MINO_ATOM    val/watches/validator     gc_write_barrier (prim_stateful.c)
 *   MINO_LAZY    cached/body/env           gc_write_barrier (mino.c force path)
 *   MINO_TRANSIENT current                 transient_set_current (transient.c)
 *   mino_env_t   bindings / ht_buckets /
 *                per-binding val / per-
 *                binding name              gc_write_barrier (runtime_env.c)
 *   valarr_t     slot[i]                   gc_valarr_set (helper)
 *
 * Containers whose fields are immutable post-construction and therefore
 * need no barrier: MINO_STRING / MINO_SYMBOL / MINO_KEYWORD data, the
 * persistent-trie slots inside vector / map / set root nodes (path-copy
 * semantics produce a brand-new trie node per edit), MINO_FN params /
 * body / env, and sorted-map / sorted-set internals (red-black tree
 * nodes are rebuilt on insert).
 *
 * Debug guardrails. Two structural checks back this table up:
 *
 *  - MINO_GC_VERIFY=1: gc_verify_remset_complete walks every live OLD
 *    header after each minor cycle and aborts if a non-remset OLD
 *    container holds a YOUNG outgoing pointer. This is the canonical
 *    "did any mutation bypass the barrier" test; the matrix above
 *    lists what it covers.
 *
 *  - NDEBUG-off builds: the assertions below trap the obvious
 *    structural violations at each barrier call (container is a
 *    recognisable gc_hdr with a legal generation tag, old/new values
 *    are gc_hdr-ish or singleton-ish). Expensive invariant checks
 *    stay gated behind MINO_GC_VERIFY.
 */

#include "mino_internal.h"

/* True iff p lies inside the mino_state_t struct, i.e. p is a
 * singleton or small-int cache entry rather than a GC allocation. */
static int gc_ptr_is_state_embedded(const mino_state_t *S, const void *p)
{
    uintptr_t u  = (uintptr_t)p;
    uintptr_t lo = (uintptr_t)S;
    uintptr_t hi = lo + sizeof(*S);
    return (u >= lo && u < hi);
}

/* Ownership: caller retains container. The remset array is owned by S
 * (allocated with realloc, freed from state teardown). */
void gc_remset_add(mino_state_t *S, gc_hdr_t *container)
{
    if (S->gc_remset_len == S->gc_remset_cap) {
        size_t      new_cap = S->gc_remset_cap == 0 ? 256 : S->gc_remset_cap * 2;
        gc_hdr_t  **nr      = (gc_hdr_t **)realloc(S->gc_remset,
                                                   new_cap * sizeof(*nr));
        if (nr == NULL) {
            abort(); /* Class I: inside write barrier, no recovery path */
        }
        S->gc_remset     = nr;
        S->gc_remset_cap = new_cap;
    }
    S->gc_remset[S->gc_remset_len++] = container;
    if (S->gc_remset_len > S->gc_remset_high_water) {
        S->gc_remset_high_water = S->gc_remset_len;
    }
    container->dirty = 1;
    gc_evt_record(S, GC_EVT_REMSET_ADD, container, NULL, NULL,
                  (uintptr_t)container->gen, 0);
}

void gc_write_barrier(mino_state_t *S, void *container,
                      const void *old_value, const void *new_value)
{
    gc_hdr_t *h_container;
    gc_hdr_t *h_new;
    /* Debug guardrail: any barrier-managed container must be either
     * NULL, a singleton embedded in state, or preceded by a gc_hdr_t
     * with a legal generation tag. A garbage pointer here means the
     * caller is handing us a non-GC heap allocation and the subsequent
     * header arithmetic would read random memory. Gated on assert so
     * the fast path remains untouched in release. */
    assert(container == NULL
           || gc_ptr_is_state_embedded(S, container)
           || ((((gc_hdr_t *)container) - 1)->gen == GC_GEN_YOUNG
               || (((gc_hdr_t *)container) - 1)->gen == GC_GEN_OLD));
    gc_evt_record(S, GC_EVT_WB, container, old_value, new_value, 0, 0);
    /* SATB: during active major marking, enqueue the previous slot
     * value so it is visited before sweep. Skip singletons (not
     * GC-managed) and NULL (empty slot). */
    if (S->gc_phase == GC_PHASE_MAJOR_MARK
        && old_value != NULL
        && !gc_ptr_is_state_embedded(S, old_value)) {
        gc_hdr_t *h_old = ((gc_hdr_t *)old_value) - 1;
        gc_mark_push(S, h_old);
    }
    if (container == NULL) {
        return;
    }
    h_container = ((gc_hdr_t *)container) - 1;
    /* Fast path: a young container needs no remset entry -- the minor
     * collector scans young space in full every cycle. */
    if (h_container->gen == GC_GEN_YOUNG) {
        return;
    }
    if (h_container->dirty) {
        return;
    }
    /* Slot cleared: no new old->young edge. */
    if (new_value == NULL) {
        return;
    }
    /* Singleton target: not GC-managed, so there is nothing the minor
     * could ever do with a remset hit on this edge. */
    if (gc_ptr_is_state_embedded(S, new_value)) {
        return;
    }
    h_new = ((gc_hdr_t *)new_value) - 1;
    if (h_new->gen == GC_GEN_OLD) {
        return;
    }
    gc_remset_add(S, h_container);
}

void gc_remset_reset(mino_state_t *S)
{
    size_t i;
    gc_evt_record(S, GC_EVT_REMSET_RESET, NULL, NULL, NULL,
                  (uintptr_t)S->gc_remset_len, 0);
    for (i = 0; i < S->gc_remset_len; i++) {
        S->gc_remset[i]->dirty = 0;
    }
    S->gc_remset_len = 0;
}

/* Drop remset entries whose container is about to be freed by sweep,
 * keep the rest. Used by major sweep instead of a full reset so that
 * OLD->YOUNG edges installed during MAJOR_MARK survive the cycle and
 * the next minor can reach the YOUNG targets. Containers are removed
 * from the remset purely to avoid dangling pointers after sweep;
 * their dirty bits would get zeroed on freelist reuse anyway. */
void gc_remset_purge_dead(mino_state_t *S)
{
    size_t i, dst = 0;
    size_t before = S->gc_remset_len;
    for (i = 0; i < S->gc_remset_len; i++) {
        gc_hdr_t *h = S->gc_remset[i];
        if (h->mark) {
            S->gc_remset[dst++] = h;
        }
    }
    S->gc_remset_len = dst;
    gc_evt_record(S, GC_EVT_REMSET_PURGE, NULL, NULL, NULL,
                  (uintptr_t)before, (uint16_t)dst);
}

/* List-building helper: tail-append a cons cell onto tail. Routes the
 * store through the write barrier so SATB sees the previous cdr and
 * the remset sees any old->young edge the append creates. Used by
 * every in-place list extension loop; caller must guarantee tail is
 * non-NULL and a cons cell. */
void mino_cons_cdr_set(mino_state_t *S, mino_val_t *tail, mino_val_t *cell)
{
    gc_write_barrier(S, tail, tail->as.cons.cdr, cell);
    tail->as.cons.cdr = cell;
}

/* Scratch-array slot store. Routes the write through the barrier so a
 * VALARR that was freshly allocated by the caller and then promoted by
 * a mid-loop minor still has every subsequent YOUNG slot store
 * observed by the remset. The one-cycle safety net on promotion covers
 * only the cycle after promotion; a loop that spans two minors needs
 * the barrier on every slot. Payload-start is the array pointer
 * itself, which matches gc_write_barrier's container convention. */
void gc_valarr_set(mino_state_t *S, mino_val_t **arr, size_t i,
                   mino_val_t *v)
{
    gc_write_barrier(S, arr, arr[i], v);
    arr[i] = v;
}
