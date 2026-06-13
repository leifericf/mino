/*
 * minor.c -- young-only mark-and-sweep with age-based promotion.
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

#include "runtime/internal.h"

/* Seed the mark stack with the YOUNG outgoing pointers held by every
 * remembered-set container. gc_trace_children pushes each child into
 * the mark stack unconditionally; gc_mark_push's minor filter then
 * drops OLD children and marks YOUNG children for later tracing.
 *
 * As a side effect, observe whether each remset entry still holds any
 * OLD->YOUNG edge. The walker flag in mino_state is raised by
 * gc_mark_child_push whenever it sees a YOUNG-gen header during this
 * iteration; if no YOUNG edge was seen by the end of one entry's
 * trace, clear the entry's dirty bit so the upcoming gc_remset_reset
 * drops it. Without this filter, an OLD container whose YOUNG children
 * outlive the one-cycle promotion safety net stays out of the remset
 * the moment its dirty bit gets cleared, and its YOUNG children become
 * invisible to subsequent minors except through conservative stack
 * scan -- a silent-correctness defect that fires only when a tight
 * nursery and a raised promotion_age combine to delay the children's
 * own promotion past one cycle. */
static void gc_mark_remset(mino_state *S)
{
    size_t i;
    S->gc_remset_walker_active = 1;
    for (i = 0; i < S->gc.remset_len; i++) {
        gc_hdr_t *h = S->gc.remset[i];
        S->gc_remset_walker_young_seen = 0;
        gc_trace_children(S, h);
        if (!S->gc_remset_walker_young_seen) {
            h->dirty = 0;
        }
    }
    S->gc_remset_walker_active = 0;
}

/* Sweep the YOUNG generation by walking gc_all_young. Dead (unmarked)
 * headers go to the free list or free(); live ones clear their mark,
 * age up, and on reaching the promotion threshold migrate from the
 * young list into gc_all_old. OLD headers live on a different list
 * entirely so minor never touches them. When saved_phase is
 * MAJOR_MARK, every promoted header is also enqueued on major's mark
 * stack so major traces it before sweep. */
static void gc_minor_sweep(mino_state *S, int saved_phase)
{
    gc_hdr_t **pp          = &S->gc.all_young;
    size_t     freed_bytes = 0;
    size_t     promoted_bytes = 0;
    while (*pp != NULL) {
        gc_hdr_t *h = *pp;
        if (h->mark) {
            unsigned age_post;
            unsigned bucket;
            h->mark = 0;
            if (h->age < 0xffu) {
                h->age++;
            }
            /* Survival age bucket: log2(age+1) clamped to [0..7]. Tracks
             * the distribution of young-generation survival depth across
             * a workload's lifetime; long-lived young objects that
             * accumulate in the high buckets are promotion-age tuning
             * candidates. */
            age_post = (unsigned)h->age;
            if (age_post == 0u) {
                bucket = 0u;
            } else {
                bucket = 0u;
                while ((age_post >> bucket) > 1u && bucket < 7u) {
                    bucket++;
                }
            }
            S->gc_young_age_bucket[bucket]++;
            if (h->age >= S->gc.promotion_age) {
                /* Promote: unlink from young list, prepend to old list,
                 * flip the gen tag, and keep accounting consistent. */
                *pp             = h->next;
                h->gen          = GC_GEN_OLD;
                h->next         = S->gc.all_old;
                S->gc.all_old   = h;
                promoted_bytes += h->size;
                S->gc_bytes_promoted_minor += h->size;
                gc_evt_record(S, GC_EVT_PROMOTE, h, NULL, NULL,
                              (uintptr_t)h->size, (uint16_t)h->type_tag);
                /* One-cycle safety net: every newly-promoted header
                 * enters the remembered set so stores performed on it
                 * in the cycle immediately following promotion are
                 * observed by the next minor, even if the mutating
                 * site itself omitted the barrier. Covers the common
                 * allocate-then-populate pattern where the container
                 * is promoted mid-fill. */
                gc_remset_add(S, h);
                /* Promotion hook: if a major mark was in flight, the
                 * just-promoted header was not in major's snapshot
                 * but is live now; enqueue it onto major's mark stack
                 * so the next gc_major_step traces its children. */
                if (saved_phase == GC_PHASE_MAJOR_MARK) {
                    gc_major_enqueue_promoted(S, h);
                }
                continue; /* pp already advanced via the unlink. */
            }
            pp = &h->next;
            continue;
        }
        /* Dead YOUNG: call any per-tag finalizer, unlink, recycle.
         * The values-side finalizer dispatches across MINO_HANDLE /
         * MINO_BIGINT / MINO_RECORD / MINO_CHUNK / MINO_HOST_ARRAY /
         * MINO_FUTURE; other tags either skip the table or register
         * NULL.  gc_hdr_recycle handles the finalizer dispatch and the
         * three-arm freelist/bump-slab/free routing; see driver.c. */
        freed_bytes += h->size;
        *pp = h->next;
        gc_evt_record(S, GC_EVT_FREE_YOUNG, h, NULL, NULL,
                      (uintptr_t)h->size, (uint16_t)h->type_tag);
        gc_hdr_recycle(S, h);
    }
    /* Accounting: promoted bytes move between generations; freed bytes
     * drop out of both the young tally and the global alloc tally. */
    S->gc.bytes_young -= freed_bytes;
    S->gc.bytes_young -= promoted_bytes;
    S->gc.bytes_old   += promoted_bytes;
    S->gc.bytes_alloc -= freed_bytes;
    S->gc.bytes_live   = S->gc.bytes_young + S->gc.bytes_old;
    S->gc.total_freed += freed_bytes;
}

/* Helper for gc_verify_remset_complete below. If `p` references a
 * live YOUNG target while its containing OLD header `h` is not in
 * the remembered set, dump diagnostic context and abort -- the only
 * call path is the verify pass and `MINO_GC_VERIFY=1` opts in. */
static inline void gc_verify_check(mino_state *S, gc_hdr_t *h, void *p)
{
    gc_hdr_t *child;
    int       vt, cvt;
    if (p == NULL) return;
    child = gc_find_header_for_ptr(S, p);
    if (child == NULL || child->gen != GC_GEN_YOUNG) return;
    vt  = (h->type_tag == GC_T_VAL)
          ? (int)((mino_val *)(h + 1))->type : -1;
    cvt = (child->type_tag == GC_T_VAL)
          ? (int)((mino_val *)(child + 1))->type : -1;
    fprintf(stderr,
            "[gc-verify] OLD %p tag=%d vtype=%d -> YOUNG %p tag=%d vtype=%d\n",
            (void *)h, (int)h->type_tag, vt,
            (void *)child, (int)child->type_tag, cvt);
    fprintf(stderr,
            "  h: dirty=%u mark=%u age=%u  child: mark=%u age=%u\n",
            (unsigned)h->dirty, (unsigned)h->mark, (unsigned)h->age,
            (unsigned)child->mark, (unsigned)child->age);
    gc_classify_offender(S, h);
    gc_evt_dump_around(S, (void *)h, (void *)child, p);
    abort(); /* Class I: remset/write-barrier invariant violated */
}

/* Walk one live marked OLD header and verify every outgoing GC pointer
 * targets an OLD object OR that the header is in the remembered set.
 * Extracted from gc_verify_remset_complete to keep that function under
 * the 250-line body limit. Only called from the verify pass. */
static void gc_verify_old_hdr(mino_state *S, gc_hdr_t *h)
{
    switch (h->type_tag) {
    case GC_T_VAL: {
        mino_val *v = (mino_val *)(h + 1);
        gc_verify_check(S, h, v->meta);
        switch (mino_type_of(v)) {
        case MINO_STRING: case MINO_SYMBOL: case MINO_KEYWORD:
            gc_verify_check(S, h, v->as.s.data); break;
        case MINO_CONS:
            gc_verify_check(S, h, v->as.cons.car);
            gc_verify_check(S, h, v->as.cons.cdr); break;
        case MINO_VECTOR:
            gc_verify_check(S, h, v->as.vec.root);
            gc_verify_check(S, h, v->as.vec.tail); break;
        case MINO_MAP:
            gc_verify_check(S, h, v->as.map.root);
            gc_verify_check(S, h, v->as.map.key_order);
            gc_verify_check(S, h, v->as.map.val_order); break;
        case MINO_SET:
            gc_verify_check(S, h, v->as.set.root);
            gc_verify_check(S, h, v->as.set.key_order); break;
        case MINO_SORTED_MAP: case MINO_SORTED_SET:
            gc_verify_check(S, h, v->as.sorted.root);
            gc_verify_check(S, h, v->as.sorted.comparator); break;
        case MINO_FN: case MINO_MACRO:
            gc_verify_check(S, h, v->as.fn.params);
            gc_verify_check(S, h, v->as.fn.body);
            gc_verify_check(S, h, v->as.fn.env);
            gc_verify_check(S, h, v->as.fn.template_fn); break;
        case MINO_ATOM:
            gc_verify_check(S, h, v->as.atom.val);
            gc_verify_check(S, h, v->as.atom.watches);
            gc_verify_check(S, h, v->as.atom.validator); break;
        case MINO_VOLATILE:
            gc_verify_check(S, h, v->as.volatile_.val); break;
        case MINO_CHUNK: {
            unsigned k;
            for (k = 0; k < v->as.chunk.len; k++) {
                gc_verify_check(S, h, v->as.chunk.vals[k]);
            }
            break;
        }
        case MINO_HOST_ARRAY: {
            size_t k;
            for (k = 0; k < v->as.host_array.len; k++) {
                gc_verify_check(S, h, v->as.host_array.vals[k]);
            }
            break;
        }
        case MINO_MAP_ENTRY:
            gc_verify_check(S, h, v->as.map_entry.k);
            gc_verify_check(S, h, v->as.map_entry.v);
            break;
        case MINO_CHUNKED_CONS:
            gc_verify_check(S, h, v->as.chunked_cons.chunk);
            gc_verify_check(S, h, v->as.chunked_cons.more); break;
        case MINO_LAZY:
            if (v->as.lazy.realized == LAZY_REALIZED) {
                gc_verify_check(S, h, v->as.lazy.cached);
            } else {
                gc_verify_check(S, h, v->as.lazy.body);
                gc_verify_check(S, h, v->as.lazy.env);
            }
            break;
        case MINO_VAR:
            gc_verify_check(S, h, v->as.var.root);
            gc_verify_check(S, h, v->as.var.watches);
            gc_verify_check(S, h, v->as.var.validator);
            break;
        case MINO_TRANSIENT:
            gc_verify_check(S, h, v->as.transient.current); break;
        case MINO_TYPE:
            gc_verify_check(S, h, v->as.record_type.fields); break;
        case MINO_RECORD: {
            size_t k, kn;
            gc_verify_check(S, h, v->as.record.type);
            gc_verify_check(S, h, v->as.record.ext);
            kn = (v->as.record.type->as.record_type.fields != NULL)
                ? v->as.record.type->as.record_type.fields->as.vec.len : 0;
            for (k = 0; k < kn; k++) {
                gc_verify_check(S, h, v->as.record.vals[k]);
            }
            break;
        }
        case MINO_FUTURE: {
            /* Trace owned values held by the impl. The impl is
             * malloc-owned, not GC-owned, so we don't verify_check
             * impl itself; we walk its referent fields. */
            if (v->as.future.impl != NULL) {
                gc_verify_check(S, h, v->as.future.impl->result);
                gc_verify_check(S, h, v->as.future.impl->exception);
                gc_verify_check(S, h, v->as.future.impl->thunk);
                gc_verify_check(S, h, v->as.future.impl->body_env);
                gc_verify_check(S, h, v->as.future.impl->dyn_snapshot);
            }
            break;
        }
        case MINO_REGEX:
            gc_verify_check(S, h, v->as.regex.source);
            break;
        case MINO_TX_REF:
            gc_verify_check(S, h, v->as.tx_ref.val);
            gc_verify_check(S, h, v->as.tx_ref.watches);
            gc_verify_check(S, h, v->as.tx_ref.validator);
            break;
        case MINO_AGENT:
            gc_verify_check(S, h, v->as.agent.val);
            gc_verify_check(S, h, v->as.agent.watches);
            gc_verify_check(S, h, v->as.agent.validator);
            gc_verify_check(S, h, v->as.agent.err);
            gc_verify_check(S, h, v->as.agent.err_handler);
            break;
        default: break;
        }
        break;
    }
    case GC_T_ENV: {
        mino_env *e = (mino_env *)(h + 1);
        gc_verify_check(S, h, e->parent);
        if (e->bindings != NULL) {
            size_t k;
            gc_verify_check(S, h, e->bindings);
            for (k = 0; k < e->len; k++) {
                gc_verify_check(S, h, e->bindings[k].name);
                gc_verify_check(S, h, e->bindings[k].val);
            }
        }
        gc_verify_check(S, h, e->ht_buckets);
        break;
    }
    case GC_T_HAMT_NODE: {
        mino_hamt_node_t *n = (mino_hamt_node_t *)(h + 1);
        unsigned count, k;
        gc_verify_check(S, h, n->slots);
        count = (n->collision_count > 0)
            ? n->collision_count : popcount32(n->bitmap);
        if (n->slots != NULL) {
            for (k = 0; k < count; k++) gc_verify_check(S, h, n->slots[k]);
        }
        break;
    }
    case GC_T_HAMT_ENTRY: {
        hamt_entry_t *e = (hamt_entry_t *)(h + 1);
        gc_verify_check(S, h, e->key);
        gc_verify_check(S, h, e->val);
        break;
    }
    case GC_T_VEC_NODE: {
        mino_vec_node_t *n = (mino_vec_node_t *)(h + 1);
        unsigned k;
        for (k = 0; k < n->count; k++) gc_verify_check(S, h, n->slots[k]);
        break;
    }
    case GC_T_VALARR: case GC_T_PTRARR: {
        void **arr = (void **)(h + 1);
        size_t n = h->size / sizeof(*arr);
        size_t k;
        for (k = 0; k < n; k++) gc_verify_check(S, h, arr[k]);
        break;
    }
    case GC_T_RB_NODE: {
        mino_rb_node_t *rb = (mino_rb_node_t *)(h + 1);
        gc_verify_check(S, h, rb->key);
        gc_verify_check(S, h, rb->val);
        gc_verify_check(S, h, rb->left);
        gc_verify_check(S, h, rb->right);
        break;
    }
    default: break;
    }
}

/* Diagnostic helper (opt-in via MINO_GC_VERIFY=1): asserts that every
 * LIVE OLD header's outgoing GC pointers reference OLD targets,
 * unless the header is in the remembered set (dirty=1). A YOUNG
 * target on a live OLD container that is not in the remset means a
 * mutation bypassed the barrier; the helper aborts so the offending
 * store surfaces loudly instead of corrupting the heap cycles later.
 *
 * "Live OLD" is the subset of OLD reachable via precise roots plus
 * conservative stack scan. The filter matters: gc_all_old contains
 * dead OLD zombies between major cycles, and those can hold stale
 * cdr values that happen to alias a reused YOUNG freelist slot.
 * Checking them would raise spurious Class C aborts (see
 * gc_classify_offender) that have no bearing on runtime correctness
 * because dead OLD cannot be observed by the mutator.
 *
 * Implementation: save every mark bit, do a precise + conservative
 * mark pass (GC_PHASE_MAJOR_MARK to bypass the minor OLD filter),
 * walk only marked OLD, then restore the original mark bits so the
 * subsequent real mark pass starts from the expected zero state.
 * Paid only when the env var is set; returns immediately otherwise.
 * Per-header pointer checking is factored into gc_verify_old_hdr. */
static void gc_verify_remset_complete(mino_state *S)
{
    gc_hdr_t               *h;
    struct gc_mark_save_ctx ctx;
    size_t                  i;
    int                     saved_phase;
    size_t                  saved_floor;
    const char             *env = getenv("MINO_GC_VERIFY");
    if (env == NULL || env[0] == '\0' || env[0] == '0') return;

    /* Count + save + zero every mark bit before our classifying pass.
     * gc_count_hdrs, gc_for_each_hdr, and gc_save_mark_fn are shared with
     * gc_classify_offender in trace.c and declared in gc/internal.h. */
    ctx.cap   = gc_count_hdrs(S);
    ctx.hdrs  = (gc_hdr_t **)calloc(ctx.cap, sizeof(*ctx.hdrs));
    ctx.marks = (unsigned char *)calloc(ctx.cap, sizeof(*ctx.marks));
    ctx.idx   = 0;
    if (ctx.hdrs == NULL || ctx.marks == NULL) {
        free(ctx.hdrs); free(ctx.marks);
        fprintf(stderr, "[gc-verify] oom allocating mark-save buffer\n");
        return;
    }
    gc_for_each_hdr(S, gc_save_mark_fn, &ctx);

    /* Precise + conservative mark pass under MAJOR_MARK so OLD is not
     * filtered from the frontier. */
    saved_phase = S->gc.phase;
    saved_floor = S->gc.mark_stack_len;
    S->gc.phase = GC_PHASE_MAJOR_MARK;
    gc_mark_roots(S);
    gc_drain_mark_stack_to(S, saved_floor);
    gc_scan_stack(S);
    gc_drain_mark_stack_to(S, saved_floor);
    S->gc.phase = saved_phase;

    for (h = S->gc.all_old; h != NULL; h = h->next) {
        if (h->dirty) continue;
        if (!h->mark) continue; /* dead OLD zombie; skip (see comment above) */
        gc_verify_old_hdr(S, h);
    }

    /* Restore every saved mark so the caller's real mark pass starts
     * from the zero state it expects. */
    for (i = 0; i < ctx.idx; i++) {
        ctx.hdrs[i]->mark = ctx.marks[i];
    }
    free(ctx.hdrs);
    free(ctx.marks);
}

void gc_minor_collect(mino_state *S)
{
    jmp_buf   jb;
    long long start_ns;
    long long mark_start_ns;
    long long roots_start_ns;
    long long sweep_start_ns;
    size_t    elapsed_ns;
    int       saved_phase;
    size_t    mark_floor;
    if (mino_current_ctx(S)->gc_depth > 0) {
        return;
    }
    mino_current_ctx(S)->gc_depth++;
    /* Save the caller's phase and the current mark-stack length.
     * When a minor runs nested inside MAJOR_MARK, the saved length
     * is the floor below which major's pending entries live; minor
     * drains only above the floor so major's work is preserved. The
     * saved phase is restored on exit so the outer major cycle
     * continues uninterrupted. */
    saved_phase = S->gc.phase;
    mark_floor  = S->gc.mark_stack_len;
    S->gc.phase = GC_PHASE_MINOR;
    gc_evt_record(S, GC_EVT_MINOR_BEGIN, NULL, NULL, NULL,
                  (uintptr_t)saved_phase, 0);
    if (!S->gc.ranges_valid) {
        gc_build_range_index(S);
    } else {
        /* Incremental path: main array stayed sorted across the last
         * minor cycle; fold in the allocations the mutator buffered
         * since. Cheaper than re-sorting everything from gc_all. */
        gc_range_merge_pending(S);
    }
    gc_verify_remset_complete(S);
    start_ns = mino_monotonic_ns();
    /* setjmp spills callee-saved registers into jb so the conservative
     * stack scan below covers any pointer that was register-resident
     * at entry. */
    if (setjmp(jb) != 0) {
        abort(); /* Class I: nonzero return only under corruption */
    }
    (void)jb;
    if (!S->gc.ranges_valid) {
        gc_build_range_index(S);
    }
    /* Mark phase: precise roots + remset + conservative stack scan,
     * each followed by a drain to floor. gc_root_scan_ns is the sub-
     * timer for the precise-root enumeration; it overlaps with the
     * outer mark_ns rather than adding to it. */
    mark_start_ns  = mino_monotonic_ns();
    roots_start_ns = mark_start_ns;
    gc_mark_roots(S);
    { long long raw_ns = mino_monotonic_ns() - roots_start_ns; S->gc_root_scan_ns += (raw_ns > 0) ? (size_t)raw_ns : 0; }
    gc_drain_mark_stack_to(S, mark_floor);
    gc_mark_remset(S);
    gc_drain_mark_stack_to(S, mark_floor);
    gc_scan_stack(S);
    gc_drain_mark_stack_to(S, mark_floor);
    { long long raw_ns = mino_monotonic_ns() - mark_start_ns; S->gc_minor_mark_ns += (raw_ns > 0) ? (size_t)raw_ns : 0; }
    /* Sweep phase: range-index compaction, remset reset, and the actual
     * young-list sweep. These three are sweep-side housekeeping; lumped
     * together so the phase sum tracks gc_total_ns closely. */
    sweep_start_ns = mino_monotonic_ns();
    /* Drop entries for YOUNG headers that sweep is about to free. OLD
     * entries and marked YOUNG survivors stay; the main array remains
     * sorted, so no rebuild at the top of the next cycle. */
    gc_range_compact_after_minor_mark(S);
    /* Reset the remset before sweep so sweep can immediately re-enqueue
     * every newly-promoted header; the remset ends the cycle
     * containing exactly those promotions, giving the next cycle a
     * safety net for any alloc-then-populate pattern that omitted a
     * barrier on a container promoted mid-fill. */
    gc_remset_reset(S);
    gc_minor_sweep(S, saved_phase);
    { long long raw_ns = mino_monotonic_ns() - sweep_start_ns; S->gc_minor_sweep_ns += (raw_ns > 0) ? (size_t)raw_ns : 0; }
    S->gc.collections_minor++;
    S->gc.phase = saved_phase;
    gc_evt_record(S, GC_EVT_MINOR_END, NULL, NULL, NULL, 0, 0);
    { long long raw_ns = mino_monotonic_ns() - start_ns; elapsed_ns = (raw_ns > 0) ? (size_t)raw_ns : 0; }
    S->gc.total_ns += elapsed_ns;
    if (elapsed_ns > S->gc.max_ns) {
        S->gc.max_ns = elapsed_ns;
    }
    gc_record_pause(S, elapsed_ns);
    mino_current_ctx(S)->gc_depth--;
}
