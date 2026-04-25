/*
 * runtime_gc.c -- allocation driver, shared trace machinery, collection
 * entry point.
 *
 * The range index, root enumeration and conservative stack scan live in
 * runtime_gc_roots.c; the full-heap sweep lives in runtime_gc_major.c.
 * The driver below stitches them together for one STW mark-and-sweep
 * cycle. Trace primitives (gc_mark_push, gc_drain_mark_stack,
 * gc_process_header) stay here because they will be reused by the
 * generational minor collector introduced in a later step.
 */

#include "runtime/internal.h"

/* Record a stack address from a host-called entry point so the collector's
 * conservative scan covers the entire host-to-mino call chain. We keep the
 * maximum address (shallowest frame on a downward-growing stack). */
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdangling-pointer"
#endif
void gc_note_host_frame(mino_state_t *S, void *addr)
{
    if (S->gc_stack_bottom == NULL
        || (char *)addr > (char *)S->gc_stack_bottom) {
        S->gc_stack_bottom = addr;
    }
}
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic pop
#endif

/* Free list size classes: indices into gc_freelists[]. Returns -1 for
 * variable-size allocations that cannot be recycled. */
int gc_freelist_class(size_t size)
{
    switch (size) {
    case sizeof(hamt_entry_t):    return 0;  /* 16 bytes */
    case sizeof(mino_hamt_node_t): return 1; /* 24 bytes */
    case sizeof(mino_env_t):      return 2;  /* 32 bytes */
    case sizeof(mino_val_t):      return 3;  /* 64 bytes */
    default:                      return -1;
    }
}

/* True when bytes_old has grown enough past the post-last-major
 * baseline to warrant starting a new major cycle. The tenths math
 * lets the default 1.5x multiplier be expressed without float. The
 * gc_threshold floor keeps the very first major from firing the
 * instant the first byte is promoted. */
static int gc_should_start_major(const mino_state_t *S)
{
    size_t trigger = S->gc_old_baseline * S->gc_major_growth_tenths / 10u;
    if (trigger < S->gc_threshold) {
        trigger = S->gc_threshold;
    }
    return S->gc_bytes_old > trigger;
}

/* Drive one incremental slice of an in-flight major: drain up to
 * gc_major_work_budget headers, and if the mark stack is empty after
 * that, close the cycle with remark + sweep. Times itself so the
 * mutator sees one pause per slice. */
static void gc_major_slice(mino_state_t *S)
{
    long long start_ns;
    size_t    elapsed_ns;
    if (S->gc_phase != GC_PHASE_MAJOR_MARK || S->gc_depth > 0) {
        return;
    }
    start_ns = mino_monotonic_ns();
    gc_major_step(S, S->gc_major_work_budget);
    S->gc_major_step_alloc = 0;
    if (S->gc_mark_stack_len == 0) {
        gc_major_remark(S);
        gc_major_sweep_phase(S);
    }
    elapsed_ns = (size_t)(mino_monotonic_ns() - start_ns);
    S->gc_total_ns += elapsed_ns;
    if (elapsed_ns > S->gc_max_ns) {
        S->gc_max_ns = elapsed_ns;
    }
}

/* Force any in-flight major to completion, with the mutator paused.
 * Used on OOM fallback so the fallback STW major can start from a
 * clean IDLE state, and at points where the caller cannot afford to
 * leave marking partway done. */
void gc_force_finish_major(mino_state_t *S)
{
    long long start_ns;
    size_t    elapsed_ns;
    if (S->gc_phase != GC_PHASE_MAJOR_MARK || S->gc_depth > 0) {
        return;
    }
    start_ns = mino_monotonic_ns();
    gc_major_step(S, (size_t)-1);
    gc_major_remark(S);
    gc_major_sweep_phase(S);
    elapsed_ns = (size_t)(mino_monotonic_ns() - start_ns);
    S->gc_total_ns += elapsed_ns;
    if (elapsed_ns > S->gc_max_ns) {
        S->gc_max_ns = elapsed_ns;
    }
}

/* Driver: called from gc_alloc_typed before each allocation. Picks
 * between starting a minor, starting a major (incrementally), or
 * advancing an in-flight major by one slice. The checks are ordered
 * so that: (1) stress mode still forces a full STW major on every
 * allocation, preserving the legacy test coverage; (2) while a major
 * is in flight, a nursery overflow has to force the major to finish
 * before the minor runs -- the minor-during-major interaction
 * requires a mark-stack floor and promotion hook that land in the
 * next step; (3) otherwise the normal IDLE-phase flow runs. */
static void gc_driver_tick(mino_state_t *S, size_t alloc_size)
{
    if (S->gc_depth > 0 || S->gc_stack_bottom == NULL) {
        return;
    }
    if (S->gc_stress) {
        if (S->gc_phase == GC_PHASE_MAJOR_MARK) {
            gc_force_finish_major(S);
        }
        if (S->gc_phase == GC_PHASE_IDLE) {
            gc_major_collect(S);
        }
        return;
    }
    if (S->gc_phase == GC_PHASE_MAJOR_MARK) {
        S->gc_major_step_alloc += alloc_size;
        if (S->gc_bytes_young > S->gc_nursery_bytes) {
            /* Finish the in-flight major before running the nursery
             * overflow minor. Running a nested minor while major's
             * mark stack holds pending entries is unsafe: minor's
             * sweep frees YOUNG objects reachable only through an
             * OLD entry still on major's stack, and major's next
             * gc_trace_children then chases the freed pointer. The
             * cost of force-finishing is bounded by whatever major
             * work is left; the alternative would require tracing
             * transitively through every mark-stack entry on every
             * nested minor, which is strictly more work in the
             * common case. */
            gc_force_finish_major(S);
            gc_minor_collect(S);
            return;
        }
        if (S->gc_major_step_alloc >= S->gc_major_alloc_quantum) {
            gc_major_slice(S);
        }
        return;
    }
    /* phase == IDLE */
    if (S->gc_bytes_young > S->gc_nursery_bytes) {
        gc_minor_collect(S);
    }
    if (S->gc_phase == GC_PHASE_IDLE && gc_should_start_major(S)) {
        gc_major_begin(S);
        gc_major_slice(S);
    }
}

void *gc_alloc_typed(mino_state_t *S, unsigned char tag, size_t size)
{
    gc_hdr_t *h;
    int fc;
    if (S->gc_stress == -1) {
        const char *e = getenv("MINO_GC_STRESS");
        S->gc_stress = (e != NULL && e[0] != '\0' && e[0] != '0') ? 1 : 0;
    }
    gc_driver_tick(S, size);
    /* Fault injection: simulate OOM when the countdown reaches zero. */
    if (S->fi_alloc_countdown > 0) {
        S->fi_alloc_countdown--;
        if (S->fi_alloc_countdown == 0) {
            if (S->try_depth > 0) {
                set_eval_diag(S, S->eval_current_form, "internal", "MIN001", "out of memory (fault injection)");
                S->try_stack[S->try_depth - 1].exception = NULL;
                longjmp(S->try_stack[S->try_depth - 1].buf, 1);
            }
            abort(); /* Class I: no error frame to recover through */
        }
    }
    /* Try free list first for fixed-size allocations. */
    fc = gc_freelist_class(size);
    if (fc >= 0 && S->gc_freelists[fc] != NULL) {
        h = S->gc_freelists[fc];
        S->gc_freelists[fc] = h->next;
        memset(h, 0, sizeof(*h) + size);
    } else {
        h = (gc_hdr_t *)calloc(1, sizeof(*h) + size);
        if (h == NULL && S->gc_depth == 0 && S->gc_stack_bottom != NULL) {
            /* OOM fallback: close any in-flight major and run one
             * full STW major before giving up. That frees anything
             * kept alive only by old-gen references the minor
             * skipped and releases the snapshot overhead of the
             * interrupted cycle. */
            gc_force_finish_major(S);
            if (S->gc_phase == GC_PHASE_IDLE) {
                gc_major_collect(S);
            }
            h = (gc_hdr_t *)calloc(1, sizeof(*h) + size);
        }
        if (h == NULL) {
            /* Recoverable when an eval try-frame exists; fatal otherwise. */
            if (S->try_depth > 0) {
                set_eval_diag(S, S->eval_current_form, "internal", "MIN001", "out of memory");
                S->try_stack[S->try_depth - 1].exception = NULL;
                longjmp(S->try_stack[S->try_depth - 1].buf, 1);
            }
            abort(); /* Class I: no error frame to recover through */
        }
    }
    h->type_tag      = tag;
    h->mark          = 0;
    h->gen           = GC_GEN_YOUNG;
    h->age           = 0;
    h->size          = size;
    h->next          = S->gc_all_young;
    S->gc_all_young  = h;
    S->gc_bytes_alloc  += size;
    S->gc_bytes_young  += size;
    gc_range_insert(S, h);
    gc_evt_record(S, GC_EVT_ALLOC, h, NULL, NULL,
                  (uintptr_t)tag, (uint16_t)(size & 0xffffu));
    return (void *)(h + 1);
}

mino_val_t *alloc_val(mino_state_t *S, mino_type_t type)
{
    mino_val_t *v = (mino_val_t *)gc_alloc_typed(S, GC_T_VAL, sizeof(*v));
    v->type = type;
    v->meta = NULL;
    return v;
}

char *dup_n(mino_state_t *S, const char *s, size_t len)
{
    char *out = (char *)gc_alloc_typed(S, GC_T_RAW, len + 1);
    if (len > 0) {
        memcpy(out, s, len);
    }
    out[len] = '\0';
    return out;
}


/* ------------------------------------------------------------------------- */
/* Shared trace machinery: mark-stack push, interior-pointer resolve, and    */
/* per-header trace. Reused by any collector (currently just the full-heap   */
/* major in gc_major_collect; the minor collector added in a later step will use   */
/* the same primitives).                                                     */
/* ------------------------------------------------------------------------- */

#define GC_MARK_STACK_INIT 256

/* Push h onto the mark stack if it is not already marked. Growth of
 * the stack on overflow is best-effort: if realloc fails, the push
 * silently drops and the collector is forced to rely on conservative
 * scan as a backstop. */
static void gc_mark_stack_push_raw(mino_state_t *S, gc_hdr_t *h)
{
    if (S->gc_mark_stack_len == S->gc_mark_stack_cap) {
        size_t new_cap = S->gc_mark_stack_cap == 0
            ? GC_MARK_STACK_INIT : S->gc_mark_stack_cap * 2;
        gc_hdr_t **ns = (gc_hdr_t **)realloc(
            S->gc_mark_stack, new_cap * sizeof(*ns));
        if (ns == NULL) return;
        S->gc_mark_stack = ns;
        S->gc_mark_stack_cap = new_cap;
    }
    S->gc_mark_stack[S->gc_mark_stack_len++] = h;
    if (S->gc_mark_stack_len > S->gc_mark_stack_high_water) {
        S->gc_mark_stack_high_water = S->gc_mark_stack_len;
    }
}

void gc_mark_push(mino_state_t *S, gc_hdr_t *h)
{
    if (h == NULL || h->mark) return;
    /* Generational filter: during minor marking we never trace into or
     * mark OLD headers. The write barrier guarantees every OLD-to-YOUNG
     * reference is reachable through the remembered set; other OLD
     * objects live by definition across minor cycles. */
    if (S->gc_phase == GC_PHASE_MINOR && h->gen == GC_GEN_OLD) return;
    h->mark = 1;
    gc_mark_stack_push_raw(S, h);
}

/* Enqueue a header onto the mark stack, bypassing the minor-phase
 * OLD filter. Used by the minor collector when it promotes YOUNG to
 * OLD during a MAJOR_MARK cycle: the newly-OLD object needs to be
 * traced by major even though the current phase is MINOR. */
void gc_major_enqueue_promoted(mino_state_t *S, gc_hdr_t *h)
{
    if (h == NULL || h->mark) return;
    h->mark = 1;
    gc_mark_stack_push_raw(S, h);
}

/* Resolve an interior pointer and push its header onto the mark stack.
 * Used by the conservative stack scan and by root enumeration, where
 * the pointer may be misaligned or not the payload start. */
static void gc_mark_interior_push(mino_state_t *S, const void *p)
{
    gc_hdr_t *h;
    if (p == NULL) return;
    h = gc_find_header_for_ptr(S, p);
    if (h != NULL) gc_mark_push(S, h);
}

/* Public entry point for conservative scanning and root marking. */
void gc_mark_interior(mino_state_t *S, const void *p)
{
    gc_mark_interior_push(S, p);
}

/* Push a child pointer held in a traced container. Pointers stored
 * inside GC containers are always exact payload starts or sentinels
 * (NULL / state-embedded singleton), so the range-index resolve of
 * gc_mark_interior_push is unnecessary here. Skipping it lets
 * gc_major_step avoid rebuilding the range index between slices,
 * which dominates the tracing cost at small heap sizes. */
static void gc_mark_child_push(mino_state_t *S, const void *p)
{
    gc_hdr_t *h;
    uintptr_t u, lo, hi;
    if (p == NULL) return;
    u  = (uintptr_t)p;
    lo = (uintptr_t)S;
    hi = lo + sizeof(*S);
    if (u >= lo && u < hi) return;  /* singleton inside state struct */
    h = ((gc_hdr_t *)p) - 1;
    gc_mark_push(S, h);
}

/* Trace every GC-managed pointer held by h, pushing each target into
 * the mark stack. Used from gc_drain_mark_stack on any header popped
 * for tracing, and directly from the minor collector when seeding the
 * mark stack from remembered-set entries. */
void gc_trace_children(mino_state_t *S, gc_hdr_t *h)
{
    switch (h->type_tag) {
    case GC_T_VAL: {
        mino_val_t *v = (mino_val_t *)(h + 1);
        gc_mark_child_push(S, v->meta);
        switch (v->type) {
        case MINO_STRING:
        case MINO_SYMBOL:
        case MINO_KEYWORD:
            gc_mark_child_push(S, v->as.s.data);
            break;
        case MINO_CONS:
            gc_mark_child_push(S, v->as.cons.car);
            gc_mark_child_push(S, v->as.cons.cdr);
            break;
        case MINO_VECTOR:
            gc_mark_child_push(S, v->as.vec.root);
            gc_mark_child_push(S, v->as.vec.tail);
            break;
        case MINO_MAP:
            gc_mark_child_push(S, v->as.map.root);
            gc_mark_child_push(S, v->as.map.key_order);
            break;
        case MINO_SET:
            gc_mark_child_push(S, v->as.set.root);
            gc_mark_child_push(S, v->as.set.key_order);
            break;
        case MINO_SORTED_MAP:
        case MINO_SORTED_SET:
            gc_mark_child_push(S, v->as.sorted.root);
            gc_mark_child_push(S, v->as.sorted.comparator);
            break;
        case MINO_FN:
        case MINO_MACRO:
            gc_mark_child_push(S, v->as.fn.params);
            gc_mark_child_push(S, v->as.fn.body);
            gc_mark_child_push(S, v->as.fn.env);
            break;
        case MINO_ATOM:
            gc_mark_child_push(S, v->as.atom.val);
            gc_mark_child_push(S, v->as.atom.watches);
            gc_mark_child_push(S, v->as.atom.validator);
            break;
        case MINO_LAZY:
            if (v->as.lazy.realized) {
                gc_mark_child_push(S, v->as.lazy.cached);
            } else {
                gc_mark_child_push(S, v->as.lazy.body);
                gc_mark_child_push(S, v->as.lazy.env);
            }
            break;
        case MINO_RECUR:
            gc_mark_child_push(S, v->as.recur.args);
            break;
        case MINO_TAIL_CALL:
            gc_mark_child_push(S, v->as.tail_call.fn);
            gc_mark_child_push(S, v->as.tail_call.args);
            break;
        case MINO_REDUCED:
            gc_mark_child_push(S, v->as.reduced.val);
            break;
        case MINO_VAR:
            gc_mark_child_push(S, v->as.var.root);
            break;
        case MINO_TRANSIENT:
            gc_mark_child_push(S, v->as.transient.current);
            break;
        case MINO_RATIO:
            gc_mark_child_push(S, v->as.ratio.num);
            gc_mark_child_push(S, v->as.ratio.denom);
            break;
        case MINO_BIGDEC:
            gc_mark_child_push(S, v->as.bigdec.unscaled);
            break;
        default:
            break;
        }
        break;
    }
    case GC_T_ENV: {
        mino_env_t *env = (mino_env_t *)(h + 1);
        size_t i;
        gc_mark_child_push(S, env->parent);
        if (env->bindings != NULL) {
            gc_mark_child_push(S, env->bindings);
            for (i = 0; i < env->len; i++) {
                gc_mark_child_push(S, env->bindings[i].name);
                gc_mark_child_push(S, env->bindings[i].val);
            }
        }
        gc_mark_child_push(S, env->ht_buckets);
        break;
    }
    case GC_T_VEC_NODE: {
        mino_vec_node_t *n = (mino_vec_node_t *)(h + 1);
        unsigned i;
        for (i = 0; i < n->count; i++) {
            gc_mark_child_push(S, n->slots[i]);
        }
        break;
    }
    case GC_T_HAMT_NODE: {
        mino_hamt_node_t *n = (mino_hamt_node_t *)(h + 1);
        unsigned count, i;
        gc_mark_child_push(S, n->slots);
        count = (n->collision_count > 0) ? n->collision_count
                                         : popcount32(n->bitmap);
        if (n->slots != NULL) {
            for (i = 0; i < count; i++) {
                gc_mark_child_push(S, n->slots[i]);
            }
        }
        break;
    }
    case GC_T_HAMT_ENTRY: {
        hamt_entry_t *e = (hamt_entry_t *)(h + 1);
        gc_mark_child_push(S, e->key);
        gc_mark_child_push(S, e->val);
        break;
    }
    case GC_T_VALARR:
    case GC_T_PTRARR: {
        void **arr = (void **)(h + 1);
        size_t n = h->size / sizeof(*arr);
        size_t i;
        for (i = 0; i < n; i++) {
            gc_mark_child_push(S, arr[i]);
        }
        break;
    }
    case GC_T_RB_NODE: {
        mino_rb_node_t *rb = (mino_rb_node_t *)(h + 1);
        gc_mark_child_push(S, rb->key);
        gc_mark_child_push(S, rb->val);
        gc_mark_child_push(S, rb->left);
        gc_mark_child_push(S, rb->right);
        break;
    }
    case GC_T_RAW:
    default:
        break;
    }
}

/* Drain the mark stack until its length drops to floor_len. The floor
 * lets a minor nested inside a MAJOR_MARK cycle process only the
 * entries it added on top, leaving major's pending OLD entries
 * untouched beneath. Callers that want a full drain pass 0. */
void gc_drain_mark_stack_to(mino_state_t *S, size_t floor_len)
{
    while (S->gc_mark_stack_len > floor_len) {
        gc_hdr_t *h = S->gc_mark_stack[--S->gc_mark_stack_len];
        gc_trace_children(S, h);
    }
}

void gc_drain_mark_stack(mino_state_t *S)
{
    gc_drain_mark_stack_to(S, 0);
}

/* ------------------------------------------------------------------------- */
/* Collection driver.                                                        */
/* ------------------------------------------------------------------------- */
/*
 * The major collector is structured as a four-step state machine
 * (begin / step / remark / sweep; see runtime_gc_major.c). For a fully
 * stop-the-world cycle the orchestrator below chains the four
 * back-to-back, keeping the mutator paused for the duration and
 * charging the combined time to a single gc_max_ns event. The
 * incremental allocator pacing (in gc_alloc_typed) calls the same
 * pieces individually, which charges each slice to its own event.
 */

void gc_major_collect(mino_state_t *S)
{
    long long start_ns;
    size_t    elapsed_ns;
    if (S->gc_depth > 0 || S->gc_phase != GC_PHASE_IDLE) {
        return;
    }
    start_ns = mino_monotonic_ns();
    gc_major_begin(S);
    gc_major_step(S, (size_t)-1);
    gc_major_remark(S);
    gc_major_sweep_phase(S);
    elapsed_ns = (size_t)(mino_monotonic_ns() - start_ns);
    S->gc_total_ns += elapsed_ns;
    if (elapsed_ns > S->gc_max_ns) {
        S->gc_max_ns = elapsed_ns;
    }
}
