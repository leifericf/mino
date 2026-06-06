/*
 * values/gc_handlers.c -- per-tag GC tracer registration for the
 * GC_T_VAL layout owned by the values component.
 *
 * trace_val knows the struct mino_val union body and pushes the
 * GC-owned pointers under every type tag. Field accesses for fields
 * that point into downstream-component structs (the bc record under
 * MINO_FN, the future impl under MINO_FUTURE) are delegated to
 * component-owned helpers (mino_bc_trace_fn_bc, mino_future_trace_impl)
 * so values/ never has to import struct mino_bc_fn or
 * struct mino_future layouts.
 *
 * Registered from runtime/state.c::state_init.
 */

#include "mino_internal.h"
#include "values/layout.h"
#include "values/internal.h"
#include "gc/internal.h"
#include "runtime/value_assert.h"   /* mino_type_of */
#include "eval/bc/internal.h"        /* mino_bc_trace_fn_bc */
#include "runtime/host_threads.h"    /* mino_future_trace_impl, mino_future_gc_sweep */
#include "collections/internal.h"    /* mino_bigint_free (collections-adjacent bignum) */
#include "async/chan.h"              /* mino_chan_trace, mino_chan_finalize */

#include <stdlib.h>                  /* free() for malloc-owned slot arrays */

void gc_mark_child_push_exported(mino_state *S, const void *p);

#define PUSH(p) gc_mark_child_push_exported(S, (p))

static void trace_val(mino_state *S, gc_hdr_t *h)
{
    mino_val *v = (mino_val *)(h + 1);
    PUSH(v->meta);
    switch (mino_type_of(v)) {
    case MINO_STRING:
    case MINO_SYMBOL:
    case MINO_KEYWORD:
        PUSH(v->as.s.data);
        break;
    case MINO_CONS:
        PUSH(v->as.cons.car);
        PUSH(v->as.cons.cdr);
        break;
    case MINO_VECTOR:
        PUSH(v->as.vec.root);
        PUSH(v->as.vec.tail);
        break;
    case MINO_MAP:
        PUSH(v->as.map.root);
        PUSH(v->as.map.key_order);
        PUSH(v->as.map.val_order);
        break;
    case MINO_SET:
        PUSH(v->as.set.root);
        PUSH(v->as.set.key_order);
        break;
    case MINO_SORTED_MAP:
    case MINO_SORTED_SET:
        PUSH(v->as.sorted.root);
        PUSH(v->as.sorted.comparator);
        break;
    case MINO_FN:
    case MINO_MACRO:
        PUSH(v->as.fn.params);
        PUSH(v->as.fn.body);
        PUSH(v->as.fn.env);
        PUSH(v->as.fn.template_fn);
        /* The bc record under fn.bc holds its own GC-owned buffers
         * (code / consts / clauses / ic_slots). eval/bc/ owns the
         * bc layout; delegate the walk so values/ stays decoupled. */
        mino_bc_trace_fn_bc(S, v->as.fn.bc);
        break;
    case MINO_ATOM:
        PUSH(v->as.atom.val);
        PUSH(v->as.atom.watches);
        PUSH(v->as.atom.validator);
        break;
    case MINO_VOLATILE:
        PUSH(v->as.volatile_.val);
        break;
    case MINO_CHUNK: {
        unsigned k;
        for (k = 0; k < v->as.chunk.len; k++) {
            PUSH(v->as.chunk.vals[k]);
        }
        break;
    }
    case MINO_HOST_ARRAY: {
        size_t k;
        for (k = 0; k < v->as.host_array.len; k++) {
            PUSH(v->as.host_array.vals[k]);
        }
        break;
    }
    case MINO_MAP_ENTRY:
        PUSH(v->as.map_entry.k);
        PUSH(v->as.map_entry.v);
        break;
    case MINO_CHUNKED_CONS:
        PUSH(v->as.chunked_cons.chunk);
        PUSH(v->as.chunked_cons.more);
        break;
    case MINO_LAZY:
        /* LAZY_REALIZING is treated like LAZY_UNREALIZED: the realizer
         * has not yet published cached, so body+env still hold the
         * live closure and must keep being traced. The barrier-paired
         * stores in lazy_realize publish cached before flipping the
         * flag to LAZY_REALIZED, so a tracer that observes REALIZED
         * also observes the populated cached slot. */
        if (v->as.lazy.realized == LAZY_REALIZED) {
            PUSH(v->as.lazy.cached);
        } else {
            PUSH(v->as.lazy.body);
            PUSH(v->as.lazy.env);
        }
        break;
    case MINO_RECUR:
        PUSH(v->as.recur.args);
        break;
    case MINO_TAIL_CALL:
        PUSH(v->as.tail_call.fn);
        PUSH(v->as.tail_call.args);
        break;
    case MINO_REDUCED:
        PUSH(v->as.reduced.val);
        break;
    case MINO_VAR:
        PUSH(v->as.var.root);
        PUSH(v->as.var.watches);
        PUSH(v->as.var.validator);
        break;
    case MINO_TRANSIENT:
        PUSH(v->as.transient.current);
        break;
    case MINO_RATIO:
        PUSH(v->as.ratio.num);
        PUSH(v->as.ratio.denom);
        break;
    case MINO_BIGDEC:
        PUSH(v->as.bigdec.unscaled);
        break;
    case MINO_TYPE:
        PUSH(v->as.record_type.fields);
        break;
    case MINO_RECORD: {
        size_t i, n;
        PUSH(v->as.record.type);
        PUSH(v->as.record.ext);
        n = (v->as.record.type->as.record_type.fields != NULL)
            ? v->as.record.type->as.record_type.fields->as.vec.len : 0;
        for (i = 0; i < n; i++) {
            PUSH(v->as.record.vals[i]);
        }
        break;
    }
    case MINO_FUTURE:
        /* The impl struct is malloc-owned and lives in runtime/;
         * delegate the slot walk so values/ stays decoupled. */
        mino_future_trace_impl(S, v);
        break;
    case MINO_REGEX:
        PUSH(v->as.regex.source);
        break;
    case MINO_TX_REF:
        PUSH(v->as.tx_ref.val);
        PUSH(v->as.tx_ref.watches);
        PUSH(v->as.tx_ref.validator);
        break;
    case MINO_AGENT:
        PUSH(v->as.agent.val);
        PUSH(v->as.agent.watches);
        PUSH(v->as.agent.validator);
        PUSH(v->as.agent.err);
        PUSH(v->as.agent.err_handler);
        break;
    case MINO_CHAN:
        mino_chan_trace(S, v);
        break;
    case MINO_BYTES:
        /* The data buffer is malloc-owned raw bytes -- no internal
         * pointers to follow. Nothing to push. */
        break;
    default:
        break;
    }
}

/* Per-type cleanup for a dying GC_T_VAL header. Called from
 * gc_minor_collect (dead YOUNG) and gc_major_sweep_phase (dead OLD)
 * via S->gc_finalizers[GC_T_VAL] right before the header is unlinked
 * and recycled. MINO_HANDLE invokes the embedder-supplied finalizer;
 * MINO_BIGINT releases the imath payload; MINO_RECORD / MINO_CHUNK /
 * MINO_HOST_ARRAY free the malloc-owned slot arrays; MINO_FUTURE
 * tears down the worker thread, mu/cv, and impl struct. All other
 * types have nothing external to release. */
static void finalize_val_impl(mino_state *S, gc_hdr_t *h, int teardown)
{
    mino_val *v = (mino_val *)(h + 1);
    (void)S;
    switch (mino_type_of(v)) {
    case MINO_HANDLE:
        if (v->as.handle.finalizer != NULL) {
            v->as.handle.finalizer(v->as.handle.ptr, v->as.handle.tag);
        }
        break;
    case MINO_BIGINT:
        mino_bigint_free(v);
        break;
    case MINO_RECORD:
        free(v->as.record.vals);
        v->as.record.vals = NULL;
        break;
    case MINO_CHUNK:
        free(v->as.chunk.vals);
        v->as.chunk.vals = NULL;
        break;
    case MINO_HOST_ARRAY:
        free(v->as.host_array.vals);
        v->as.host_array.vals = NULL;
        break;
    case MINO_FUTURE:
        /* Sweep-time collection joins the worker and destroys the
         * cv/mu; at state teardown the quiesce pass has already
         * joined everything, so only the impl shell is released. */
        if (teardown) mino_future_teardown_free(v);
        else          mino_future_gc_sweep(v);
        break;
    case MINO_CHAN:
        mino_chan_finalize(S, v);
        break;
    case MINO_BYTES:
        free(v->as.bytes.data);
        v->as.bytes.data = NULL;
        break;
    default:
        break;
    }
}

static void finalize_val(mino_state *S, gc_hdr_t *h)
{
    finalize_val_impl(S, h, 0);
}

void mino_val_finalize_teardown(mino_state *S, gc_hdr_t *h)
{
    finalize_val_impl(S, h, 1);
}

void mino_values_register_gc_handlers(mino_state *S)
{
    gc_register_tracer(S, GC_T_VAL, trace_val);
    gc_register_finalizer(S, GC_T_VAL, finalize_val);
}
