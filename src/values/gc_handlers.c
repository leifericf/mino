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
#include "runtime/host_threads.h"    /* mino_future_trace_impl */

void gc_mark_child_push_exported(mino_state_t *S, const void *p);

#define PUSH(p) gc_mark_child_push_exported(S, (p))

static void trace_val(mino_state_t *S, gc_hdr_t *h)
{
    mino_val_t *v = (mino_val_t *)(h + 1);
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
        if (v->as.lazy.realized) {
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
    default:
        break;
    }
}

void mino_values_register_gc_handlers(mino_state_t *S)
{
    gc_register_tracer(S, GC_T_VAL, trace_val);
}
