/*
 * collections/gc_handlers.c -- per-tag tracer registration for the
 * GC layouts owned by the collections component.
 *
 * gc_trace_children dispatches through S->gc_tracers; the collection
 * node layouts (vec trie, HAMT node, HAMT entry, RB tree node) live
 * here so gc/driver.c never has to import collections/internal.h.
 *
 * Registered from runtime/state.c::state_init before the first
 * allocation.
 */

#include "mino_internal.h"
#include "collections/internal.h"
#include "gc/internal.h"

/* gc_mark_child_push pushes a child pointer onto the mark stack,
 * skipping inline-tagged values, NULL, and singletons inside the
 * state struct. Declared (and defined static) in gc/driver.c; we
 * re-declare its external shim here so each tracer can call it. */
void gc_mark_child_push_exported(mino_state *S, const void *p);

#define PUSH(p) gc_mark_child_push_exported(S, (p))

static void trace_vec_node(mino_state *S, gc_hdr_t *h)
{
    mino_vec_node_t *n = (mino_vec_node_t *)(h + 1);
    unsigned i;
    for (i = 0; i < n->count; i++) {
        PUSH(n->slots[i]);
    }
}

static void trace_hamt_node(mino_state *S, gc_hdr_t *h)
{
    mino_hamt_node_t *n = (mino_hamt_node_t *)(h + 1);
    /* The child pointers live in the out-of-line slots[] array, not
     * inline in the node: pushing the array is enough, its own tracer
     * (trace_pointer_array over the exact-sized allocation) walks every
     * child. Do not also push n->slots[i] directly -- that would name
     * the node as owner of pointers whose physical home is the array,
     * so a per-slot store (barriered against the array in map_owned.c)
     * would leave the node's dirty bit clear and the remembered-set
     * edge unrecorded. One owner per pointer keeps barrier and tracer
     * agreeing. */
    PUSH(n->slots);
}

static void trace_hamt_entry(mino_state *S, gc_hdr_t *h)
{
    hamt_entry_t *e = (hamt_entry_t *)(h + 1);
    PUSH(e->key);
    PUSH(e->val);
}

static void trace_rb_node(mino_state *S, gc_hdr_t *h)
{
    mino_rb_node_t *rb = (mino_rb_node_t *)(h + 1);
    PUSH(rb->key);
    PUSH(rb->val);
    PUSH(rb->left);
    PUSH(rb->right);
}

void mino_collections_register_gc_handlers(mino_state *S)
{
    gc_register_tracer(S, GC_T_VEC_NODE,   trace_vec_node);
    gc_register_tracer(S, GC_T_HAMT_NODE,  trace_hamt_node);
    gc_register_tracer(S, GC_T_HAMT_ENTRY, trace_hamt_entry);
    gc_register_tracer(S, GC_T_RB_NODE,    trace_rb_node);
}
