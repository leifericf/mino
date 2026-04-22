/*
 * runtime_gc_minor.c -- young-only mark-and-sweep with age-based
 * promotion.
 *
 * The minor collector walks the YOUNG subset of the heap in one STW
 * cycle. Its three input sources are: the usual roots (registered
 * envs, interns, module/metadata/var registries, async queues, host
 * refs, dyn bindings, sort comparator, GC save stack, cached core
 * forms, trampoline sentinels, async timers), the remembered set --
 * every OLD container that observed a YOUNG store since the last
 * cycle -- and a conservative scan of the C stack between the host's
 * saved frame and the collector's own frame.
 *
 * Tracing honours gc_phase: gc_mark_push filters OLD headers out so
 * the minor mark frontier never descends into long-lived data. OLD
 * remset entries are an exception; gc_mark_remset calls
 * gc_trace_children on each one directly to pick up the YOUNG pointees
 * without marking the container itself.
 *
 * Sweep walks gc_all and affects only YOUNG headers: dead YOUNG go to
 * the free list or free(); live YOUNG have their mark bit cleared,
 * their age counter incremented, and -- once the counter reaches
 * gc_promotion_age -- are flipped to OLD with size re-accounted.
 * OLD headers are left alone.
 */

#include "mino_internal.h"

/* Seed the mark stack with the YOUNG outgoing pointers held by every
 * remembered-set container. gc_trace_children pushes each child into
 * the mark stack unconditionally; gc_mark_push's minor filter then
 * drops OLD children and marks YOUNG children for later tracing. */
static void gc_mark_remset(mino_state_t *S)
{
    size_t i;
    for (i = 0; i < S->gc_remset_len; i++) {
        gc_trace_children(S, S->gc_remset[i]);
    }
}

/* Sweep the YOUNG generation in one walk of gc_all. Dead (unmarked)
 * YOUNG headers go to the free list or free(); live YOUNG clear their
 * mark, age up, and promote to OLD when the age threshold is reached.
 * OLD headers are untouched in both reachability and accounting. */
static void gc_minor_sweep(mino_state_t *S)
{
    gc_hdr_t **pp          = &S->gc_all;
    size_t     freed_bytes = 0;
    size_t     promoted_bytes = 0;
    while (*pp != NULL) {
        gc_hdr_t *h = *pp;
        if (h->gen == GC_GEN_OLD) {
            /* Major owns the OLD generation; minor leaves mark and
             * linkage alone. */
            pp = &h->next;
            continue;
        }
        if (h->mark) {
            h->mark = 0;
            if (h->age < 0xffu) {
                h->age++;
            }
            if (h->age >= S->gc_promotion_age) {
                h->gen           = GC_GEN_OLD;
                promoted_bytes  += h->size;
                /* One-cycle safety net: every newly-promoted header
                 * enters the remembered set so stores performed on it
                 * in the cycle immediately following promotion are
                 * observed by the next minor, even if the mutating
                 * site itself omitted the barrier. Covers the common
                 * allocate-then-populate pattern where the container
                 * is promoted mid-fill. */
                gc_remset_add(S, h);
            }
            pp = &h->next;
            continue;
        }
        /* Dead YOUNG: call any finalizer, unlink, recycle. */
        if (h->type_tag == GC_T_VAL) {
            mino_val_t *v = (mino_val_t *)(h + 1);
            if (v->type == MINO_HANDLE && v->as.handle.finalizer != NULL) {
                v->as.handle.finalizer(v->as.handle.ptr,
                                       v->as.handle.tag);
            }
        }
        freed_bytes += h->size;
        *pp = h->next;
        {
            int fc = gc_freelist_class(h->size);
            if (fc >= 0) {
                h->next            = S->gc_freelists[fc];
                S->gc_freelists[fc] = h;
            } else {
                free(h);
            }
        }
    }
    /* Accounting: promoted bytes move between generations; freed bytes
     * drop out of both the young tally and the global alloc tally. */
    S->gc_bytes_young -= freed_bytes;
    S->gc_bytes_young -= promoted_bytes;
    S->gc_bytes_old   += promoted_bytes;
    S->gc_bytes_alloc -= freed_bytes;
    S->gc_bytes_live   = S->gc_bytes_young + S->gc_bytes_old;
    S->gc_total_freed += freed_bytes;
}

void gc_minor_collect(mino_state_t *S)
{
    jmp_buf   jb;
    long long start_ns;
    size_t    elapsed_ns;
    if (S->gc_depth > 0) {
        return;
    }
    S->gc_depth++;
    S->gc_phase = GC_PHASE_MINOR;
    start_ns = mino_monotonic_ns();
    /* setjmp spills callee-saved registers into jb so the conservative
     * stack scan below covers any pointer that was register-resident
     * at entry. */
    if (setjmp(jb) != 0) {
        abort(); /* Class I: nonzero return only under corruption */
    }
    (void)jb;
    if (!S->gc_ranges_valid) {
        gc_build_range_index(S);
    }
    gc_mark_roots(S);
    gc_drain_mark_stack(S);
    gc_mark_remset(S);
    gc_drain_mark_stack(S);
    gc_scan_stack(S);
    gc_drain_mark_stack(S);
    /* Reset the remset before sweep so sweep can immediately re-enqueue
     * every newly-promoted header; the remset ends the cycle
     * containing exactly those promotions, giving the next cycle a
     * safety net for any alloc-then-populate pattern that omitted a
     * barrier on a container promoted mid-fill. */
    gc_remset_reset(S);
    gc_minor_sweep(S);
    /* Dead YOUNG entries are still in the range index. Rather than
     * compact it we invalidate and rebuild at the next collection -- a
     * cheap O(n) walk that fits naturally into the quiescent state
     * between cycles. */
    S->gc_ranges_valid = 0;
    S->gc_collections_minor++;
    S->gc_phase = GC_PHASE_IDLE;
    elapsed_ns = (size_t)(mino_monotonic_ns() - start_ns);
    S->gc_total_ns += elapsed_ns;
    if (elapsed_ns > S->gc_max_ns) {
        S->gc_max_ns = elapsed_ns;
    }
    S->gc_depth--;
}
