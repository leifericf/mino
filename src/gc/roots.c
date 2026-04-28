/*
 * roots.c -- root enumeration, conservative stack scan, and the sorted
 * range index used to resolve raw machine words to their owning
 * headers. The range index is rebuilt at the start of every collection;
 * new allocations append to a small pending buffer so the sorted array
 * avoids an O(n) memmove per alloc.
 */

#include "runtime/internal.h"
#include "async/scheduler.h"
#include "async/timer.h"

/* Helpers with file-local linkage. */
static int  gc_range_cmp(const void *a, const void *b);
static void gc_mark_intern_table(mino_state_t *S, const intern_table_t *tbl);

static int gc_range_cmp(const void *a, const void *b)
{
    const gc_range_t *ra = (const gc_range_t *)a;
    const gc_range_t *rb = (const gc_range_t *)b;
    if (ra->start < rb->start) return -1;
    if (ra->start > rb->start) return 1;
    return 0;
}

/*
 * Rebuild the sorted range index from scratch by walking gc_all.
 * Called once per collection before any mark work that may resolve
 * interior pointers. After this returns, gc_ranges holds one entry per
 * live header in ascending payload-start order.
 */
void gc_build_range_index(mino_state_t *S)
{
    gc_hdr_t *h;
    size_t    n = 0;
    for (h = S->gc_all_young; h != NULL; h = h->next) n++;
    for (h = S->gc_all_old;   h != NULL; h = h->next) n++;
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
    for (h = S->gc_all_young; h != NULL; h = h->next) {
        S->gc_ranges[S->gc_ranges_len].start = (uintptr_t)(h + 1);
        S->gc_ranges[S->gc_ranges_len].end   = (uintptr_t)(h + 1) + h->size;
        S->gc_ranges[S->gc_ranges_len].h     = h;
        S->gc_ranges_len++;
    }
    for (h = S->gc_all_old; h != NULL; h = h->next) {
        S->gc_ranges[S->gc_ranges_len].start = (uintptr_t)(h + 1);
        S->gc_ranges[S->gc_ranges_len].end   = (uintptr_t)(h + 1) + h->size;
        S->gc_ranges[S->gc_ranges_len].h     = h;
        S->gc_ranges_len++;
    }
    qsort(S->gc_ranges, S->gc_ranges_len, sizeof(*S->gc_ranges), gc_range_cmp);
    S->gc_ranges_valid = 1;
    S->gc_ranges_pending_len = 0;
    if (S->gc_ranges_len > 0) {
        S->gc_heap_min = S->gc_ranges[0].start;
        S->gc_heap_max = S->gc_ranges[S->gc_ranges_len - 1].end;
    } else {
        S->gc_heap_min = 0;
        S->gc_heap_max = 0;
    }
}

/*
 * Buffer a newly allocated header for the next collection. Appends to a
 * growable pending array; gc_range_merge_pending folds pending into the
 * sorted main array at the next GC in O(K log K + n + K), avoiding the
 * O(n log n) qsort that gc_build_range_index pays when it rebuilds from
 * scratch. gc_find_header_for_ptr handles the transient state (sorted
 * main + unsorted pending) via a binary search followed by a linear
 * pending scan.
 */
void gc_range_insert(mino_state_t *S, gc_hdr_t *h)
{
    gc_range_t entry;

    if (!S->gc_ranges_valid) {
        return;
    }

    if (S->gc_ranges_pending_len == S->gc_ranges_pending_cap) {
        size_t      new_cap = S->gc_ranges_pending_cap == 0
            ? 64 : S->gc_ranges_pending_cap * 2;
        gc_range_t *nr      = (gc_range_t *)realloc(
            S->gc_ranges_pending, new_cap * sizeof(*nr));
        if (nr == NULL) {
            /* Fallback to the invalidate path so mutation can continue
             * even under memory pressure. Next collection rebuilds from
             * gc_all. */
            S->gc_ranges_valid = 0;
            return;
        }
        S->gc_ranges_pending     = nr;
        S->gc_ranges_pending_cap = new_cap;
    }

    entry.start = (uintptr_t)(h + 1);
    entry.end   = (uintptr_t)(h + 1) + h->size;
    entry.h     = h;
    S->gc_ranges_pending[S->gc_ranges_pending_len] = entry;
    S->gc_ranges_pending_len++;
}

/*
 * Sort pending and merge into the sorted main array. Called at the top
 * of a collection, before any code that does ptr->header lookups on the
 * index. After this returns, gc_ranges_pending is empty and gc_ranges
 * holds one sorted entry per allocation.
 *
 * Cost: O(K log K) sort of pending plus O(n + K) merge into main, where
 * K is the number of allocations since the last collection. Replaces
 * the previous "invalidate + rebuild from gc_all + qsort" path, which
 * paid O(n log n) every collection.
 */
void gc_range_merge_pending(mino_state_t *S)
{
    size_t K, N, need, i, j, k;
    gc_range_t *merged;

    if (!S->gc_ranges_valid) {
        return;
    }
    K = S->gc_ranges_pending_len;
    if (K == 0) {
        return;
    }
    qsort(S->gc_ranges_pending, K, sizeof(*S->gc_ranges_pending), gc_range_cmp);

    N = S->gc_ranges_len;
    need = N + K;
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
    /* In-place merge from the back to avoid a scratch buffer. Walk both
     * inputs from high to low and fill gc_ranges from index need-1
     * downward; N and K cursors track remaining unmerged entries. */
    merged = S->gc_ranges;
    i = N;
    j = K;
    k = need;
    while (j > 0) {
        if (i > 0 && merged[i - 1].start > S->gc_ranges_pending[j - 1].start) {
            merged[k - 1] = merged[i - 1];
            i--;
        } else {
            merged[k - 1] = S->gc_ranges_pending[j - 1];
            j--;
        }
        k--;
    }
    S->gc_ranges_len = need;
    S->gc_ranges_pending_len = 0;
    if (S->gc_ranges_len > 0) {
        S->gc_heap_min = S->gc_ranges[0].start;
        S->gc_heap_max = S->gc_ranges[S->gc_ranges_len - 1].end;
    }
}

/*
 * Compact the range index after a minor mark. Keeps every OLD entry
 * (minor does not touch OLD, so their mark bits are zero even though
 * they are live) and every YOUNG entry that the mark phase reached.
 * Dropped YOUNG entries point at headers gc_minor_sweep is about to
 * free. Preserves sort order in one pass.
 *
 * Call site: gc_minor_collect, after the drain loops and before
 * gc_minor_sweep, while mark bits still indicate YOUNG liveness.
 */
void gc_range_compact_after_minor_mark(mino_state_t *S)
{
    size_t dst = 0, src;
    if (!S->gc_ranges_valid) {
        return;
    }
    for (src = 0; src < S->gc_ranges_len; src++) {
        gc_hdr_t *h = S->gc_ranges[src].h;
        if (h->gen == GC_GEN_OLD || h->mark) {
            S->gc_ranges[dst++] = S->gc_ranges[src];
        }
    }
    S->gc_ranges_len = dst;
    if (S->gc_ranges_len > 0) {
        S->gc_heap_min = S->gc_ranges[0].start;
        S->gc_heap_max = S->gc_ranges[S->gc_ranges_len - 1].end;
    } else {
        S->gc_heap_min = 0;
        S->gc_heap_max = 0;
    }
}

/*
 * Resolve p to its owning header, or NULL if p is not within any live
 * payload. Handles interior pointers (word lands in the middle of an
 * allocation). Fast-rejects words outside [heap_min, heap_max) when no
 * pending inserts are in flight.
 */
gc_hdr_t *gc_find_header_for_ptr(mino_state_t *S, const void *p)
{
    uintptr_t u  = (uintptr_t)p;
    size_t    lo = 0;
    size_t    hi = S->gc_ranges_len;
    size_t    i;
    /* Fast reject for stack words outside the heap — the conservative
     * scan examines every aligned machine word, and most of them are
     * not pointers into the managed heap. */
    if ((u < S->gc_heap_min || u >= S->gc_heap_max)
        && S->gc_ranges_pending_len == 0) {
        return NULL;
    }
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

/* Mark every interned symbol or keyword value. The intern table holds
 * strong references into the managed heap.
 *
 * Fast path: intern entries are always exact payload starts produced by
 * mino_symbol_n / mino_keyword_n (never NULL, never singletons inside
 * the state struct, never interior pointers). The header always sits
 * immediately before the payload, so we can bypass the interior-pointer
 * resolve (binary search over gc_ranges) that gc_mark_interior pays and
 * push the header directly. During MINOR, gc_mark_push filters OLD
 * entries in O(1) -- symbols outlive nurseries so after the first
 * major almost every entry is OLD and minor does essentially no work
 * proportional to intern table size. Replaces an O(N log M) hot spot
 * that dominated gc_mark_roots at 190k+ interned symbols (one per
 * gensym call) in spawn-heavy workloads. */
static void gc_mark_intern_table(mino_state_t *S, const intern_table_t *tbl)
{
    size_t i;
    for (i = 0; i < tbl->len; i++) {
        mino_val_t *v = tbl->entries[i];
        gc_hdr_t   *h = ((gc_hdr_t *)v) - 1;
        gc_mark_push(S, h);
    }
}

/*
 * Seed the mark stack from every source of pinned state: user-registered
 * root envs, symbol/keyword intern tables, try-catch exceptions, module
 * cache, metadata table, var registry, host-retained refs, dynamic
 * binding values, diagnostic cache, sort comparator, GC save stack,
 * cached core forms, async scheduler queue, trampoline sentinels, and
 * async timer channels.
 */
void gc_mark_roots(mino_state_t *S)
{
    root_env_t *r;
    int i;
    for (r = S->gc_root_envs; r != NULL; r = r->next) {
        gc_mark_interior(S, r->env);
    }
    gc_mark_intern_table(S, &S->sym_intern);
    gc_mark_intern_table(S, &S->kw_intern);
    /* Pin try/catch exception values and module cache results. */
    for (i = 0; i < mino_current_ctx(S)->try_depth; i++) {
        gc_mark_interior(S, mino_current_ctx(S)->try_stack[i].exception);
    }
    {
        size_t mi;
        for (mi = 0; mi < S->module_cache_len; mi++) {
            gc_mark_interior(S, S->module_cache[mi].value);
        }
    }
    /* Pin per-namespace metadata maps. */
    {
        size_t ni;
        for (ni = 0; ni < S->ns_env_len; ni++) {
            if (S->ns_env_table[ni].meta != NULL) {
                gc_mark_interior(S, S->ns_env_table[ni].meta);
            }
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
    /* Pin dynamic binding values for every live ctx (main + workers). */
    {
        dyn_frame_t *f;
        dyn_binding_t *b;
        mino_thread_ctx_t *w;
        for (f = S->main_ctx.dyn_stack; f != NULL; f = f->prev) {
            for (b = f->bindings; b != NULL; b = b->next) {
                gc_mark_interior(S, b->val);
            }
        }
        for (w = S->worker_ctxs_head; w != NULL; w = w->next_worker) {
            for (f = w->dyn_stack; f != NULL; f = f->prev) {
                for (b = f->bindings; b != NULL; b = b->next) {
                    gc_mark_interior(S, b->val);
                }
            }
        }
    }
    /* Pin diagnostic data and cached map (current ctx; workers don't
     * publish diagnostics back through this path). */
    if (mino_current_ctx(S)->last_diag != NULL) {
        gc_mark_interior(S, mino_current_ctx(S)->last_diag->data);
        gc_mark_interior(S, mino_current_ctx(S)->last_diag->cached_map);
    }
    /* Pin sort comparator if active. */
    gc_mark_interior(S, S->sort_comp_fn);
    /* Pin print-method hook if installed. */
    gc_mark_interior(S, S->print_method_fn);
    /* Pin values on the GC save stack of every live ctx. The current
     * thread's ctx is whichever is mino_current_ctx; other ctxs are
     * blocked workers (Cycle G4.3) whose gc_save would otherwise be
     * invisible to a GC initiated from a different thread. We walk
     * S->main_ctx + S->worker_ctxs_head; the current thread's ctx is
     * one of those, so it's covered. */
    {
        mino_thread_ctx_t *ctxs[2];
        mino_thread_ctx_t *w;
        int               ci;
        ctxs[0] = &S->main_ctx;
        ctxs[1] = NULL; /* sentinel */
        for (ci = 0; ctxs[ci] != NULL; ci++) {
            mino_thread_ctx_t *c = ctxs[ci];
            int si;
            int limit = c->gc_save_len < GC_SAVE_MAX ? c->gc_save_len : GC_SAVE_MAX;
            for (si = 0; si < limit; si++) {
                gc_mark_interior(S, c->gc_save[si]);
            }
        }
        for (w = S->worker_ctxs_head; w != NULL; w = w->next_worker) {
            int si;
            int limit = w->gc_save_len < GC_SAVE_MAX ? w->gc_save_len : GC_SAVE_MAX;
            for (si = 0; si < limit; si++) {
                gc_mark_interior(S, w->gc_save[si]);
            }
        }
    }
    /* Pin cached core.clj parsed forms. */
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
    /* Pin current trampoline sentinel payloads (args/fn pointers). */
    gc_mark_interior(S, S->recur_sentinel.as.recur.args);
    gc_mark_interior(S, S->tail_call_sentinel.as.tail_call.fn);
    gc_mark_interior(S, S->tail_call_sentinel.as.tail_call.args);
    /* Pin async timer channel values. */
    async_timers_mark(S);
    /* Pin record-type registry entries. Record types are interned per
     * (ns, name) and live for the state's lifetime so re-eval'd
     * defrecord forms keep the same MINO_TYPE pointer identity. */
    {
        record_type_entry_t *rt;
        for (rt = S->record_types; rt != NULL; rt = rt->next) {
            gc_mark_interior(S, rt->type);
        }
    }
}

/*
 * Conservative stack scan between gc_stack_bottom (the shallowest host
 * frame on a downward-growing stack) and the collector's own frame.
 * Every aligned machine word is treated as a candidate pointer and
 * resolved through the range index; non-pointer words fast-reject.
 *
 * ASan inserts red zones between locals; a conservative scan that
 * walks through them looks like stack-buffer-overflow to the
 * sanitizer. The scan is the entire point, so suppress the check.
 */
#if defined(__has_feature)
# if __has_feature(address_sanitizer)
__attribute__((no_sanitize("address")))
# endif
#endif
void gc_scan_stack(mino_state_t *S)
{
    volatile char probe = 0;
    char         *lo;
    char         *hi;
    char         *cur;
    if (mino_current_ctx(S)->gc_stack_bottom == NULL) {
        return;
    }
    if ((char *)&probe < (char *)mino_current_ctx(S)->gc_stack_bottom) {
        lo = (char *)&probe;
        hi = (char *)mino_current_ctx(S)->gc_stack_bottom;
    } else {
        lo = (char *)mino_current_ctx(S)->gc_stack_bottom;
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
