/*
 * plugin.c — a plugin host that loads .mino files and calls into them.
 *
 * Demonstrates: module loading, calling mino functions from C with
 * mino_call, the resolver callback, sandboxed vs full environments.
 *
 * Build:  cc -std=c99 -I.. -o plugin plugin.c ../mino.c
 * Run:    ./plugin
 *
 * In this example the "plugins" are defined inline for portability,
 * but a real host would load them from disk via mino_load_file.
 */

#include "mino.h"
#include <stdio.h>
#include <string.h>

/* Inline plugin source (normally loaded from a file). */
static const char *greeter_plugin =
    "(def greet\n"
    "  (fn (name)\n"
    "    (str \"Hello, \" name \"!\")))\n"
    "\n"
    "(def farewell\n"
    "  (fn (name)\n"
    "    (str \"Goodbye, \" name \".\")))";

static const char *math_plugin =
    "(def square (fn (x) (* x x)))\n"
    "(def cube   (fn (x) (* x x x)))\n"
    "(def hypot  (fn (a b)\n"
    "              (let (a2 (square a)\n"
    "                    b2 (square b))\n"
    "                (reduce + 0 (list a2 b2)))))\n";

/* Call a named function in env with a single argument. */
static mino_val_t *call1(mino_state_t *S, mino_env_t *env, const char *name,
                         mino_val_t *arg)
{
    mino_val_t *fn   = mino_env_get(env, name);
    mino_val_t *args = mino_cons(S, arg, mino_nil(S));
    if (fn == NULL) {
        fprintf(stderr, "plugin: function '%s' not found\n", name);
        return NULL;
    }
    return mino_call(S, fn, args, env);
}

static mino_val_t *call2(mino_state_t *S, mino_env_t *env, const char *name,
                         mino_val_t *a, mino_val_t *b)
{
    mino_val_t *fn   = mino_env_get(env, name);
    mino_val_t *args = mino_cons(S, a, mino_cons(S, b, mino_nil(S)));
    if (fn == NULL) {
        fprintf(stderr, "plugin: function '%s' not found\n", name);
        return NULL;
    }
    return mino_call(S, fn, args, env);
}

int main(void)
{
    mino_state_t *S = mino_state_new();
    mino_env_t *env = mino_new(S);
    mino_val_t *result;

    /* Load the greeter plugin. */
    if (mino_eval_string(S, greeter_plugin, env) == NULL) {
        fprintf(stderr, "load error: %s\n", mino_last_error(S));
        return 1;
    }

    /* Call plugin functions from C. */
    result = call1(S, env, "greet", mino_string(S, "World"));
    if (result != NULL) {
        const char *s;
        size_t      n;
        if (mino_to_string(result, &s, &n)) {
            printf("%.*s\n", (int)n, s);
        }
    }

    result = call1(S, env, "farewell", mino_string(S, "World"));
    if (result != NULL) {
        const char *s;
        size_t      n;
        if (mino_to_string(result, &s, &n)) {
            printf("%.*s\n", (int)n, s);
        }
    }

    /* Load the math plugin. */
    if (mino_eval_string(S, math_plugin, env) == NULL) {
        fprintf(stderr, "load error: %s\n", mino_last_error(S));
        return 1;
    }

    result = call1(S, env, "square", mino_int(S, 7));
    if (result != NULL) {
        long long v;
        if (mino_to_int(result, &v)) {
            printf("square(7) = %lld\n", v);
        }
    }

    result = call2(S, env, "hypot", mino_int(S, 3), mino_int(S, 4));
    if (result != NULL) {
        long long v;
        if (mino_to_int(result, &v)) {
            printf("hypot(3,4) = %lld (sum of squares)\n", v);
        }
    }

    /* Demonstrate protected call — catches errors gracefully. */
    {
        mino_val_t *fn  = mino_env_get(env, "square");
        mino_val_t *bad = mino_cons(S, mino_string(S, "oops"), mino_nil(S));
        mino_val_t *out = NULL;
        int rc = mino_pcall(S, fn, bad, env, &out);
        if (rc != 0) {
            printf("pcall caught: %s\n", mino_last_error(S));
        }
    }

    mino_env_free(S, env);
    mino_state_free(S);
    return 0;
}
