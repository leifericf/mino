/*
 * runtime_gc_barrier.c -- write barrier and remembered-set storage.
 *
 * The barrier is called at every source-level mutation site that can
 * install a new pointer into a GC-managed container. Only five such
 * sites exist in mino today: atom.val, atom.watches, atom.validator,
 * and lazy.cached (twice in lazy_force). Every structural
 * collection is built up-then-frozen, so the written-after path that
 * the barrier has to cover is small and enumerable.
 *
 * Policy: when an OLD-gen container observes a pointer to a YOUNG-gen
 * value, record the container in the remembered set so the next minor
 * collection can pick up the referenced young object. Stores from
 * YOUNG or to NULL/OLD are free -- they cannot create a new old->young
 * reference. Duplicates are deduped via gc_hdr_t::dirty so the remset
 * never grows past one entry per live old-gen container in each epoch.
 *
 * While no minor collector exists yet, the barrier still installs the
 * bookkeeping. With every allocation starting YOUNG and no promotion
 * path, gc_write_barrier_val short-circuits at the container-is-young
 * fast path and never appends; the remset stays empty and the full-
 * heap major collector remains correct.
 */

#include "mino_internal.h"

/* Ownership: caller retains container. The remset array is owned by S
 * (allocated with realloc, freed from state teardown). */
static void gc_remset_append(mino_state_t *S, gc_hdr_t *container)
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
    container->dirty = 1;
}

void gc_write_barrier_val(mino_state_t *S, mino_val_t *container,
                          const mino_val_t *new_value)
{
    gc_hdr_t *h_container;
    gc_hdr_t *h_new;
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
    /* No value (field cleared) or an old-gen target: no new
     * old->young edge to remember. */
    if (new_value == NULL) {
        return;
    }
    h_new = ((gc_hdr_t *)new_value) - 1;
    if (h_new->gen == GC_GEN_OLD) {
        return;
    }
    gc_remset_append(S, h_container);
}

void gc_remset_reset(mino_state_t *S)
{
    size_t i;
    for (i = 0; i < S->gc_remset_len; i++) {
        S->gc_remset[i]->dirty = 0;
    }
    S->gc_remset_len = 0;
}
