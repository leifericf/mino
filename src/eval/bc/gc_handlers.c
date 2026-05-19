/*
 * eval/bc/gc_handlers.c -- per-tag GC tracer registration for the
 * GC_T_BC bc-fn layout owned by the eval/bc subsystem.
 *
 * mino_bc_trace_ic_slots is exported so the MINO_FN branch of the
 * values-side GC_T_VAL tracer can walk the bc record's inline-cache
 * slots without re-implementing the slot-kind dispatch.
 *
 * Registered from runtime/state.c::state_init.
 */

#include "mino_internal.h"
#include "eval/bc/internal.h"
#include "gc/internal.h"

void gc_mark_child_push_exported(mino_state_t *S, const void *p);

#define PUSH(p) gc_mark_child_push_exported(S, (p))

/* Walk a bc fn's IC slot array: push the buffer itself plus every
 * embedded value pointer. The slot array is a GC_T_RAW POD buffer
 * whose sym / cached / atom / cached_map / cached_type fields the
 * GC can't see without an explicit walk; this helper centralises
 * the slot-kind -> field mapping so the MINO_FN walker and the
 * GC_T_BC walker can't drift. */
void mino_bc_trace_ic_slots(mino_state_t *S, const struct mino_bc_fn *bc)
{
    int i;
    if (bc == NULL || bc->ic_slots == NULL || bc->ic_slots_len <= 0) return;
    PUSH(bc->ic_slots);
    for (i = 0; i < bc->ic_slots_len; i++) {
        PUSH(bc->ic_slots[i].sym);
        PUSH(bc->ic_slots[i].cached);
        if (bc->ic_slots[i].kind == MINO_BC_IC_PROTOCOL) {
            PUSH(bc->ic_slots[i].atom);
            PUSH(bc->ic_slots[i].cached_map);
            PUSH(bc->ic_slots[i].cached_type);
        }
    }
}

/* The bc record carries pointers to its code, consts, clauses, and
 * ic_slots buffers. These are normally reached via MINO_FN's trace,
 * but a write barrier on the bc record (e.g. ensure_code growing the
 * code buffer) adds the bc directly to the remset, and minor mark
 * then needs to push its YOUNG children itself. Without this case
 * the bc lives but its buffers are silently swept. */
static void trace_bc(mino_state_t *S, gc_hdr_t *h)
{
    struct mino_bc_fn *bc = (struct mino_bc_fn *)(h + 1);
    PUSH(bc->code);
    PUSH(bc->consts);
    PUSH(bc->clauses);
    PUSH(bc->source_map.positions);
    mino_bc_trace_ic_slots(S, bc);
    /* Optional ic_stats POD buffer (allocated only under
     * MINO_JIT_IC_STATS=1). Plain GC_T_RAW counters; no embedded
     * pointers, so the buffer itself is the only thing to mark. */
    PUSH(bc->ic_stats);
}

void mino_bc_register_gc_handlers(mino_state_t *S)
{
    gc_register_tracer(S, GC_T_BC, trace_bc);
}
