/*
 * roots.c -- root enumeration and the conservative stack scan. Every
 * source of pinned state seeds the mark stack here; raw machine words
 * resolve to headers through the generation-split range index owned
 * by ranges.c.
 */

#include "runtime/internal.h"
#include "async/scheduler.h"
#include "async/timer.h"

/* AddressSanitizer's stack-use-after-return detection moves a function's
 * locals to a heap "fake stack", so &local no longer reflects the real
 * machine stack. gc_scan_stack (below) is a CONSERVATIVE scan: it walks
 * from a probe frame to the recorded stack bottom (gc_note_host_frame),
 * both of which must be real machine-stack addresses. Under a fake
 * stack the anchor becomes a heap address while the scan probe (this
 * unit is compiled no_sanitize_address at gc_scan_stack) stays on the
 * real stack, so the range spans unrelated regions and the scan walks
 * off into unmapped memory. A conservative collector cannot coexist
 * with the fake stack, so disable it for every ASan build of mino. UBSan
 * and TSan are unaffected; the rest of ASan (heap, use-after-free, real
 * stack-buffer overflow) stays on. */
/* __has_feature(address_sanitizer) is the clang spelling; __SANITIZE_ADDRESS__
 * is the gcc one. Test them in separate directives: gcc has no __has_feature,
 * and `defined(__has_feature) && __has_feature(...)` in one #if still expands
 * the __has_feature(...) token on gcc (0(...) -> syntax error). */
#if defined(__SANITIZE_ADDRESS__)
#  define MINO_ASAN_BUILD 1
#elif defined(__has_feature)
#  if __has_feature(address_sanitizer)
#    define MINO_ASAN_BUILD 1
#  endif
#endif
#ifdef MINO_ASAN_BUILD
const char *__asan_default_options(void);
const char *__asan_default_options(void)
{
    return "detect_stack_use_after_return=0";
}
#endif

/* Helpers with file-local linkage. */
static void gc_mark_intern_table(mino_state *S, const intern_table_t *tbl);

/* Mark every interned symbol or keyword value. The intern table holds
 * strong references into the managed heap.
 *
 * Fast path: intern entries are always exact payload starts produced by
 * mino_symbol_n / mino_keyword_n (never NULL, never singletons inside
 * the state struct, never interior pointers). The header always sits
 * immediately before the payload, so we can bypass the interior-pointer
 * resolve (binary search over the range buffers) that gc_mark_interior
 * pays and push the header directly. During MINOR, gc_mark_push filters OLD
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

/* gc_mark_ctx_bc_cursor -- mark the active BC fn struct pinned in ctx.
 * bc_current_bc is a raw pointer to a GC_T_BC allocation. Normally the
 * fn is also reachable via its enclosing MINO_FN value, but under a
 * throw that longjmps past a BC frame's normal exit-time restore the
 * cursor can briefly outlive its only other owner. Marking it here keeps
 * the BC source map readable for :mino/location attribution on the catch
 * side without relying on the broader fn val's reachability. */
static void gc_mark_ctx_bc_cursor(mino_state *S, mino_thread_ctx_t *ctx)
{
    if (ctx->bc_current_bc != NULL) {
        gc_mark_interior(S, (void *)ctx->bc_current_bc);
    }
}

/* gc_mark_ctx_parked_stack -- conservatively scan the C stack of a
 * thread parked inside a yielding primitive (state_lock released,
 * blocked in send/recv/accept/cv_wait). mino_yield_lock published
 * parked_sp as an anchor deeper than every frame that survives the
 * park, so [parked_sp, gc_stack_bottom) is exactly the region holding
 * the parked thread's live references: prim arguments, AST interpreter
 * let-locals, raw payload pointers. No other root category can see
 * those frames -- the owner cannot scan its own stack (it is blocked
 * in a syscall) and pins/snapshots only cover what they name. Without
 * this scan a collection forced from a peer can sweep the payload a
 * parked worker is mid-send on, or the socket record itself.
 *
 * Safe by construction: the region is frozen while the owner is parked
 * (it holds no state_lock and executes no code), the anchor is stored
 * under state_lock on both edges, and a conservative scan can only
 * retain more, never less. Word loop mirrors gc_scan_stack; the ASan
 * suppression rationale is identical (the scan deliberately reads
 * across frame red zones). */
#if defined(__has_feature) && !defined(_MSC_VER)
#  if __has_feature(address_sanitizer)
__attribute__((no_sanitize_address))
#  endif
#elif defined(_MSC_VER) && defined(__SANITIZE_ADDRESS__)
__declspec(no_sanitize_address)
#elif defined(__SANITIZE_ADDRESS__)
__attribute__((no_sanitize_address))
#endif
static void gc_mark_ctx_parked_stack(mino_state *S, mino_thread_ctx_t *ctx)
{
    char *lo;
    char *hi;
    char *cur;
    if (ctx->parked_sp == NULL || ctx->gc_stack_bottom == NULL) {
        return;
    }
    lo = (char *)ctx->parked_sp;
    hi = (char *)ctx->gc_stack_bottom;
    if (lo >= hi) {
        return;
    }
    while (((uintptr_t)lo % sizeof(void *)) != 0 && lo < hi) {
        lo++;
    }
    for (cur = lo; cur + sizeof(void *) <= hi; cur += sizeof(void *)) {
        void *word;
        memcpy(&word, cur, sizeof(word));
        gc_mark_interior(S, word);
    }
}

/* gc_mark_ctx_try_stack -- mark exception values held in a context's
 * try_stack.  The main_ctx walk is done in gc_mark_module_and_meta;
 * worker ctxs need the same treatment so that a caught exception held
 * in a worker's catch handler body is not collected during a GC cycle
 * that runs before the handler body finishes. */
static void gc_mark_ctx_try_stack(mino_state *S, mino_thread_ctx_t *ctx)
{
    int i;
    for (i = 0; i < ctx->try_depth; i++) {
        gc_mark_interior(S, ctx->try_stack[i].exception);
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

/* Function pointer type for the per-ctx marking helpers above. */
typedef void (*gc_ctx_mark_fn)(mino_state *, mino_thread_ctx_t *);

/* Apply fn to main_ctx (no lock needed -- GC always runs on the main
 * thread), then acquire worker_list_lock, walk all worker ctxs applying
 * fn to each, and release the lock.  The lock is held only for the
 * worker-list walk, not for the marking work itself, to keep the
 * effective lock window as narrow as possible (see the gc_mark_thread_state
 * comment for the full rationale). */
static void gc_mark_each_ctx(mino_state *S, gc_ctx_mark_fn fn)
{
    mino_thread_ctx_t *w;
    fn(S, &S->main_ctx);
    mino_worker_list_lock_acquire(S);
    for (w = S->threading.worker_ctxs_head; w != NULL; w = w->next_worker) {
        fn(S, w);
    }
    mino_worker_list_lock_release(S);
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
 * matching the spawn path.
 *
 * Why worker_list_lock is acquired and released separately for each
 * per-worker category (dyn_stack, bc_current_bc, gc_save, tx,
 * lazy_inflight): each marking helper (gc_mark_ctx_dyn_stack, etc.)
 * may call gc_mark_push, which in turn may trigger realloc on the mark
 * stack. Holding worker_list_lock across the full mark pass would
 * prevent a worker from attaching during that window, which is
 * acceptable, but would also exclude the per-category main_ctx pass
 * (done without the lock) from being interleaved correctly. Releasing
 * and re-acquiring between categories keeps the lock held only for the
 * duration of the list walk, not for the mark work itself, and lets
 * the same category pattern apply uniformly to main_ctx (no lock) and
 * worker ctxs (with lock) without structural divergence. */
static void gc_mark_thread_state(mino_state *S)
{
    gc_mark_each_ctx(S, gc_mark_ctx_dyn_stack);
    gc_mark_each_ctx(S, gc_mark_ctx_bc_cursor);
    gc_mark_each_ctx(S, gc_mark_ctx_gc_save);
    gc_mark_each_ctx(S, gc_mark_ctx_tx);
    gc_mark_each_ctx(S, gc_mark_ctx_lazy_inflight);
    /* Conservative scan of every parked thread's frozen C stack. Must
     * run for main_ctx and workers alike: either can be the one parked
     * in a yielding prim while a peer collects. */
    gc_mark_each_ctx(S, gc_mark_ctx_parked_stack);
    /* Pin in-flight try/catch exception values for each worker context.
     * The main_ctx try_stack is already walked in gc_mark_module_and_meta;
     * worker ctxs are parallel execution contexts that can hold their own
     * in-flight exceptions between the throw and the catch longjmp. */
    gc_mark_each_ctx(S, gc_mark_ctx_try_stack);
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
    gc_mark_interior(S, S->oom_exception);
    /* Signal handlers and at-exit hooks live only in these C-side slots
     * between registration and delivery; without this walk a trapped fn
     * could be collected before its signal ever arrives. */
    {
        size_t si;
        for (si = 0; si < 5; si++) {
            gc_mark_interior(S, S->signal_handlers[si]);
        }
        for (si = 0; si < S->atexit_len; si++) {
            gc_mark_interior(S, S->atexit_hooks[si]);
        }
    }
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
     * main_ctx is walked here unconditionally (it holds state_lock's
     * BC stack whenever it runs; its snapshot fields mirror the live
     * S->bc fields at yield time), so the mirror case -- main parked
     * in a yielding prim while a worker collects -- keeps main's
     * register slots alive too. */
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
        /* main_ctx's snapshot needs no list lock -- the GC already runs
         * under state_lock, which main cannot be holding concurrently.
         * Walk it exactly while main is parked (parked_sp published at
         * its yield): when main itself collects, its snapshot may be
         * stale from an earlier era and its live registers are covered
         * by the S->bc walk above. */
        if (S->main_ctx.parked_sp != NULL
                && S->main_ctx.bc_snapshot_valid
                && S->main_ctx.bc_regs_storage != NULL) {
            size_t bi;
            gc_mark_interior(S, S->main_ctx.bc_regs_storage);
            if (S->main_ctx.bc_top_snapshot
                    <= S->main_ctx.bc_regs_storage_cap) {
                for (bi = 0; bi < S->main_ctx.bc_top_snapshot; bi++) {
                    if (S->main_ctx.bc_regs_storage[bi] != NULL) {
                        gc_mark_interior(S,
                            S->main_ctx.bc_regs_storage[bi]);
                    }
                }
            }
        }
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

/*
 * Seed the mark stack from every source of pinned state: user-registered
 * root envs, symbol/keyword intern tables, try-catch exceptions, module
 * cache, metadata table, var registry, host-retained refs, dynamic
 * binding values, diagnostic cache, sort comparator, GC save stack,
 * cached core forms, async scheduler queue, trampoline sentinels, and
 * async timer channels.
 */
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
 * MSVC is excluded from the __has_feature arm so that newer MSVC
 * versions that expose __has_feature as a compatibility extension
 * always land on the __declspec arm rather than emitting the
 * GCC/Clang-only __attribute__ form, which MSVC would reject.
 */
#if defined(__has_feature) && !defined(_MSC_VER)
#  if __has_feature(address_sanitizer)
__attribute__((no_sanitize_address))
#  endif
#elif defined(_MSC_VER) && defined(__SANITIZE_ADDRESS__)
__declspec(no_sanitize_address)
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
