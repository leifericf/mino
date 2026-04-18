/*
 * embed_cpp.cpp — embedding mino from C++.
 *
 * Same API calls as the C example, but with C++17 patterns:
 * structured bindings, RAII-style cleanup, auto, and lambdas.
 * mino.h includes extern "C" guards, so no wrapper is needed.
 *
 * Build:
 *   make
 *   c++ -std=c++17 -Isrc -o examples/bindings/embed_cpp \
 *       examples/bindings/embed_cpp.cpp src/[a-z]*.o -lm
 */

#include "mino.h"
#include <cstdio>
#include <vector>

/* --- Build event data --- */

struct Event {
    const char *type;
    const char *device;
    double      value;
    int         ts;
};

static mino_val_t *make_event(mino_state_t *S, const Event &ev)
{
    mino_val_t *ks[4], *vs[4];
    ks[0] = mino_keyword(S, "type");    vs[0] = mino_keyword(S, ev.type);
    ks[1] = mino_keyword(S, "device");  vs[1] = mino_string(S, ev.device);
    ks[2] = mino_keyword(S, "value");   vs[2] = mino_float(S, ev.value);
    ks[3] = mino_keyword(S, "ts");      vs[3] = mino_int(S, ev.ts);
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

int main()
{
    std::vector<Event> data = {
        {"temp",     "sensor-01", 21.3, 1000},
        {"humidity", "sensor-01", 45.0, 1001},
        {"temp",     "sensor-02", 19.8, 1002},
        {"temp",     "sensor-01", 22.1, 1003},
        {"temp",     "sensor-02", 20.4, 1004},
        {"temp",     "sensor-01", 22.9, 1005},
    };

    auto *S   = mino_state_new();
    auto *env = mino_new(S);

    /* Build event vector, rooting each record. */
    std::vector<mino_ref_t *> refs;
    for (auto &ev : data)
        refs.push_back(mino_ref(S, make_event(S, ev)));

    std::vector<mino_val_t *> items;
    for (auto *r : refs)
        items.push_back(mino_deref(r));
    mino_env_set(S, env, "events",
                 mino_vector(S, items.data(), items.size()));
    for (auto *r : refs)
        mino_unref(S, r);

    auto *result = mino_eval_string(S, script, env);

    if (result) {
        printf("result: ");
        mino_println(S, result);
    } else {
        fprintf(stderr, "error: %s\n", mino_last_error(S));
    }

    mino_env_free(S, env);
    mino_state_free(S);
}
