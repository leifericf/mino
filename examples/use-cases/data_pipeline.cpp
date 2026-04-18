/*
 * data_pipeline.cpp — transform host data through mino pipelines.
 *
 * A metrics collector pushes timestamped measurements into mino.
 * The script defines transformation rules using threading macros
 * and persistent data structures. Data flows through the pipeline
 * without copying: structural sharing means intermediate results
 * reuse memory from earlier stages.
 *
 * Build:
 *   make
 *   c++ -std=c++17 -Isrc -o examples/use-cases/data_pipeline \
 *       examples/use-cases/data_pipeline.cpp src/[a-z]*.o -lm
 */

#include "mino.h"
#include <cstdio>
#include <vector>

/* ── Expose ────────────────────────────────────────────────────────── */

/* Each measurement is a map with `:metric`, `:host`, `:value`, `:ts` keys.
 * Maps are immutable once created. The script receives read-only
 * snapshots of the C++ data. */

struct Measurement {
    const char *metric;
    const char *host;
    double      value;
    int         ts;
};

static mino_val_t *make_measurement(mino_state_t *S, const Measurement &m)
{
    mino_val_t *ks[4], *vs[4];
    ks[0] = mino_keyword(S, "metric"); vs[0] = mino_keyword(S, m.metric);
    ks[1] = mino_keyword(S, "host");   vs[1] = mino_string(S, m.host);
    ks[2] = mino_keyword(S, "value");  vs[2] = mino_float(S, m.value);
    ks[3] = mino_keyword(S, "ts");     vs[3] = mino_int(S, m.ts);
    return mino_map(S, ks, vs, 4);
}

/* ── Script ────────────────────────────────────────────────────────── */

/* The pipeline uses `->>` to thread data through each stage.
 * Keywords like `:metric` and `:value` act directly as accessor
 * functions. Sets like `#{:cpu :mem}` act as membership predicates
 * in `filter`. Named helpers keep the top-level pipeline flat. */

static const char *script =
    ";; Average of a numeric sequence.\n"
    "(defn avg [xs]\n"
    "  (/ (reduce + xs) (count xs)))\n"
    "\n"
    ";; Summarize a [host measurements] group.\n"
    "(defn summarize [[host readings]]\n"
    "  (let [values (map :value readings)]\n"
    "    [host {:count (count readings)\n"
    "           :avg   (avg values)\n"
    "           :min   (apply min values)\n"
    "           :max   (apply max values)}]))\n"
    "\n"
    ";; Build a summary table for one metric.\n"
    "(defn metric-summary [data metric-key]\n"
    "  (->> data\n"
    "       (filter #(= (:metric %) metric-key))\n"
    "       (group-by :host)\n"
    "       (map summarize)\n"
    "       (into (sorted-map))))\n"
    "\n"
    ";; Top-level: summarize CPU and memory metrics.\n"
    "(let [summaries (->> [:cpu :mem]\n"
    "                     (map (fn [m] [m (metric-summary data m)]))\n"
    "                     (into (sorted-map)))]\n"
    "  summaries)\n";

/* ── Embed ─────────────────────────────────────────────────────────── */

int main()
{
    mino_state_t *S   = mino_state_new();
    mino_env_t   *env = mino_new(S);

    /* Simulated metrics batch from a monitoring agent. */
    std::vector<Measurement> batch = {
        {"cpu", "web-01", 72.5, 1000},
        {"mem", "web-01", 61.2, 1000},
        {"cpu", "web-02", 45.3, 1000},
        {"mem", "web-02", 78.9, 1000},
        {"cpu", "web-01", 68.1, 1001},
        {"mem", "web-01", 62.0, 1001},
        {"cpu", "web-02", 51.7, 1001},
        {"mem", "web-02", 77.4, 1001},
        {"cpu", "web-01", 75.0, 1002},
        {"cpu", "web-02", 48.2, 1002},
    };

    /* Push data into mino as a vector of maps.
     * Each record is rooted via mino_ref so the GC cannot collect
     * earlier records while later ones are still being allocated. */
    std::vector<mino_ref_t *> refs;
    for (auto &m : batch)
        refs.push_back(mino_ref(S, make_measurement(S, m)));

    std::vector<mino_val_t *> records;
    for (auto *r : refs)
        records.push_back(mino_deref(r));
    mino_env_set(S, env, "data",
                 mino_vector(S, records.data(), records.size()));
    for (auto *r : refs)
        mino_unref(S, r);

    /* Run the pipeline. */
    mino_val_t *result = mino_eval_string(S, script, env);

    if (result) {
        printf("summaries:\n");
        mino_println(S, result);
    } else {
        fprintf(stderr, "error: %s\n", mino_last_error(S));
    }

    mino_env_free(S, env);
    mino_state_free(S);
}
