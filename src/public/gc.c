/*
 * public_gc.c -- host-facing garbage collector control.
 *
 * Thin wrappers over the internal collector: host asks for a cheap
 * minor, a cycle-closing major, or a full STW reclamation; host reads
 * tuning knobs via one enum-tagged setter; host queries counters via
 * one struct-out getter. All three calls are safe at any GC phase.
 */

#include "mino.h"
#include "runtime/internal.h"

void mino_gc_collect(mino_state_t *S, mino_gc_kind_t kind)
{
    if (S == NULL || S->ctx->gc_depth > 0) {
        return;
    }
    switch (kind) {
    case MINO_GC_MINOR:
        if (S->ctx->gc_stack_bottom != NULL) {
            gc_minor_collect(S);
        }
        return;
    case MINO_GC_MAJOR:
        if (S->ctx->gc_stack_bottom == NULL) {
            return;
        }
        if (S->gc_phase == GC_PHASE_MAJOR_MARK) {
            gc_force_finish_major(S);
        } else if (S->gc_phase == GC_PHASE_IDLE) {
            gc_major_collect(S);
        }
        return;
    case MINO_GC_FULL:
        if (S->ctx->gc_stack_bottom == NULL) {
            return;
        }
        if (S->gc_bytes_young > 0) {
            gc_minor_collect(S);
        }
        if (S->gc_phase == GC_PHASE_MAJOR_MARK) {
            gc_force_finish_major(S);
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
    out->remset_entries    = S->gc_remset_len;
    out->remset_cap        = S->gc_remset_cap;
    out->remset_high_water = S->gc_remset_high_water;
    out->mark_stack_cap        = S->gc_mark_stack_cap;
    out->mark_stack_high_water = S->gc_mark_stack_high_water;
    out->phase             = phase_to_public(S->gc_phase);
}
