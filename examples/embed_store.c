/*
 * embed_store.c -- demonstrates the host-side EAVT fact store API.
 *
 * Opens an in-memory store, transacts facts, reads them back, exercises
 * the durability round-trip (open-with-path, checkpoint, close, reopen),
 * and probes the C-side predicates mino_is_store / mino_store_deref.
 * Used as a smoke test that mino.h's store surface stays symmetrical
 * with the mino.store script layer.
 *
 * Build (from repo root):
 *   ./mino task examples
 * Or use the amalgamation:
 *   ./mino task amalgamate
 *   cc -std=c99 -Idist -c dist/mino.c -o dist/mino.o
 *   cc -std=c99 -Idist examples/embed_store.c dist/mino.o -lm -lpthread \
 *      -o build/embed_store
 */

#include "mino.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SNAP_PATH "embed_store_snap.edn"

static int read_long(const mino_val *v, long long *out)
{
    return mino_to_int(v, out);
}

int main(void)
{
    mino_state *S;
    mino_env   *env;
    mino_val   *conn, *db, *result;
    long long    age, level;
    char         durbuf[512];
    int          n;

    S = mino_state_new();
    if (S == NULL) {
        fprintf(stderr, "state_new failed\n");
        return 1;
    }
    env = mino_env_new(S);
    mino_install_all(S, env);

    /* Open an in-memory store via the mino.store script layer and bind
     * it to conn in the script env. The bundled lib is registered by
     * MINO_CAP_STORE (folded into MINO_CAP_ALL); require loads it. */
    result = mino_eval_string(S,
        "(do (require 'mino.store)"
        "  (def conn (mino.store/open))"
        "  conn)",
        env);
    if (result == NULL || !mino_is_store(result)) {
        fprintf(stderr, "mino.store/open failed: %s\n", mino_last_error(S));
        return 1;
    }
    printf("opened in-memory store, mino_is_store = %d\n",
           mino_is_store(result));

    /* Transact facts using the map sugar ({e {a v ...}}). */
    result = mino_eval_string(S,
        "(mino.store/transact conn"
        "  {:alice {:name \"Alice\" :age 30}"
        "   :bob   {:name \"Bob\"   :age 25}})",
        env);
    if (result == NULL) {
        fprintf(stderr, "transact failed: %s\n", mino_last_error(S));
        return 1;
    }

    /* Read back and verify via the script API. */
    result = mino_eval_string(S,
        "(mino.store/read (mino.store/db conn) :alice :age)",
        env);
    if (result == NULL || !read_long(result, &age)) {
        fprintf(stderr, "read :alice :age failed: %s\n", mino_last_error(S));
        return 1;
    }
    printf("read :alice :age = %lld (expected 30)\n", age);
    if (age != 30) return 1;

    /* C-side predicates on the script-held connection. Pull the conn
     * value back through eval and inspect it from C. */
    conn = mino_eval_string(S, "conn", env);
    if (conn == NULL || !mino_is_store(conn)) {
        fprintf(stderr, "C-side conn is not a store\n");
        return 1;
    }
    db = mino_store_deref(conn);
    if (db == NULL || !mino_is_map(db)) {
        fprintf(stderr, "mino_store_deref did not return a map\n");
        return 1;
    }
    printf("mino_is_store(conn)=%d mino_store_deref -> map=%d\n",
           mino_is_store(conn), mino_is_map(db));

    /* Durability round-trip via the script layer: open with a path,
     * transact, checkpoint, close, reopen, and read back. The snapshot
     * is a printed db value; reopen reads and evals it. */
    remove(SNAP_PATH);
    n = snprintf(durbuf, sizeof durbuf,
        "(do"
        "  (require 'mino.store)"
        "  (def dconn (mino.store/open \"%s\"))"
        "  (mino.store/transact dconn {:carol {:role :admin :level 5}})"
        "  (mino.store/checkpoint dconn)"
        "  (mino.store/close dconn)"
        "  (def rconn (mino.store/open \"%s\"))"
        "  (mino.store/read (mino.store/db rconn) :carol :level))",
        SNAP_PATH, SNAP_PATH);
    if (n < 0 || (size_t)n >= sizeof durbuf) {
        fprintf(stderr, "durability eval buffer too small\n");
        return 1;
    }
    result = mino_eval_string(S, durbuf, env);
    if (result == NULL || !read_long(result, &level)) {
        fprintf(stderr, "durability round-trip failed: %s\n",
                mino_last_error(S));
        return 1;
    }
    printf("durability :carol :level = %lld (expected 5)\n", level);
    if (level != 5) return 1;

    remove(SNAP_PATH);

    /* C-API WAL replay: transact via the script layer (which appends
     * to the WAL), do NOT checkpoint, then reopen via the C API
     * mino_store_open. The C API must replay the WAL — if it only
     * reads the snapshot, the entity will be absent. */
    #define WAL_PATH "embed_store_wal.edn"
    remove(WAL_PATH);
    remove(WAL_PATH ".wal");

    result = mino_eval_string(S,
        "(do (require 'mino.store)"
        "    (mino.store/transact"
        "      (mino.store/open \"" WAL_PATH "\")"
        "      {:dave {:role :dev}})"
        "    nil)", env);
    if (result == NULL) {
        fprintf(stderr, "WAL setup failed: %s\n", mino_last_error(S));
        return 1;
    }
    /* Reopen via C API — WAL must be replayed. */
    conn = mino_store_open(S, WAL_PATH, NULL, NULL);
    if (conn == NULL || !mino_is_store(conn)) {
        fprintf(stderr, "mino_store_open WAL replay failed: %s\n",
                mino_last_error(S));
        return 1;
    }
    mino_env_set(S, env, "cconn", conn);
    result = mino_eval_string(S,
        "(mino.store/read (mino.store/db cconn) :dave :role)", env);
    if (result == NULL) {
        fprintf(stderr, "C-API WAL replay read failed: %s\n",
                mino_last_error(S));
        return 1;
    }
    printf("C-API WAL replay: :dave :role = ");
    mino_print(S, result);
    printf(" (expected :dev)\n");

    remove(WAL_PATH);
    remove(WAL_PATH ".wal");

    printf("ok\n");
    mino_env_free(S, env);
    mino_state_free(S);
    return 0;
}
