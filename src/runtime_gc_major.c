/*
 * runtime_gc_major.c -- major collector state machine.
 *
 * Major collection is a four-phase pipeline:
 *
 *   gc_major_begin        IDLE       -> MAJOR_MARK   (seed roots)
 *   gc_major_step         MAJOR_MARK -> MAJOR_MARK   (bounded drain)
 *   gc_major_remark       MAJOR_MARK -> MAJOR_MARK   (stack rescan, drain)
 *   gc_major_sweep_phase  MAJOR_MARK -> IDLE         (sweep OLD dead)
 *
 * gc_major_collect in runtime_gc.c chains the four back-to-back for a
 * fully STW cycle. The incremental allocator pacing (B.3) calls
 * gc_major_step in slices interleaved with mutator progress, then fires
 * gc_major_remark + gc_major_sweep_phase when the mark stack drains.
 *
 * Each function owns its own depth guard and is a no-op if the phase
 * it expects is not the current phase, so the driver can call them
 * defensively without reasoning about exact call ordering.
 */

#include "mino_internal.h"

/* Seed the mark stack with every root. Does NOT perform the
 * conservative stack scan -- that is deferred to gc_major_remark so
 * the scan sees whatever stack state is live at the END of mark, not
 * whatever was live at the start. */
void gc_major_begin(mino_state_t *S)
{
    if (S->gc_depth > 0 || S->gc_phase != GC_PHASE_IDLE) {
        return;
    }
    S->gc_depth++;
    S->gc_phase = GC_PHASE_MAJOR_MARK;
    if (!S->gc_ranges_valid) {
        gc_build_range_index(S);
    }
    gc_mark_roots(S);
    S->gc_major_step_alloc = 0;
    S->gc_depth--;
}

/* Pop up to budget_words headers from the mark stack and trace each
 * one. Callers that want a full drain pass (size_t)-1. */
void gc_major_step(mino_state_t *S, size_t budget_words)
{
    size_t popped = 0;
    if (S->gc_depth > 0 || S->gc_phase != GC_PHASE_MAJOR_MARK) {
        return;
    }
    S->gc_depth++;
    while (S->gc_mark_stack_len > 0 && popped < budget_words) {
        gc_hdr_t *h = S->gc_mark_stack[--S->gc_mark_stack_len];
        gc_trace_children(S, h);
        popped++;
    }
    S->gc_depth--;
}

/* Finalise major mark: rescan the C stack (picks up anything the
 * mutator parked between slices) and drain until the mark stack is
 * empty. The setjmp spills callee-saved registers into jb, which
 * lives in this frame and is visible to gc_scan_stack. Must run with
 * the mutator paused -- no partial progress is acceptable between
 * remark and sweep. */
void gc_major_remark(mino_state_t *S)
{
    jmp_buf jb;
    if (S->gc_depth > 0 || S->gc_phase != GC_PHASE_MAJOR_MARK) {
        return;
    }
    S->gc_depth++;
    if (setjmp(jb) != 0) {
        abort(); /* Class I: nonzero return only under corruption */
    }
    (void)jb;
    if (!S->gc_ranges_valid) {
        gc_build_range_index(S);
    }
    gc_scan_stack(S);
    gc_drain_mark_stack(S);
    S->gc_depth--;
}

/* Sweep dead OLD objects and return to IDLE. Invariant: mark stack
 * empty (remark drained it). The reset of the remembered set is done
 * BEFORE gc_sweep so sweep cannot free a remembered OLD header and
 * then have the reset walk write dirty=0 through a dangling pointer.
 * Major has traced everything reachable, so the remset is redundant
 * this cycle; the barrier will repopulate it as mutator stores
 * reintroduce old->young edges. */
void gc_major_sweep_phase(mino_state_t *S)
{
    if (S->gc_depth > 0 || S->gc_phase != GC_PHASE_MAJOR_MARK) {
        return;
    }
    S->gc_depth++;
    S->gc_phase = GC_PHASE_MAJOR_SWEEP;
    gc_range_compact(S);
    gc_remset_reset(S);
    gc_sweep(S);
    S->gc_collections_major++;
    S->gc_phase = GC_PHASE_IDLE;
    S->gc_depth--;
}

/* Full-heap sweep. Called from gc_major_sweep_phase; frees every
 * allocation whose mark bit is clear, resets the mark bit on
 * survivors, grows the next cycle's threshold so amortised collection
 * cost stays bounded. */
void gc_sweep(mino_state_t *S)
{
    gc_hdr_t **pp         = &S->gc_all;
    size_t     live_young = 0;
    size_t     live_old   = 0;
    size_t     live;
    while (*pp != NULL) {
        gc_hdr_t *h = *pp;
        if (h->mark) {
            h->mark = 0;
            if (h->gen == GC_GEN_OLD) {
                live_old += h->size;
            } else {
                live_young += h->size;
            }
            pp = &h->next;
        } else {
            /* Call finalizer for handles being collected. */
            if (h->type_tag == GC_T_VAL) {
                mino_val_t *v = (mino_val_t *)(h + 1);
                if (v->type == MINO_HANDLE && v->as.handle.finalizer != NULL) {
                    v->as.handle.finalizer(v->as.handle.ptr,
                                           v->as.handle.tag);
                }
            }
            *pp = h->next;
            {
                int fc = gc_freelist_class(h->size);
                if (fc >= 0) {
                    h->next = S->gc_freelists[fc];
                    S->gc_freelists[fc] = h;
                } else {
                    free(h);
                }
            }
        }
    }
    live = live_young + live_old;
    S->gc_total_freed   += S->gc_bytes_alloc - live;
    S->gc_bytes_young    = live_young;
    S->gc_bytes_old      = live_old;
    S->gc_bytes_live     = live;
    S->gc_bytes_alloc    = live;
    S->gc_old_baseline   = live_old;
    /* Next cycle triggers after another threshold's worth of growth above
     * the live set; threshold grows to keep collection amortized. */
    if (live * 2 > S->gc_threshold) {
        S->gc_threshold = live * 2;
    }
}
