/*
 * trace.c -- diagnostic GC event ring + abort-time
 * reachability classifier. Opt-in via MINO_GC_EVT=1 at state init.
 *
 * Design goal: capture enough context to reconstruct the timeline
 * around a gc_verify_remset_complete() abort WITHOUT perturbing the
 * timing of the collector on the hot path.
 *
 * Ring pattern:
 *   - fixed power-of-two buffer preallocated at init (GC_EVT_CAP)
 *   - single-threaded producer => no atomics needed
 *   - writes are pure stores; no I/O, no allocation, no locking
 *   - payload fields written first; seq committed last so a partial
 *     write is distinguishable (seq < expected => skip slot)
 *   - dump and classifier only run from the verify abort path
 *
 * Classifier pattern (Codex A-vs-B discriminator):
 *   Pass 1: clear marks, precise-only (gc_mark_roots + drain).
 *           Offender marked => reachable without conservative scan,
 *                              i.e. a genuine remset miss (class A).
 *   Pass 2: clear marks, precise + conservative stack + drain.
 *           Offender marked only here => zombie kept alive by a
 *                                        stale stack word (class B).
 *   Offender not marked in either => bookkeeping corruption (class C).
 *
 * Non-destructive: every mark bit is saved before pass 1 and restored
 * after pass 2 so the calling verify loop sees the same heap state it
 * was inspecting. The classifier flips gc_phase to MAJOR_MARK for the
 * duration of each pass so gc_mark_push does NOT filter OLD headers
 * out of the frontier -- otherwise pass 1 would be a minor-only mark
 * and could never mark the offender.
 */

#include "runtime/internal.h"
#include <inttypes.h>

/* ---------------------------------------------------------------- */
/* Ring buffer: init / free / record / dump                         */
/* ---------------------------------------------------------------- */

void gc_evt_init(mino_state *S)
{
    const char *env = getenv("MINO_GC_EVT");
    if (env == NULL || env[0] == '\0' || env[0] == '0') {
        S->gc.evt_ring = NULL;
        return;
    }
    S->gc.evt_ring = (gc_evt_t *)calloc(GC_EVT_CAP, sizeof(gc_evt_t));
    if (S->gc.evt_ring == NULL) {
        /* Diagnostic feature only: OOM disables the ring rather than
         * aborting the host. The ring is not required for correctness;
         * leaving it NULL causes every gc_evt_record call to no-op via
         * the macro guard, so collection continues uninstrumented.
         * Not a Class I abort: the failure is user-triggered (env var)
         * and the process can run correctly without the event buffer. */
        fprintf(stderr,
                "[gc-evt] MINO_GC_EVT=1: calloc failed; event ring disabled\n");
        return;
    }
    S->gc.evt_seq = 1; /* 0 reserved for "empty" */
}

void gc_evt_free(mino_state *S)
{
    free(S->gc.evt_ring);
    S->gc.evt_ring = NULL;
}

/* Callers reach this via the gc_evt_record macro, which already
 * short-circuits when the ring is NULL. Never call this directly. */
void gc_evt_record_impl(mino_state *S, uint8_t kind, const void *a,
                        const void *b, const void *c, uintptr_t aux,
                        uint16_t extra)
{
    gc_evt_t *e;
    uint64_t  seq;
    seq = S->gc.evt_seq++;
    e   = &S->gc.evt_ring[seq & GC_EVT_CAP_MASK];
    /* Payload first; seq last as commit marker. Single-threaded so no
     * barriers needed; the ordering just keeps the dump consistent if
     * we ever read from a signal handler. */
    e->cycle = (uint32_t)S->gc.collections_minor;
    e->kind  = kind;
    e->phase = (uint8_t)S->gc.phase;
    e->extra = extra;
    e->a     = (void *)a;
    e->b     = (void *)b;
    e->c     = (void *)c;
    e->aux   = aux;
    e->seq   = seq;
}

static const char *gc_evt_kind_name(uint8_t k)
{
    switch (k) {
    case GC_EVT_WB:           return "WB";
    case GC_EVT_REMSET_ADD:   return "RA";
    case GC_EVT_REMSET_RESET: return "RR";
    case GC_EVT_REMSET_PURGE: return "RP";
    case GC_EVT_PROMOTE:      return "PR";
    case GC_EVT_FREE_YOUNG:   return "FR";
    case GC_EVT_MINOR_BEGIN:  return "MCB";
    case GC_EVT_MINOR_END:    return "MCE";
    case GC_EVT_MAJOR_BEGIN:  return "MJB";
    case GC_EVT_MAJOR_SWEEP:  return "MJS";
    case GC_EVT_ALLOC:        return "AL";
    default:                  return "??";
    }
}

static int gc_evt_mentions(const gc_evt_t *e, const void *p1,
                           const void *p2, const void *p3)
{
    const void *targets[3];
    int         n = 0, i;
    if (p1 != NULL) targets[n++] = p1;
    if (p2 != NULL) targets[n++] = p2;
    if (p3 != NULL) targets[n++] = p3;
    if (n == 0) return 1; /* no filter: accept all */
    for (i = 0; i < n; i++) {
        if (e->a == targets[i]) return 1;
        if (e->b == targets[i]) return 1;
        if (e->c == targets[i]) return 1;
    }
    return 0;
}

void gc_evt_dump_around(mino_state *S, const void *p1, const void *p2,
                        const void *p3)
{
    uint64_t end, start, i;
    size_t   dumped = 0;
    if (S->gc.evt_ring == NULL) {
        fprintf(stderr, "[gc-evt] ring not enabled (set MINO_GC_EVT=1)\n");
        return;
    }
    end   = S->gc.evt_seq;
    start = (end > GC_EVT_CAP) ? (end - GC_EVT_CAP) : 1;
    fprintf(stderr,
            "[gc-evt] dumping events seq=[%llu..%llu) filter=%p/%p/%p\n",
            (unsigned long long)start, (unsigned long long)end,
            p1, p2, p3);
    for (i = start; i < end; i++) {
        const gc_evt_t *e = &S->gc.evt_ring[i & GC_EVT_CAP_MASK];
        if (e->seq != i) continue; /* overwritten or never committed */
        if (!gc_evt_mentions(e, p1, p2, p3)) continue;
        fprintf(stderr,
                "  #%llu c=%u ph=%u %-3s a=%p b=%p c=%p aux=0x%" PRIxPTR " x=%u\n",
                (unsigned long long)e->seq,
                (unsigned)e->cycle, (unsigned)e->phase,
                gc_evt_kind_name(e->kind),
                e->a, e->b, e->c,
                (uintptr_t)e->aux, (unsigned)e->extra);
        dumped++;
    }
    fprintf(stderr, "[gc-evt] %zu events match filter\n", dumped);
}

/* ---------------------------------------------------------------- */
/* Reachability classifier                                          */
/* ---------------------------------------------------------------- */

/* Walk both generation lists and apply fn to every header. Used by
 * the classifier and by gc_verify_remset_complete to save, clear, and
 * restore mark bits.  Declared in gc/internal.h so minor.c can call it. */
void gc_for_each_hdr(mino_state *S,
                     void (*fn)(gc_hdr_t *h, void *user),
                     void *user)
{
    gc_hdr_t *h;
    for (h = S->gc.all_young; h != NULL; h = h->next) fn(h, user);
    for (h = S->gc.all_old;   h != NULL; h = h->next) fn(h, user);
}

/* Count headers live on both lists, used to size mark-save buffers.
 * Declared in gc/internal.h so minor.c can call it. */
size_t gc_count_hdrs(mino_state *S)
{
    size_t    n = 0;
    gc_hdr_t *h;
    for (h = S->gc.all_young; h != NULL; h = h->next) n++;
    for (h = S->gc.all_old;   h != NULL; h = h->next) n++;
    return n;
}

/* Save callback: copies h->mark into the save buffer and clears h->mark.
 * Declared in gc/internal.h (as gc_save_mark_fn) so minor.c can call it. */
void gc_save_mark_fn(gc_hdr_t *h, void *user)
{
    struct gc_mark_save_ctx *c = (struct gc_mark_save_ctx *)user;
    if (c->idx >= c->cap) return;
    c->hdrs[c->idx]  = h;
    c->marks[c->idx] = h->mark;
    h->mark          = 0;
    c->idx++;
}

int gc_classify_offender(mino_state *S, gc_hdr_t *offender)
{
    struct gc_mark_save_ctx ctx;
    size_t   i, n;
    int      saved_phase = S->gc.phase;
    size_t   saved_floor = S->gc.mark_stack_len;
    int      pass1, pass2;
    size_t   saved_ranges_valid = S->gc.ranges_valid;

    /* Guard: increment gc_depth for the duration of classification so
     * that any allocation triggered by gc_mark_roots (or transitively
     * by gc_build_range_index) does not re-enter the collector while
     * we are mutating phase and mark bits.  The increment is
     * unconditional: callers already inside a collection have depth
     * >= 1 and this extra increment is harmless; callers at depth 0
     * (e.g., a post-abort debug helper) would otherwise allow an
     * alloc-triggered minor to run and corrupt the temporary state. */
    mino_current_ctx(S)->gc_depth++;

    /* Build a fresh range index so gc_find_header_for_ptr works, then
     * freeze it for the duration of classification. */
    if (!S->gc.ranges_valid) {
        gc_build_range_index(S);
    }

    n = gc_count_hdrs(S);
    ctx.hdrs  = (gc_hdr_t **)calloc(n, sizeof(*ctx.hdrs));
    ctx.marks = (unsigned char *)calloc(n, sizeof(*ctx.marks));
    ctx.idx   = 0;
    ctx.cap   = n;
    if (ctx.hdrs == NULL || ctx.marks == NULL) {
        free(ctx.hdrs);
        free(ctx.marks);
        mino_current_ctx(S)->gc_depth--;
        fprintf(stderr, "[gc-classify] oom allocating mark-save buffer\n");
        return -1;
    }

    /* Save + clear all marks. Order matters: gc_save_mark_fn zeros each
     * mark as it copies it. */
    gc_for_each_hdr(S, gc_save_mark_fn, &ctx);

    /* Use GC_PHASE_IDLE for both passes.  This satisfies two constraints:
     * (1) gc_mark_push's OLD filter fires only for GC_PHASE_MINOR, so
     *     OLD headers are not filtered out of the mark frontier -- the
     *     offender (an OLD header) will be reached if it is reachable.
     * (2) gc_mark_intern_table skips its walk only for GC_PHASE_MAJOR_MARK;
     *     with IDLE it runs in full, ensuring freshly interned YOUNG
     *     symbols that have no other root are marked and do not generate
     *     spurious CLASS-A reports (intern-reachable YOUNG appearing to
     *     be an unmarked OLD->YOUNG edge because the verify missed them). */
    S->gc.phase = GC_PHASE_IDLE;

    /* Pass 1: precise-only. */
    gc_mark_roots(S);
    gc_drain_mark_stack_to(S, saved_floor);
    pass1 = offender->mark ? 1 : 0;

    /* Reset marks for pass 2. */
    for (i = 0; i < ctx.idx; i++) ctx.hdrs[i]->mark = 0;

    /* Pass 2: precise + conservative stack. */
    gc_mark_roots(S);
    gc_drain_mark_stack_to(S, saved_floor);
    gc_scan_stack(S);
    gc_drain_mark_stack_to(S, saved_floor);
    pass2 = offender->mark ? 1 : 0;

    /* Restore every saved mark and restore phase. */
    for (i = 0; i < ctx.idx; i++) {
        ctx.hdrs[i]->mark = ctx.marks[i];
    }
    S->gc.phase        = saved_phase;
    S->gc.ranges_valid = saved_ranges_valid;

    free(ctx.hdrs);
    free(ctx.marks);
    mino_current_ctx(S)->gc_depth--;

    if (pass1) {
        fprintf(stderr,
                "[gc-classify] offender %p: CLASS A "
                "(precise-reachable; genuine barrier/remset miss)\n",
                (void *)offender);
        return 1;
    }
    if (pass2) {
        fprintf(stderr,
                "[gc-classify] offender %p: CLASS B "
                "(zombie; only conservative-stack-reachable)\n",
                (void *)offender);
        return 2;
    }
    fprintf(stderr,
            "[gc-classify] offender %p: CLASS C "
            "(not reachable at all; bookkeeping corruption)\n",
            (void *)offender);
    return 0;
}
