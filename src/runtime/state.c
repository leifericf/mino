/*
 * state.c -- state lifecycle, refs, public eval entry points,
 *            execution limits, fault injection, interrupt.
 */

#include "runtime/internal.h"
#include "runtime/host_threads.h"
#include "async/scheduler.h"
#include "async/timer.h"

#ifdef _WIN32
#  define WIN32_LEAN_AND_MEAN
#  include <windows.h>
#elif defined(CLOCK_MONOTONIC)
#  include <time.h>
#endif

/* ------------------------------------------------------------------------- */
/* Version                                                                   */
/* ------------------------------------------------------------------------- */

#define MINO_VERSION_STR_HELPER(x) #x
#define MINO_VERSION_STR(x) MINO_VERSION_STR_HELPER(x)

const char *mino_version_string(void)
{
    return MINO_VERSION_STR(MINO_VERSION_MAJOR) "."
           MINO_VERSION_STR(MINO_VERSION_MINOR) "."
           MINO_VERSION_STR(MINO_VERSION_PATCH);
}

/* ------------------------------------------------------------------------- */
/* TLS pointer to the active per-thread ctx.                                  */
/*                                                                            */
/* NULL on the embedder's main thread (mino_state_new caller). Spawned host  */
/* worker threads (Cycle G4.3+) set this to their freshly-allocated ctx at   */
/* thread entry and clear it before exit. The inline mino_current_ctx        */
/* accessor in internal.h reads this and falls through to &S->main_ctx       */
/* when NULL.                                                                 */
/* ------------------------------------------------------------------------- */

#if defined(_WIN32) && defined(_MSC_VER)
__declspec(thread)
#else
__thread
#endif
mino_thread_ctx_t *mino_tls_ctx = NULL;

/* ------------------------------------------------------------------------- */
/* State lifecycle                                                           */
/* ------------------------------------------------------------------------- */

static void state_init(mino_state_t *S)
{
    memset(S, 0, sizeof(*S));
    /* main_ctx is the embedder thread's view; spawned worker threads
     * (Cycle G4.3+) install their own ctx via TLS at thread entry. */
    S->gc_threshold            = 1u << 20;
    S->gc_nursery_bytes        = 1u << 20;  /* 1 MiB default */
    {
        const char *nb = getenv("MINO_GC_NURSERY_BYTES");
        if (nb != NULL && nb[0] != '\0') {
            unsigned long v = strtoul(nb, NULL, 10);
            if (v >= 64u * 1024u) S->gc_nursery_bytes = (size_t)v;
        }
    }
    S->gc_promotion_age        = 1;
    S->gc_major_growth_tenths  = 15;        /* 1.5x old-gen growth */
    /* Headers popped per slice. 4096 amortizes the per-slice overhead
     * on small-heap allocation-heavy workloads; max pause rises but
     * stays under 20 ms on the GC stress shards. */
    S->gc_major_work_budget    = 4096;
    S->gc_major_alloc_quantum  = 16u * 1024u;  /* bytes between auto steps */
    S->gc_major_step_alloc     = 0;
    S->gc_stress               = -1;
    S->nil_singleton.type  = MINO_NIL;
    S->true_singleton.type = MINO_BOOL;
    S->true_singleton.as.b = 1;
    S->false_singleton.type = MINO_BOOL;
    S->recur_sentinel.type     = MINO_RECUR;
    S->tail_call_sentinel.type = MINO_TAIL_CALL;
    {
        int si;
        for (si = 0; si < 256; si++) {
            S->small_ints[si].type = MINO_INT;
            S->small_ints[si].as.i = (long long)(si + MINO_SMALL_INT_LO);
        }
    }
    S->reader_line         = 1;
    S->reader_col          = 1;
    S->reader_dialect      = "mino";
    S->current_ns          = "user";
    /* Host-thread grant defaults to single-threaded. Standalone `./mino`
     * raises this to cpu_count after mino_install_all; embedders opt
     * in per state via mino_set_thread_limit. */
    S->thread_limit        = 1;
    S->thread_count        = 0;
    S->multi_threaded      = 0;
    mino_state_lock_init(S);
    gc_evt_init(S);
}

mino_state_t *mino_state_new(void)
{
    mino_state_t *st = (mino_state_t *)calloc(1, sizeof(*st));
    if (st == NULL) {
        abort(); /* unrecoverable: no state to report error through */
    }
    state_init(st);
    return st;
}

/* Free the linked list of registered root environments. The mino_env
 * objects themselves are GC-owned; only the root_env_t link nodes
 * (malloc'd by mino_env_new_root) belong to us here. */
static void state_free_root_envs(mino_state_t *S)
{
    root_env_t *r, *rnext;
    for (r = S->gc_root_envs; r != NULL; r = rnext) {
        rnext = r->next;
        free(r);
    }
}

/* Free the host-retained value ref doubly-linked list. */
static void state_free_refs(mino_state_t *S)
{
    mino_ref_t *ref = S->ref_roots;
    mino_ref_t *rnxt;
    while (ref != NULL) {
        rnxt = ref->next;
        free(ref);
        ref = rnxt;
    }
}

/* Free namespace alias strings (owning_ns + alias + full_name) and the array. */
static void state_free_ns_aliases(mino_state_t *S)
{
    size_t i;
    for (i = 0; i < S->ns_alias_len; i++) {
        free(S->ns_aliases[i].owning_ns);
        free(S->ns_aliases[i].alias);
        free(S->ns_aliases[i].full_name);
    }
    free(S->ns_aliases);
}

/* Free the per-ns env table. Names are interned via intern_filename
 * (freed by state_free_string_interns); envs themselves are GC-owned
 * (freed by state_free_heap). Only the array storage is malloc-owned. */
static void state_free_ns_env_table(mino_state_t *S)
{
    free(S->ns_env_table);
}

/* Free the module cache: per-entry names, then the array itself. */
static void state_free_module_cache(mino_state_t *S)
{
    size_t i;
    for (i = 0; i < S->module_cache_len; i++) {
        free(S->module_cache[i].name);
    }
    free(S->module_cache);
    for (i = 0; i < S->load_stack_len; i++) {
        free(S->load_stack[i]);
    }
    free(S->load_stack);
}

/* Free the bundled-stdlib registry: per-entry names, then the array.
 * Source pointers are static literals and not freed. */
static void state_free_bundled_libs(mino_state_t *S)
{
    size_t i;
    for (i = 0; i < S->bundled_libs_len; i++) {
        free(S->bundled_libs[i].name);
    }
    free(S->bundled_libs);
}

/* Truncate the load stack back to LEN, freeing any entries pushed past
 * that point. Used when a throw unwinds out of a require call before its
 * normal cleanup runs. */
void load_stack_truncate(mino_state_t *S, size_t len)
{
    while (S->load_stack_len > len) {
        free(S->load_stack[--S->load_stack_len]);
    }
}

/* Free the host-interop type registry: per-type member arrays, then
 * the host_types array. */
static void state_free_host_types(mino_state_t *S)
{
    size_t i;
    for (i = 0; i < S->host_types_len; i++) {
        free(S->host_types[i].members);
    }
    free(S->host_types);
}

/* Free the metadata table: per-entry name + docstring + capability,
 * then array. */
static void state_free_meta_table(mino_state_t *S)
{
    size_t i;
    for (i = 0; i < S->meta_table_len; i++) {
        free(S->meta_table[i].name);
        free(S->meta_table[i].docstring);
        free(S->meta_table[i].capability);
    }
    free(S->meta_table);
}

/* Free both intern tables (symbols + keywords). The interned values
 * themselves are GC-owned and freed by state_free_heap below. */
static void state_free_intern_tables(mino_state_t *S)
{
    free(S->sym_intern.entries);
    free(S->sym_intern.ht_buckets);
    free(S->kw_intern.entries);
    free(S->kw_intern.ht_buckets);
}

/* Free the record-type registry chain. Type values themselves are
 * GC-owned and reclaimed by state_free_heap below; we only release
 * the malloc'd link nodes here. */
static void state_free_record_types(mino_state_t *S)
{
    record_type_entry_t *e = S->record_types;
    while (e != NULL) {
        record_type_entry_t *next = e->next;
        free(e);
        e = next;
    }
    S->record_types = NULL;
}

/* Free the filename and var-name interning tables (string copies plus
 * the pointer arrays). */
static void state_free_string_interns(mino_state_t *S)
{
    size_t i;
    for (i = 0; i < S->interned_files_len; i++) {
        free((void *)S->interned_files[i]);
    }
    free(S->interned_files);
    for (i = 0; i < S->interned_var_strs_len; i++) {
        free((void *)S->interned_var_strs[i]);
    }
    free(S->interned_var_strs);
}

/* Free GC bookkeeping arrays + freelists. The actual heap (gc_all_young
 * / gc_all_old) is freed last, by state_free_heap. */
static void state_free_gc_aux(mino_state_t *S)
{
    int i;
    free(S->gc_ranges);
    free(S->gc_ranges_pending);
    free(S->gc_mark_stack);
    free(S->gc_remset);
    gc_evt_free(S);
    for (i = 0; i < 4; i++) {
        gc_hdr_t *h = S->gc_freelists[i];
        while (h != NULL) {
            gc_hdr_t *next = h->next;
            free(h);
            h = next;
        }
    }
    free(S->core_forms);
}

/* Free the structured-diagnostic chain plus the source-cache strings.
 * The diag may reference into source_cache, so it must run first. */
static void state_free_diag_state(mino_state_t *S)
{
    int sci;
    diag_free(mino_current_ctx(S)->last_diag);
    mino_current_ctx(S)->last_diag = NULL;
    for (sci = 0; sci < MINO_SOURCE_CACHE_SIZE; sci++) {
        free(S->source_cache[sci].text);
    }
}

/* Free the async-scheduler run queue and timer priority queue. */
static void state_free_async(mino_state_t *S)
{
    struct sched_entry *e = S->async_run_head;
    struct sched_entry *enext;
    while (e != NULL) {
        enext = e->next;
        free(e);
        e = enext;
    }
    async_timers_free(S);
}

/* Walk every live header on both generation lists, run any registered
 * finalizers, and free the underlying memory. Must run last because
 * earlier helpers may inspect heap-resident state (interned values,
 * meta map entries, etc.). */
static void state_free_heap(mino_state_t *S)
{
    gc_hdr_t *h, *hnext;
    for (h = S->gc_all_young; h != NULL; h = hnext) {
        hnext = h->next;
        if (h->type_tag == GC_T_VAL) {
            mino_val_t *v = (mino_val_t *)(h + 1);
            if (v->type == MINO_HANDLE && v->as.handle.finalizer != NULL) {
                v->as.handle.finalizer(v->as.handle.ptr, v->as.handle.tag);
            }
        }
        free(h);
    }
    for (h = S->gc_all_old; h != NULL; h = hnext) {
        hnext = h->next;
        if (h->type_tag == GC_T_VAL) {
            mino_val_t *v = (mino_val_t *)(h + 1);
            if (v->type == MINO_HANDLE && v->as.handle.finalizer != NULL) {
                v->as.handle.finalizer(v->as.handle.ptr, v->as.handle.tag);
            }
        }
        free(h);
    }
}

/* Tear down a state in the deterministic order each helper expects.
 * Order matters: registries that name heap-resident values release
 * the names before state_free_heap walks the heap; diag releases
 * source-cache references before the cache itself goes away; the
 * heap is always last. */
void mino_state_free(mino_state_t *S)
{
    if (S == NULL) {
        return;
    }
    /* Cycle G4.3: join every outstanding worker before tearing down
     * any heap state. Workers depend on S being live; freeing under
     * them would crash. */
    mino_host_threads_quiesce(S);
    state_free_root_envs(S);
    state_free_refs(S);
    state_free_ns_aliases(S);
    state_free_ns_env_table(S);
    free(S->var_registry);
    state_free_host_types(S);
    state_free_module_cache(S);
    state_free_bundled_libs(S);
    state_free_meta_table(S);
    state_free_intern_tables(S);
    state_free_record_types(S);
    state_free_gc_aux(S);
    state_free_diag_state(S);
    state_free_string_interns(S);
    state_free_async(S);
    state_free_heap(S);
    mino_state_lock_destroy(S);
    free(S);
}

/* ------------------------------------------------------------------------- */
/* Per-state PRNG (xorshift64*)                                              */
/* ------------------------------------------------------------------------- */

uint64_t state_rand64(mino_state_t *S)
{
    uint64_t x = S->rand_state;
    if (x == 0) {
        /* Seed from wall clock mixed with the state's address so two
         * states initialised in the same second get distinct streams. */
        x = (uint64_t)time(NULL);
        x ^= (uint64_t)(uintptr_t)S * 0x9E3779B97F4A7C15ULL;
        if (x == 0) x = 0x243F6A8885A308D3ULL; /* avoid degenerate zero */
    }
    x ^= x >> 12;
    x ^= x << 25;
    x ^= x >> 27;
    S->rand_state = x;
    return x * 0x2545F4914F6CDD1DULL;
}

/* ------------------------------------------------------------------------- */
/* Value retention (refs)                                                    */
/* ------------------------------------------------------------------------- */

mino_ref_t *mino_ref(mino_state_t *S, mino_val_t *val)
{
    mino_ref_t *r = (mino_ref_t *)calloc(1, sizeof(*r));
    if (r == NULL) {
        return NULL;
    }
    r->val  = val;
    r->prev = NULL;
    r->next = S->ref_roots;
    if (S->ref_roots != NULL) {
        S->ref_roots->prev = r;
    }
    S->ref_roots = r;
    return r;
}

mino_val_t *mino_deref(const mino_ref_t *ref)
{
    if (ref == NULL) {
        return NULL;
    }
    return ref->val;
}

void mino_unref(mino_state_t *S, mino_ref_t *ref)
{
    if (ref == NULL) {
        return;
    }
    if (ref->prev != NULL) {
        ref->prev->next = ref->next;
    } else {
        S->ref_roots = ref->next;
    }
    if (ref->next != NULL) {
        ref->next->prev = ref->prev;
    }
    free(ref);
}

/* ------------------------------------------------------------------------- */
/* Public eval entry points                                                  */
/* ------------------------------------------------------------------------- */

static mino_val_t *mino_eval_inner(mino_state_t *S, mino_val_t *form, mino_env_t *env)
{
    volatile char probe = 0;
    mino_val_t   *v;
    int           saved_try;
    saved_try = mino_current_ctx(S)->try_depth;
    gc_note_host_frame(S, (void *)&probe);
    (void)probe;
    mino_current_ctx(S)->eval_steps     = 0;
    mino_current_ctx(S)->limit_exceeded = 0;
    mino_current_ctx(S)->interrupted    = 0;
    mino_current_ctx(S)->trace_added    = 0;
    mino_current_ctx(S)->call_depth     = 0;

    /* Top-level try frame so that OOM and unhandled throw during eval
     * surface as a NULL return instead of aborting the process. */
    if (mino_current_ctx(S)->try_depth < MAX_TRY_DEPTH) {
        mino_current_ctx(S)->try_stack[mino_current_ctx(S)->try_depth].exception      = NULL;
        mino_current_ctx(S)->try_stack[mino_current_ctx(S)->try_depth].saved_ns       = S->current_ns;
        mino_current_ctx(S)->try_stack[mino_current_ctx(S)->try_depth].saved_ambient  = S->fn_ambient_ns;
        mino_current_ctx(S)->try_stack[mino_current_ctx(S)->try_depth].saved_load_len = S->load_stack_len;
        if (setjmp(mino_current_ctx(S)->try_stack[mino_current_ctx(S)->try_depth].buf) != 0) {
            /* Landed here from longjmp (OOM or uncaught throw). */
            mino_val_t *ex = mino_current_ctx(S)->try_stack[saved_try].exception;
            S->current_ns    = mino_current_ctx(S)->try_stack[saved_try].saved_ns;
            S->fn_ambient_ns = mino_current_ctx(S)->try_stack[saved_try].saved_ambient;
            load_stack_truncate(S, mino_current_ctx(S)->try_stack[saved_try].saved_load_len);
            mino_current_ctx(S)->try_depth = saved_try;
            if (mino_last_error(S) == NULL) {
                /* If the exception is a diagnostic map, extract its
                 * message for the error buffer. */
                if (ex != NULL && ex->type == MINO_MAP) {
                    mino_val_t *msg = map_get_val(ex,
                        mino_keyword(S, "mino/message"));
                    mino_val_t *kind = map_get_val(ex,
                        mino_keyword(S, "mino/kind"));
                    mino_val_t *code = map_get_val(ex,
                        mino_keyword(S, "mino/code"));
                    set_eval_diag(S, mino_current_ctx(S)->eval_current_form,
                        (kind && kind->type == MINO_KEYWORD)
                            ? kind->as.s.data : "internal",
                        (code && code->type == MINO_STRING)
                            ? code->as.s.data : "MIN001",
                        (msg && msg->type == MINO_STRING)
                            ? msg->as.s.data : "unhandled exception");
                } else if (ex != NULL && ex->type == MINO_STRING) {
                    char msg[512];
                    snprintf(msg, sizeof(msg), "unhandled exception: %.*s",
                             (int)ex->as.s.len, ex->as.s.data);
                    set_eval_diag(S, mino_current_ctx(S)->eval_current_form,
                                  "user", "MUS001", msg);
                } else {
                    set_eval_diag(S, mino_current_ctx(S)->eval_current_form,
                                  "internal", "MIN001",
                                  "unhandled exception");
                }
            }
            mino_current_ctx(S)->call_depth = 0;
            return NULL;
        }
        mino_current_ctx(S)->try_depth++;
    }

    v = eval(S, form, env);
    mino_current_ctx(S)->try_depth = saved_try;
    if (v == NULL) {
        append_trace(S);
        mino_current_ctx(S)->call_depth = 0;
        return NULL;
    }
    if (v->type == MINO_RECUR) {
        set_eval_diag(S, mino_current_ctx(S)->eval_current_form, "syntax", "MSY001", "recur must be in tail position");
        mino_current_ctx(S)->call_depth = 0;
        return NULL;
    }
    if (v->type == MINO_TAIL_CALL) {
        set_eval_diag(S, mino_current_ctx(S)->eval_current_form, "syntax", "MSY001", "tail call escaped to top level");
        mino_current_ctx(S)->call_depth = 0;
        return NULL;
    }
    mino_current_ctx(S)->call_depth = 0;
    /* Drain async scheduler run queue after each top-level eval. */
    async_sched_drain(S, env);
    return v;
}

/* Public eval entry: hold state_lock for the entire call when host
 * threads are active. Recursive lock so nested mino_call from
 * primitives is fine; lock_depth tracks recursion so future-deref
 * can yield + resume around a blocking cv_wait. */
mino_val_t *mino_eval(mino_state_t *S, mino_val_t *form, mino_env_t *env)
{
    mino_val_t *v;
    mino_lock(S);
    v = mino_eval_inner(S, form, env);
    mino_unlock(S);
    return v;
}

static mino_val_t *mino_eval_string_inner(mino_state_t *S, const char *src, mino_env_t *env)
{
    volatile char   probe = 0;
    mino_val_t     *last  = mino_nil(S);
    const char     *saved_file = S->reader_file;
    int             saved_line = S->reader_line;
    int             saved_try  = mino_current_ctx(S)->try_depth;
    gc_note_host_frame(S, (void *)&probe);
    (void)probe;
    mino_current_ctx(S)->eval_steps     = 0;
    mino_current_ctx(S)->limit_exceeded = 0;
    mino_current_ctx(S)->interrupted    = 0;
    if (S->reader_file == NULL) {
        S->reader_file = intern_filename(S, "<string>");
    }
    S->reader_line = 1;

    /* Top-level try frame so that OOM during read or eval surfaces as a
     * NULL return instead of aborting the process. */
    if (mino_current_ctx(S)->try_depth < MAX_TRY_DEPTH) {
        mino_current_ctx(S)->try_stack[mino_current_ctx(S)->try_depth].exception      = NULL;
        mino_current_ctx(S)->try_stack[mino_current_ctx(S)->try_depth].saved_ns       = S->current_ns;
        mino_current_ctx(S)->try_stack[mino_current_ctx(S)->try_depth].saved_ambient  = S->fn_ambient_ns;
        mino_current_ctx(S)->try_stack[mino_current_ctx(S)->try_depth].saved_load_len = S->load_stack_len;
        if (setjmp(mino_current_ctx(S)->try_stack[mino_current_ctx(S)->try_depth].buf) != 0) {
            mino_val_t *ex = mino_current_ctx(S)->try_stack[saved_try].exception;
            S->current_ns    = mino_current_ctx(S)->try_stack[saved_try].saved_ns;
            S->fn_ambient_ns = mino_current_ctx(S)->try_stack[saved_try].saved_ambient;
            load_stack_truncate(S, mino_current_ctx(S)->try_stack[saved_try].saved_load_len);
            mino_current_ctx(S)->try_depth   = saved_try;
            S->reader_file = saved_file;
            S->reader_line = saved_line;
            if (mino_last_error(S) == NULL) {
                /* Preserve the original exception message if available,
                 * and include the file being loaded for context. */
                const char *file = S->reader_file;
                if (ex != NULL && ex->type == MINO_STRING
                    && ex->as.s.len > 0) {
                    if (file != NULL && strcmp(file, "<string>") != 0
                        && strcmp(file, "<input>") != 0) {
                        char msg[512];
                        snprintf(msg, sizeof(msg), "in %s: %.*s",
                                 file, (int)ex->as.s.len, ex->as.s.data);
                        set_eval_diag(S, mino_current_ctx(S)->eval_current_form, "eval/contract", "MCT001", msg);
                    } else {
                        set_eval_diag(S, mino_current_ctx(S)->eval_current_form, "eval/contract", "MCT001",
                                      ex->as.s.data);
                    }
                } else {
                    set_eval_diag(S, mino_current_ctx(S)->eval_current_form, "eval/contract", "MCT001", "unhandled exception");
                }
            }
            mino_current_ctx(S)->call_depth = 0;
            return NULL;
        }
        mino_current_ctx(S)->try_depth++;
    }

    while (*src != '\0') {
        const char *end  = NULL;
        mino_val_t *form = mino_read(S, src, &end);
        if (form == NULL) {
            if (mino_last_error(S) != NULL) {
                mino_current_ctx(S)->try_depth   = saved_try;
                S->reader_file = saved_file;
                S->reader_line = saved_line;
                return NULL;
            }
            if (end != NULL && end > src) {
                src = end; /* reader conditional produced nothing; skip */
                continue;
            }
            break; /* EOF */
        }
        last = mino_eval(S, form, env);
        if (last == NULL) {
            mino_current_ctx(S)->try_depth   = saved_try;
            S->reader_file = saved_file;
            S->reader_line = saved_line;
            return NULL;
        }
        src = end;
    }
    mino_current_ctx(S)->try_depth   = saved_try;
    S->reader_file = saved_file;
    S->reader_line = saved_line;
    return last;
}

mino_val_t *mino_eval_string(mino_state_t *S, const char *src, mino_env_t *env)
{
    mino_val_t *v;
    mino_lock(S);
    v = mino_eval_string_inner(S, src, env);
    mino_unlock(S);
    return v;
}

mino_val_t *mino_load_file(mino_state_t *S, const char *path, mino_env_t *env)
{
    FILE  *f;
    char  *buf;
    long   sz;
    size_t rd;
    mino_val_t    *result;
    const char    *saved_file;
    if (path == NULL || env == NULL) {
        set_eval_diag(S, mino_current_ctx(S)->eval_current_form, "internal", "MIN001", "mino_load_file: NULL argument");
        return NULL;
    }
    f = fopen(path, "rb");
    if (f == NULL) {
        char msg[300];
        snprintf(msg, sizeof(msg), "cannot open file: %s", path);
        set_eval_diag(S, mino_current_ctx(S)->eval_current_form, "name", "MNS001", msg);
        return NULL;
    }
    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        set_eval_diag(S, mino_current_ctx(S)->eval_current_form, "host", "MHO001", "cannot seek to end of file");
        return NULL;
    }
    sz = ftell(f);
    if (sz < 0) {
        fclose(f);
        set_eval_diag(S, mino_current_ctx(S)->eval_current_form, "host", "MHO001", "cannot determine file size");
        return NULL;
    }
    if (fseek(f, 0, SEEK_SET) != 0) {
        fclose(f);
        set_eval_diag(S, mino_current_ctx(S)->eval_current_form, "host", "MHO001", "cannot seek to start of file");
        return NULL;
    }
    buf = (char *)malloc((size_t)sz + 1);
    if (buf == NULL) {
        fclose(f);
        set_eval_diag(S, mino_current_ctx(S)->eval_current_form, "internal", "MIN001", "out of memory loading file");
        return NULL;
    }
    rd = fread(buf, 1, (size_t)sz, f);
    fclose(f);
    if (rd != (size_t)sz) {
        free(buf);
        set_eval_diag(S, mino_current_ctx(S)->eval_current_form, "host", "MHO001", "short read loading file");
        return NULL;
    }
    buf[rd] = '\0';
    saved_file  = S->reader_file;
    S->reader_file = intern_filename(S, path);
    source_cache_store(S, S->reader_file, buf, rd);
    result = mino_eval_string(S, buf, env);
    S->reader_file = saved_file;
    free(buf);
    return result;
}

mino_env_t *mino_new(mino_state_t *S)
{
    mino_env_t *env = mino_env_new(S);
    mino_install_core(S, env);
    mino_install_io(S, env);
    return env;
}

void mino_register_fn(mino_state_t *S, mino_env_t *env, const char *name, mino_prim_fn fn)
{
    mino_env_set(S, env, name, mino_prim(S, name, fn));
}

mino_val_t *mino_call(mino_state_t *S, mino_val_t *fn, mino_val_t *args, mino_env_t *env)
{
    volatile char probe = 0;
    mino_val_t   *result;
    mino_lock(S);
    gc_note_host_frame(S, (void *)&probe);
    (void)probe;
    result = apply_callable(S, fn, args, env);
    mino_unlock(S);
    return result;
}

int mino_pcall(mino_state_t *S, mino_val_t *fn, mino_val_t *args, mino_env_t *env,
               mino_val_t **out)
{
    int saved_try = mino_current_ctx(S)->try_depth;
    mino_val_t *result;

    if (mino_current_ctx(S)->try_depth >= MAX_TRY_DEPTH) {
        if (out != NULL) {
            *out = NULL;
        }
        return -1;
    }

    mino_current_ctx(S)->try_stack[mino_current_ctx(S)->try_depth].exception      = NULL;
    mino_current_ctx(S)->try_stack[mino_current_ctx(S)->try_depth].saved_ns       = S->current_ns;
    mino_current_ctx(S)->try_stack[mino_current_ctx(S)->try_depth].saved_ambient  = S->fn_ambient_ns;
    mino_current_ctx(S)->try_stack[mino_current_ctx(S)->try_depth].saved_load_len = S->load_stack_len;
    if (setjmp(mino_current_ctx(S)->try_stack[mino_current_ctx(S)->try_depth].buf) != 0) {
        /* Landed here from longjmp -- error was thrown. */
        mino_val_t *ex = mino_current_ctx(S)->try_stack[saved_try].exception;
        S->current_ns    = mino_current_ctx(S)->try_stack[saved_try].saved_ns;
        S->fn_ambient_ns = mino_current_ctx(S)->try_stack[saved_try].saved_ambient;
        load_stack_truncate(S, mino_current_ctx(S)->try_stack[saved_try].saved_load_len);
        mino_current_ctx(S)->try_depth = saved_try;
        /* Populate last_error from the exception value so the host
         * can inspect it via mino_last_error(). */
        if (mino_last_error(S) == NULL || mino_last_error(S)[0] == '\0') {
            const char *s = NULL;
            size_t slen = 0;
            if (ex != NULL && mino_to_string(ex, &s, &slen)) {
                set_eval_diag(S, mino_current_ctx(S)->eval_current_form, "eval/contract", "MCT001", s);
            } else {
                set_eval_diag(S, mino_current_ctx(S)->eval_current_form, "eval/contract", "MCT001", "unhandled exception");
            }
        }
        if (out != NULL) {
            *out = NULL;
        }
        return -1;
    }
    mino_current_ctx(S)->try_depth++;

    result = mino_call(S, fn, args, env);
    mino_current_ctx(S)->try_depth = saved_try;

    if (out != NULL) {
        *out = result;
    }
    return result == NULL ? -1 : 0;
}

/* ------------------------------------------------------------------------- */
/* Execution limits, fault injection, interrupt                              */
/* ------------------------------------------------------------------------- */

void mino_set_limit(mino_state_t *S, int kind, size_t value)
{
    switch (kind) {
    case MINO_LIMIT_STEPS: S->limit_steps = value; break;
    case MINO_LIMIT_HEAP:  S->limit_heap  = value; break;
    default: break;
    }
}

void mino_set_fail_alloc_at(mino_state_t *S, long n)
{
    S->fi_alloc_countdown = n;
}

void mino_set_fail_raw_at(mino_state_t *S, long n)
{
    S->fi_raw_countdown = n;
}

int mino_fi_should_fail_raw(mino_state_t *S)
{
    if (S->fi_raw_countdown > 0) {
        S->fi_raw_countdown--;
        if (S->fi_raw_countdown == 0) {
            return 1;
        }
    }
    return 0;
}

void mino_interrupt(mino_state_t *S)
{
    mino_current_ctx(S)->interrupted = 1;
}

/* ------------------------------------------------------------------------- */
/* Host-thread grant (Cycle G4 foundation)                                   */
/* ------------------------------------------------------------------------- */

void mino_set_thread_limit(mino_state_t *S, int n)
{
    if (S == NULL) { return; }
    if (n < 0) { n = 0; }
    S->thread_limit = n;
}

int mino_get_thread_limit(mino_state_t *S)
{
    if (S == NULL) { return 1; }
    return S->thread_limit;
}

int mino_thread_count(mino_state_t *S)
{
    if (S == NULL) { return 0; }
    return S->thread_count;
}

void mino_quiesce_threads(mino_state_t *S)
{
    /* Walk S->future_list_head and join every worker that hasn't been
     * joined yet. Cycle G4.3 added the real implementation in
     * runtime/host_threads.c; mino_state_free calls this before tearing
     * down the heap so workers don't run after free. Embedders also
     * call this directly when they need to wait for in-flight futures
     * before doing other work. */
    if (S == NULL) { return; }
    mino_host_threads_quiesce(S);
}

void mino_set_thread_pool(mino_state_t *S, mino_thread_pool_t *pool)
{
    if (S == NULL) { return; }
    S->thread_pool = pool;
}

void mino_set_thread_factory(mino_state_t *S,
                             mino_thread_lifecycle_fn start_fn,
                             mino_thread_lifecycle_fn end_fn,
                             void *ctx)
{
    if (S == NULL) { return; }
    S->thread_start_fn     = start_fn;
    S->thread_end_fn       = end_fn;
    S->thread_factory_ctx  = ctx;
}

void mino_set_thread_stack_size(mino_state_t *S, size_t n)
{
    if (S == NULL) { return; }
    S->thread_stack_size = n;
}

/* ------------------------------------------------------------------------- */
/* Safepoint and STW (Cycle G4.2).                                           */
/*                                                                           */
/* `mino_safepoint_poll` is the inline fast path defined in internal.h; it  */
/* checks `ctx->should_yield` and falls through to mino_safepoint_park when */
/* set. The collector calls `gc_request_stw` to ask every worker to park,  */
/* then `gc_release_stw` to wake them after the sweep. Single-threaded     */
/* today: there's exactly one ctx (S->main_ctx) and the GC runs on the     */
/* same thread that drove the alloc, so park/release are O(1) flag toggles  */
/* with no contention. Cycle G4 later sub-cycles add the multi-worker walk. */
/* ------------------------------------------------------------------------- */

void mino_safepoint_park(mino_state_t *S)
{
    /* Single-threaded path: should_yield is set and cleared by
     * gc_request_stw / gc_release_stw on the same thread, so the
     * only way to reach this slow path is if release-then-set raced
     * or a host driver flipped the flag between sweeps. Clear the
     * local flag and return — no other thread to coordinate with.
     * Cycle G4 later sub-cycles replace this body with a
     * condition-variable wait keyed to S->stw_request. */
    if (S == NULL) { return; }
    mino_current_ctx(S)->should_yield = 0;
}

void gc_request_stw(mino_state_t *S)
{
    if (S == NULL) { return; }
    S->stw_request = 1;
    /* Single-threaded today: only S->main_ctx exists. Cycle G4 later
     * sub-cycles walk the worker set and set should_yield on each.
     * The calling thread is the mutator-and-collector both, so it's
     * already at the safepoint by definition; setting should_yield
     * on its current ctx is a formal record, not a wake signal. */
    mino_current_ctx(S)->should_yield = 1;
}

void gc_release_stw(mino_state_t *S)
{
    if (S == NULL) { return; }
    S->stw_request = 0;
    mino_current_ctx(S)->should_yield = 0;
}

/* ------------------------------------------------------------------------- */
/* Per-state lock (Cycle G4.3).                                              */
/*                                                                            */
/* Initialized at state_init unconditionally so any caller can take it       */
/* whether or not the host has granted threads. The mino_lock / mino_unlock  */
/* macros gate on S->multi_threaded so single-threaded states pay no         */
/* lock/unlock cost; only states with active worker threads serialize.       */
/* ------------------------------------------------------------------------- */

#if defined(_WIN32) && defined(_MSC_VER)
#  include <windows.h>
void mino_state_lock_init(mino_state_t *S)
{
    CRITICAL_SECTION *cs = (CRITICAL_SECTION *)calloc(1, sizeof(*cs));
    if (cs == NULL) { abort(); }
    InitializeCriticalSection(cs);
    S->state_lock = cs;
}
void mino_state_lock_destroy(mino_state_t *S)
{
    CRITICAL_SECTION *cs = (CRITICAL_SECTION *)S->state_lock;
    if (cs != NULL) {
        DeleteCriticalSection(cs);
        free(cs);
        S->state_lock = NULL;
    }
}
void mino_state_lock_acquire(mino_state_t *S)
{
    EnterCriticalSection((CRITICAL_SECTION *)S->state_lock);
}
void mino_state_lock_release(mino_state_t *S)
{
    LeaveCriticalSection((CRITICAL_SECTION *)S->state_lock);
}
#else
void mino_state_lock_init(mino_state_t *S)
{
    pthread_mutexattr_t attr;
    pthread_mutexattr_init(&attr);
    /* Recursive: gc_alloc_typed is the most heavily-locked critical
     * section, and its OOM fallback path calls gc_major_collect which
     * itself can allocate. The same thread re-acquiring is correct;
     * non-recursive would deadlock against the same thread. */
    pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE);
    pthread_mutex_init(&S->state_lock, &attr);
    pthread_mutexattr_destroy(&attr);
}
void mino_state_lock_destroy(mino_state_t *S)
{
    pthread_mutex_destroy(&S->state_lock);
}
void mino_state_lock_acquire(mino_state_t *S)
{
    pthread_mutex_lock(&S->state_lock);
}
void mino_state_lock_release(mino_state_t *S)
{
    pthread_mutex_unlock(&S->state_lock);
}
#endif

/* Drop the current thread's lock holding to zero so a blocking wait
 * (e.g. cv_wait inside future_deref) doesn't starve other workers.
 * Returns the saved depth; the caller must call mino_resume_lock
 * with that value when it's safe to re-take. */
int mino_yield_lock(mino_state_t *S)
{
    int depth = mino_current_ctx(S)->lock_depth;
    while (mino_current_ctx(S)->lock_depth > 0) {
        mino_current_ctx(S)->lock_depth--;
        mino_state_lock_release(S);
    }
    return depth;
}

void mino_resume_lock(mino_state_t *S, int saved_depth)
{
    while (saved_depth-- > 0) {
        mino_state_lock_acquire(S);
        mino_current_ctx(S)->lock_depth++;
    }
}

/* ------------------------------------------------------------------------- */
/* In-process REPL handle                                                    */
/* ------------------------------------------------------------------------- */

struct mino_repl {
    mino_state_t *state;
    mino_env_t   *env;
    char         *buf;
    size_t        len;
    size_t        cap;
};

mino_repl_t *mino_repl_new(mino_state_t *S, mino_env_t *env)
{
    mino_repl_t *r = (mino_repl_t *)malloc(sizeof(*r));
    if (r == NULL) { return NULL; }
    r->state = S;
    r->env   = env;
    r->buf   = NULL;
    r->len   = 0;
    r->cap   = 0;
    return r;
}

static int repl_is_whitespace(const char *s)
{
    while (*s) {
        unsigned char c = (unsigned char)*s++;
        if (c != ' ' && c != '\t' && c != '\n' && c != '\r' && c != ',') {
            return 0;
        }
    }
    return 1;
}

int mino_repl_feed(mino_repl_t *repl, const char *line, mino_val_t **out)
{
    mino_state_t  *S;
    size_t         add;
    const char    *cursor;
    const char    *end;
    mino_val_t    *form;
    mino_val_t    *result;

    if (out != NULL) { *out = NULL; }
    if (repl == NULL) { return MINO_REPL_ERROR; }
    S = repl->state;

    /* Append the line to the buffer. */
    add = (line != NULL) ? strlen(line) : 0;
    if (repl->len + add + 1 > repl->cap) {
        size_t new_cap = repl->cap == 0 ? 256 : repl->cap;
        char  *nb;
        while (new_cap < repl->len + add + 1) { new_cap *= 2; }
        nb = (char *)realloc(repl->buf, new_cap);
        if (nb == NULL) {
            set_eval_diag(S, mino_current_ctx(S)->eval_current_form, "internal", "MIN001", "repl: out of memory");
            return MINO_REPL_ERROR;
        }
        repl->buf = nb;
        repl->cap = new_cap;
    }
    if (add > 0) {
        memcpy(repl->buf + repl->len, line, add);
    }
    repl->len += add;
    repl->buf[repl->len] = '\0';

    /* If buffer is only whitespace, need more input. */
    if (repl_is_whitespace(repl->buf)) {
        return MINO_REPL_MORE;
    }

    /* Try to read a form. */
    cursor = repl->buf;
    end    = repl->buf;
    form   = mino_read(S, cursor, &end);
    if (form == NULL) {
        const char *err = mino_last_error(S);
        if (err != NULL && strstr(err, "unterminated") != NULL) {
            return MINO_REPL_MORE;
        }
        /* Hard parse error -- reset buffer. */
        repl->len = 0;
        repl->buf[0] = '\0';
        return MINO_REPL_ERROR;
    }

    /* Shift remaining bytes to the front. */
    {
        size_t consumed  = (size_t)(end - repl->buf);
        size_t remaining = repl->len - consumed;
        memmove(repl->buf, end, remaining + 1);
        repl->len = remaining;
    }

    /* Evaluate the form. */
    result = mino_eval(S, form, repl->env);
    if (result == NULL) {
        return MINO_REPL_ERROR;
    }
    if (out != NULL) { *out = result; }
    return MINO_REPL_OK;
}

void mino_repl_free(mino_repl_t *repl)
{
    if (repl == NULL) { return; }
    free(repl->buf);
    free(repl);
}

long long mino_monotonic_ns(void)
{
#if defined(_WIN32)
    LARGE_INTEGER freq, count;
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&count);
    return (long long)(count.QuadPart * 1000000000LL / freq.QuadPart);
#elif defined(CLOCK_MONOTONIC)
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000000000LL + (long long)ts.tv_nsec;
#else
    return (long long)((double)clock() / (double)CLOCKS_PER_SEC * 1e9);
#endif
}
