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
static void gc_mark_intern_table(mino_state *S, const intern_table_t *tbl);

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
void gc_build_range_index(mino_state *S)
{
    gc_hdr_t *h;
    size_t    n = 0;
    for (h = S->gc.all_young; h != NULL; h = h->next) n++;
    for (h = S->gc.all_old;   h != NULL; h = h->next) n++;
    if (n > S->gc.ranges_cap) {
        size_t      new_cap = n * 2 + 16;
        gc_range_t *nr      = (gc_range_t *)realloc(
            S->gc.ranges, new_cap * sizeof(*nr));
        if (nr == NULL) {
            abort(); /* Class I: inside GC; no safe recovery path */
        }
        S->gc.ranges     = nr;
        S->gc.ranges_cap = new_cap;
    }
    S->gc.ranges_len = 0;
    for (h = S->gc.all_young; h != NULL; h = h->next) {
        S->gc.ranges[S->gc.ranges_len].start = (uintptr_t)(h + 1);
        S->gc.ranges[S->gc.ranges_len].end   = (uintptr_t)(h + 1) + h->size;
        S->gc.ranges[S->gc.ranges_len].h     = h;
        S->gc.ranges_len++;
    }
    for (h = S->gc.all_old; h != NULL; h = h->next) {
        S->gc.ranges[S->gc.ranges_len].start = (uintptr_t)(h + 1);
        S->gc.ranges[S->gc.ranges_len].end   = (uintptr_t)(h + 1) + h->size;
        S->gc.ranges[S->gc.ranges_len].h     = h;
        S->gc.ranges_len++;
    }
    qsort(S->gc.ranges, S->gc.ranges_len, sizeof(*S->gc.ranges), gc_range_cmp);
    S->gc.ranges_valid = 1;
    S->gc.ranges_pending_len = 0;
    if (S->gc.ranges_len > 0) {
        S->gc.heap_min = S->gc.ranges[0].start;
        S->gc.heap_max = S->gc.ranges[S->gc.ranges_len - 1].end;
    } else {
        S->gc.heap_min = 0;
        S->gc.heap_max = 0;
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
void gc_range_insert(mino_state *S, gc_hdr_t *h)
{
    gc_range_t entry;

    if (!S->gc.ranges_valid) {
        return;
    }

    if (S->gc.ranges_pending_len == S->gc.ranges_pending_cap) {
        size_t      new_cap = S->gc.ranges_pending_cap == 0
            ? 64 : S->gc.ranges_pending_cap * 2;
        gc_range_t *nr      = (gc_range_t *)realloc(
            S->gc.ranges_pending, new_cap * sizeof(*nr));
        if (nr == NULL) {
            /* Fallback to the invalidate path so mutation can continue
             * even under memory pressure. Next collection rebuilds from
             * gc_all. */
            S->gc.ranges_valid = 0;
            return;
        }
        S->gc.ranges_pending     = nr;
        S->gc.ranges_pending_cap = new_cap;
    }

    entry.start = (uintptr_t)(h + 1);
    entry.end   = (uintptr_t)(h + 1) + h->size;
    entry.h     = h;
    S->gc.ranges_pending[S->gc.ranges_pending_len] = entry;
    S->gc.ranges_pending_len++;
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
void gc_range_merge_pending(mino_state *S)
{
    size_t K, N, need, i, j, k;
    gc_range_t *merged;

    if (!S->gc.ranges_valid) {
        return;
    }
    K = S->gc.ranges_pending_len;
    if (K == 0) {
        return;
    }
    qsort(S->gc.ranges_pending, K, sizeof(*S->gc.ranges_pending), gc_range_cmp);

    N = S->gc.ranges_len;
    need = N + K;
    if (need > S->gc.ranges_cap) {
        size_t      new_cap = need * 2 + 16;
        gc_range_t *nr      = (gc_range_t *)realloc(
            S->gc.ranges, new_cap * sizeof(*nr));
        if (nr == NULL) {
            abort(); /* Class I: inside GC; no safe recovery path */
        }
        S->gc.ranges     = nr;
        S->gc.ranges_cap = new_cap;
    }
    /* In-place merge from the back to avoid a scratch buffer. Walk both
     * inputs from high to low and fill gc_ranges from index need-1
     * downward; N and K cursors track remaining unmerged entries. */
    merged = S->gc.ranges;
    i = N;
    j = K;
    k = need;
    while (j > 0) {
        if (i > 0 && merged[i - 1].start > S->gc.ranges_pending[j - 1].start) {
            merged[k - 1] = merged[i - 1];
            i--;
        } else {
            merged[k - 1] = S->gc.ranges_pending[j - 1];
            j--;
        }
        k--;
    }
    S->gc.ranges_len = need;
    S->gc.ranges_pending_len = 0;
    if (S->gc.ranges_len > 0) {
        S->gc.heap_min = S->gc.ranges[0].start;
        S->gc.heap_max = S->gc.ranges[S->gc.ranges_len - 1].end;
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
void gc_range_compact_after_minor_mark(mino_state *S)
{
    size_t dst = 0, src;
    if (!S->gc.ranges_valid) {
        return;
    }
    for (src = 0; src < S->gc.ranges_len; src++) {
        gc_hdr_t *h = S->gc.ranges[src].h;
        if (h->gen == GC_GEN_OLD || h->mark) {
            S->gc.ranges[dst++] = S->gc.ranges[src];
        }
    }
    S->gc.ranges_len = dst;
    if (S->gc.ranges_len > 0) {
        S->gc.heap_min = S->gc.ranges[0].start;
        S->gc.heap_max = S->gc.ranges[S->gc.ranges_len - 1].end;
    } else {
        S->gc.heap_min = 0;
        S->gc.heap_max = 0;
    }
}

/*
 * Resolve p to its owning header, or NULL if p is not within any live
 * payload. Handles interior pointers (word lands in the middle of an
 * allocation). Fast-rejects words outside [heap_min, heap_max) when no
 * pending inserts are in flight.
 */
gc_hdr_t *gc_find_header_for_ptr(mino_state *S, const void *p)
{
    uintptr_t u  = (uintptr_t)p;
    size_t    lo = 0;
    size_t    hi = S->gc.ranges_len;
    size_t    i;
    /* Fast reject for stack words outside the heap — the conservative
     * scan examines every aligned machine word, and most of them are
     * not pointers into the managed heap. */
    if ((u < S->gc.heap_min || u >= S->gc.heap_max)
        && S->gc.ranges_pending_len == 0) {
        return NULL;
    }
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        if (u < S->gc.ranges[mid].start) {
            hi = mid;
        } else if (u >= S->gc.ranges[mid].end) {
            lo = mid + 1;
        } else {
            return S->gc.ranges[mid].h;
        }
    }
    for (i = 0; i < S->gc.ranges_pending_len; i++) {
        if (u >= S->gc.ranges_pending[i].start && u < S->gc.ranges_pending[i].end) {
            return S->gc.ranges_pending[i].h;
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
static void gc_mark_intern_table(mino_state *S, const intern_table_t *tbl)
{
    size_t i;
    /* MAJOR_MARK skips the walk: intern entries survive only when
     * reached through other roots (vars, ns env, code consts, live
     * runtime values). Slots whose underlying header is unmarked at
     * end-of-mark are tombstoned in gc_intern_sweep_tombstones before
     * gc_sweep frees the header memory. MINOR keeps the walk so a
     * freshly interned YOUNG sym/keyword stays alive until either a
     * major cycle prunes it or another root captures it; gc_mark_push's
     * per-phase filter short-circuits OLD entries during MINOR so the
     * loop is bounded by intern.len. */
    if (S->gc.phase == GC_PHASE_MAJOR_MARK) return;
    for (i = 0; i < tbl->len; i++) {
        mino_val *v = tbl->entries[i];
        gc_hdr_t   *h;
        if (v == NULL) continue;
        h = ((gc_hdr_t *)v) - 1;
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
/* gc_mark_ctx_dyn_stack -- mark every value bound in this ctx's dyn
 * stack. Walks frame -> bindings -> val. */
static void gc_mark_ctx_dyn_stack(mino_state *S, mino_thread_ctx_t *ctx)
{
    dyn_frame_t   *f;
    dyn_binding_t *b;
    for (f = ctx->dyn_stack; f != NULL; f = f->prev) {
        for (b = f->bindings; b != NULL; b = b->next) {
            gc_mark_interior(S, b->val);
            /* The canonical var is normally rooted via the var
             * registry, but an uninterned var can survive only
             * through a live binding. */
            if (b->var != NULL) gc_mark_interior(S, b->var);
        }
    }
}

/* gc_mark_ctx_gc_save -- mark every value pinned on this ctx's gc_save
 * stack. Used so blocked workers' pinned values stay visible to a GC
 * initiated from another thread. */
static void gc_mark_ctx_gc_save(mino_state *S, mino_thread_ctx_t *ctx)
{
    int si;
    int limit = ctx->gc_save_len < GC_SAVE_MAX
        ? ctx->gc_save_len : GC_SAVE_MAX;
    for (si = 0; si < limit; si++) {
        gc_mark_interior(S, ctx->gc_save[si]);
    }
}

/* gc_mark_ctx_lazy_inflight -- mark the lazy cells this ctx has
 * CAS-claimed but not yet published or rolled back. Between the claim
 * and the pop the cell is also live on the owning thread's C stack
 * (the active lazy_realize frame), but a GC initiated from another
 * thread while this worker is parked cannot see that stack, so the
 * tracking array doubles as the precise root. */
static void gc_mark_ctx_lazy_inflight(mino_state *S, mino_thread_ctx_t *ctx)
{
    size_t li;
    for (li = 0; li < ctx->lazy_inflight_len; li++) {
        gc_mark_interior(S, ctx->lazy_inflight[li]);
    }
}

/* gc_mark_ctx_tx -- mark the per-ref tentative values and commute log
 * cells held by this thread's active transaction (if any). Without
 * this, a tentative value not yet committed could be collected
 * mid-transaction since it has no other reachable owner. */
static void gc_mark_ctx_tx(mino_state *S, mino_thread_ctx_t *ctx)
{
    tx_state_t     *tx = ctx->current_tx;
    tx_ref_state_t *rs;
    if (tx == NULL) return;
    for (rs = tx->refs_head; rs != NULL; rs = rs->next) {
        gc_mark_interior(S, rs->ref);
        gc_mark_interior(S, rs->tentative);
        gc_mark_interior(S, rs->commute_log);
        gc_mark_interior(S, rs->committed_old);
        gc_mark_interior(S, rs->committed_new);
    }
    gc_mark_interior(S, tx->validator_thrown_ex);
    gc_mark_interior(S, tx->pending_sends);
}

/* Pin lexical environments published as GC roots, the symbol/keyword
 * intern tables, and the cached special-form symbol pointers used by
 * the O(1) eval_try_special_form dispatch. The sf_* fields hold
 * intern_table entries by pointer identity; without a precise root
 * here the weak intern sweep would tombstone them once nothing else
 * mentioned the symbol, leaving the cached pointer dangling. */
static void gc_mark_envs_and_interns(mino_state *S)
{
    root_env_t *r;
    for (r = S->gc.root_envs; r != NULL; r = r->next) {
        gc_mark_interior(S, r->env);
    }
    gc_mark_intern_table(S, &S->sym_intern);
    gc_mark_intern_table(S, &S->kw_intern);
    /* Cached special-form symbols: keep alive so eval_try_special_form's
     * pointer-identity dispatch never sees a tombstoned entry. */
    gc_mark_interior(S, S->sf_quote);
    gc_mark_interior(S, S->sf_quasiquote);
    gc_mark_interior(S, S->sf_unquote);
    gc_mark_interior(S, S->sf_unquote_splicing);
    gc_mark_interior(S, S->sf_defmacro);
    gc_mark_interior(S, S->sf_declare);
    gc_mark_interior(S, S->sf_ns);
    gc_mark_interior(S, S->sf_var);
    gc_mark_interior(S, S->sf_def);
    gc_mark_interior(S, S->sf_if);
    gc_mark_interior(S, S->sf_do);
    gc_mark_interior(S, S->sf_let);
    gc_mark_interior(S, S->sf_let_star);
    gc_mark_interior(S, S->sf_letfn_star);
    gc_mark_interior(S, S->sf_fn);
    gc_mark_interior(S, S->sf_fn_star);
    gc_mark_interior(S, S->sf_recur);
    gc_mark_interior(S, S->sf_loop);
    gc_mark_interior(S, S->sf_loop_star);
    gc_mark_interior(S, S->sf_try);
    gc_mark_interior(S, S->sf_binding);
    gc_mark_interior(S, S->sf_lazy_seq);
    gc_mark_interior(S, S->sf_new);
    gc_mark_interior(S, S->sf_when);
    gc_mark_interior(S, S->sf_and);
    gc_mark_interior(S, S->sf_or);
}

/* Pin in-flight try/catch exception values, cached module require
 * results, namespace metadata maps, source-form metadata, the var
 * registry, and host-retained refs. These are all pre-allocated tables
 * or linked structures that hold runtime-visible state. */
static void gc_mark_module_and_meta(mino_state *S)
{
    int         i;
    size_t      idx;
    mino_ref *ref;
    for (i = 0; i < mino_current_ctx(S)->try_depth; i++) {
        gc_mark_interior(S, mino_current_ctx(S)->try_stack[i].exception);
    }
    /* The pending raw-payload stash from an inner-eval catch is only
     * referenced from this slot until the outer pcall consumes it. */
    gc_mark_interior(S, mino_current_ctx(S)->pending_user_ex);
    for (idx = 0; idx < S->module.module_cache_len; idx++) {
        gc_mark_interior(S, S->module.module_cache[idx].value);
    }
    for (idx = 0; idx < S->ns_vars.ns_env_len; idx++) {
        if (S->ns_vars.ns_env_table[idx].meta != NULL) {
            gc_mark_interior(S, S->ns_vars.ns_env_table[idx].meta);
        }
    }
    for (idx = 0; idx < S->module.meta_table_len; idx++) {
        gc_mark_interior(S, S->module.meta_table[idx].source);
    }
    for (idx = 0; idx < S->ns_vars.var_registry_len; idx++) {
        gc_mark_interior(S, S->ns_vars.var_registry[idx].var);
    }
    for (ref = S->ref_roots; ref != NULL; ref = ref->next) {
        gc_mark_interior(S, ref->val);
    }
}

/* Pin per-thread-context state: dynamic-binding values, GC save-stack
 * payloads, and current-ctx diagnostic objects. Workers don't publish
 * diagnostics back through this path, so only the current ctx's diag
 * is walked.
 *
 * worker_list_lock guards the linked-list walk: workers attach/detach
 * themselves under this lock, and per-worker fields (dyn_stack /
 * gc_save / tx) remain stable while the worker is parked at the
 * state_lock waiting to enter mino_call. The GC reaches this from
 * inside state_lock (gc_alloc_typed -> major collect), so the
 * effective lock order is state_lock outer, worker_list_lock inner --
 * matching the spawn path. */
static void gc_mark_thread_state(mino_state *S)
{
    mino_thread_ctx_t *w;
    gc_mark_ctx_dyn_stack(S, &S->main_ctx);
    mino_worker_list_lock_acquire(S);
    for (w = S->threading.worker_ctxs_head; w != NULL; w = w->next_worker) {
        gc_mark_ctx_dyn_stack(S, w);
    }
    mino_worker_list_lock_release(S);
    /* bc_current_bc: a raw pointer to the active BC fn struct
     * (GC_T_BC allocation). Normally the fn is also reachable via
     * its enclosing MINO_FN value (env binding, closure capture,
     * etc.) so the GC would already keep it alive. But under a
     * throw that longjmps past a BC frame's normal exit-time
     * restore, ctx->bc_current_bc can briefly outlive its only
     * other reachable owner -- e.g. an anonymous fn invoked once
     * whose only env binding was the let frame that already
     * unwound. Marking the cursor pin here makes the BC source
     * map readable for :mino/location attribution on the catch
     * side without relying on the broader fn val's reachability. */
    if (S->main_ctx.bc_current_bc != NULL) {
        gc_mark_interior(S, (void *)S->main_ctx.bc_current_bc);
    }
    mino_worker_list_lock_acquire(S);
    for (w = S->threading.worker_ctxs_head; w != NULL; w = w->next_worker) {
        if (w->bc_current_bc != NULL) {
            gc_mark_interior(S, (void *)w->bc_current_bc);
        }
    }
    mino_worker_list_lock_release(S);
    gc_mark_ctx_gc_save(S, &S->main_ctx);
    mino_worker_list_lock_acquire(S);
    for (w = S->threading.worker_ctxs_head; w != NULL; w = w->next_worker) {
        gc_mark_ctx_gc_save(S, w);
    }
    mino_worker_list_lock_release(S);
    gc_mark_ctx_tx(S, &S->main_ctx);
    mino_worker_list_lock_acquire(S);
    for (w = S->threading.worker_ctxs_head; w != NULL; w = w->next_worker) {
        gc_mark_ctx_tx(S, w);
    }
    mino_worker_list_lock_release(S);
    gc_mark_ctx_lazy_inflight(S, &S->main_ctx);
    mino_worker_list_lock_acquire(S);
    for (w = S->threading.worker_ctxs_head; w != NULL; w = w->next_worker) {
        gc_mark_ctx_lazy_inflight(S, w);
    }
    mino_worker_list_lock_release(S);
    if (mino_current_ctx(S)->last_diag != NULL) {
        gc_mark_interior(S, mino_current_ctx(S)->last_diag->data);
        gc_mark_interior(S, mino_current_ctx(S)->last_diag->cached_map);
    }
}

/* Pin runtime singletons: hooks (sort comparator, print-method),
 * trampoline sentinel payloads, and the cached core.clj form vector. */
static void gc_mark_runtime_globals(mino_state *S)
{
    gc_mark_interior(S, S->sort_comp_fn);
    gc_mark_interior(S, S->print_method_fn);
    gc_mark_interior(S, S->recur_sentinel.as.recur.args);
    gc_mark_interior(S, S->tail_call_sentinel.as.tail_call.fn);
    gc_mark_interior(S, S->tail_call_sentinel.as.tail_call.args);
    if (S->core_forms != NULL) {
        size_t ci;
        for (ci = 0; ci < S->core_forms_len; ci++) {
            gc_mark_interior(S, S->core_forms[ci]);
        }
    }
    /* Inline call cache: pin the form pointer (keys the slot) and the
     * cached callable. Without this, a freed form could be GC-recycled
     * for an unrelated allocation and the slot would alias to the new
     * object, producing wrong dispatch.
     *
     * Tag-safety: gc_mark_interior fast-rejects inline-tagged values
     * (low three bits non-zero) at the top, so a tagged scalar that
     * landed in the cache slot is harmless even though forms and
     * callables in practice are always heap pointers. */
    if (S->ns_vars.ic_table != NULL) {
        size_t ic_i;
        for (ic_i = 0; ic_i < S->ns_vars.ic_cap; ic_i++) {
            if (S->ns_vars.ic_table[ic_i].form != NULL) {
                gc_mark_interior(S, S->ns_vars.ic_table[ic_i].form);
                gc_mark_interior(S, S->ns_vars.ic_table[ic_i].callable);
            }
        }
    }
    /* Bytecode VM register stack. Every slot in [0, bc_top) is a live
     * register value held by some active VM frame. Without this walk,
     * a value computed into a register but not yet stored elsewhere
     * could be collected mid-call. The bc_regs buffer itself is a
     * GC_T_VALARR, so it's already kept alive through the state's
     * indirect-pointer scan; the explicit per-slot mark is what
     * keeps the values they point at alive. */
    /* Mark the bc register-stack buffer itself so its allocation is
     * not freed mid-VM-execution. The MINOR collector does not trace
     * inside OLD allocations -- it relies on the remembered set for
     * OLD-to-YOUNG references. Since the bc_regs buffer can be OLD
     * while every register write inside the VM is hot-path code that
     * skips the write barrier for speed, we walk the live slot range
     * explicitly here so a minor cycle finds every YOUNG value held
     * in a register. */
    if (S->bc.bc_regs != NULL) {
        gc_mark_interior(S, S->bc.bc_regs);
        if (S->bc.bc_top <= S->bc.bc_regs_cap) {
            size_t bi;
            for (bi = 0; bi < S->bc.bc_top; bi++) {
                if (S->bc.bc_regs[bi] != NULL) {
                    gc_mark_interior(S, S->bc.bc_regs[bi]);
                }
            }
        }
    }
    /* Per-ctx BC stack snapshots. When a worker yields state_lock,
     * its bc_regs/cap/top are saved into its ctx so a sibling
     * worker can install its own bc_regs into S during the yield
     * window. The saved snapshots are live roots while the worker
     * is parked: every slot in [0, bc_top_snapshot) is an active
     * register value the worker will resume reading. Without this
     * walk, a yielded worker's slot could be collected during a
     * peer's allocation pressure.
     *
     * main_ctx's snapshot is reachable here only when the embedder
     * is itself yielded (a nested mino_call from a primitive that
     * yields), which currently doesn't happen but is included for
     * symmetry. */
    {
        mino_thread_ctx_t *w;
        mino_worker_list_lock_acquire(S);
        for (w = S->threading.worker_ctxs_head; w != NULL; w = w->next_worker) {
            if (w->bc_snapshot_valid && w->bc_regs_storage != NULL) {
                size_t bi;
                gc_mark_interior(S, w->bc_regs_storage);
                if (w->bc_top_snapshot <= w->bc_regs_storage_cap) {
                    for (bi = 0; bi < w->bc_top_snapshot; bi++) {
                        if (w->bc_regs_storage[bi] != NULL) {
                            gc_mark_interior(S, w->bc_regs_storage[bi]);
                        }
                    }
                }
            }
        }
        mino_worker_list_lock_release(S);
    }
}

/* Pin async-subsystem live values: scheduler run-queue callbacks/values
 * and timer channel payloads. */
static void gc_mark_async_roots(mino_state *S)
{
    struct sched_entry *e;
    for (e = S->async.run_head; e != NULL; e = e->next) {
        gc_mark_interior(S, e->callback);
        gc_mark_interior(S, e->value);
    }
    async_timers_mark(S);
}

/* Pin record-type registry entries. Record types are interned per
 * (ns, name) and live for the state's lifetime so re-eval'd defrecord
 * forms keep the same MINO_TYPE pointer identity. */
static void gc_mark_record_types(mino_state *S)
{
    record_type_entry_t *rt;
    for (rt = S->record_types; rt != NULL; rt = rt->next) {
        gc_mark_interior(S, rt->type);
    }
}

/* Pin queued agent action nodes. The nodes themselves are malloc'd
 * (not GC values), but they hold mino_val pointers (agent, fn,
 * extra args, dyn snapshot, env) that must stay live until the
 * worker thread pops and applies the action. The worker holds
 * state_lock while running each action, so concurrent mutation of
 * the queue is impossible during a major GC (which suspends all
 * workers via the safepoint mechanism). */
static void gc_mark_agent_runq(mino_state *S)
{
    int pi;
    for (pi = 0; pi < AGENT_POOL_COUNT; pi++) {
        agent_action_node_t *n;
        for (n = S->agent.pool[pi].run_head; n != NULL; n = n->next) {
            gc_mark_interior(S, n->agent);
            gc_mark_interior(S, n->fn);
            gc_mark_interior(S, n->extra);
            gc_mark_interior(S, n->dyn_snap);
            gc_mark_interior(S, (mino_val *)n->env);
        }
    }
}

void gc_mark_roots(mino_state *S)
{
    gc_mark_envs_and_interns(S);
    gc_mark_module_and_meta(S);
    gc_mark_thread_state(S);
    gc_mark_runtime_globals(S);
    gc_mark_async_roots(S);
    gc_mark_record_types(S);
    gc_mark_agent_runq(S);
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
 *
 * Clang exposes ASan via `__has_feature(address_sanitizer)`; gcc
 * uses the `__SANITIZE_ADDRESS__` predefined macro. The `__has_feature`
 * check is nested inside its own `defined` test because gcc evaluates
 * the second half of an `&&` syntactically even when the first half is
 * false. Without the gcc branch the attribute is silently dropped and
 * libsanitizer flags every cross-frame word read in the scan loop --
 * which surfaced as a CI failure on ubuntu-24.04 when release-gate's
 * ASan suite ran on a non-clang host for the first time.
 */
#if defined(__has_feature)
#  if __has_feature(address_sanitizer)
__attribute__((no_sanitize_address))
#  endif
#elif defined(__SANITIZE_ADDRESS__)
__attribute__((no_sanitize_address))
#endif
void gc_scan_stack(mino_state *S)
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
