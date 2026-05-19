/*
 * public_gc.c -- host-facing garbage collector control.
 *
 * Thin wrappers over the internal collector: host asks for a cheap
 * minor, a cycle-closing major, or a full STW reclamation; host reads
 * tuning knobs via one enum-tagged setter; host queries counters via
 * one struct-out getter. All three calls are safe at any GC phase.
 */

#include <stdio.h>
#include <stdlib.h>
#include "mino.h"
#include "public/internal_bridge.h"
#include "gc/internal.h"        /* for GC_T__COUNT in stats copy */
#include "eval/bc/internal.h"  /* for mino_bc_fn_t + mino_sample_t */

void mino_gc_collect(mino_state_t *S, mino_gc_kind_t kind)
{
    if (S == NULL || mino_current_ctx(S)->gc_depth > 0) {
        return;
    }
    switch (kind) {
    case MINO_GC_MINOR:
        if (mino_current_ctx(S)->gc_stack_bottom != NULL) {
            gc_minor_collect(S);
        }
        return;
    case MINO_GC_MAJOR:
        if (mino_current_ctx(S)->gc_stack_bottom == NULL) {
            return;
        }
        if (S->gc_phase == GC_PHASE_MAJOR_MARK) {
            gc_force_finish_major(S);
        } else if (S->gc_phase == GC_PHASE_IDLE) {
            gc_major_collect(S);
        }
        return;
    case MINO_GC_FULL:
        if (mino_current_ctx(S)->gc_stack_bottom == NULL) {
            return;
        }
        /* Finish any in-flight major BEFORE running the minor. A nested
         * minor while major's mark stack still holds OLD entries could
         * free a YOUNG object reachable only through an OLD pointer
         * pending on major's stack; major's next gc_trace_children
         * would then chase the freed pointer. Same invariant the
         * auto-tick path (gc_tick_during_major) honours. */
        if (S->gc_phase == GC_PHASE_MAJOR_MARK) {
            gc_force_finish_major(S);
        }
        if (S->gc_bytes_young > 0) {
            gc_minor_collect(S);
        }
        if (S->gc_phase == GC_PHASE_IDLE) {
            gc_major_collect(S);
        }
        return;
    }
}

/* Range-validating setters. Each returns 1 on accept, 0 on reject.
 * The outer dispatcher turns the 1/0 result into 0/-1 per the
 * documented mino_set_limit convention. */
static int set_nursery_bytes(mino_state_t *S, size_t v)
{
    if (v < 64u * 1024u || v > 256u * 1024u * 1024u) {
        return 0;
    }
    S->gc_nursery_bytes = v;
    return 1;
}

static int set_major_growth_tenths(mino_state_t *S, size_t v)
{
    if (v < 11u || v > 40u) {
        return 0;
    }
    S->gc_major_growth_tenths = (unsigned)v;
    return 1;
}

static int set_promotion_age(mino_state_t *S, size_t v)
{
    if (v < 1u || v > 8u) {
        return 0;
    }
    S->gc_promotion_age = (unsigned)v;
    return 1;
}

static int set_incremental_budget(mino_state_t *S, size_t v)
{
    if (v < 64u || v > 65536u) {
        return 0;
    }
    S->gc_major_work_budget = v;
    return 1;
}

static int set_step_alloc_bytes(mino_state_t *S, size_t v)
{
    if (v < 1024u || v > 16u * 1024u * 1024u) {
        return 0;
    }
    S->gc_major_alloc_quantum = v;
    return 1;
}

int mino_gc_set_param(mino_state_t *S, mino_gc_param_t p, size_t value)
{
    int ok = 0;
    if (S == NULL) {
        return -1;
    }
    switch (p) {
    case MINO_GC_NURSERY_BYTES:       ok = set_nursery_bytes(S, value);       break;
    case MINO_GC_MAJOR_GROWTH_TENTHS: ok = set_major_growth_tenths(S, value); break;
    case MINO_GC_PROMOTION_AGE:       ok = set_promotion_age(S, value);       break;
    case MINO_GC_INCREMENTAL_BUDGET:  ok = set_incremental_budget(S, value);  break;
    case MINO_GC_STEP_ALLOC_BYTES:    ok = set_step_alloc_bytes(S, value);    break;
    default:                          return -1;
    }
    return ok ? 0 : -1;
}

/* Map the internal GC phase enum onto the public phase tag. Kept
 * deliberately separate from the internal enum so the public values
 * can stay stable even if the collector adds more internal states. */
static int phase_to_public(int gc_phase)
{
    switch (gc_phase) {
    case GC_PHASE_IDLE:        return MINO_GC_PHASE_IDLE;
    case GC_PHASE_MINOR:       return MINO_GC_PHASE_MINOR;
    case GC_PHASE_MAJOR_MARK:  return MINO_GC_PHASE_MAJOR_MARK;
    case GC_PHASE_MAJOR_SWEEP: return MINO_GC_PHASE_MAJOR_SWEEP;
    default:                   return MINO_GC_PHASE_IDLE;
    }
}

void mino_gc_stats(mino_state_t *S, mino_gc_stats_t *out)
{
    if (S == NULL || out == NULL) {
        return;
    }
    out->collections_minor = S->gc_collections_minor;
    out->collections_major = S->gc_collections_major;
    out->bytes_live        = S->gc_bytes_live;
    out->bytes_young       = S->gc_bytes_young;
    out->bytes_old         = S->gc_bytes_old;
    out->bytes_alloc       = S->gc_bytes_alloc;
    out->bytes_freed       = S->gc_total_freed;
    out->total_gc_ns       = S->gc_total_ns;
    out->max_gc_ns         = S->gc_max_ns;
    out->minor_mark_ns     = S->gc_minor_mark_ns;
    out->minor_sweep_ns    = S->gc_minor_sweep_ns;
    out->major_mark_ns     = S->gc_major_mark_ns;
    out->major_sweep_ns    = S->gc_major_sweep_ns;
    out->root_scan_ns      = S->gc_root_scan_ns;
    out->barrier_satb_pushes     = S->gc_barrier_satb_pushes;
    out->barrier_dijkstra_pushes = S->gc_barrier_dijkstra_pushes;
    out->mark_stack_overflows    = S->gc_mark_stack_overflows;
    out->bytes_promoted_minor    = S->gc_bytes_promoted_minor;
    {
        size_t i;
        for (i = 0; i < 8; i++) {
            out->young_age_bucket[i] = S->gc_young_age_bucket[i];
        }
        for (i = 0; i < 16; i++) {
            out->alloc_by_tag[i] = (i < (size_t)GC_T__COUNT)
                                   ? S->gc_alloc_by_tag[i] : 0;
        }
    }
    out->remset_entries    = S->gc_remset_len;
    out->remset_cap        = S->gc_remset_cap;
    out->remset_high_water = S->gc_remset_high_water;
    out->mark_stack_cap        = S->gc_mark_stack_cap;
    out->mark_stack_high_water = S->gc_mark_stack_high_water;
    out->phase             = phase_to_public(S->gc_phase);
}

/* qsort comparator on uint32_t in ascending order. */
static int u32_cmp(const void *a, const void *b)
{
    uint32_t ua = *(const uint32_t *)a;
    uint32_t ub = *(const uint32_t *)b;
    if (ua < ub) return -1;
    if (ua > ub) return  1;
    return 0;
}

void mino_gc_stats_pauses(mino_state_t *S,
                          uint64_t *out_p50_ns,
                          uint64_t *out_p95_ns,
                          uint64_t *out_p99_ns,
                          uint64_t *out_max_ns)
{
    uint32_t buf[256];
    unsigned n, i;
    if (S == NULL) {
        if (out_p50_ns != NULL) *out_p50_ns = 0;
        if (out_p95_ns != NULL) *out_p95_ns = 0;
        if (out_p99_ns != NULL) *out_p99_ns = 0;
        if (out_max_ns != NULL) *out_max_ns = 0;
        return;
    }
    n = S->gc_pause_ring_count;
    if (n > 256u) n = 256u;
    for (i = 0; i < n; i++) {
        buf[i] = S->gc_pause_ring[i];
    }
    if (n == 0u) {
        if (out_p50_ns != NULL) *out_p50_ns = 0;
        if (out_p95_ns != NULL) *out_p95_ns = 0;
        if (out_p99_ns != NULL) *out_p99_ns = 0;
        if (out_max_ns != NULL) *out_max_ns = 0;
        return;
    }
    qsort(buf, n, sizeof(buf[0]), u32_cmp);
    /* Nearest-rank percentile: ceil(p/100 * n) - 1. */
    if (out_p50_ns != NULL) {
        unsigned idx = (50u * n + 99u) / 100u;
        if (idx == 0u) idx = 1u;
        *out_p50_ns = (uint64_t)buf[idx - 1u];
    }
    if (out_p95_ns != NULL) {
        unsigned idx = (95u * n + 99u) / 100u;
        if (idx == 0u) idx = 1u;
        *out_p95_ns = (uint64_t)buf[idx - 1u];
    }
    if (out_p99_ns != NULL) {
        unsigned idx = (99u * n + 99u) / 100u;
        if (idx == 0u) idx = 1u;
        *out_p99_ns = (uint64_t)buf[idx - 1u];
    }
    if (out_max_ns != NULL) {
        *out_max_ns = (uint64_t)buf[n - 1u];
    }
}

void mino_gc_pause_hist(mino_state_t *S,
                        uint32_t out_buckets[24],
                        unsigned *out_count)
{
    size_t i;
    if (S == NULL || out_buckets == NULL) {
        if (out_count != NULL) *out_count = 0;
        return;
    }
    for (i = 0; i < 24; i++) {
        out_buckets[i] = S->gc_pause_hist[i];
    }
    if (out_count != NULL) {
        *out_count = S->gc_pause_ring_count;
    }
}

/* CPU sampler dump. Aggregates the current ring's samples by
 * (fn, pc, op) tuple and writes one line per tuple to `out`, sorted
 * by sample count descending. Lines look like:
 *   samples=N fn=<file:line> pc=<P> op=<op-name> [native]
 *
 * Returns the number of distinct (fn, pc) pairs written. No-op when
 * the ring is empty.
 *
 * Aggregation uses a fixed-size open-addressed scratch table on the
 * stack so the dump avoids any heap allocation; entries beyond
 * SAMPLER_DUMP_TABLE_MAX (the cap) are folded into a single residual
 * bucket. */
typedef struct sampler_agg {
    const mino_bc_fn_t *bc;
    uint32_t            pc;
    uint16_t            op;
    uint32_t            count;
    uint32_t            native_count;  /* subset of count where flags & 1 */
} sampler_agg_t;

#define SAMPLER_DUMP_TABLE_MAX 4096

static int sampler_agg_cmp(const void *a, const void *b)
{
    const sampler_agg_t *aa = (const sampler_agg_t *)a;
    const sampler_agg_t *bb = (const sampler_agg_t *)b;
    /* Descending count. */
    if (aa->count > bb->count) return -1;
    if (aa->count < bb->count) return  1;
    return 0;
}

unsigned mino_sampler_dump(mino_state_t *S, FILE *out)
{
    static sampler_agg_t agg[SAMPLER_DUMP_TABLE_MAX];
    unsigned             n_agg = 0;
    unsigned             i;
    unsigned             ring_n;
    if (S == NULL || out == NULL) return 0;
    if (S->sampler_ring == NULL || S->sampler_ring_count == 0) return 0;
    ring_n = S->sampler_ring_count;
    for (i = 0; i < ring_n; i++) {
        const mino_sample_t *s = &S->sampler_ring[i];
        unsigned j, found = 0;
        for (j = 0; j < n_agg; j++) {
            if (agg[j].bc == s->bc && agg[j].pc == s->pc) {
                agg[j].count++;
                if (s->flags & 1u) agg[j].native_count++;
                found = 1;
                break;
            }
        }
        if (!found && n_agg < SAMPLER_DUMP_TABLE_MAX) {
            agg[n_agg].bc           = s->bc;
            agg[n_agg].pc           = s->pc;
            agg[n_agg].op           = s->op;
            agg[n_agg].count        = 1u;
            agg[n_agg].native_count = (s->flags & 1u) ? 1u : 0u;
            n_agg++;
        }
    }
    qsort(agg, n_agg, sizeof(agg[0]), sampler_agg_cmp);
    fprintf(out, "[sampler] %u samples over %u distinct PCs\n",
            ring_n, n_agg);
    for (i = 0; i < n_agg; i++) {
        fprintf(out, "  samples=%u  native=%u  fn=%p  pc=%u  op=%u\n",
                agg[i].count, agg[i].native_count,
                (void *)agg[i].bc, agg[i].pc, agg[i].op);
    }
    return n_agg;
}

/* Allocation-site sampler dump. Aggregates the ring's samples by
 * (site, tag, size_bucket) triple and writes top entries to `out`
 * sorted by count descending. Returns the number of distinct triples
 * written. */
typedef struct alloc_agg {
    const void *site;
    uint8_t     tag;
    uint8_t     size_bucket;
    uint32_t    count;
} alloc_agg_t;

static int alloc_agg_cmp(const void *a, const void *b)
{
    const alloc_agg_t *aa = (const alloc_agg_t *)a;
    const alloc_agg_t *bb = (const alloc_agg_t *)b;
    if (aa->count > bb->count) return -1;
    if (aa->count < bb->count) return  1;
    return 0;
}

unsigned mino_alloc_sampler_dump(mino_state_t *S, FILE *out)
{
    static alloc_agg_t agg[1024];
    unsigned           n_agg = 0;
    unsigned           i;
    unsigned           ring_n;
    if (S == NULL || out == NULL) return 0;
    if (S->alloc_sampler_ring == NULL || S->alloc_sampler_ring_count == 0)
        return 0;
    ring_n = S->alloc_sampler_ring_count;
    for (i = 0; i < ring_n; i++) {
        const mino_alloc_sample_t *s = &S->alloc_sampler_ring[i];
        unsigned j, found = 0;
        for (j = 0; j < n_agg; j++) {
            if (agg[j].site == s->site && agg[j].tag == s->tag
                && agg[j].size_bucket == s->size_bucket) {
                agg[j].count++;
                found = 1;
                break;
            }
        }
        if (!found && n_agg < 1024u) {
            agg[n_agg].site        = s->site;
            agg[n_agg].tag         = s->tag;
            agg[n_agg].size_bucket = s->size_bucket;
            agg[n_agg].count       = 1u;
            n_agg++;
        }
    }
    qsort(agg, n_agg, sizeof(agg[0]), alloc_agg_cmp);
    fprintf(out, "[alloc-sampler] %u samples over %u distinct sites\n",
            ring_n, n_agg);
    for (i = 0; i < n_agg; i++) {
        fprintf(out,
                "  samples=%u  site=%p  tag=%u  size-bucket=%u\n",
                agg[i].count, agg[i].site,
                (unsigned)agg[i].tag, (unsigned)agg[i].size_bucket);
    }
    return n_agg;
}
