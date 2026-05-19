/*
 * driver.c -- allocation driver, shared trace machinery, collection
 * entry point.
 *
 * The range index, root enumeration, and conservative stack scan
 * live in roots.c; the full-heap sweep lives in major.c. The driver
 * below stitches them together for one mark-and-sweep cycle.
 * Trace primitives (gc_mark_push, gc_drain_mark_stack,
 * gc_process_header) stay here because they are reused by the
 * generational minor collector in minor.c.
 */

#include "runtime/internal.h"
#include "eval/bc/internal.h"

/* Record a stack address from a host-called entry point so the collector's
 * conservative scan covers the entire host-to-mino call chain. We keep the
 * maximum address (shallowest frame on a downward-growing stack). */
/* -Wdangling-pointer was added in gcc-12; older gcc warns "unknown
 * option" with -Werror=pragmas if we name it unconditionally. */
#if defined(__GNUC__) && !defined(__clang__) && __GNUC__ >= 12
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdangling-pointer"
#endif
void gc_note_host_frame(mino_state_t *S, void *addr)
{
    if (mino_current_ctx(S)->gc_stack_bottom == NULL
        || (char *)addr > (char *)mino_current_ctx(S)->gc_stack_bottom) {
        mino_current_ctx(S)->gc_stack_bottom = addr;
    }
}
#if defined(__GNUC__) && !defined(__clang__) && __GNUC__ >= 12
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

/* Record one STW pause sample. Saturates the ring slot at UINT32_MAX
 * ns; bucket-clamps the log2 histogram at index 23 ([8.4ms, ...)). */
void gc_record_pause(mino_state_t *S, size_t ns)
{
    unsigned idx;
    unsigned bucket;
    size_t   n;
    uint32_t sample;
    sample = (ns > (size_t)UINT32_MAX) ? UINT32_MAX : (uint32_t)ns;
    idx = S->gc_pause_ring_idx;
    S->gc_pause_ring[idx] = sample;
    S->gc_pause_ring_idx = (idx + 1u) & 0xffu;
    if (S->gc_pause_ring_count < 256u) {
        S->gc_pause_ring_count++;
    }
    /* Log2 bucket: floor(log2(ns)). ns == 0 lands in bucket 0; any
     * value at or above 2^23 ns (~8.4 ms) clamps to bucket 23. */
    bucket = 0u;
    n = ns;
    while ((n >> 1) > 0u && bucket < 23u) {
        n >>= 1;
        bucket++;
    }
    S->gc_pause_hist[bucket]++;
}

/* Adapt gc_major_work_budget toward gc_pause_target_ns. Reads the
 * recent N=8 pauses from gc_pause_ring, computes the median, and
 * scales the budget up (multiply by 1.5) when pauses are under 80 %
 * of target, or halves when over 120 %. Damping: at least 10 slices
 * must pass between adjustments. Bounds: [256, 65536] headers.
 * Stress mode bypasses adaptive entirely (full STW per alloc means
 * the budget is irrelevant). */
static void gc_adapt_major_budget(mino_state_t *S)
{
    unsigned count, take, i;
    uint32_t samples[8];
    size_t   target;
    size_t   median;
    size_t   budget;
    if (S->gc.stress == 1) return;
    if (S->gc_pause_ring_count == 0) return;
    S->gc_budget_slices_since_adjust++;
    if (S->gc_budget_slices_since_adjust < 10u) return;
    count = S->gc_pause_ring_count;
    take  = count < 8u ? count : 8u;
    /* Pull the most recent `take` samples (newest first). The ring's
     * write index points at the NEXT slot, so the most recent valid
     * sample is at (idx - 1). */
    for (i = 0; i < take; i++) {
        unsigned slot = (S->gc_pause_ring_idx + 256u - 1u - i) & 0xffu;
        samples[i] = S->gc_pause_ring[slot];
    }
    /* Simple selection sort over up to 8 elements: cheap, no allocs. */
    for (i = 0; i + 1u < take; i++) {
        unsigned j, min_j = i;
        for (j = i + 1u; j < take; j++) {
            if (samples[j] < samples[min_j]) min_j = j;
        }
        if (min_j != i) {
            uint32_t tmp = samples[i];
            samples[i]   = samples[min_j];
            samples[min_j] = tmp;
        }
    }
    median = (size_t)samples[take / 2u];
    target = S->gc_pause_target_ns;
    if (target == 0u) return;
    budget = S->gc.major_work_budget;
    if (median > target + (target / 5u)) {        /* > 120 % of target */
        if (budget > 256u) {
            size_t halved = budget / 2u;
            S->gc.major_work_budget = halved < 256u ? 256u : halved;
            S->gc_budget_slices_since_adjust = 0;
        }
    } else if (median < (target * 4u) / 5u) {     /* < 80 % of target */
        if (budget < 65536u) {
            size_t scaled = budget + (budget / 2u);  /* * 1.5 */
            S->gc.major_work_budget = scaled > 65536u ? 65536u : scaled;
            S->gc_budget_slices_since_adjust = 0;
        }
    }
}

/* True when bytes_old has grown enough past the post-last-major
 * baseline to warrant starting a new major cycle. The tenths math
 * lets the default 1.5x multiplier be expressed without float. The
 * gc_threshold floor keeps the very first major from firing the
 * instant the first byte is promoted. */
static int gc_should_start_major(const mino_state_t *S)
{
    size_t trigger = S->gc.old_baseline * S->gc.major_growth_tenths / 10u;
    if (trigger < S->gc.threshold) {
        trigger = S->gc.threshold;
    }
    return S->gc.bytes_old > trigger;
}

/* Drive one incremental slice of an in-flight major: drain up to
 * gc_major_work_budget headers, and if the mark stack is empty after
 * that, close the cycle with remark + sweep. Times itself so the
 * mutator sees one pause per slice. */
static void gc_major_slice(mino_state_t *S)
{
    long long start_ns;
    size_t    elapsed_ns;
    if (S->gc.phase != GC_PHASE_MAJOR_MARK || mino_current_ctx(S)->gc_depth > 0) {
        return;
    }
    start_ns = mino_monotonic_ns();
    gc_major_step(S, S->gc.major_work_budget);
    S->gc.major_step_alloc = 0;
    if (S->gc.mark_stack_len == 0) {
        gc_major_remark(S);
        gc_major_sweep_phase(S);
    }
    elapsed_ns = (size_t)(mino_monotonic_ns() - start_ns);
    S->gc.total_ns += elapsed_ns;
    if (elapsed_ns > S->gc.max_ns) {
        S->gc.max_ns = elapsed_ns;
    }
    gc_record_pause(S, elapsed_ns);
    gc_adapt_major_budget(S);
}

/* Force any in-flight major to completion, with the mutator paused.
 * Used on OOM fallback so the fallback STW major can start from a
 * clean IDLE state, and at points where the caller cannot afford to
 * leave marking partway done. */
void gc_force_finish_major(mino_state_t *S)
{
    long long start_ns;
    size_t    elapsed_ns;
    if (S->gc.phase != GC_PHASE_MAJOR_MARK || mino_current_ctx(S)->gc_depth > 0) {
        return;
    }
    start_ns = mino_monotonic_ns();
    gc_major_step(S, (size_t)-1);
    gc_major_remark(S);
    gc_major_sweep_phase(S);
    elapsed_ns = (size_t)(mino_monotonic_ns() - start_ns);
    S->gc.total_ns += elapsed_ns;
    if (elapsed_ns > S->gc.max_ns) {
        S->gc.max_ns = elapsed_ns;
    }
    gc_record_pause(S, elapsed_ns);
}

/* Suppress collection: collection is only safe when this thread holds
 * the world (no other host threads alive, no recursive alloc), and the
 * conservative scan needs gc_stack_bottom recorded for the running
 * frame. Memory normalizes again after mino_quiesce_threads. */
static int gc_tick_should_suppress(mino_state_t *S)
{
    /* Relaxed-atomic read: workers mutate thread_count under
     * worker_list_lock, but this fast-path consults it without
     * locking. See internal.h thread_count comment. */
    return mino_current_ctx(S)->gc_depth > 0
        || mino_current_ctx(S)->gc_stack_bottom == NULL
        || __atomic_load_n(&S->thread_count, __ATOMIC_RELAXED) > 0;
}

/* Stress mode: every alloc forces a full STW major, preserving the
 * legacy stress-mode test coverage. Any in-flight incremental major is
 * driven to completion first so the stress major starts from IDLE. */
static void gc_tick_stress(mino_state_t *S)
{
    if (S->gc.phase == GC_PHASE_MAJOR_MARK) {
        gc_force_finish_major(S);
    }
    if (S->gc.phase == GC_PHASE_IDLE) {
        gc_major_collect(S);
    }
}

/* MAJOR_MARK phase: a nursery overflow forces the in-flight major to
 * finish before running the minor. Otherwise advance the major by one
 * slice once enough mutator allocation has accumulated.
 *
 * Why finish-then-minor rather than nest: a nested minor while major's
 * mark stack still holds OLD entries could free a YOUNG object reachable
 * only through an OLD pointer pending on major's stack, and major's next
 * gc_trace_children would then chase the freed pointer. Force-finishing
 * is bounded by the remaining major work; the alternative would require
 * transitively tracing every pending mark-stack entry on every nested
 * minor, which is strictly more work in the common case. */
static void gc_tick_during_major(mino_state_t *S, size_t alloc_size)
{
    S->gc.major_step_alloc += alloc_size;
    if (S->gc.bytes_young > S->gc.nursery_bytes) {
        gc_force_finish_major(S);
        gc_minor_collect(S);
        return;
    }
    if (S->gc.major_step_alloc >= S->gc.major_alloc_quantum) {
        gc_major_slice(S);
    }
}

/* IDLE phase: minor on nursery overflow; if the heap has grown enough
 * past the post-major baseline, kick off a fresh incremental major. */
static void gc_tick_idle(mino_state_t *S)
{
    if (S->gc.bytes_young > S->gc.nursery_bytes) {
        gc_minor_collect(S);
    }
    if (S->gc.phase == GC_PHASE_IDLE && gc_should_start_major(S)) {
        gc_major_begin(S);
        gc_major_slice(S);
    }
}

/* Driver: called from gc_alloc_typed before each allocation. Dispatches
 * to the right tick handler based on stress mode and current phase. */
static void gc_driver_tick(mino_state_t *S, size_t alloc_size)
{
    if (gc_tick_should_suppress(S)) return;
    if (S->gc.stress) {
        gc_tick_stress(S);
        return;
    }
    if (S->gc.phase == GC_PHASE_MAJOR_MARK) {
        gc_tick_during_major(S, alloc_size);
        return;
    }
    gc_tick_idle(S);
}

/* Slab-backed bump allocator. Allocates a fresh 64 KiB slab via calloc
 * and zeroes it (the bump fast path relies on slab memory being zero
 * so headers are returned in calloc-equivalent state without a per-alloc
 * memset). Links the slab onto S->gc_bump_slabs and parks the cursor
 * pair at the start of the usable payload region. Returns 0 on calloc
 * failure (caller falls back to plain calloc per allocation). */
static int gc_bump_slab_refill(mino_state_t *S)
{
    gc_bump_slab_t *slab;
    slab = (gc_bump_slab_t *)calloc(1, MINO_BUMP_SLAB_BYTES);
    if (slab == NULL) return 0;
    slab->next = S->gc_bump_slabs;
    S->gc_bump_slabs = slab;
    S->gc_bump_cur = (char *)slab + sizeof(*slab);
    S->gc_bump_end = (char *)slab + MINO_BUMP_SLAB_BYTES;
    S->gc_bump_slab_refills++;
    return 1;
}

/* gc_alloc_raw -- pure mechanism: pull a header from the freelist (when
 * a size-class slot is available), bump from the slab arena when the
 * bump allocator is enabled and the alloc fits, or calloc a fresh one;
 * initialize the header fields, link onto the young list, register with
 * the range index, and emit the alloc event. Returns NULL on alloc
 * failure; no GC, no fault-injection, no OOM recovery here -- those
 * are the policy wrapper's job. */
static gc_hdr_t *gc_alloc_raw(mino_state_t *S, unsigned char tag,
                              size_t size)
{
    gc_hdr_t *h = NULL;
    int       fc;
    /* Try free list first for fixed-size allocations. */
    fc = gc_freelist_class(size);
    if (fc >= 0 && S->gc.freelists[fc] != NULL) {
        unsigned char was_bump;
        h = S->gc.freelists[fc];
        S->gc.freelists[fc] = h->next;
        was_bump = h->bump;
        memset(h, 0, sizeof(*h) + size);
        h->bump = was_bump;
        S->gc_alloc_freelist_hits++;
    } else if (S->gc_bump_enabled) {
        /* Bump path: zero-fill comes for free from the slab's calloc.
         * Total = header + payload, rounded up to 8 bytes for alignment.
         * Oversized requests (won't fit in a slab) bypass bump and fall
         * through to the calloc arm. */
        size_t total = sizeof(*h) + size;
        total = (total + 7u) & ~(size_t)7u;
        if (total <= MINO_BUMP_SLAB_BYTES - sizeof(gc_bump_slab_t)) {
            if ((size_t)(S->gc_bump_end - S->gc_bump_cur) < total) {
                if (!gc_bump_slab_refill(S)) {
                    /* Slab refill failed; fall through to plain calloc. */
                    goto calloc_path;
                }
            }
            h = (gc_hdr_t *)S->gc_bump_cur;
            S->gc_bump_cur += total;
            h->bump = 1;
            S->gc_bump_alloc_hits++;
        } else {
            goto calloc_path;
        }
    } else {
    calloc_path:
        h = (gc_hdr_t *)calloc(1, sizeof(*h) + size);
        if (h == NULL) return NULL;
        if (fc >= 0) {
            S->gc_alloc_calloc_size_class_miss++;
        } else {
            S->gc_alloc_calloc_no_class++;
        }
    }
    h->type_tag        = tag;
    h->mark            = 0;
    h->gen             = GC_GEN_YOUNG;
    h->age             = 0;
    h->size            = size;
    h->next            = S->gc.all_young;
    S->gc.all_young    = h;
    S->gc.bytes_alloc += size;
    S->gc.bytes_young += size;
    gc_range_insert(S, h);
    gc_evt_record(S, GC_EVT_ALLOC, h, NULL, NULL,
                  (uintptr_t)tag, (uint16_t)(size & 0xffffu));
    return h;
}

/* gc_oom_throw -- raise the standard OOM mino diagnostic by longjmp'ing
 * into the active try frame, or abort if no try frame exists. Non-static
 * so checked-size paths in env/module/etc. can reach the same throw when
 * they detect overflow before the GC allocator gets a chance. */
void gc_oom_throw(mino_state_t *S, const char *msg)
{
    if (mino_current_ctx(S)->try_depth > 0) {
        set_eval_diag(S, mino_current_ctx(S)->eval_current_form,
                      "internal", "MIN001", msg);
        mino_current_ctx(S)->try_stack[mino_current_ctx(S)->try_depth - 1].exception = NULL;
        longjmp(mino_current_ctx(S)->try_stack[mino_current_ctx(S)->try_depth - 1].buf, 1);
    }
    abort(); /* Class I: no error frame to recover through */
}

/* Allocation-site sampler: with probability 1/alloc_sampler_rate
 * (default 4096), record (return address, tag, size_bucket) for this
 * alloc into the small ring. Env-gated. The return address is the
 * immediate caller -- one frame up from gc_alloc_typed_inner. */
static void mino_alloc_sampler_fire(mino_state_t *S, unsigned char tag,
                                    size_t size, void *site_ra)
{
    unsigned     idx;
    unsigned     bucket;
    size_t       sb;
    if (S->alloc_sampler_enabled == 0) {
        const char *e = getenv("MINO_ALLOC_SAMPLE");
        if (e == NULL || e[0] == '\0' || e[0] == '0') {
            S->alloc_sampler_enabled = -1;
            return;
        }
        const char *r = getenv("MINO_ALLOC_SAMPLE_RATE");
        S->alloc_sampler_rate = (r != NULL && r[0] != '\0')
                                ? (unsigned)atoi(r) : 4096u;
        if (S->alloc_sampler_rate == 0u) S->alloc_sampler_rate = 4096u;
        S->alloc_sampler_enabled = 1;
    }
    if (S->alloc_sampler_enabled != 1) return;
    S->alloc_sampler_counter++;
    if ((S->alloc_sampler_counter % S->alloc_sampler_rate) != 0u) return;
    if (S->alloc_sampler_ring == NULL) {
        S->alloc_sampler_ring_cap = 4096u;
        S->alloc_sampler_ring = (mino_alloc_sample_t *)calloc(
            S->alloc_sampler_ring_cap, sizeof(mino_alloc_sample_t));
        if (S->alloc_sampler_ring == NULL) {
            S->alloc_sampler_enabled = -1;
            return;
        }
    }
    bucket = 0u;
    sb     = size + 1u;
    while ((sb >> 1) > 0u && bucket < 31u) { sb >>= 1; bucket++; }
    idx = S->alloc_sampler_ring_idx;
    S->alloc_sampler_ring[idx].site        = site_ra;
    S->alloc_sampler_ring[idx].tag         = tag;
    S->alloc_sampler_ring[idx].size_bucket = (uint8_t)bucket;
    S->alloc_sampler_ring[idx].count       = 0;
    S->alloc_sampler_ring_idx = (idx + 1u) % S->alloc_sampler_ring_cap;
    if (S->alloc_sampler_ring_count < S->alloc_sampler_ring_cap) {
        S->alloc_sampler_ring_count++;
    }
}

void *gc_alloc_typed_inner(mino_state_t *S, unsigned char tag, size_t size)
{
    gc_hdr_t *h;
    /* Capture the immediate caller's return address for the alloc-site
     * sampler. The fire helper bails fast when disabled, so the
     * builtin call cost is only paid when sampling is on. */
    mino_alloc_sampler_fire(S, tag, size,
                            __builtin_return_address(0));
    if (S->gc.stress == -1) {
        const char *e = getenv("MINO_GC_STRESS");
        S->gc.stress = (e != NULL && e[0] != '\0' && e[0] != '0') ? 1 : 0;
    }
    /* Safepoint: every alloc is an opportunity for the collector to ask
     * the mutator to park. Gated on gc_depth == 0 so a recursive alloc
     * reached from inside trace or sweep doesn't re-enter the park
     * machinery. */
    if (mino_current_ctx(S)->gc_depth == 0) {
        mino_safepoint_poll(S);
    }
    gc_driver_tick(S, size);
    /* Fault injection: simulate OOM when the countdown reaches zero. */
    if (S->fi_alloc_countdown > 0) {
        S->fi_alloc_countdown--;
        if (S->fi_alloc_countdown == 0) {
            gc_oom_throw(S, "out of memory (fault injection)");
        }
    }
    h = gc_alloc_raw(S, tag, size);
    if (h != NULL && tag < GC_T__COUNT) {
        S->gc_alloc_by_tag[tag]++;
    }
    if (h == NULL
        && mino_current_ctx(S)->gc_depth == 0
        && mino_current_ctx(S)->gc_stack_bottom != NULL) {
        /* OOM fallback: close any in-flight major and run one full STW
         * major before giving up. That frees anything kept alive only
         * by old-gen references the minor skipped and releases the
         * snapshot overhead of the interrupted cycle. */
        gc_force_finish_major(S);
        if (S->gc.phase == GC_PHASE_IDLE) {
            gc_major_collect(S);
        }
        h = gc_alloc_raw(S, tag, size);
    }
    if (h == NULL) {
        gc_oom_throw(S, "out of memory");
    }
    {
        void *payload = (void *)(h + 1);
        /* Every heap payload must be 8-byte aligned: the pointer-tag
         * scheme relies on tag-bit-zero in the low 3 bits of every
         * legitimate pointer. A misaligned alloc silently corrupts the
         * tag and is fatal in production. */
        MINO_ASSERT_ALIGNED(payload);
        return payload;
    }
}

mino_val_t *alloc_val_inner(mino_state_t *S, mino_type_t type)
{
    mino_val_t *v = (mino_val_t *)gc_alloc_typed_inner(S, GC_T_VAL, sizeof(*v));
    MINO_ASSERT_ALIGNED(v);
    v->type = type;
    v->meta = NULL;
    return v;
}

char *dup_n_inner(mino_state_t *S, const char *s, size_t len)
{
    char *out = (char *)gc_alloc_typed_inner(S, GC_T_RAW, len + 1);
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
    if (S->gc.mark_stack_len == S->gc.mark_stack_cap) {
        size_t     new_cap;
        gc_hdr_t **ns;
        if (S->gc.mark_stack_cap == 0) {
            new_cap = GC_MARK_STACK_INIT;
        } else if (S->gc.mark_stack_cap > SIZE_MAX / 2 / sizeof(*ns)) {
            S->gc_mark_stack_overflows++;
            return; /* Cap overflow: drop the push (collector falls back
                     * on conservative scan as a backstop). */
        } else {
            new_cap = S->gc.mark_stack_cap * 2;
        }
        ns = (gc_hdr_t **)realloc(S->gc.mark_stack, new_cap * sizeof(*ns));
        if (ns == NULL) {
            S->gc_mark_stack_overflows++;
            return;
        }
        S->gc.mark_stack = ns;
        S->gc.mark_stack_cap = new_cap;
    }
    S->gc.mark_stack[S->gc.mark_stack_len++] = h;
    if (S->gc.mark_stack_len > S->gc.mark_stack_high_water) {
        S->gc.mark_stack_high_water = S->gc.mark_stack_len;
    }
}

void gc_mark_push(mino_state_t *S, gc_hdr_t *h)
{
    if (h == NULL || h->mark) return;
    /* Generational filter: during minor marking we never trace into or
     * mark OLD headers. The write barrier guarantees every OLD-to-YOUNG
     * reference is reachable through the remembered set; other OLD
     * objects live by definition across minor cycles. */
    if (S->gc.phase == GC_PHASE_MINOR && h->gen == GC_GEN_OLD) return;
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
    /* Inline-tagged values (low 3 bits non-zero) are not heap pointers;
     * the conservative scan can encounter them on the C stack when a
     * tagged val happens to land in a register/stack slot during a
     * scan. Skipping them avoids a bogus header lookup. */
    if (((uintptr_t)p & MINO_TAG_MASK) != 0) return;
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
 * which dominates the tracing cost at small heap sizes.
 *
 * Tracers in component-owned files (collections/gc_handlers.c,
 * eval/bc/gc_handlers.c, values/val.c) call through this entry
 * point via gc_mark_child_push_exported. */
static void gc_mark_child_push(mino_state_t *S, const void *p);

void gc_mark_child_push_exported(mino_state_t *S, const void *p)
{
    gc_mark_child_push(S, p);
}

static void gc_mark_child_push(mino_state_t *S, const void *p)
{
    gc_hdr_t *h;
    uintptr_t u, lo, hi;
    if (p == NULL) return;
    /* Inline-tagged values (low 3 bits non-zero) hold their payload
     * directly in the pointer-sized slot; there's no heap cell to
     * trace. The check has to come before the singleton-range test
     * because the singleton range straddles aligned addresses. */
    if (((uintptr_t)p & MINO_TAG_MASK) != 0) return;
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

/* ------------------------------------------------------------------------- */
/* Per-tag tracer functions                                                  */
/*                                                                           */
/* One function per GC_T_* tag, registered into S->gc_tracers at boot.       */
/* gc_trace_children dispatches through that table. Component-owned tracers  */
/* register from their own gc_handlers.c file:                               */
/*   GC_T_VAL       -> values/gc_handlers.c                                  */
/*   GC_T_VEC_NODE,                                                          */
/*   GC_T_HAMT_NODE,                                                         */
/*   GC_T_HAMT_ENTRY,                                                        */
/*   GC_T_RB_NODE   -> collections/gc_handlers.c                             */
/*   GC_T_BC        -> eval/bc/gc_handlers.c                                 */
/* gc/driver.c only owns the gc-internal tags: GC_T_ENV (env layout that     */
/* runtime/collections share), GC_T_VALARR + GC_T_PTRARR (generic pointer    */
/* arrays). GC_T_RAW intentionally has no tracer.                            */
/* ------------------------------------------------------------------------- */

static void trace_env(mino_state_t *S, gc_hdr_t *h)
{
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
}

/* trace_vec_node / trace_hamt_node / trace_hamt_entry / trace_rb_node
 * live in src/collections/gc_handlers.c and register themselves via
 * mino_collections_register_gc_handlers(S). */

/* Walks GC_T_VALARR and GC_T_PTRARR alike: both layouts are a flat
 * array of void *. */
static void trace_pointer_array(mino_state_t *S, gc_hdr_t *h)
{
    void **arr = (void **)(h + 1);
    size_t n = h->size / sizeof(*arr);
    size_t i;
    for (i = 0; i < n; i++) {
        gc_mark_child_push(S, arr[i]);
    }
}

/* trace_bc lives in src/eval/bc/gc_handlers.c and registers itself
 * via mino_bc_register_gc_handlers(S). */

void gc_trace_children(mino_state_t *S, gc_hdr_t *h)
{
    gc_tracer_fn fn;
    unsigned tag = h->type_tag;
    if (tag >= GC_T__COUNT) return;
    fn = S->gc_tracers[tag];
    if (fn != NULL) fn(S, h);
}

/* ------------------------------------------------------------------------- */
/* Tracer + finalizer registration                                           */
/* ------------------------------------------------------------------------- */

void gc_register_tracer(mino_state_t *S, unsigned char tag, gc_tracer_fn fn)
{
    if (tag < GC_T__COUNT) S->gc_tracers[tag] = fn;
}

void gc_register_finalizer(mino_state_t *S, unsigned char tag,
                           gc_finalizer_fn fn)
{
    if (tag < GC_T__COUNT) S->gc_finalizers[tag] = fn;
}

/* Wire up every built-in tag. Component-owned tracers (collections,
 * eval/bc, values) currently live in driver.c; later cycles move
 * them out to their owning components and the component-side hook
 * (mino_collections_register_gc_handlers etc.) calls
 * gc_register_tracer. */
void gc_register_default_tracers(mino_state_t *S)
{
    /* GC_T_RAW intentionally stays NULL: a POD buffer has nothing to
     * trace; gc_trace_children no-ops on NULL slots.
     *
     * Component-owned slots (GC_T_VEC_NODE, GC_T_HAMT_NODE,
     * GC_T_HAMT_ENTRY, GC_T_RB_NODE, GC_T_BC, GC_T_VAL) are
     * populated by each component's register hook called from
     * runtime/state.c::state_init. */
    gc_register_tracer(S, GC_T_ENV,    trace_env);
    gc_register_tracer(S, GC_T_PTRARR, trace_pointer_array);
    gc_register_tracer(S, GC_T_VALARR, trace_pointer_array);
}

/* Drain the mark stack until its length drops to floor_len. The floor
 * lets a minor nested inside a MAJOR_MARK cycle process only the
 * entries it added on top, leaving major's pending OLD entries
 * untouched beneath. Callers that want a full drain pass 0. */
void gc_drain_mark_stack_to(mino_state_t *S, size_t floor_len)
{
    while (S->gc.mark_stack_len > floor_len) {
        gc_hdr_t *h = S->gc.mark_stack[--S->gc.mark_stack_len];
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
 * (begin / step / remark / sweep; see major.c). For a fully
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
    if (mino_current_ctx(S)->gc_depth > 0 || S->gc.phase != GC_PHASE_IDLE) {
        return;
    }
    start_ns = mino_monotonic_ns();
    /* Stop the world for the duration of the major sweep.
     * Single-threaded today this is a flag toggle pair on
     * S->main_ctx; multi-threaded variants ask every worker to park
     * at its next safepoint, wait for the count to reach zero, run
     * the sweep, then release. */
    gc_request_stw(S);
    gc_major_begin(S);
    gc_major_step(S, (size_t)-1);
    gc_major_remark(S);
    gc_major_sweep_phase(S);
    gc_release_stw(S);
    elapsed_ns = (size_t)(mino_monotonic_ns() - start_ns);
    S->gc.total_ns += elapsed_ns;
    if (elapsed_ns > S->gc.max_ns) {
        S->gc.max_ns = elapsed_ns;
    }
    gc_record_pause(S, elapsed_ns);
}
