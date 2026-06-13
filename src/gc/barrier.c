/*
 * barrier.c -- write barrier, remembered set, and incremental-major
 * snapshot push.
 *
 * The barrier handles two tasks at every mutation of a GC-managed slot:
 *
 *  1. Remembered set. When the store creates an OLD -> YOUNG edge, the
 *     OLD container is appended to the remembered set so the next minor
 *     cycle can reach the YOUNG target. Duplicates are deduped via
 *     gc_hdr_t::dirty; the remset never grows past one entry per live
 *     OLD container per epoch. This runs in every phase.
 *
 *  2. Incremental-major snapshot push (Yuasa SATB plus Dijkstra
 *     insertion). While the major collector is in MAJOR_MARK, two
 *     pointers ride along onto the mark stack:
 *       - old_value (SATB): the previous slot contents. Any object
 *         reachable at snapshot time survives this cycle even if the
 *         mutator unlinks it before the mark frontier reaches it.
 *       - new_value (insertion barrier): the just-installed pointer.
 *         Catches the case where the snapshot path that used to keep
 *         an OLD reachable has been overwritten in this same store,
 *         and the only surviving path runs through the new edge.
 *     gc_mark_push deduplicates against h->mark, so the second push
 *     is free when the value was already in the snapshot.
 *
 * Singletons -- nil, true, false, small-int cache, recur/tail-call
 * sentinels -- live inside mino_state and are not GC-managed. The
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
 *   MINO_VAR     root                      gc_write_barrier (runtime/var.c)
 *   MINO_ATOM    val/watches/validator     gc_write_barrier (prim/stateful.c)
 *   MINO_LAZY    cached/body/env           gc_write_barrier (eval/eval.c force path)
 *   MINO_TRANSIENT current                 transient_set_current (collections/transient.c)
 *   mino_env   bindings / ht_buckets /
 *                per-binding val / per-
 *                binding name              gc_write_barrier (runtime/env.c)
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

#include "runtime/internal.h"

/* True iff p lies inside the mino_state struct, i.e. p is a singleton
 * or small-int cache entry rather than a GC allocation.
 * Declared in gc/internal.h so driver.c can call it without duplicating
 * the range check. */
int gc_ptr_is_state_embedded(const mino_state *S, const void *p)
{
    uintptr_t u  = (uintptr_t)p;
    uintptr_t lo = (uintptr_t)S;
    uintptr_t hi = lo + sizeof(*S);
    return (u >= lo && u < hi);
}

/* Ownership: caller retains container. The remset array is owned by S
 * (allocated with realloc, freed from state teardown). */
void gc_remset_add(mino_state *S, gc_hdr_t *container)
{
    if (S->gc.remset_len == S->gc.remset_cap) {
        size_t      new_cap;
        gc_hdr_t  **nr;
        if (S->gc.remset_cap == 0) {
            new_cap = 256;
        } else if (S->gc.remset_cap > SIZE_MAX / 2 / sizeof(*nr)) {
            abort(); /* Class I: capacity overflow inside write barrier */
        } else {
            new_cap = S->gc.remset_cap * 2;
        }
        nr = (gc_hdr_t **)realloc(S->gc.remset, new_cap * sizeof(*nr));
        if (nr == NULL) {
            /* Class I abort: the write barrier cannot silently drop a
             * remset entry — doing so would break the GC invariant that
             * every OLD container holding a YOUNG pointer is in the
             * remembered set, causing the next minor cycle to miss the
             * YOUNG referent and collect it while it is still live.
             * Heap corruption is certain; aborting is safer than
             * continuing. */
            abort(); /* Class I: GC remset OOM; skipping would break write barrier invariant */
        }
        S->gc.remset     = nr;
        S->gc.remset_cap = new_cap;
    }
    S->gc.remset[S->gc.remset_len++] = container;
    if (S->gc.remset_len > S->gc.remset_high_water) {
        S->gc.remset_high_water = S->gc.remset_len;
    }
    container->dirty = 1;
    gc_evt_record(S, GC_EVT_REMSET_ADD, container, NULL, NULL,
                  (uintptr_t)container->gen, 0);
}

void gc_write_barrier(mino_state *S, void *container,
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
    /* During active major marking, the slot store needs only the
     * Dijkstra (insertion) half of the barrier: enqueue the just-
     * installed value so any OLD it transitively reaches gets marked
     * even if the snapshot path that used to reach those OLDs has been
     * overwritten in the same write.
     *
     * The Yuasa (SATB) half that used to push old_value was removed
     * once gc_major_remark grew a full gc_mark_roots pass. Anything
     * the mutator drops from a slot during the cycle either:
     *
     *   - is still reachable through some other root; end-of-mark
     *     re-walks every root and captures it.
     *   - has lost every root path; correct collection on next sweep.
     *
     * Dijkstra still skips singletons (not GC-managed), NULL (empty
     * slot), and tagged inline values. gc_mark_push deduplicates
     * against h->mark, so the push is free when the value was already
     * in the snapshot or rooted. */
    if (S->gc.phase == GC_PHASE_MAJOR_MARK) {
        if (new_value != NULL
            && ((uintptr_t)new_value & MINO_TAG_MASK) == 0
            && !gc_ptr_is_state_embedded(S, new_value)) {
            gc_hdr_t *h_new_satb = ((gc_hdr_t *)new_value) - 1;
            if (!h_new_satb->mark) {
                gc_mark_push(S, h_new_satb);
                S->gc_barrier_dijkstra_pushes++;
            }
        }
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
    /* Tagged inline value (int/bool/nil/char): no heap object behind
     * this pointer, so no edge to record. */
    if (((uintptr_t)new_value & MINO_TAG_MASK) != 0) {
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

/* Re-add the bytecode VM register stacks to the remset. These buffers
 * are state-owned VALARRs whose slots receive YOUNG values from the
 * VM hot path WITHOUT routing through gc_write_barrier (the per-op
 * register write is the inner-loop cost that the design optimises
 * for). The minor collector compensates by walking the live slot
 * range from gc_mark_roots, but the remset itself stays unaware of
 * those edges -- which is fine for marking (gc_mark_roots covers it)
 * but trips MINO_GC_VERIFY=1 by leaving an OLD-with-YOUNG-slots
 * container outside the remset. Re-adding here after every reset
 * keeps the invariant "every OLD container with potential YOUNG
 * slots is in the remset" intact while costing one remset entry per
 * worker. */
static void gc_remset_pin_bc_regs(mino_state *S)
{
    mino_thread_ctx_t *w;
    if (S->bc.bc_regs != NULL) {
        gc_hdr_t *h = ((gc_hdr_t *)S->bc.bc_regs) - 1;
        if (h->gen == GC_GEN_OLD && !h->dirty) {
            gc_remset_add(S, h);
        }
    }
    mino_worker_list_lock_acquire(S);
    for (w = S->threading.worker_ctxs_head; w != NULL; w = w->next_worker) {
        if (w->bc_regs_storage != NULL) {
            gc_hdr_t *h = ((gc_hdr_t *)w->bc_regs_storage) - 1;
            if (h->gen == GC_GEN_OLD && !h->dirty) {
                gc_remset_add(S, h);
            }
        }
    }
    mino_worker_list_lock_release(S);
}

/* Filter pass: keep entries whose dirty bit is still set (the parent
 * still holds at least one OLD->YOUNG edge as observed by the trace-
 * time walker in gc_mark_remset), drop entries whose dirty bit was
 * cleared during the walk. Entries kept here ride into the next minor
 * with dirty=1; entries dropped here are out of the remset and will
 * be re-added by gc_write_barrier on the next mutator store that
 * creates a fresh OLD->YOUNG edge. The kept-entry path is the multi-
 * cycle safety net that the promote-then-add-once design lacked. */
void gc_remset_reset(mino_state *S)
{
    size_t i, dst = 0;
    gc_evt_record(S, GC_EVT_REMSET_RESET, NULL, NULL, NULL,
                  (uintptr_t)S->gc.remset_len, 0);
    for (i = 0; i < S->gc.remset_len; i++) {
        gc_hdr_t *h = S->gc.remset[i];
        if (h->dirty) {
            S->gc.remset[dst++] = h;
        }
    }
    S->gc.remset_len = dst;
    gc_remset_pin_bc_regs(S);
}

/* Drop remset entries whose container is about to be freed by sweep,
 * keep the rest. Used by major sweep instead of a full reset so that
 * OLD->YOUNG edges installed during MAJOR_MARK survive the cycle and
 * the next minor can reach the YOUNG targets. Containers are removed
 * from the remset purely to avoid dangling pointers after sweep;
 * their dirty bits would get zeroed on freelist reuse anyway. */
void gc_remset_purge_dead(mino_state *S)
{
    size_t i, dst = 0;
    size_t before = S->gc.remset_len;
    for (i = 0; i < S->gc.remset_len; i++) {
        gc_hdr_t *h = S->gc.remset[i];
        if (h->mark) {
            S->gc.remset[dst++] = h;
        }
    }
    S->gc.remset_len = dst;
    gc_evt_record(S, GC_EVT_REMSET_PURGE, NULL, NULL, NULL,
                  (uintptr_t)before, (uint16_t)dst);
}

/* List-building helper: tail-append a cons cell onto tail. Routes the
 * store through the write barrier so SATB sees the previous cdr and
 * the remset sees any old->young edge the append creates. Used by
 * every in-place list extension loop; caller must guarantee tail is
 * non-NULL and a cons cell. */
void mino_cons_cdr_set(mino_state *S, mino_val *tail, mino_val *cell)
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
void gc_valarr_set(mino_state *S, mino_val **arr, size_t i,
                   mino_val *v)
{
    gc_write_barrier(S, arr, arr[i], v);
    arr[i] = v;
}
