/*
 * runtime_gc.c -- GC allocation, mark-and-sweep collector, range index.
 *
 * Extracted from mino.c. No behavior change.
 */

#include "mino_internal.h"
#include "async_scheduler.h"
#include "async_timer.h"

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

static void gc_range_insert(mino_state_t *S, gc_hdr_t *h);

void *gc_alloc_typed(mino_state_t *S, unsigned char tag, size_t size)
{
    gc_hdr_t *h;
    if (S->gc_stress == -1) {
        const char *e = getenv("MINO_GC_STRESS");
        S->gc_stress = (e != NULL && e[0] != '\0' && e[0] != '0') ? 1 : 0;
    }
    if (S->gc_depth == 0 && S->gc_stack_bottom != NULL
        && (S->gc_stress || S->gc_bytes_alloc - S->gc_bytes_live > S->gc_threshold)) {
        gc_collect(S);
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
    h->type_tag      = tag;
    h->mark          = 0;
    h->size          = size;
    h->next          = S->gc_all;
    S->gc_all           = h;
    S->gc_bytes_alloc  += size;
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
/* Garbage collector: mark-and-sweep                                         */
/* ------------------------------------------------------------------------- */
/*
 * Marking is driven by three root sources: the registry of user-held envs
 * (`gc_root_envs`), the symbol and keyword intern tables, and a conservative
 * scan of the C stack between `gc_stack_bottom` (saved on the first
 * mino_eval call) and the collector's own frame.
 *
 * The marker traces each object according to its type tag, following every
 * owned pointer it knows about. Conservative stack scan treats every aligned
 * machine word as a candidate pointer and consults a sorted range index
 * built from the allocation registry to check, in O(log n), whether the
 * word falls inside any managed payload. Interior pointers (into the middle
 * of a payload) count; this keeps the scan correct under normal compiler
 * optimizations that may retain base+offset forms in registers.
 *
 * Sweep walks the registry in place, freeing every allocation whose mark
 * bit is clear and resetting the mark bit on survivors. The live-byte tally
 * becomes the next cycle's baseline; the threshold grows to 2x live so the
 * collector's amortized cost stays bounded under steady-state programs.
 */

static int gc_range_cmp(const void *a, const void *b)
{
    const gc_range_t *ra = (const gc_range_t *)a;
    const gc_range_t *rb = (const gc_range_t *)b;
    if (ra->start < rb->start) return -1;
    if (ra->start > rb->start) return 1;
    return 0;
}

static void gc_build_range_index(mino_state_t *S)
{
    gc_hdr_t *h;
    size_t    n = 0;
    for (h = S->gc_all; h != NULL; h = h->next) {
        n++;
    }
    if (n > S->gc_ranges_cap) {
        size_t      new_cap = n * 2 + 16;
        gc_range_t *nr      = (gc_range_t *)realloc(
            S->gc_ranges, new_cap * sizeof(*nr));
        if (nr == NULL) {
            abort(); /* Class I: inside GC; no safe recovery path */
        }
        S->gc_ranges     = nr;
        S->gc_ranges_cap = new_cap;
    }
    S->gc_ranges_len = 0;
    for (h = S->gc_all; h != NULL; h = h->next) {
        S->gc_ranges[S->gc_ranges_len].start = (uintptr_t)(h + 1);
        S->gc_ranges[S->gc_ranges_len].end   = (uintptr_t)(h + 1) + h->size;
        S->gc_ranges[S->gc_ranges_len].h     = h;
        S->gc_ranges_len++;
    }
    qsort(S->gc_ranges, S->gc_ranges_len, sizeof(*S->gc_ranges), gc_range_cmp);
    S->gc_ranges_valid = 1;
    S->gc_ranges_pending_len = 0;
}

/*
 * Buffer a newly allocated header for the next collection's range index.
 * Instead of doing an O(n) memmove into the sorted main array on every
 * allocation, we append to a small pending buffer.  gc_find_header_for_ptr
 * checks the pending buffer with a linear scan, which is fast for the
 * 1-2 entries that accumulate between stress-mode collections.
 * If the pending buffer fills (normal mode with many allocations between
 * collections), the range index is invalidated and rebuilt from scratch
 * at the next collection.
 */
static void gc_range_insert(mino_state_t *S, gc_hdr_t *h)
{
    gc_range_t entry;

    if (!S->gc_ranges_valid) {
        return;
    }

    if (S->gc_ranges_pending_len >= sizeof(S->gc_ranges_pending) / sizeof(S->gc_ranges_pending[0])) {
        S->gc_ranges_valid = 0;
        return;
    }

    entry.start = (uintptr_t)(h + 1);
    entry.end   = (uintptr_t)(h + 1) + h->size;
    entry.h     = h;
    S->gc_ranges_pending[S->gc_ranges_pending_len] = entry;
    S->gc_ranges_pending_len++;
}

/*
 * Remove entries for unmarked (dead) objects from the range index,
 * then merge in any pending entries from recent allocations.
 * Called after marking but before sweep, while mark bits still indicate
 * liveness.  Preserves sort order.  O(n) in the size of the index.
 */
static void gc_range_compact(mino_state_t *S)
{
    size_t dst = 0;
    size_t src;
    size_t i;
    size_t need;
    if (!S->gc_ranges_valid) {
        return;
    }
    for (src = 0; src < S->gc_ranges_len; src++) {
        if (S->gc_ranges[src].h->mark) {
            S->gc_ranges[dst] = S->gc_ranges[src];
            dst++;
        }
    }
    S->gc_ranges_len = dst;

    /* Merge pending entries (new allocations since last collection). */
    need = S->gc_ranges_len + S->gc_ranges_pending_len;
    if (need > S->gc_ranges_cap) {
        size_t      new_cap = need * 2 + 16;
        gc_range_t *nr      = (gc_range_t *)realloc(
            S->gc_ranges, new_cap * sizeof(*nr));
        if (nr == NULL) {
            abort(); /* Class I: inside GC; no safe recovery path */
        }
        S->gc_ranges     = nr;
        S->gc_ranges_cap = new_cap;
    }
    for (i = 0; i < S->gc_ranges_pending_len; i++) {
        size_t lo = 0;
        size_t hi = S->gc_ranges_len;
        while (lo < hi) {
            size_t mid = lo + (hi - lo) / 2;
            if (S->gc_ranges_pending[i].start < S->gc_ranges[mid].start) {
                hi = mid;
            } else {
                lo = mid + 1;
            }
        }
        if (lo < S->gc_ranges_len) {
            memmove(&S->gc_ranges[lo + 1], &S->gc_ranges[lo],
                    (S->gc_ranges_len - lo) * sizeof(*S->gc_ranges));
        }
        S->gc_ranges[lo] = S->gc_ranges_pending[i];
        S->gc_ranges_len++;
    }
    S->gc_ranges_pending_len = 0;
}

static gc_hdr_t *gc_find_header_for_ptr(mino_state_t *S, const void *p)
{
    uintptr_t u  = (uintptr_t)p;
    size_t    lo = 0;
    size_t    hi = S->gc_ranges_len;
    size_t    i;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        if (u < S->gc_ranges[mid].start) {
            hi = mid;
        } else if (u >= S->gc_ranges[mid].end) {
            lo = mid + 1;
        } else {
            return S->gc_ranges[mid].h;
        }
    }
    for (i = 0; i < S->gc_ranges_pending_len; i++) {
        if (u >= S->gc_ranges_pending[i].start && u < S->gc_ranges_pending[i].end) {
            return S->gc_ranges_pending[i].h;
        }
    }
    return NULL;
}

static void gc_mark_header(mino_state_t *S, gc_hdr_t *h);

void gc_mark_interior(mino_state_t *S, const void *p)
{
    gc_hdr_t *h;
    if (p == NULL) {
        return;
    }
    h = gc_find_header_for_ptr(S, p);
    if (h != NULL) {
        gc_mark_header(S, h);
    }
}

static void gc_mark_val(mino_state_t *S, mino_val_t *v)
{
    if (v == NULL) {
        return;
    }
    /* Metadata can be attached to any value type. */
    gc_mark_interior(S, v->meta);
    switch (v->type) {
    case MINO_STRING:
    case MINO_SYMBOL:
    case MINO_KEYWORD:
        gc_mark_interior(S, v->as.s.data);
        break;
    case MINO_CONS:
        gc_mark_interior(S, v->as.cons.car);
        gc_mark_interior(S, v->as.cons.cdr);
        break;
    case MINO_VECTOR:
        gc_mark_interior(S, v->as.vec.root);
        gc_mark_interior(S, v->as.vec.tail);
        break;
    case MINO_MAP:
        gc_mark_interior(S, v->as.map.root);
        gc_mark_interior(S, v->as.map.key_order);
        break;
    case MINO_SET:
        gc_mark_interior(S, v->as.set.root);
        gc_mark_interior(S, v->as.set.key_order);
        break;
    case MINO_SORTED_MAP:
    case MINO_SORTED_SET:
        gc_mark_interior(S, v->as.sorted.root);
        gc_mark_interior(S, v->as.sorted.comparator);
        break;
    case MINO_FN:
    case MINO_MACRO:
        gc_mark_interior(S, v->as.fn.params);
        gc_mark_interior(S, v->as.fn.body);
        gc_mark_interior(S, v->as.fn.env);
        break;
    case MINO_ATOM:
        gc_mark_interior(S, v->as.atom.val);
        gc_mark_interior(S, v->as.atom.watches);
        gc_mark_interior(S, v->as.atom.validator);
        break;
    case MINO_LAZY:
        if (v->as.lazy.realized) {
            gc_mark_interior(S, v->as.lazy.cached);
        } else {
            gc_mark_interior(S, v->as.lazy.body);
            gc_mark_interior(S, v->as.lazy.env);
        }
        break;
    case MINO_RECUR:
        gc_mark_interior(S, v->as.recur.args);
        break;
    case MINO_TAIL_CALL:
        gc_mark_interior(S, v->as.tail_call.fn);
        gc_mark_interior(S, v->as.tail_call.args);
        break;
    case MINO_REDUCED:
        gc_mark_interior(S, v->as.reduced.val);
        break;
    case MINO_VAR:
        gc_mark_interior(S, v->as.var.root);
        break;
    default:
        /* NIL, BOOL, INT, FLOAT, PRIM, HANDLE: no owned children. prim.name
         * and handle.tag are static/host-owned C strings. */
        break;
    }
}

static void gc_mark_env(mino_state_t *S, mino_env_t *env)
{
    size_t i;
    if (env == NULL) {
        return;
    }
    gc_mark_interior(S, env->parent);
    if (env->bindings != NULL) {
        gc_mark_interior(S, env->bindings);
        for (i = 0; i < env->len; i++) {
            gc_mark_interior(S, env->bindings[i].name);
            gc_mark_interior(S, env->bindings[i].val);
        }
    }
}

static void gc_mark_vec_node(mino_state_t *S, mino_vec_node_t *n)
{
    unsigned i;
    if (n == NULL) {
        return;
    }
    /* Leaf slots hold mino_val_t*; branch slots hold mino_vec_node_t*.
     * gc_mark_interior dispatches on the header's type tag either way. */
    for (i = 0; i < n->count; i++) {
        gc_mark_interior(S, n->slots[i]);
    }
}

static void gc_mark_hamt_node(mino_state_t *S, mino_hamt_node_t *n)
{
    unsigned count;
    unsigned i;
    if (n == NULL) {
        return;
    }
    gc_mark_interior(S, n->slots);
    count = (n->collision_count > 0) ? n->collision_count
                                     : popcount32(n->bitmap);
    if (n->slots != NULL) {
        for (i = 0; i < count; i++) {
            gc_mark_interior(S, n->slots[i]);
        }
    }
}

static void gc_mark_hamt_entry(mino_state_t *S, hamt_entry_t *e)
{
    if (e == NULL) {
        return;
    }
    gc_mark_interior(S, e->key);
    gc_mark_interior(S, e->val);
}

static void gc_mark_ptr_array(mino_state_t *S, void **arr, size_t bytes)
{
    size_t n = bytes / sizeof(*arr);
    size_t i;
    if (arr == NULL) {
        return;
    }
    for (i = 0; i < n; i++) {
        gc_mark_interior(S, arr[i]);
    }
}

static void gc_mark_header(mino_state_t *S, gc_hdr_t *h)
{
    if (h == NULL || h->mark) {
        return;
    }
    h->mark = 1;
    switch (h->type_tag) {
    case GC_T_VAL:
        gc_mark_val(S, (mino_val_t *)(h + 1));
        break;
    case GC_T_ENV:
        gc_mark_env(S, (mino_env_t *)(h + 1));
        break;
    case GC_T_VEC_NODE:
        gc_mark_vec_node(S, (mino_vec_node_t *)(h + 1));
        break;
    case GC_T_HAMT_NODE:
        gc_mark_hamt_node(S, (mino_hamt_node_t *)(h + 1));
        break;
    case GC_T_HAMT_ENTRY:
        gc_mark_hamt_entry(S, (hamt_entry_t *)(h + 1));
        break;
    case GC_T_VALARR:
    case GC_T_PTRARR:
        gc_mark_ptr_array(S, (void **)(h + 1), h->size);
        break;
    case GC_T_RB_NODE: {
        mino_rb_node_t *rb = (mino_rb_node_t *)(h + 1);
        gc_mark_interior(S, rb->key);
        gc_mark_interior(S, rb->val);
        gc_mark_interior(S, rb->left);
        gc_mark_interior(S, rb->right);
        break;
    }
    case GC_T_RAW:
    default:
        /* Leaf allocation: no children. */
        break;
    }
}

static void gc_scan_stack(mino_state_t *S)
{
    volatile char probe = 0;
    char         *lo;
    char         *hi;
    char         *cur;
    if (S->gc_stack_bottom == NULL) {
        return;
    }
    if ((char *)&probe < (char *)S->gc_stack_bottom) {
        lo = (char *)&probe;
        hi = (char *)S->gc_stack_bottom;
    } else {
        lo = (char *)S->gc_stack_bottom;
        hi = (char *)&probe;
    }
    while (((uintptr_t)lo % sizeof(void *)) != 0 && lo < hi) {
        lo++;
    }
    for (cur = lo; cur + sizeof(void *) <= hi; cur += sizeof(void *)) {
        void *word;
        memcpy(&word, cur, sizeof(word));
        gc_mark_interior(S, word);
    }
    (void)probe;
}

static void gc_mark_intern_table(mino_state_t *S, const intern_table_t *tbl)
{
    size_t i;
    for (i = 0; i < tbl->len; i++) {
        gc_mark_interior(S, tbl->entries[i]);
    }
}

static void gc_mark_roots(mino_state_t *S)
{
    root_env_t *r;
    int i;
    for (r = S->gc_root_envs; r != NULL; r = r->next) {
        gc_mark_interior(S, r->env);
    }
    gc_mark_intern_table(S, &S->sym_intern);
    gc_mark_intern_table(S, &S->kw_intern);
    /* Pin try/catch exception values and module cache results. */
    for (i = 0; i < S->try_depth; i++) {
        gc_mark_interior(S, S->try_stack[i].exception);
    }
    {
        size_t mi;
        for (mi = 0; mi < S->module_cache_len; mi++) {
            gc_mark_interior(S, S->module_cache[mi].value);
        }
    }
    /* Pin metadata source forms. */
    {
        size_t mi;
        for (mi = 0; mi < S->meta_table_len; mi++) {
            gc_mark_interior(S, S->meta_table[mi].source);
        }
    }
    /* Pin var registry entries. */
    {
        size_t vi;
        for (vi = 0; vi < S->var_registry_len; vi++) {
            gc_mark_interior(S, S->var_registry[vi].var);
        }
    }
    /* Pin host-retained refs. */
    {
        mino_ref_t *ref;
        for (ref = S->ref_roots; ref != NULL; ref = ref->next) {
            gc_mark_interior(S, ref->val);
        }
    }
    /* Pin dynamic binding values. */
    {
        dyn_frame_t *f;
        dyn_binding_t *b;
        for (f = S->dyn_stack; f != NULL; f = f->prev) {
            for (b = f->bindings; b != NULL; b = b->next) {
                gc_mark_interior(S, b->val);
            }
        }
    }
    /* Pin diagnostic data and cached map. */
    if (S->last_diag != NULL) {
        gc_mark_interior(S, S->last_diag->data);
        gc_mark_interior(S, S->last_diag->cached_map);
    }
    /* Pin sort comparator if active. */
    gc_mark_interior(S, S->sort_comp_fn);
    /* Pin values on the GC save stack. */
    {
        int si;
        int limit = S->gc_save_len < GC_SAVE_MAX ? S->gc_save_len : GC_SAVE_MAX;
        for (si = 0; si < limit; si++) {
            gc_mark_interior(S, S->gc_save[si]);
        }
    }
    /* Pin cached core.mino parsed forms. */
    if (S->core_forms != NULL) {
        size_t ci;
        for (ci = 0; ci < S->core_forms_len; ci++) {
            gc_mark_interior(S, S->core_forms[ci]);
        }
    }
    /* Pin async scheduler run queue values. */
    {
        struct sched_entry *e;
        for (e = S->async_run_head; e != NULL; e = e->next) {
            gc_mark_interior(S, e->callback);
            gc_mark_interior(S, e->value);
        }
    }
    /* Pin async timer channel values. */
    async_timers_mark(S);
}

static void gc_sweep(mino_state_t *S)
{
    gc_hdr_t **pp   = &S->gc_all;
    size_t     live = 0;
    while (*pp != NULL) {
        gc_hdr_t *h = *pp;
        if (h->mark) {
            h->mark = 0;
            live += h->size;
            pp = &h->next;
        } else {
            /* Call finalizer for handles being collected. */
            if (h->type_tag == GC_T_VAL) {
                mino_val_t *v = (mino_val_t *)(h + 1);
                if (v->type == MINO_HANDLE && v->as.handle.finalizer != NULL) {
                    v->as.handle.finalizer(v->as.handle.ptr,
                                           v->as.handle.tag);
                }
            }
            *pp = h->next;
            free(h);
        }
    }
    S->gc_total_freed += S->gc_bytes_alloc - live;
    S->gc_bytes_live  = live;
    S->gc_bytes_alloc = live;
    /* Next cycle triggers after another threshold's worth of growth above
     * the live set; threshold grows to keep collection amortized. */
    if (live * 2 > S->gc_threshold) {
        S->gc_threshold = live * 2;
    }
}

void gc_collect(mino_state_t *S)
{
    jmp_buf jb;
    if (S->gc_depth > 0) {
        return;
    }
    S->gc_depth++;
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
    gc_scan_stack(S);
    gc_range_compact(S);
    gc_sweep(S);
    S->gc_collections++;
    S->gc_depth--;
}
