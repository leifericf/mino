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
    long long t0, troots;
    if (mino_current_ctx(S)->gc_depth > 0 || S->gc_phase != GC_PHASE_IDLE) {
        return;
    }
    mino_current_ctx(S)->gc_depth++;
    S->gc_phase = GC_PHASE_MAJOR_MARK;
    gc_evt_record(S, GC_EVT_MAJOR_BEGIN, NULL, NULL, NULL, 0, 0);
    if (!S->gc_ranges_valid) {
        gc_build_range_index(S);
    }
    t0     = mino_monotonic_ns();
    troots = t0;
    gc_mark_roots(S);
    S->gc_root_scan_ns += (size_t)(mino_monotonic_ns() - troots);
    S->gc_major_mark_ns += (size_t)(mino_monotonic_ns() - t0);
    S->gc_major_step_alloc = 0;
    mino_current_ctx(S)->gc_depth--;
}

/* Pop up to budget_words headers from the mark stack and trace each
 * one. gc_trace_children uses direct payload-pointer arithmetic and
 * does not need the range index, so step skips a rebuild here even
 * though the mutator may have invalidated the index between slices.
 * Callers that want a full drain pass (size_t)-1. */
void gc_major_step(mino_state_t *S, size_t budget_words)
{
    long long t0;
    size_t    popped = 0;
    if (mino_current_ctx(S)->gc_depth > 0 || S->gc_phase != GC_PHASE_MAJOR_MARK) {
        return;
    }
    mino_current_ctx(S)->gc_depth++;
    t0 = mino_monotonic_ns();
    while (S->gc_mark_stack_len > 0 && popped < budget_words) {
        gc_hdr_t *h = S->gc_mark_stack[--S->gc_mark_stack_len];
        gc_trace_children(S, h);
        popped++;
    }
    S->gc_major_mark_ns += (size_t)(mino_monotonic_ns() - t0);
    mino_current_ctx(S)->gc_depth--;
}

/* Finalise major mark: rescan the C stack (picks up anything the
 * mutator parked between slices) and drain until the mark stack is
 * empty. The setjmp spills callee-saved registers into jb, which
 * lives in this frame and is visible to gc_scan_stack. Must run with
 * the mutator paused -- no partial progress is acceptable between
 * remark and sweep. */
void gc_major_remark(mino_state_t *S)
{
    jmp_buf   jb;
    long long t0;
    if (mino_current_ctx(S)->gc_depth > 0 || S->gc_phase != GC_PHASE_MAJOR_MARK) {
        return;
    }
    mino_current_ctx(S)->gc_depth++;
    if (setjmp(jb) != 0) {
        abort(); /* Class I: nonzero return only under corruption */
    }
    (void)jb;
    if (!S->gc_ranges_valid) {
        gc_build_range_index(S);
    }
    t0 = mino_monotonic_ns();
    /* Re-walk every precise root before the final stack scan. The
     * Dijkstra-only barrier captures inserted edges but does not push
     * the values that fall off slots; anything still reachable through
     * a root must be marked from the root walk at end-of-mark, not
     * from the begin-of-mark snapshot. State-owned containers whose
     * slot writes bypass the barrier (bc_regs and per-worker
     * bc_regs_storage) get their YOUNG / OLD frontier covered here. */
    gc_mark_roots(S);
    gc_scan_stack(S);
    gc_drain_mark_stack(S);
    S->gc_major_mark_ns += (size_t)(mino_monotonic_ns() - t0);
    mino_current_ctx(S)->gc_depth--;
}

/* Sweep dead OLD objects and return to IDLE. Invariant: mark stack
 * empty (remark drained it). The reset of the remembered set is done
 * BEFORE gc_sweep so sweep cannot free a remembered OLD header and
 * then have the reset walk write dirty=0 through a dangling pointer.
 * Major has traced everything reachable, so the remset is redundant
 * this cycle; the barrier will repopulate it as mutator stores
 * reintroduce old->young edges. */
/* Pre-sweep tombstone pass for the weak intern tables. Walks every
 * sym / keyword entry and tombstones the slot whose underlying header
 * is unmarked at end-of-mark. Without this, gc_sweep would free the
 * header and leave entries[i] / ht_buckets[bucket] pointing at recycled
 * memory; subsequent intern_lookup_or_create probes would dereference
 * the dangling pointer. YOUNG entries are not swept by major (minor
 * owns YOUNG lifecycle), so they are skipped. */
static void gc_intern_sweep_tombstones(mino_state_t *S)
{
    intern_table_t *tables[2];
    int             ti;
    tables[0] = &S->sym_intern;
    tables[1] = &S->kw_intern;
    for (ti = 0; ti < 2; ti++) {
        intern_table_t *tbl = tables[ti];
        size_t          i;
        size_t          mask;
        for (i = 0; i < tbl->len; i++) {
            mino_val_t *e = tbl->entries[i];
            gc_hdr_t   *h;
            uint32_t    hash;
            size_t      idx;
            if (e == NULL) continue;
            h = ((gc_hdr_t *)e) - 1;
            if (h->gen != GC_GEN_OLD) continue;
            if (h->mark) continue;
            /* Tombstone: clear entries[] slot and mark every probe-
             * chain bucket pointing at this slot as TOMBSTONE so a
             * future intern with the same content lands here. */
            hash = e->as.s.hash;
            tbl->entries[i] = NULL;
            if (tbl->ht_buckets == NULL) continue;
            mask = tbl->ht_cap - 1;
            idx  = hash & mask;
            while (tbl->ht_buckets[idx] != INTERN_HT_EMPTY) {
                if (tbl->ht_buckets[idx] == i) {
                    tbl->ht_buckets[idx] = INTERN_HT_TOMBSTONE;
                    break;
                }
                idx = (idx + 1) & mask;
            }
        }
    }
}

void gc_major_sweep_phase(mino_state_t *S)
{
    long long t0;
    if (mino_current_ctx(S)->gc_depth > 0 || S->gc_phase != GC_PHASE_MAJOR_MARK) {
        return;
    }
    mino_current_ctx(S)->gc_depth++;
    S->gc_phase = GC_PHASE_MAJOR_SWEEP;
    gc_evt_record(S, GC_EVT_MAJOR_SWEEP, NULL, NULL, NULL, 0, 0);
    /* Sweep phase: tombstone weak intern entries whose header was not
     * reached through any root, purge the remset of dead containers,
     * then run the OLD sweep itself. All three are sweep-side
     * housekeeping; lumped together so the phase sum tracks
     * gc_total_ns closely. */
    t0 = mino_monotonic_ns();
    gc_intern_sweep_tombstones(S);
    gc_remset_purge_dead(S);
    gc_sweep(S);
    S->gc_major_sweep_ns += (size_t)(mino_monotonic_ns() - t0);
    /* Invalidate the range index. gc_range_compact would filter by
     * mark bit, but gc_sweep leaves YOUNG alive regardless of mark,
     * so compact would wrongly drop unmarked YOUNG survivors from the
     * index and the next collector could not resolve their headers.
     * The next collector touchpoint rebuilds from gc_all. */
    S->gc_ranges_valid = 0;
    S->gc_collections_major++;
    S->gc_phase = GC_PHASE_IDLE;
    mino_current_ctx(S)->gc_depth--;
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
        /* Dead OLD: call any per-tag finalizer, unlink, recycle.
         * See gc/minor.c for the dispatch shape; quiesce should have
         * joined any outstanding MINO_FUTURE worker before we get
         * here, otherwise the impl is leaked rather than freed under
         * it. */
        {
            gc_finalizer_fn fin = S->gc_finalizers[h->type_tag];
            if (fin != NULL) fin(S, h);
        }
        freed_old += h->size;
        *pp = h->next;
        {
            int fc = gc_freelist_class(h->size);
            if (fc >= 0) {
                /* See minor.c -- bump-origin and calloc-origin both
                 * round-trip through the freelist; gc_alloc_raw
                 * preserves h->bump across the pull-side memset. */
                h->next = S->gc_freelists[fc];
                S->gc_freelists[fc] = h;
            } else if (h->bump) {
                /* No size class: bump-origin leaks in its slab. */
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
