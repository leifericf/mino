/*
 * major.c -- major collector state machine.
 *
 * Major collection is a four-phase pipeline:
 *
 *   gc_major_begin        IDLE       -> MAJOR_MARK   (seed roots)
 *   gc_major_step         MAJOR_MARK -> MAJOR_MARK   (bounded drain)
 *   gc_major_remark       MAJOR_MARK -> MAJOR_MARK   (stack rescan, drain)
 *   gc_major_sweep_phase  MAJOR_MARK -> IDLE         (sweep OLD dead)
 *
 * gc_major_collect in driver.c chains the four back-to-back for a
 * fully STW cycle. The incremental allocator pacing calls
 * gc_major_step in slices interleaved with mutator progress, then
 * fires gc_major_remark + gc_major_sweep_phase when the mark stack
 * drains.
 *
 * Each function owns its own depth guard and is a no-op if the phase
 * it expects is not the current phase, so the driver can call them
 * defensively without reasoning about exact call ordering.
 */

#include "runtime/internal.h"

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
    gc_evt_record(S, GC_EVT_MAJOR_BEGIN, NULL, NULL, NULL, 0, 0);
    if (!S->gc_ranges_valid) {
        gc_build_range_index(S);
    }
    gc_mark_roots(S);
    S->gc_major_step_alloc = 0;
    S->gc_depth--;
}

/* Pop up to budget_words headers from the mark stack and trace each
 * one. gc_trace_children uses direct payload-pointer arithmetic and
 * does not need the range index, so step skips a rebuild here even
 * though the mutator may have invalidated the index between slices.
 * Callers that want a full drain pass (size_t)-1. */
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
/* Pre-sweep diagnostic (opt-in via MINO_GC_VERIFY=1): every OLD intern
 * entry must have its mark bit set, otherwise sweep would free a
 * root-reachable header. Surfaces root-enumeration bugs at the exact
 * point they matter. YOUNG entries are not swept by major, so they
 * are exempt from this check. */
static void gc_verify_roots_marked(mino_state_t *S)
{
    const char *env;
    size_t      i;
    env = getenv("MINO_GC_VERIFY");
    if (env == NULL || env[0] == '\0' || env[0] == '0') {
        return;
    }
    for (i = 0; i < S->sym_intern.len; i++) {
        mino_val_t *e = S->sym_intern.entries[i];
        gc_hdr_t   *h;
        if (e == NULL) continue;
        h = ((gc_hdr_t *)e) - 1;
        if (h->gen == GC_GEN_OLD && !h->mark) {
            fprintf(stderr, "[gc-verify] sym_intern[%zu] OLD unmarked at sweep "
                "(e=%p h=%p)\n", i, (void *)e, (void *)h);
            abort();
        }
    }
    for (i = 0; i < S->kw_intern.len; i++) {
        mino_val_t *e = S->kw_intern.entries[i];
        gc_hdr_t   *h;
        if (e == NULL) continue;
        h = ((gc_hdr_t *)e) - 1;
        if (h->gen == GC_GEN_OLD && !h->mark) {
            fprintf(stderr, "[gc-verify] kw_intern[%zu] OLD unmarked at sweep "
                "(e=%p h=%p)\n", i, (void *)e, (void *)h);
            abort();
        }
    }
}

void gc_major_sweep_phase(mino_state_t *S)
{
    if (S->gc_depth > 0 || S->gc_phase != GC_PHASE_MAJOR_MARK) {
        return;
    }
    S->gc_depth++;
    S->gc_phase = GC_PHASE_MAJOR_SWEEP;
    gc_evt_record(S, GC_EVT_MAJOR_SWEEP, NULL, NULL, NULL, 0, 0);
    gc_verify_roots_marked(S);
    /* Purge remset entries whose container is about to be freed, but
     * keep the rest with their dirty bits intact. OLD->YOUNG edges
     * installed by the mutator during MAJOR_MARK must survive so the
     * next minor can trace the YOUNG targets. */
    gc_remset_purge_dead(S);
    gc_sweep(S);
    /* Invalidate the range index. gc_range_compact would filter by
     * mark bit, but gc_sweep leaves YOUNG alive regardless of mark,
     * so compact would wrongly drop unmarked YOUNG survivors from the
     * index and the next collector could not resolve their headers.
     * The next collector touchpoint rebuilds from gc_all. */
    S->gc_ranges_valid = 0;
    S->gc_collections_major++;
    S->gc_phase = GC_PHASE_IDLE;
    S->gc_depth--;
}

/* Major sweep. Called from gc_major_sweep_phase. Frees dead OLD
 * headers by walking gc_all_old; leaves the young list alone -- minor
 * owns YOUNG lifecycle, and new YOUNG allocations that land after
 * gc_major_begin seeded the mark stack will not be marked in this
 * cycle but still need to survive until the next minor evaluates them
 * against its own roots. A separate pass clears any mark bit the
 * major frontier set on YOUNG via cross-gen tracing so the next minor
 * starts from a clean slate. */
void gc_sweep(mino_state_t *S)
{
    gc_hdr_t **pp         = &S->gc_all_old;
    size_t     live_young = 0;
    size_t     live_old   = 0;
    size_t     freed_old  = 0;
    /* Clear mark bits on YOUNG survivors and tally live young bytes. */
    {
        gc_hdr_t *yh;
        for (yh = S->gc_all_young; yh != NULL; yh = yh->next) {
            yh->mark = 0;
            live_young += yh->size;
        }
    }
    while (*pp != NULL) {
        gc_hdr_t *h = *pp;
        if (h->mark) {
            h->mark = 0;
            live_old += h->size;
            pp = &h->next;
            continue;
        }
        /* Dead OLD: call finalizer, unlink, recycle. */
        if (h->type_tag == GC_T_VAL) {
            mino_val_t *v = (mino_val_t *)(h + 1);
            if (v->type == MINO_HANDLE && v->as.handle.finalizer != NULL) {
                v->as.handle.finalizer(v->as.handle.ptr,
                                       v->as.handle.tag);
            } else if (v->type == MINO_BIGINT) {
                mino_bigint_free(v);
            } else if (v->type == MINO_RECORD) {
                free(v->as.record.vals);
                v->as.record.vals = NULL;
            }
        }
        freed_old += h->size;
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
    S->gc_total_freed   += freed_old;
    S->gc_bytes_young    = live_young;
    S->gc_bytes_old      = live_old;
    S->gc_bytes_live     = live_young + live_old;
    S->gc_bytes_alloc    = S->gc_bytes_live;
    S->gc_old_baseline   = live_old;
    /* Next major triggers after another threshold's worth of growth
     * above the live set so collection cost stays amortised. */
    if (S->gc_bytes_live * 2 > S->gc_threshold) {
        S->gc_threshold = S->gc_bytes_live * 2;
    }
}
