/*
 * event_processing.cpp — event processing example.
 *
 * C++ owns the event source (parsing, I/O, buffering).
 * mino scripts define the processing rules (filtering, grouping,
 * aggregation) using persistent data structures.
 *
 * Build:
 *   make
 *   c++ -std=c++17 -Isrc -o examples/event_processing \
 *       examples/event_processing.cpp src/[a-z]*.o -lm
 */

#include "mino.h"
#include <cstdio>
#include <vector>

/* --- Event source: a stream of sensor readings --- */

struct EventSource {
    std::vector<mino_val_t *> events;
    size_t cursor = 0;
};

static mino_val_t *make_event(mino_state_t *S, const char *type,
                              const char *device, double value, int ts)
{
    mino_val_t *ks[4], *vs[4];
    ks[0] = mino_keyword(S, "type");
    ks[1] = mino_keyword(S, "device");
    ks[2] = mino_keyword(S, "value");
    ks[3] = mino_keyword(S, "ts");
    vs[0] = mino_keyword(S, type);
    vs[1] = mino_string(S, device);
    vs[2] = mino_float(S, value);
    vs[3] = mino_int(S, ts);
    return mino_map(S, ks, vs, 4);
}

/* Callbacks registered with the capability registry. */

static mino_val_t *source_new(mino_state_t *S, mino_val_t *,
                              mino_val_t *, void *)
{
    auto *src = new EventSource;

    /* Simulate a batch of sensor readings. */
    src->events.push_back(make_event(S, "temp",     "sensor-01", 21.3, 1000));
    src->events.push_back(make_event(S, "humidity", "sensor-01", 45.0, 1001));
    src->events.push_back(make_event(S, "temp",     "sensor-02", 19.8, 1002));
    src->events.push_back(make_event(S, "pressure", "sensor-01", 1013.2, 1003));
    src->events.push_back(make_event(S, "temp",     "sensor-01", 22.1, 1004));
    src->events.push_back(make_event(S, "temp",     "sensor-02", 20.4, 1005));
    src->events.push_back(make_event(S, "humidity", "sensor-02", 52.0, 1006));
    src->events.push_back(make_event(S, "temp",     "sensor-01", 21.7, 1007));
    src->events.push_back(make_event(S, "temp",     "sensor-02", 19.5, 1008));
    src->events.push_back(make_event(S, "pressure", "sensor-02", 1012.8, 1009));
    src->events.push_back(make_event(S, "temp",     "sensor-01", 22.9, 1010));
    src->events.push_back(make_event(S, "humidity", "sensor-01", 43.0, 1011));

    return mino_handle_ex(S, src, "EventSource",
        [](void *p, const char *) { delete static_cast<EventSource *>(p); });
}

static mino_val_t *source_next(mino_state_t *S, mino_val_t *target,
                               mino_val_t *, void *)
{
    auto *src = static_cast<EventSource *>(mino_handle_ptr(target));
    if (src->cursor >= src->events.size())
        return mino_nil(S);
    return src->events[src->cursor++];
}

static mino_val_t *source_count(mino_state_t *S, mino_val_t *target,
                                mino_val_t *, void *)
{
    auto *src = static_cast<EventSource *>(mino_handle_ptr(target));
    return mino_int(S, (long long)src->cursor);
}

/* --- mino processing script --- */

static const char *script =
    ";; Consume all events from a source into a vector.\n"
    "(defn drain [source acc]\n"
    "  (let [evt (.next source)]\n"
    "    (if (nil? evt)\n"
    "      acc\n"
    "      (drain source (conj acc evt)))))\n"
    "\n"
    ";; Summarize a [device readings] group.\n"
    "(defn summarize [[device readings]]\n"
    "  [device {:count (count readings)\n"
    "           :avg   (/ (reduce + (map :value readings))\n"
    "                     (count readings))\n"
    "           :min   (apply min (map :value readings))\n"
    "           :max   (apply max (map :value readings))}])\n"
    "\n"
    ";; Filter, group, summarize.\n"
    "(defn analyze [events type-filter]\n"
    "  (->> events\n"
    "       (filter #(type-filter (:type %)))\n"
    "       (group-by :device)\n"
    "       (map summarize)\n"
    "       (into (sorted-map))))\n"
    "\n"
    ";; Process: drain the source, analyze temperature readings.\n"
    "(let [events (drain (new EventSource) [])]\n"
    "  (println \"total events:\" (count events))\n"
    "  (println \"devices:\" (set (map :device events)))\n"
    "  (analyze events #{:temp}))\n";

/* --- Main --- */

int main()
{
    mino_state_t *S   = mino_state_new();
    mino_env_t   *env = mino_new(S);

    mino_host_enable(S);
    mino_host_register_ctor(S, "EventSource", 0, source_new, nullptr);
    mino_host_register_method(S, "EventSource", "next", 0, source_next, nullptr);
    mino_host_register_getter(S, "EventSource", "count", source_count, nullptr);

    mino_val_t *result = mino_eval_string(S, script, env);

    if (result) {
        printf("result: ");
        mino_println(S, result);
    } else {
        fprintf(stderr, "error: %s\n", mino_last_error(S));
    }

    mino_env_free(S, env);
    mino_state_free(S);
}
