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
 * OLD headers are untouched in both reachability and accounting.
 * When saved_phase is MAJOR_MARK, every promoted header is also
 * enqueued on major's mark stack so major traces it before sweep. */
static void gc_minor_sweep(mino_state_t *S, int saved_phase)
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
                /* Promotion hook: if a major mark was in flight, the
                 * just-promoted header was not in major's snapshot
                 * but is live now; enqueue it onto major's mark stack
                 * so the next gc_major_step traces its children. */
                if (saved_phase == GC_PHASE_MAJOR_MARK) {
                    gc_major_enqueue_promoted(S, h);
                }
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

/* Diagnostic helper (opt-in via MINO_GC_VERIFY=1): walks every OLD
 * header and asserts that its outgoing GC pointers reference OLD
 * targets, unless the header is in the remembered set (dirty=1). A
 * YOUNG target on an OLD container that is not in the remset means a
 * mutation bypassed the barrier; the helper aborts so the offending
 * store surfaces loudly instead of corrupting the heap cycles later.
 * Paid only when the env var is set; returns immediately otherwise. */
static void gc_verify_remset_complete(mino_state_t *S)
{
    gc_hdr_t   *h;
    const char *env = getenv("MINO_GC_VERIFY");
    if (env == NULL || env[0] == '\0' || env[0] == '0') return;

#define MINO_GC_VERIFY_CHECK(p) do { \
    if ((p) != NULL) { \
        gc_hdr_t *__child = gc_find_header_for_ptr(S, (p)); \
        if (__child != NULL && __child->gen == GC_GEN_YOUNG) { \
            int __vt = (h->type_tag == GC_T_VAL) \
                ? (int)((mino_val_t *)(h + 1))->type : -1; \
            fprintf(stderr, \
                "[gc-verify] OLD %p tag=%d vtype=%d -> YOUNG %p tag=%d\n", \
                (void *)h, (int)h->type_tag, __vt, \
                (void *)__child, (int)__child->type_tag); \
            abort(); \
        } \
    } \
} while (0)

    for (h = S->gc_all; h != NULL; h = h->next) {
        if (h->gen != GC_GEN_OLD || h->dirty) continue;
        switch (h->type_tag) {
        case GC_T_VAL: {
            mino_val_t *v = (mino_val_t *)(h + 1);
            MINO_GC_VERIFY_CHECK(v->meta);
            switch (v->type) {
            case MINO_STRING: case MINO_SYMBOL: case MINO_KEYWORD:
                MINO_GC_VERIFY_CHECK(v->as.s.data); break;
            case MINO_CONS:
                MINO_GC_VERIFY_CHECK(v->as.cons.car);
                MINO_GC_VERIFY_CHECK(v->as.cons.cdr); break;
            case MINO_VECTOR:
                MINO_GC_VERIFY_CHECK(v->as.vec.root);
                MINO_GC_VERIFY_CHECK(v->as.vec.tail); break;
            case MINO_MAP:
                MINO_GC_VERIFY_CHECK(v->as.map.root);
                MINO_GC_VERIFY_CHECK(v->as.map.key_order); break;
            case MINO_SET:
                MINO_GC_VERIFY_CHECK(v->as.set.root);
                MINO_GC_VERIFY_CHECK(v->as.set.key_order); break;
            case MINO_SORTED_MAP: case MINO_SORTED_SET:
                MINO_GC_VERIFY_CHECK(v->as.sorted.root);
                MINO_GC_VERIFY_CHECK(v->as.sorted.comparator); break;
            case MINO_FN: case MINO_MACRO:
                MINO_GC_VERIFY_CHECK(v->as.fn.params);
                MINO_GC_VERIFY_CHECK(v->as.fn.body);
                MINO_GC_VERIFY_CHECK(v->as.fn.env); break;
            case MINO_ATOM:
                MINO_GC_VERIFY_CHECK(v->as.atom.val);
                MINO_GC_VERIFY_CHECK(v->as.atom.watches);
                MINO_GC_VERIFY_CHECK(v->as.atom.validator); break;
            case MINO_LAZY:
                if (v->as.lazy.realized) MINO_GC_VERIFY_CHECK(v->as.lazy.cached);
                else {
                    MINO_GC_VERIFY_CHECK(v->as.lazy.body);
                    MINO_GC_VERIFY_CHECK(v->as.lazy.env);
                }
                break;
            case MINO_VAR:
                MINO_GC_VERIFY_CHECK(v->as.var.root); break;
            default: break;
            }
            break;
        }
        case GC_T_ENV: {
            mino_env_t *e = (mino_env_t *)(h + 1);
            MINO_GC_VERIFY_CHECK(e->parent);
            if (e->bindings != NULL) {
                size_t k;
                MINO_GC_VERIFY_CHECK(e->bindings);
                for (k = 0; k < e->len; k++) {
                    MINO_GC_VERIFY_CHECK(e->bindings[k].name);
                    MINO_GC_VERIFY_CHECK(e->bindings[k].val);
                }
            }
            MINO_GC_VERIFY_CHECK(e->ht_buckets);
            break;
        }
        case GC_T_HAMT_NODE: {
            mino_hamt_node_t *n = (mino_hamt_node_t *)(h + 1);
            unsigned count, k;
            MINO_GC_VERIFY_CHECK(n->slots);
            count = (n->collision_count > 0)
                ? n->collision_count : popcount32(n->bitmap);
            if (n->slots != NULL) {
                for (k = 0; k < count; k++) MINO_GC_VERIFY_CHECK(n->slots[k]);
            }
            break;
        }
        case GC_T_HAMT_ENTRY: {
            hamt_entry_t *e = (hamt_entry_t *)(h + 1);
            MINO_GC_VERIFY_CHECK(e->key);
            MINO_GC_VERIFY_CHECK(e->val);
            break;
        }
        case GC_T_VEC_NODE: {
            mino_vec_node_t *n = (mino_vec_node_t *)(h + 1);
            unsigned k;
            for (k = 0; k < n->count; k++) MINO_GC_VERIFY_CHECK(n->slots[k]);
            break;
        }
        case GC_T_VALARR: case GC_T_PTRARR: {
            void **arr = (void **)(h + 1);
            size_t n = h->size / sizeof(*arr);
            size_t k;
            for (k = 0; k < n; k++) MINO_GC_VERIFY_CHECK(arr[k]);
            break;
        }
        case GC_T_RB_NODE: {
            mino_rb_node_t *rb = (mino_rb_node_t *)(h + 1);
            MINO_GC_VERIFY_CHECK(rb->key);
            MINO_GC_VERIFY_CHECK(rb->val);
            MINO_GC_VERIFY_CHECK(rb->left);
            MINO_GC_VERIFY_CHECK(rb->right);
            break;
        }
        default: break;
        }
    }
#undef MINO_GC_VERIFY_CHECK
}

void gc_minor_collect(mino_state_t *S)
{
    jmp_buf   jb;
    long long start_ns;
    size_t    elapsed_ns;
    int       saved_phase;
    size_t    mark_floor;
    if (S->gc_depth > 0) {
        return;
    }
    S->gc_depth++;
    /* Save the caller's phase and the current mark-stack length.
     * When a minor runs nested inside MAJOR_MARK, the saved length
     * is the floor below which major's pending entries live; minor
     * drains only above the floor so major's work is preserved. The
     * saved phase is restored on exit so the outer major cycle
     * continues uninterrupted. */
    saved_phase = S->gc_phase;
    mark_floor  = S->gc_mark_stack_len;
    S->gc_phase = GC_PHASE_MINOR;
    if (!S->gc_ranges_valid) {
        gc_build_range_index(S);
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
    /* Reset the remset before sweep so sweep can immediately re-enqueue
     * every newly-promoted header; the remset ends the cycle
     * containing exactly those promotions, giving the next cycle a
     * safety net for any alloc-then-populate pattern that omitted a
     * barrier on a container promoted mid-fill. */
    gc_remset_reset(S);
    gc_minor_sweep(S, saved_phase);
    /* Dead YOUNG entries are still in the range index. Rather than
     * compact it we invalidate and rebuild at the next collection -- a
     * cheap O(n) walk that fits naturally into the quiescent state
     * between cycles. */
    S->gc_ranges_valid = 0;
    S->gc_collections_minor++;
    S->gc_phase = saved_phase;
    elapsed_ns = (size_t)(mino_monotonic_ns() - start_ns);
    S->gc_total_ns += elapsed_ns;
    if (elapsed_ns > S->gc_max_ns) {
        S->gc_max_ns = elapsed_ns;
    }
    S->gc_depth--;
}
