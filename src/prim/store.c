/*
 * store.c -- EAVT fact store connection (MINO_STORE).
 *
 * A store connection is an identity cell wrapping an immutable db
 * value (a persistent map) plus optional host-owned durability state
 * (a malloc'd mino_store_handle carrying the snapshot path and clock).
 * Durability is snapshot-on-checkpoint only for v1: checkpoint writes
 * the current db value to handle->path via mino_print_to; close
 * flushes and releases the handle. There is no WAL.
 *
 * The store is per-state isolated: cross-state access throws MST007,
 * matching the agent/ref pattern. publish is the raw write barrier +
 * val slot update (peer to mino_atom_reset); store-commit* wraps it
 * with watch dispatch. The handle is malloc-owned (not GC-traced);
 * mino_store_gc_finalize releases it on GC sweep if the host did not
 * call close explicitly.
 */

#include "prim/internal.h"
#include "mino.h"
#include "eval/internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* --- host-owned handle ---------------------------------------------------- */

typedef struct mino_store_handle {
    char                *path;       /* NULL for in-memory stores */
    mino_store_clock_fn  clock;      /* NULL = wall-clock ms */
    void                *clock_ctx;
} mino_store_handle;

/* Duplicate a NUL-terminated C string into freshly malloc'd storage.
 * strdup is not C99; this keeps the TU -std=c99 -Wpedantic clean. */
static char *store_dup_path(const char *s)
{
    size_t n = strlen(s);
    char  *out = (char *)malloc(n + 1);
    if (out == NULL) return NULL;
    memcpy(out, s, n + 1);
    return out;
}

/* --- public-API constructor + predicate ----------------------------------- */

mino_val *mino_store_val(mino_state *S, mino_val *db_val,
                           const char *path,
                           mino_store_clock_fn clock,
                           void *clock_ctx)
{
    mino_val *v = alloc_val(S, MINO_STORE);
    v->as.store.val          = db_val;
    v->as.store.watches      = NULL;
    v->as.store.owning_state = S;
    v->as.store.store_id     = (uint64_t)(uintptr_t)v;
    if (path != NULL) {
        mino_store_handle *h = (mino_store_handle *)malloc(sizeof(*h));
        char              *pcopy;
        if (h == NULL) {
            gc_oom_throw(S, "store: out of memory");
            return NULL;
        }
        pcopy = store_dup_path(path);
        if (pcopy == NULL) {
            free(h);
            gc_oom_throw(S, "store: out of memory");
            return NULL;
        }
        h->path      = pcopy;
        h->clock     = clock;
        h->clock_ctx = clock_ctx;
        v->as.store.handle = h;
    } else {
        v->as.store.handle = NULL;
    }
    return v;
}

int mino_is_store(const mino_val *v)
{
    return v != NULL && mino_type_of(v) == MINO_STORE;
}

mino_val *mino_store_deref(const mino_val *v)
{
    if (v == NULL || mino_type_of(v) != MINO_STORE) return NULL;
    return v->as.store.val;
}

/* Cross-state defense, mirroring agent_check_state. Throws MST007 if
 * the store was allocated in a different state than S. Returns 0 on
 * match, 1 on mismatch (caller should propagate NULL). */
static int store_check_state(mino_state *S, mino_val *store)
{
    if (store->as.store.owning_state != S) {
        prim_throw_classified(S, "eval/state", "MST007",
            "store from foreign state");
        return 1;
    }
    return 0;
}

/* Notify all watches after a state change. Callback signature:
 * (fn key store old-state new-state). Returns -1 if any watch threw
 * (the exception propagates per Clojure JVM semantics), 0 otherwise.
 * Mirrors atom_notify_watches in prim/stateful.c. */
static int store_notify_watches(mino_state *S, mino_val *store,
                                 mino_val *old_val, mino_val *new_val,
                                 mino_env *env)
{
    mino_val *watches = store->as.store.watches;
    size_t i, len;
    if (watches == NULL || mino_type_of(watches) != MINO_MAP
        || watches->as.map.len == 0)
        return 0;
    len = watches->as.map.len;
    for (i = 0; i < len; i++) {
        mino_val *key = vec_nth(watches->as.map.key_order, i);
        mino_val *fn  = map_get_val(watches, key);
        mino_val *wargs;
        if (fn == NULL) continue;
        wargs = mino_cons(S, key,
                  mino_cons(S, store,
                    mino_cons(S, old_val,
                      mino_cons(S, new_val, mino_nil(S)))));
        if (mino_call(S, fn, wargs, env) == NULL) return -1;
    }
    return 0;
}

/* --- publish (write barrier + set val) ------------------------------------ */

mino_val *mino_store_publish(mino_state *S, mino_val *conn,
                               mino_val *new_db)
{
    if (!mino_is_store(conn)) {
        prim_throw_classified(S, "eval/type", "MTY001",
            "store-publish requires a store connection");
        return NULL;
    }
    if (store_check_state(S, conn)) return NULL;
    gc_write_barrier(S, conn, conn->as.store.val, new_db);
    conn->as.store.val = new_db;
    return new_db;
}

/* --- C API: open / checkpoint / close ------------------------------------- */

/* Read a snapshot file as EDN. Returns the parsed db value, or NULL
 * if the file does not exist or cannot be read/parsed. Used by both
 * the C API mino_store_open and the store-open* primitive. */
static mino_val *store_read_snapshot(mino_state *S, const char *path,
                                        mino_env *env)
{
    FILE     *f;
    long       sz;
    char      *buf;
    mino_val *db;
    if (path == NULL) return NULL;
    f = fopen(path, "rb");
    if (f == NULL) return NULL;
    fseek(f, 0, SEEK_END);
    sz = ftell(f);
    if (sz < 0) { fclose(f); return NULL; }
    fseek(f, 0, SEEK_SET);
    buf = (char *)malloc((size_t)sz + 1);
    if (buf == NULL) { fclose(f); return NULL; }
    if (sz > 0) {
        size_t got = fread(buf, 1, (size_t)sz, f);
        if (got != (size_t)sz) { free(buf); fclose(f); return NULL; }
    }
    buf[sz] = '\0';
    fclose(f);
    db = mino_eval_string(S, buf, env);
    free(buf);
    return db;
}

mino_val *mino_store_open(mino_state *S, const char *path,
                           mino_store_clock_fn clock, void *clock_ctx)
{
    mino_env  *env = ns_env_ensure(S, "clojure.core");
    mino_val *db  = NULL;
    if (path != NULL) {
        db = store_read_snapshot(S, path, env);
    }
    if (db == NULL) {
        db = mino_eval_string(S, "{:entities {} :log [] :tx 0}", env);
        if (db == NULL) return NULL;
    }
    return mino_store_val(S, db, path, clock, clock_ctx);
}

int mino_store_checkpoint(mino_state *S, mino_val *conn)
{
    mino_store_handle *h;
    FILE              *f;
    if (!mino_is_store(conn)) {
        prim_throw_classified(S, "eval/type", "MTY001",
            "store-checkpoint requires a store connection");
        return -1;
    }
    if (store_check_state(S, conn)) return -1;
    h = (mino_store_handle *)conn->as.store.handle;
    if (h == NULL || h->path == NULL) return 0;
    f = fopen(h->path, "wb");
    if (f == NULL) {
        set_eval_diag(S, NULL, "io", "MIO001",
            "store-checkpoint: cannot open file for writing");
        return -1;
    }
    mino_print_to(S, f, conn->as.store.val);
    fclose(f);
    return 0;
}

int mino_store_close(mino_state *S, mino_val *conn)
{
    mino_store_handle *h;
    if (!mino_is_store(conn)) {
        prim_throw_classified(S, "eval/type", "MTY001",
            "store-close requires a store connection");
        return -1;
    }
    if (store_check_state(S, conn)) return -1;
    h = (mino_store_handle *)conn->as.store.handle;
    if (h == NULL) return 0;
    mino_store_checkpoint(S, conn);
    free(h->path);
    free(h);
    conn->as.store.handle = NULL;
    return 0;
}

/* --- GC sweep finalizer --------------------------------------------------- */

/* Release the malloc'd handle if the host did not call close. Called
 * from values/gc_handlers.c finalize_val_impl on GC sweep and state
 * teardown. The db value is not flushed here -- the host should
 * checkpoint explicitly; this path is best-effort resource release. */
void mino_store_gc_finalize(mino_val *v)
{
    mino_store_handle *h = (mino_store_handle *)v->as.store.handle;
    if (h == NULL) return;
    free(h->path);
    free(h);
    v->as.store.handle = NULL;
}

/* --- primitives ----------------------------------------------------------- */

/* (store-open* initial-db path) -> store connection */
static mino_val *prim_store_open(mino_state *S, mino_val *args,
                                     mino_env *env)
{
    mino_val   *db;
    mino_val   *path_val;
    const char *path_str = NULL;
    if (!mino_is_cons(args) || !mino_is_cons(args->as.cons.cdr)
        || mino_is_cons(args->as.cons.cdr->as.cons.cdr)) {
        return prim_throw_classified(S, "eval/arity", "MAR001",
            "store-open* requires two arguments");
    }
    db       = args->as.cons.car;
    path_val = args->as.cons.cdr->as.cons.car;
    if (path_val != NULL && mino_type_of(path_val) != MINO_NIL) {
        if (mino_type_of(path_val) != MINO_STRING) {
            return prim_throw_classified(S, "eval/type", "MTY001",
                "store-open*: path must be a string or nil");
        }
        path_str = path_val->as.s.data;
    }
    if (path_str != NULL) {
        mino_val *snapshot = store_read_snapshot(S, path_str, env);
        if (snapshot != NULL) db = snapshot;
    }
    return mino_store_val(S, db, path_str, NULL, NULL);
}

/* (store-commit* conn new-db) -> new-db (publish + watches) */
static mino_val *prim_store_commit(mino_state *S, mino_val *args,
                                      mino_env *env)
{
    mino_val *conn;
    mino_val *new_db;
    mino_val *old_val;
    if (!mino_is_cons(args) || !mino_is_cons(args->as.cons.cdr)
        || mino_is_cons(args->as.cons.cdr->as.cons.cdr)) {
        return prim_throw_classified(S, "eval/arity", "MAR001",
            "store-commit* requires two arguments");
    }
    conn   = args->as.cons.car;
    new_db = args->as.cons.cdr->as.cons.car;
    if (!mino_is_store(conn)) {
        return prim_throw_classified(S, "eval/type", "MTY001",
            "store-commit* requires a store connection");
    }
    if (store_check_state(S, conn)) return NULL;
    old_val = conn->as.store.val;
    if (mino_store_publish(S, conn, new_db) == NULL) return NULL;
    store_notify_watches(S, conn, old_val, new_db, env);
    return new_db;
}

/* (store-checkpoint* conn) -> nil */
static mino_val *prim_store_checkpoint(mino_state *S, mino_val *args,
                                          mino_env *env)
{
    mino_val *conn;
    (void)env;
    if (!mino_is_cons(args) || mino_is_cons(args->as.cons.cdr)) {
        return prim_throw_classified(S, "eval/arity", "MAR001",
            "store-checkpoint* requires one argument");
    }
    conn = args->as.cons.car;
    if (!mino_is_store(conn)) {
        return prim_throw_classified(S, "eval/type", "MTY001",
            "store-checkpoint* requires a store connection");
    }
    if (store_check_state(S, conn)) return NULL;
    if (mino_store_checkpoint(S, conn) != 0) return NULL;
    return mino_nil(S);
}

/* (store-close* conn) -> nil */
static mino_val *prim_store_close(mino_state *S, mino_val *args,
                                     mino_env *env)
{
    mino_val *conn;
    (void)env;
    if (!mino_is_cons(args) || mino_is_cons(args->as.cons.cdr)) {
        return prim_throw_classified(S, "eval/arity", "MAR001",
            "store-close* requires one argument");
    }
    conn = args->as.cons.car;
    if (!mino_is_store(conn)) {
        return prim_throw_classified(S, "eval/type", "MTY001",
            "store-close* requires a store connection");
    }
    if (store_check_state(S, conn)) return NULL;
    if (mino_store_close(S, conn) != 0) return NULL;
    return mino_nil(S);
}

/* (store? x) -> bool */
static mino_val *prim_store_p(mino_state *S, mino_val *args,
                                 mino_env *env)
{
    mino_val *x;
    (void)env;
    if (!mino_is_cons(args) || mino_is_cons(args->as.cons.cdr)) {
        return prim_throw_classified(S, "eval/arity", "MAR001",
            "store? requires one argument");
    }
    x = args->as.cons.car;
    return mino_is_store(x) ? mino_true(S) : mino_false(S);
}

/* (store-clock* conn) -> instant (long long) */
static mino_val *prim_store_clock(mino_state *S, mino_val *args,
                                     mino_env *env)
{
    mino_val          *conn;
    mino_store_handle *h;
    long long          now;
    (void)env;
    if (!mino_is_cons(args) || mino_is_cons(args->as.cons.cdr)) {
        return prim_throw_classified(S, "eval/arity", "MAR001",
            "store-clock* requires one argument");
    }
    conn = args->as.cons.car;
    /* nil means "no connection": the pure (store/with ...) variant has no
       conn to read a clock from, so fall back to the wall clock, mirroring
       the NULL-clock path below. Any other non-store value is a type error. */
    if (mino_is_nil(conn)) {
        return mino_int(S, mino_monotonic_ns() / 1000000);
    }
    if (!mino_is_store(conn)) {
        return prim_throw_classified(S, "eval/type", "MTY001",
            "store-clock* requires a store connection");
    }
    if (store_check_state(S, conn)) return NULL;
    h = (mino_store_handle *)conn->as.store.handle;
    if (h != NULL && h->clock != NULL) {
        now = h->clock(h->clock_ctx);
    } else {
        now = mino_monotonic_ns() / 1000000;
    }
    return mino_int(S, now);
}

/* --- primitive table + install hook --------------------------------------- */

const mino_prim_def k_prims_store[] = {
    {"store-open*",       prim_store_open,
     "Create a store connection from an initial db value and optional path."},
    {"store-commit*",     prim_store_commit,
     "Publish a new db value to the store and fire watches."},
    {"store-checkpoint*", prim_store_checkpoint,
     "Flush the store's db value to disk if durable."},
    {"store-close*",      prim_store_close,
     "Close the store, flushing if durable."},
    {"store?",            prim_store_p,
     "Return true if x is a store connection."},
    {"store-clock*",      prim_store_clock,
     "Return the current instant from the store's clock."},
};

const size_t k_prims_store_count = sizeof(k_prims_store)
                                   / sizeof(k_prims_store[0]);

void mino_install_store(mino_state *S, mino_env *env)
{
    mino_env *core_env = ns_env_ensure(S, "clojure.core");
    (void)env;
    prim_install_table_with_capability(S, core_env, "clojure.core",
                                       k_prims_store, k_prims_store_count,
                                       "store");
    mino_install_mino_store(S, env);
    S->caps_installed |= MINO_CAP_STORE;
}
