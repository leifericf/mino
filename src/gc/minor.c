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
 * drops OLD children and marks YOUNG children for later tracing. */
static void gc_mark_remset(mino_state_t *S)
{
    size_t i;
    for (i = 0; i < S->gc_remset_len; i++) {
        gc_trace_children(S, S->gc_remset[i]);
    }
}

/* Sweep the YOUNG generation by walking gc_all_young. Dead (unmarked)
 * headers go to the free list or free(); live ones clear their mark,
 * age up, and on reaching the promotion threshold migrate from the
 * young list into gc_all_old. OLD headers live on a different list
 * entirely so minor never touches them. When saved_phase is
 * MAJOR_MARK, every promoted header is also enqueued on major's mark
 * stack so major traces it before sweep. */
static void gc_minor_sweep(mino_state_t *S, int saved_phase)
{
    gc_hdr_t **pp          = &S->gc_all_young;
    size_t     freed_bytes = 0;
    size_t     promoted_bytes = 0;
    while (*pp != NULL) {
        gc_hdr_t *h = *pp;
        if (h->mark) {
            h->mark = 0;
            if (h->age < 0xffu) {
                h->age++;
            }
            if (h->age >= S->gc_promotion_age) {
                /* Promote: unlink from young list, prepend to old list,
                 * flip the gen tag, and keep accounting consistent. */
                *pp             = h->next;
                h->gen          = GC_GEN_OLD;
                h->next         = S->gc_all_old;
                S->gc_all_old   = h;
                promoted_bytes += h->size;
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
        /* Dead YOUNG: call any finalizer, unlink, recycle. */
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
            } else if (v->type == MINO_FUTURE) {
                extern void mino_future_gc_sweep(mino_val_t *fut);
                mino_future_gc_sweep(v);
            }
        }
        freed_bytes += h->size;
        *pp = h->next;
        gc_evt_record(S, GC_EVT_FREE_YOUNG, h, NULL, NULL,
                      (uintptr_t)h->size, (uint16_t)h->type_tag);
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

/* Helper for gc_verify_remset_complete below. If `p` references a
 * live YOUNG target while its containing OLD header `h` is not in
 * the remembered set, dump diagnostic context and abort -- the only
 * call path is the verify pass and `MINO_GC_VERIFY=1` opts in. */
static inline void gc_verify_check(mino_state_t *S, gc_hdr_t *h, void *p)
{
    gc_hdr_t *child;
    int       vt, cvt;
    if (p == NULL) return;
    child = gc_find_header_for_ptr(S, p);
    if (child == NULL || child->gen != GC_GEN_YOUNG) return;
    vt  = (h->type_tag == GC_T_VAL)
          ? (int)((mino_val_t *)(h + 1))->type : -1;
    cvt = (child->type_tag == GC_T_VAL)
          ? (int)((mino_val_t *)(child + 1))->type : -1;
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
    abort();
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
 * Paid only when the env var is set; returns immediately otherwise. */
static void gc_verify_remset_complete(mino_state_t *S)
{
    gc_hdr_t      *h;
    gc_hdr_t     **saved_hdrs;
    unsigned char *saved_marks;
    size_t         n_hdrs, idx;
    int            saved_phase;
    size_t         saved_floor;
    const char    *env = getenv("MINO_GC_VERIFY");
    if (env == NULL || env[0] == '\0' || env[0] == '0') return;

    /* Count + save + zero every mark bit before our classifying pass. */
    n_hdrs = 0;
    for (h = S->gc_all_young; h != NULL; h = h->next) n_hdrs++;
    for (h = S->gc_all_old;   h != NULL; h = h->next) n_hdrs++;
    saved_hdrs  = (gc_hdr_t **)calloc(n_hdrs, sizeof(*saved_hdrs));
    saved_marks = (unsigned char *)calloc(n_hdrs, sizeof(*saved_marks));
    if (saved_hdrs == NULL || saved_marks == NULL) {
        free(saved_hdrs); free(saved_marks);
        fprintf(stderr, "[gc-verify] oom allocating mark-save buffer\n");
        return;
    }
    idx = 0;
    for (h = S->gc_all_young; h != NULL; h = h->next) {
        saved_hdrs[idx]  = h;
        saved_marks[idx] = h->mark;
        h->mark          = 0;
        idx++;
    }
    for (h = S->gc_all_old; h != NULL; h = h->next) {
        saved_hdrs[idx]  = h;
        saved_marks[idx] = h->mark;
        h->mark          = 0;
        idx++;
    }

    /* Precise + conservative mark pass under MAJOR_MARK so OLD is not
     * filtered from the frontier. */
    saved_phase = S->gc_phase;
    saved_floor = S->gc_mark_stack_len;
    S->gc_phase = GC_PHASE_MAJOR_MARK;
    gc_mark_roots(S);
    gc_drain_mark_stack_to(S, saved_floor);
    gc_scan_stack(S);
    gc_drain_mark_stack_to(S, saved_floor);
    S->gc_phase = saved_phase;

    for (h = S->gc_all_old; h != NULL; h = h->next) {
        if (h->dirty) continue;
        if (!h->mark) continue; /* dead OLD zombie; skip (see comment above) */
        switch (h->type_tag) {
        case GC_T_VAL: {
            mino_val_t *v = (mino_val_t *)(h + 1);
            gc_verify_check(S, h, v->meta);
            switch (v->type) {
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
                gc_verify_check(S, h, v->as.map.key_order); break;
            case MINO_SET:
                gc_verify_check(S, h, v->as.set.root);
                gc_verify_check(S, h, v->as.set.key_order); break;
            case MINO_SORTED_MAP: case MINO_SORTED_SET:
                gc_verify_check(S, h, v->as.sorted.root);
                gc_verify_check(S, h, v->as.sorted.comparator); break;
            case MINO_FN: case MINO_MACRO:
                gc_verify_check(S, h, v->as.fn.params);
                gc_verify_check(S, h, v->as.fn.body);
                gc_verify_check(S, h, v->as.fn.env); break;
            case MINO_ATOM:
                gc_verify_check(S, h, v->as.atom.val);
                gc_verify_check(S, h, v->as.atom.watches);
                gc_verify_check(S, h, v->as.atom.validator); break;
            case MINO_LAZY:
                if (v->as.lazy.realized) gc_verify_check(S, h, v->as.lazy.cached);
                else {
                    gc_verify_check(S, h, v->as.lazy.body);
                    gc_verify_check(S, h, v->as.lazy.env);
                }
                break;
            case MINO_VAR:
                gc_verify_check(S, h, v->as.var.root); break;
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
            default: break;
            }
            break;
        }
        case GC_T_ENV: {
            mino_env_t *e = (mino_env_t *)(h + 1);
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

    /* Restore every saved mark so the caller's real mark pass starts
     * from the zero state it expects. */
    {
        size_t k;
        for (k = 0; k < idx; k++) {
            saved_hdrs[k]->mark = saved_marks[k];
        }
    }
    free(saved_hdrs);
    free(saved_marks);
}

void gc_minor_collect(mino_state_t *S)
{
    jmp_buf   jb;
    long long start_ns;
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
    saved_phase = S->gc_phase;
    mark_floor  = S->gc_mark_stack_len;
    S->gc_phase = GC_PHASE_MINOR;
    gc_evt_record(S, GC_EVT_MINOR_BEGIN, NULL, NULL, NULL,
                  (uintptr_t)saved_phase, 0);
    if (!S->gc_ranges_valid) {
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
    if (!S->gc_ranges_valid) {
        gc_build_range_index(S);
    }
    gc_mark_roots(S);
    gc_drain_mark_stack_to(S, mark_floor);
    gc_mark_remset(S);
    gc_drain_mark_stack_to(S, mark_floor);
    gc_scan_stack(S);
    gc_drain_mark_stack_to(S, mark_floor);
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
    S->gc_collections_minor++;
    S->gc_phase = saved_phase;
    gc_evt_record(S, GC_EVT_MINOR_END, NULL, NULL, NULL, 0, 0);
    elapsed_ns = (size_t)(mino_monotonic_ns() - start_ns);
    S->gc_total_ns += elapsed_ns;
    if (elapsed_ns > S->gc_max_ns) {
        S->gc_max_ns = elapsed_ns;
    }
    mino_current_ctx(S)->gc_depth--;
}
