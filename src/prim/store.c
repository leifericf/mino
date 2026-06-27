/*
 * store.c -- EAVT fact store connection (MINO_STORE).
 *
 * A store connection is an identity cell wrapping an immutable db
 * value (a persistent map) plus optional host-owned durability state
 * (a malloc'd mino_store_handle carrying the snapshot path and clock).
 *
 * Durability uses snapshot + WAL (see ADR 11). Snapshot files carry a
 * 1-byte format-version header (0x00 = EDN text). The WAL at
 * <path>.wal is line-delimited EDN, one tx-info map per line.
 * store-commit* appends to the WAL before publishing when given a
 * tx-info argument. Checkpoint writes the snapshot and deletes the
 * WAL. On open, the snapshot is read, then the WAL is replayed entry
 * by entry (torn-write recovery: stop at first parse failure).
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

/* --- WAL path / append / read --------------------------------------------- */

/* Snapshot format version byte. 0x00 = EDN text follows. No other
 * value is assigned; the read path treats any non-0x00 first byte as
 * a headerless (v1) snapshot and parses the entire file as EDN. */
#define STORE_SNAPSHOT_VERSION  0x00

/* Construct the WAL path "<snap_path>.wal" in fresh malloc'd storage.
 * Returns NULL if snap_path is NULL or allocation fails. Caller frees. */
static char *store_wal_path(const char *snap_path)
{
    size_t plen;
    char  *wal;
    if (snap_path == NULL) return NULL;
    plen = strlen(snap_path);
    wal = (char *)malloc(plen + 5);          /* ".wal" + NUL */
    if (wal == NULL) return NULL;
    memcpy(wal, snap_path, plen);
    memcpy(wal + plen, ".wal", 5);
    return wal;
}

/* Construct the atomic-write temp path "<snap_path>.tmp" in fresh
 * malloc'd storage. Checkpoint writes the snapshot here first, then
 * renames it into place so a crash mid-write cannot corrupt the
 * canonical snapshot. Returns NULL on allocation failure. Caller frees. */
static char *store_tmp_path(const char *snap_path)
{
    size_t plen;
    char  *tmp;
    if (snap_path == NULL) return NULL;
    plen = strlen(snap_path);
    tmp = (char *)malloc(plen + 5);          /* ".tmp" + NUL */
    if (tmp == NULL) return NULL;
    memcpy(tmp, snap_path, plen);
    memcpy(tmp + plen, ".tmp", 5);
    return tmp;
}

/* Append tx-info as one line of EDN to the WAL at <snap_path>.wal.
 * The file is opened in append mode (created if absent), written, and
 * closed per call — no persistent FILE* in the handle, avoiding fd
 * exhaustion when many stores are open. Returns 0 on success, -1 on
 * I/O failure (diagnostic set). */
static int store_wal_append(mino_state *S, const char *snap_path,
                             mino_val *tx_info)
{
    char *wal_path;
    FILE *f;
    wal_path = store_wal_path(snap_path);
    if (wal_path == NULL) {
        gc_oom_throw(S, "store: out of memory");
        return -1;
    }
    f = fopen(wal_path, "ab");
    free(wal_path);
    if (f == NULL) {
        set_eval_diag(S, NULL, "io", "MIO001",
            "store: cannot open WAL for append");
        return -1;
    }
    mino_print_to(S, f, tx_info);
    fputc('\n', f);
    fclose(f);
    return 0;
}

/* Read the WAL at <snap_path>.wal and return a vector of parsed
 * tx-info maps. Returns nil if the file does not exist. On torn-write
 * (a line that fails to parse), parsing stops and entries read so far
 * are returned — the malformed trailing line is silently dropped. */
static mino_val *store_wal_read(mino_state *S, const char *snap_path,
                                 mino_env *env)
{
    char              *wal_path;
    FILE              *f;
    long                sz;
    char              *buf;
    char              *line_start;
    char              *p;
    mino_vec_builder  *vb;
    mino_val          *result;

    wal_path = store_wal_path(snap_path);
    if (wal_path == NULL) return mino_nil(S);
    f = fopen(wal_path, "rb");
    free(wal_path);
    if (f == NULL) return mino_nil(S);

    fseek(f, 0, SEEK_END);
    sz = ftell(f);
    if (sz < 0) { fclose(f); return mino_nil(S); }
    fseek(f, 0, SEEK_SET);
    buf = (char *)malloc((size_t)sz + 1);
    if (buf == NULL) { fclose(f); gc_oom_throw(S, "store: oom"); return NULL; }
    if (sz > 0) {
        size_t got = fread(buf, 1, (size_t)sz, f);
        if (got != (size_t)sz) { free(buf); fclose(f); return mino_nil(S); }
    }
    buf[sz] = '\0';
    fclose(f);

    vb = mino_vector_builder_new(S);
    line_start = buf;
    for (p = buf; ; p++) {
        if (*p == '\n' || *p == '\0') {
            size_t llen = (size_t)(p - line_start);
            if (llen > 0) {
                char saved = *p;
                char *trim = line_start;
                mino_val *entry = NULL;
                mino_val *err_ex = NULL;
                int rc;
                *p = '\0';
                while (*trim == ' ' || *trim == '\t' || *trim == '\r') trim++;
                if (*trim != '\0') {
                    rc = mino_eval_string_ex(S, trim, env, &entry, &err_ex);
                    if (rc == 0 && entry != NULL) {
                        mino_vector_builder_push(vb, entry);
                    } else {
                        /* Torn write: stop here, keep what we have */
                        *p = saved;
                        break;
                    }
                }
                *p = saved;
            }
            line_start = p + 1;
            if (*p == '\0') break;
        }
    }
    free(buf);
    result = mino_vector_builder_finish(vb);
    return result;
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

/* Read a snapshot file as EDN. The file may carry a 1-byte version
 * header (0x00 = EDN text); if the first byte is 0x00, the rest is
 * parsed as EDN. Otherwise the entire file is parsed as EDN (backward
 * compat with headerless v1 snapshots). Returns the parsed db value,
 * or NULL if the file does not exist or cannot be read/parsed. */
static mino_val *store_read_snapshot(mino_state *S, const char *path,
                                        mino_env *env)
{
    FILE     *f;
    long       sz;
    char      *buf;
    mino_val *db;
    size_t     offset = 0;
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
    /* Version header check: skip one byte if it's the version sentinel */
    if (sz > 0 && (unsigned char)buf[0] == STORE_SNAPSHOT_VERSION)
        offset = 1;
    db = mino_eval_string(S, buf + offset, env);
    free(buf);
    return db;
}

/* Escape a C string for safe embedding in a Clojure string literal.
 * Doubles backslashes and double-quotes. Returns malloc'd storage;
 * caller frees. Returns NULL on allocation failure. */
static char *store_escape_clj_str(const char *s)
{
    size_t      raw_len, esc_len, i, j;
    const char *p;
    char       *out;
    raw_len = strlen(s);
    esc_len = raw_len;
    for (p = s; *p; p++)
        if (*p == '"' || *p == '\\') esc_len++;
    out = (char *)malloc(esc_len + 1);
    if (out == NULL) return NULL;
    for (i = 0, j = 0; i < raw_len; i++) {
        if (s[i] == '"' || s[i] == '\\') out[j++] = '\\';
        out[j++] = s[i];
    }
    out[j] = '\0';
    return out;
}

mino_val *mino_store_open(mino_state *S, const char *path,
                           mino_store_clock_fn clock, void *clock_ctx)
{
    mino_env  *env = ns_env_ensure(S, "clojure.core");
    mino_val *db  = NULL;
    if (path == NULL) {
        db = mino_eval_string(S, "{:entities {} :log [] :tx 0}", env);
        if (db == NULL) return NULL;
        return mino_store_val(S, db, NULL, clock, clock_ctx);
    }
    /* For durable stores, delegate to the Clojure layer which handles
     * snapshot read + WAL replay (including schema, indexes, retention).
     * The returned store has a NULL clock; extract the db and re-wrap
     * with the caller's clock so C-side transacts see the right time. */
    {
        char       *escaped;
        char       *buf;
        int          n;
        mino_val   *conn;
        escaped = store_escape_clj_str(path);
        if (escaped == NULL) {
            gc_oom_throw(S, "store: out of memory");
            return NULL;
        }
        n = snprintf(NULL, 0,
            "(require 'mino.store)"
            "(mino.store/open \"%s\")", escaped);
        if (n < 0) {
            free(escaped);
            set_eval_diag(S, NULL, "io", "MIO001",
                "store-open: path encoding failed");
            return NULL;
        }
        buf = (char *)malloc((size_t)n + 1);
        if (buf == NULL) {
            free(escaped);
            gc_oom_throw(S, "store: out of memory");
            return NULL;
        }
        snprintf(buf, (size_t)n + 1,
            "(require 'mino.store)"
            "(mino.store/open \"%s\")", escaped);
        free(escaped);
        conn = mino_eval_string(S, buf, env);
        free(buf);
        if (conn == NULL || !mino_is_store(conn)) return NULL;
        db = mino_store_deref(conn);
        /* conn will be collected by GC; its handle freed by the
         * finalizer. The db value survives because we reference it. */
        return mino_store_val(S, db, path, clock, clock_ctx);
    }
}

int mino_store_checkpoint(mino_state *S, mino_val *conn)
{
    mino_store_handle *h;
    FILE              *f;
    char              *wal_path;
    char              *tmp_path;
    if (!mino_is_store(conn)) {
        prim_throw_classified(S, "eval/type", "MTY001",
            "store-checkpoint requires a store connection");
        return -1;
    }
    if (store_check_state(S, conn)) return -1;
    h = (mino_store_handle *)conn->as.store.handle;
    if (h == NULL || h->path == NULL) return 0;
    /* Atomic snapshot: write to <path>.tmp, then rename into place.
     * A crash between write and rename leaves a stale .tmp and the
     * previous snapshot intact; the next checkpoint overwrites .tmp. */
    tmp_path = store_tmp_path(h->path);
    if (tmp_path == NULL) {
        gc_oom_throw(S, "store: out of memory");
        return -1;
    }
    f = fopen(tmp_path, "wb");
    if (f == NULL) {
        set_eval_diag(S, NULL, "io", "MIO001",
            "store-checkpoint: cannot open file for writing");
        free(tmp_path);
        return -1;
    }
    fputc(STORE_SNAPSHOT_VERSION, f);
    mino_print_to(S, f, conn->as.store.val);
    fclose(f);
    if (rename(tmp_path, h->path) != 0) {
        set_eval_diag(S, NULL, "io", "MIO001",
            "store-checkpoint: cannot rename snapshot into place");
        remove(tmp_path);
        free(tmp_path);
        return -1;
    }
    free(tmp_path);
    /* Delete the WAL — the snapshot captures all state up to :tx */
    wal_path = store_wal_path(h->path);
    if (wal_path != NULL) {
        remove(wal_path);
        free(wal_path);
    }
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

/* (store-open* db path) -> store connection
 * Pure constructor: wraps the given db value in a store identity.
 * Snapshot reading and WAL replay happen in the Clojure layer
 * (store/open) via store-read-snapshot* and store-read-wal*. */
static mino_val *prim_store_open(mino_state *S, mino_val *args,
                                      mino_env *env)
{
    mino_val   *db;
    mino_val   *path_val;
    const char *path_str = NULL;
    (void)env;
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
    return mino_store_val(S, db, path_str, NULL, NULL);
}

/* (store-commit* conn new-db) -> new-db          (no WAL)
 * (store-commit* conn new-db tx-info) -> new-db  (append tx-info to WAL)
 * Publish + watches. When tx-info is a non-nil 3rd arg and the store
 * is durable (has a path), tx-info is appended to the WAL before the
 * publish. This ordering ensures crash safety: if the append succeeds
 * but the process dies before the in-memory publish takes effect, the
 * WAL has the data and it is replayed on next open. */
static mino_val *prim_store_commit(mino_state *S, mino_val *args,
                                       mino_env *env)
{
    mino_val          *conn;
    mino_val          *new_db;
    mino_val          *tx_info = NULL;
    mino_val          *old_val;
    mino_store_handle *h;
    if (!mino_is_cons(args) || !mino_is_cons(args->as.cons.cdr)) {
        return prim_throw_classified(S, "eval/arity", "MAR001",
            "store-commit* requires at least two arguments");
    }
    conn   = args->as.cons.car;
    new_db = args->as.cons.cdr->as.cons.car;
    /* Optional 3rd arg */
    if (mino_is_cons(args->as.cons.cdr->as.cons.cdr)) {
        tx_info = args->as.cons.cdr->as.cons.cdr->as.cons.car;
        if (mino_is_cons(args->as.cons.cdr->as.cons.cdr->as.cons.cdr)) {
            return prim_throw_classified(S, "eval/arity", "MAR001",
                "store-commit* accepts at most three arguments");
        }
    }
    if (!mino_is_store(conn)) {
        return prim_throw_classified(S, "eval/type", "MTY001",
            "store-commit* requires a store connection");
    }
    if (store_check_state(S, conn)) return NULL;
    /* WAL append before publish (crash safety) */
    if (tx_info != NULL && !mino_is_nil(tx_info)) {
        h = (mino_store_handle *)conn->as.store.handle;
        if (h != NULL && h->path != NULL) {
            if (store_wal_append(S, h->path, tx_info) != 0) return NULL;
        }
    }
    old_val = conn->as.store.val;
    if (mino_store_publish(S, conn, new_db) == NULL) return NULL;
    store_notify_watches(S, conn, old_val, new_db, env);
    return new_db;
}

/* (store-checkpoint* conn) -> nil
 * Writes the snapshot (version header + EDN) and deletes the WAL. */
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

/* --- snapshot / WAL read primitives --------------------------------------- */

/* (store-read-snapshot* path) -> db-value or nil
 * Reads a snapshot file (with version header) and returns the parsed
 * db value. Returns nil if the file does not exist or cannot be parsed. */
static mino_val *prim_store_read_snapshot(mino_state *S, mino_val *args,
                                              mino_env *env)
{
    mino_val   *path_val;
    const char *path_str;
    mino_val   *db;
    if (!mino_is_cons(args) || mino_is_cons(args->as.cons.cdr)) {
        return prim_throw_classified(S, "eval/arity", "MAR001",
            "store-read-snapshot* requires one argument");
    }
    path_val = args->as.cons.car;
    if (mino_type_of(path_val) != MINO_STRING) {
        return prim_throw_classified(S, "eval/type", "MTY001",
            "store-read-snapshot*: path must be a string");
    }
    path_str = path_val->as.s.data;
    db = store_read_snapshot(S, path_str, env);
    return db != NULL ? db : mino_nil(S);
}

/* (store-read-wal* path) -> [tx-info...] or nil
 * Reads the WAL at <path>.wal and returns a vector of parsed tx-info
 * maps. Returns nil if the WAL file does not exist. Stops at the first
 * line that fails to parse (torn-write recovery). */
static mino_val *prim_store_read_wal(mino_state *S, mino_val *args,
                                         mino_env *env)
{
    mino_val   *path_val;
    const char *path_str;
    if (!mino_is_cons(args) || mino_is_cons(args->as.cons.cdr)) {
        return prim_throw_classified(S, "eval/arity", "MAR001",
            "store-read-wal* requires one argument");
    }
    path_val = args->as.cons.car;
    if (mino_type_of(path_val) != MINO_STRING) {
        return prim_throw_classified(S, "eval/type", "MTY001",
            "store-read-wal*: path must be a string");
    }
    path_str = path_val->as.s.data;
    return store_wal_read(S, path_str, env);
}

/* --- primitive table + install hook --------------------------------------- */

const mino_prim_def k_prims_store[] = {
    {"store-open*",              prim_store_open,
     "Create a store connection from a db value and optional path."},
    {"store-commit*",            prim_store_commit,
     "Publish a new db value to the store, optionally appending to WAL."},
    {"store-checkpoint*",        prim_store_checkpoint,
     "Write the db value to disk and truncate the WAL if durable."},
    {"store-close*",             prim_store_close,
     "Close the store, flushing if durable."},
    {"store?",                   prim_store_p,
     "Return true if x is a store connection."},
    {"store-clock*",             prim_store_clock,
     "Return the current instant from the store's clock."},
    {"store-read-snapshot*",     prim_store_read_snapshot,
     "Read a snapshot file and return the db value, or nil."},
    {"store-read-wal*",          prim_store_read_wal,
     "Read the WAL and return a vector of tx-info maps, or nil."},
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
