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

#include "mino_internal.h"

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

void *gc_alloc_typed(mino_state_t *S, unsigned char tag, size_t size)
{
    gc_hdr_t *h;
    int fc;
    if (S->gc_stress == -1) {
        const char *e = getenv("MINO_GC_STRESS");
        S->gc_stress = (e != NULL && e[0] != '\0' && e[0] != '0') ? 1 : 0;
    }
    if (S->gc_depth == 0 && S->gc_stack_bottom != NULL
        && (S->gc_stress || S->gc_bytes_alloc - S->gc_bytes_live > S->gc_threshold)) {
        gc_major_collect(S);
    }
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
    h->next          = S->gc_all;
    S->gc_all           = h;
    S->gc_bytes_alloc  += size;
    S->gc_bytes_young  += size;
    if (S->gc_stress > 0) {
        gc_range_insert(S, h);
    } else {
        S->gc_ranges_valid = 0;
    }
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

void gc_mark_push(mino_state_t *S, gc_hdr_t *h)
{
    if (h == NULL || h->mark) return;
    /* Generational filter: during minor marking we never trace into or
     * mark OLD headers. The write barrier guarantees every OLD-to-YOUNG
     * reference is reachable through the remembered set; other OLD
     * objects live by definition across minor cycles. */
    if (S->gc_phase == GC_PHASE_MINOR && h->gen == GC_GEN_OLD) return;
    h->mark = 1;
    if (S->gc_mark_stack_len == S->gc_mark_stack_cap) {
        size_t new_cap = S->gc_mark_stack_cap == 0
            ? GC_MARK_STACK_INIT : S->gc_mark_stack_cap * 2;
        gc_hdr_t **ns = (gc_hdr_t **)realloc(
            S->gc_mark_stack, new_cap * sizeof(*ns));
        if (ns == NULL) return;  /* OOM: best effort */
        S->gc_mark_stack = ns;
        S->gc_mark_stack_cap = new_cap;
    }
    S->gc_mark_stack[S->gc_mark_stack_len++] = h;
}

/* Resolve an interior pointer and push its header onto the mark stack. */
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

/* Trace every GC-managed pointer held by h, pushing each target into
 * the mark stack. Used from gc_drain_mark_stack on any header popped
 * for tracing, and directly from the minor collector when seeding the
 * mark stack from remembered-set entries. */
void gc_trace_children(mino_state_t *S, gc_hdr_t *h)
{
    switch (h->type_tag) {
    case GC_T_VAL: {
        mino_val_t *v = (mino_val_t *)(h + 1);
        gc_mark_interior_push(S, v->meta);
        switch (v->type) {
        case MINO_STRING:
        case MINO_SYMBOL:
        case MINO_KEYWORD:
            gc_mark_interior_push(S, v->as.s.data);
            break;
        case MINO_CONS:
            gc_mark_interior_push(S, v->as.cons.car);
            gc_mark_interior_push(S, v->as.cons.cdr);
            break;
        case MINO_VECTOR:
            gc_mark_interior_push(S, v->as.vec.root);
            gc_mark_interior_push(S, v->as.vec.tail);
            break;
        case MINO_MAP:
            gc_mark_interior_push(S, v->as.map.root);
            gc_mark_interior_push(S, v->as.map.key_order);
            break;
        case MINO_SET:
            gc_mark_interior_push(S, v->as.set.root);
            gc_mark_interior_push(S, v->as.set.key_order);
            break;
        case MINO_SORTED_MAP:
        case MINO_SORTED_SET:
            gc_mark_interior_push(S, v->as.sorted.root);
            gc_mark_interior_push(S, v->as.sorted.comparator);
            break;
        case MINO_FN:
        case MINO_MACRO:
            gc_mark_interior_push(S, v->as.fn.params);
            gc_mark_interior_push(S, v->as.fn.body);
            gc_mark_interior_push(S, v->as.fn.env);
            break;
        case MINO_ATOM:
            gc_mark_interior_push(S, v->as.atom.val);
            gc_mark_interior_push(S, v->as.atom.watches);
            gc_mark_interior_push(S, v->as.atom.validator);
            break;
        case MINO_LAZY:
            if (v->as.lazy.realized) {
                gc_mark_interior_push(S, v->as.lazy.cached);
            } else {
                gc_mark_interior_push(S, v->as.lazy.body);
                gc_mark_interior_push(S, v->as.lazy.env);
            }
            break;
        case MINO_RECUR:
            gc_mark_interior_push(S, v->as.recur.args);
            break;
        case MINO_TAIL_CALL:
            gc_mark_interior_push(S, v->as.tail_call.fn);
            gc_mark_interior_push(S, v->as.tail_call.args);
            break;
        case MINO_REDUCED:
            gc_mark_interior_push(S, v->as.reduced.val);
            break;
        case MINO_VAR:
            gc_mark_interior_push(S, v->as.var.root);
            break;
        default:
            break;
        }
        break;
    }
    case GC_T_ENV: {
        mino_env_t *env = (mino_env_t *)(h + 1);
        size_t i;
        gc_mark_interior_push(S, env->parent);
        if (env->bindings != NULL) {
            gc_mark_interior_push(S, env->bindings);
            for (i = 0; i < env->len; i++) {
                gc_mark_interior_push(S, env->bindings[i].name);
                gc_mark_interior_push(S, env->bindings[i].val);
            }
        }
        gc_mark_interior_push(S, env->ht_buckets);
        break;
    }
    case GC_T_VEC_NODE: {
        mino_vec_node_t *n = (mino_vec_node_t *)(h + 1);
        unsigned i;
        for (i = 0; i < n->count; i++) {
            gc_mark_interior_push(S, n->slots[i]);
        }
        break;
    }
    case GC_T_HAMT_NODE: {
        mino_hamt_node_t *n = (mino_hamt_node_t *)(h + 1);
        unsigned count, i;
        gc_mark_interior_push(S, n->slots);
        count = (n->collision_count > 0) ? n->collision_count
                                         : popcount32(n->bitmap);
        if (n->slots != NULL) {
            for (i = 0; i < count; i++) {
                gc_mark_interior_push(S, n->slots[i]);
            }
        }
        break;
    }
    case GC_T_HAMT_ENTRY: {
        hamt_entry_t *e = (hamt_entry_t *)(h + 1);
        gc_mark_interior_push(S, e->key);
        gc_mark_interior_push(S, e->val);
        break;
    }
    case GC_T_VALARR:
    case GC_T_PTRARR: {
        void **arr = (void **)(h + 1);
        size_t n = h->size / sizeof(*arr);
        size_t i;
        for (i = 0; i < n; i++) {
            gc_mark_interior_push(S, arr[i]);
        }
        break;
    }
    case GC_T_RB_NODE: {
        mino_rb_node_t *rb = (mino_rb_node_t *)(h + 1);
        gc_mark_interior_push(S, rb->key);
        gc_mark_interior_push(S, rb->val);
        gc_mark_interior_push(S, rb->left);
        gc_mark_interior_push(S, rb->right);
        break;
    }
    case GC_T_RAW:
    default:
        break;
    }
}

/* Drain the mark stack: pop headers and trace their children until empty. */
void gc_drain_mark_stack(mino_state_t *S)
{
    while (S->gc_mark_stack_len > 0) {
        gc_hdr_t *h = S->gc_mark_stack[--S->gc_mark_stack_len];
        gc_trace_children(S, h);
    }
}

/* ------------------------------------------------------------------------- */
/* Collection driver.                                                        */
/* ------------------------------------------------------------------------- */
/*
 * Three root sources are consulted every cycle: the registered user envs
 * and other pinned state enumerated in gc_mark_roots, plus a conservative
 * scan of the C stack between gc_stack_bottom (saved on the first
 * mino_eval call) and the collector's own frame. A sorted range index
 * over the live allocation list lets the conservative scan resolve each
 * aligned machine word to its owning header in O(log n).
 *
 * Sweep walks the registry in place, frees every allocation whose mark
 * bit is clear, and resets the mark bit on survivors. The live-byte
 * tally becomes the next cycle's baseline; the threshold grows to 2x
 * live so the collector's amortized cost stays bounded under
 * steady-state programs.
 */

void gc_major_collect(mino_state_t *S)
{
    jmp_buf jb;
    long long start_ns;
    size_t    elapsed_ns;
    if (S->gc_depth > 0) {
        return;
    }
    S->gc_depth++;
    S->gc_phase = GC_PHASE_MAJOR;
    start_ns = mino_monotonic_ns();
    /* setjmp spills callee-saved registers into jb, which lives in this
     * stack frame. gc_scan_stack scans from a deeper frame up through ours,
     * so any pointer that was register-resident at entry is visible. */
    if (setjmp(jb) != 0) {
        /* Class I: we never longjmp here; a nonzero return indicates
         * corruption or an unexpected jump. */
        abort();
    }
    (void)jb;
    if (!S->gc_ranges_valid) {
        gc_build_range_index(S);
    }
    gc_mark_roots(S);
    gc_drain_mark_stack(S);
    gc_scan_stack(S);
    gc_drain_mark_stack(S);
    gc_range_compact(S);
    gc_sweep(S);
    gc_remset_reset(S);
    S->gc_collections_major++;
    S->gc_phase = GC_PHASE_IDLE;
    elapsed_ns = (size_t)(mino_monotonic_ns() - start_ns);
    S->gc_total_ns += elapsed_ns;
    if (elapsed_ns > S->gc_max_ns) {
        S->gc_max_ns = elapsed_ns;
    }
    S->gc_depth--;
}
