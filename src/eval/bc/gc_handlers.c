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
#include "eval/bc/jit.h"

void gc_mark_child_push_exported(mino_state *S, const void *p);

#define PUSH(p) gc_mark_child_push_exported(S, (p))

/* Walk a bc fn's IC slot array: push the buffer itself plus every
 * embedded value pointer. The slot array is a GC_T_RAW POD buffer
 * whose sym / cached / atom / cached_map / cached_type fields the
 * GC can't see without an explicit walk; this helper centralises
 * the slot-kind -> field mapping so the MINO_FN walker and the
 * GC_T_BC walker can't drift. */
void mino_bc_trace_ic_slots(mino_state *S, const struct mino_bc_fn *bc)
{
    int i;
    if (bc == NULL || bc->ic_slots == NULL || bc->ic_slots_len <= 0) return;
    PUSH(bc->ic_slots);
    for (i = 0; i < bc->ic_slots_len; i++) {
        PUSH(bc->ic_slots[i].sym);
        PUSH(bc->ic_slots[i].cached);
        PUSH(bc->ic_slots[i].cached_bc);
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
static void trace_bc(mino_state *S, gc_hdr_t *h)
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

/* Walk a MINO_FN's bc record (and its sub-buffers) when present.
 * The values-side trace_val tracer delegates here so it doesn't
 * have to know struct mino_bc_fn's layout. Compiled bytecode: the
 * bc record and its code/consts/clauses buffers are separate GC
 * allocations reachable only through MINO_FN.bc, so each one needs
 * an explicit push. The const pool is GC_T_VALARR so the GC's
 * tag-walk scans its slots automatically once the buffer itself
 * is marked. */
void mino_bc_trace_fn_bc(mino_state *S, const void *bc_ptr)
{
    const struct mino_bc_fn *bc = (const struct mino_bc_fn *)bc_ptr;
    int i;
    if (bc == NULL || bc == &mino_bc_declined) return;
    PUSH(bc);
    PUSH(bc->code);
    PUSH(bc->consts);
    PUSH(bc->clauses);
    /* Clause params_vec: when the compiler rewrites a destructuring
     * param list it mints a fresh gensym vector and stores it on the
     * clause. The original (pre-rewrite) params vector still hangs
     * off fn.params, but the gensym vector is reachable ONLY via
     * clauses[i].params_vec. clauses is GC_T_RAW (POD), so the GC's
     * tag-walk can't see its embedded value pointers -- push them
     * explicitly here so gensym slots survive the next major cycle. */
    if (bc->clauses != NULL && bc->n_clauses > 0) {
        for (i = 0; i < bc->n_clauses; i++) {
            PUSH(bc->clauses[i].params_vec);
        }
    }
    mino_bc_trace_ic_slots(S, bc);
    /* Optional ic_stats POD buffer (MINO_JIT_IC_STATS=1). */
    PUSH(bc->ic_stats);
}

/* A dead bc record may still be referenced by the MINO_CPJIT_STATS
 * ring (the atexit dump pulls end-of-run counters through it). Seal
 * the matching entries before the sweep frees the record's memory.
 * No-op unless the env var enabled the ring. */
static void finalize_bc(mino_state *S, gc_hdr_t *h)
{
    (void)S;
    mino_jit_stats_seal_bc((const struct mino_bc_fn *)(h + 1));
}

void mino_bc_register_gc_handlers(mino_state *S)
{
    gc_register_tracer(S, GC_T_BC, trace_bc);
    gc_register_finalizer(S, GC_T_BC, finalize_bc);
}
