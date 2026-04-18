/*
 * console.cpp — in-app scripting console.
 *
 * An application embeds a command console where users type mino
 * expressions to inspect and control the running system. The host
 * registers query and action functions. Step limits prevent runaway
 * scripts from blocking the application.
 *
 * Build:
 *   make
 *   c++ -std=c++17 -Isrc -o examples/use-cases/console \
 *       examples/use-cases/console.cpp src/[a-z]*.o -lm
 */

#include "mino.h"
#include <cstdio>
#include <cstring>

/* ── Expose ────────────────────────────────────────────────────────── */

/* Simulated application state: a key-value store. The console
 * exposes read/write access through registered functions. */

struct AppState {
    struct Entry { char key[64]; mino_val_t *val; };
    Entry entries[64];
    int   count = 0;
};

static AppState app;

static mino_val_t *host_get(mino_state_t *S, mino_val_t *args,
                            mino_env_t *)
{
    const char *key;
    size_t len;
    if (!mino_is_cons(args) ||
        !mino_to_string(mino_car(args), &key, &len))
        return mino_nil(S);
    for (int i = 0; i < app.count; i++) {
        if (strncmp(app.entries[i].key, key, len) == 0 &&
            app.entries[i].key[len] == '\0')
            return app.entries[i].val;
    }
    return mino_nil(S);
}

static mino_val_t *host_put(mino_state_t *S, mino_val_t *args,
                            mino_env_t *)
{
    const char *key;
    size_t len;
    if (!mino_is_cons(args) ||
        !mino_to_string(mino_car(args), &key, &len) ||
        !mino_is_cons(mino_cdr(args)))
        return mino_nil(S);
    mino_val_t *val = mino_car(mino_cdr(args));

    /* Update existing or append. */
    for (int i = 0; i < app.count; i++) {
        if (strncmp(app.entries[i].key, key, len) == 0 &&
            app.entries[i].key[len] == '\0') {
            app.entries[i].val = val;
            return val;
        }
    }
    if (app.count < 64) {
        snprintf(app.entries[app.count].key, 64, "%.*s", (int)len, key);
        app.entries[app.count].val = val;
        app.count++;
    }
    return val;
}

static mino_val_t *host_keys(mino_state_t *S, mino_val_t *,
                             mino_env_t *)
{
    mino_val_t *items[64];
    for (int i = 0; i < app.count; i++)
        items[i] = mino_string(S, app.entries[i].key);
    return mino_vector(S, items, (size_t)app.count);
}

static mino_val_t *host_dump(mino_state_t *S, mino_val_t *,
                             mino_env_t *)
{
    mino_val_t *ks[64], *vs[64];
    for (int i = 0; i < app.count; i++) {
        ks[i] = mino_string(S, app.entries[i].key);
        vs[i] = app.entries[i].val;
    }
    return mino_map(S, ks, vs, (size_t)app.count);
}

/* ── Script ────────────────────────────────────────────────────────── */

/* Convenience commands defined in mino, building on the host
 * primitives. These are loaded once at startup. */

static const char *prelude =
    "(defn batch-put [pairs]\n"
    "  (->> pairs\n"
    "       (map (fn [[k v]] (put k v)))\n"
    "       (count)))\n"
    "\n"
    "(defn search [pattern]\n"
    "  (->> (store-keys)\n"
    "       (filter (fn [k] (includes? k pattern)))\n"
    "       (map (fn [k] [k (get-val k)]))))\n";

/* ── Embed ─────────────────────────────────────────────────────────── */

int main()
{
    mino_state_t *S   = mino_state_new();
    mino_env_t   *env = mino_new(S);

    /* Step limit: prevent runaway scripts. */
    mino_set_limit(S, MINO_LIMIT_STEPS, 100000);

    /* Register host functions. */
    mino_register_fn(S, env, "get-val",    host_get);
    mino_register_fn(S, env, "put",        host_put);
    mino_register_fn(S, env, "store-keys", host_keys);
    mino_register_fn(S, env, "dump",       host_dump);

    /* Load the prelude. */
    if (!mino_eval_string(S, prelude, env)) {
        fprintf(stderr, "prelude error: %s\n", mino_last_error(S));
        return 1;
    }

    /* Simulate a console session. */
    printf("=== console session ===\n\n");

    static const char *commands[] = {
        "(put \"version\" \"2.7.1\")",
        "(put \"max-connections\" 256)",
        "(put \"log-level\" :info)",
        "(dump)",
        "(batch-put [[\"db-host\" \"10.0.1.5\"] [\"db-port\" 5432]])",
        "(search \"db\")",
        "(get-val \"max-connections\")",
    };

    for (auto *cmd : commands) {
        printf("> %s\n", cmd);
        mino_val_t *result = mino_eval_string(S, cmd, env);
        if (result) {
            mino_println(S, result);
        } else {
            printf("error: %s\n", mino_last_error(S));
        }
        printf("\n");
    }

    /* Demonstrate step limit enforcement. */
    printf("> (loop [] (recur))   ; infinite loop\n");
    mino_val_t *r = mino_eval_string(S, "(loop [] (recur))", env);
    if (!r)
        printf("error: %s\n", mino_last_error(S));

    mino_env_free(S, env);
    mino_state_free(S);
}
