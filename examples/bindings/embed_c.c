/*
 * embed_c.c — embedding mino from C.
 *
 * The baseline reference. mino's API is plain C with simple types
 * (pointers, integers, doubles, strings), so there is no wrapper
 * layer and no translation overhead.
 *
 * This example builds sensor event maps from C structs, pushes them
 * into the runtime as a vector, and evaluates a mino processing
 * script that filters, groups, and summarizes the data.
 *
 * Build:
 *   make
 *   cc -std=c99 -Isrc -o examples/bindings/embed_c \
 *      examples/bindings/embed_c.c src/[a-z]*.o -lm
 */

#include "mino.h"
#include <stdio.h>

/* --- Build event data from C structs --- */

typedef struct {
    const char *type;
    const char *device;
    double      value;
    int         ts;
} event_t;

static mino_val_t *make_event(mino_state_t *S, const event_t *ev)
{
    mino_val_t *ks[4], *vs[4];
    ks[0] = mino_keyword(S, "type");    vs[0] = mino_keyword(S, ev->type);
    ks[1] = mino_keyword(S, "device");  vs[1] = mino_string(S, ev->device);
    ks[2] = mino_keyword(S, "value");   vs[2] = mino_float(S, ev->value);
    ks[3] = mino_keyword(S, "ts");      vs[3] = mino_int(S, ev->ts);
    return mino_map(S, ks, vs, 4);
}

/* --- Processing script --- */

static const char *script =
    "(defn avg [xs]\n"
    "  (/ (reduce + xs) (count xs)))\n"
    "\n"
    "(defn summarize [[device readings]]\n"
    "  [device {:count (count readings)\n"
    "           :avg   (avg (map :value readings))}])\n"
    "\n"
    "(->> events\n"
    "     (filter #(= (:type %) :temp))\n"
    "     (group-by :device)\n"
    "     (map summarize)\n"
    "     (into (sorted-map)))\n";

/* --- Main --- */

int main(void)
{
    static const event_t data[] = {
        {"temp",     "sensor-01", 21.3, 1000},
        {"humidity", "sensor-01", 45.0, 1001},
        {"temp",     "sensor-02", 19.8, 1002},
        {"temp",     "sensor-01", 22.1, 1003},
        {"temp",     "sensor-02", 20.4, 1004},
        {"temp",     "sensor-01", 22.9, 1005},
    };
    size_t n = sizeof(data) / sizeof(data[0]);

    mino_state_t *S   = mino_state_new();
    mino_env_t   *env = mino_new(S);

    /* Build event vector, rooting each record. */
    {
        mino_ref_t *refs[6];
        mino_val_t *items[6];
        size_t i;
        for (i = 0; i < n; i++)
            refs[i] = mino_ref(S, make_event(S, &data[i]));
        for (i = 0; i < n; i++)
            items[i] = mino_deref(refs[i]);
        mino_env_set(S, env, "events", mino_vector(S, items, n));
        for (i = 0; i < n; i++)
            mino_unref(S, refs[i]);
    }

    mino_val_t *result = mino_eval_string(S, script, env);

    if (result) {
        printf("result: ");
        mino_println(S, result);
    } else {
        fprintf(stderr, "error: %s\n", mino_last_error(S));
    }

    mino_env_free(S, env);
    mino_state_free(S);
    return 0;
}
