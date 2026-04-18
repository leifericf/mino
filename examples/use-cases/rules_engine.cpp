/*
 * rules_engine.cpp — declarative business rules over host data.
 *
 * A loan approval system where C++ owns the applicant database and
 * mino scripts define the underwriting rules. Rules are pure functions
 * over immutable data: no side effects, no mutation of host state.
 * Change the approval criteria without recompiling.
 *
 * Build:
 *   make
 *   c++ -std=c++17 -Isrc -o examples/use-cases/rules_engine \
 *       examples/use-cases/rules_engine.cpp src/[a-z]*.o -lm
 */

#include "mino.h"
#include <cstdio>
#include <vector>

/* ── Expose ────────────────────────────────────────────────────────── */

/* Applicant records are built as mino maps from C++ structs.
 * Each map is immutable once created. The script cannot modify
 * applicant data through the values it receives. */

struct Applicant {
    const char *name;
    int         age;
    int         credit_score;
    double      income;
    double      debt;
    int         years_employed;
};

static mino_val_t *make_applicant(mino_state_t *S, const Applicant &a)
{
    mino_val_t *ks[6], *vs[6];
    ks[0] = mino_keyword(S, "name");           vs[0] = mino_string(S, a.name);
    ks[1] = mino_keyword(S, "age");            vs[1] = mino_int(S, a.age);
    ks[2] = mino_keyword(S, "credit-score");   vs[2] = mino_int(S, a.credit_score);
    ks[3] = mino_keyword(S, "income");         vs[3] = mino_float(S, a.income);
    ks[4] = mino_keyword(S, "debt");           vs[4] = mino_float(S, a.debt);
    ks[5] = mino_keyword(S, "years-employed"); vs[5] = mino_int(S, a.years_employed);
    return mino_map(S, ks, vs, 6);
}

/* ── Script ────────────────────────────────────────────────────────── */

/* Keywords like `:credit-score` act as data accessors.
 * Sets like `#{:approved :review}` act as membership predicates.
 * The pipeline is flat: each step takes and returns data. */

static const char *script =
    ";; Debt-to-income ratio.\n"
    "(defn dti [applicant]\n"
    "  (/ (:debt applicant) (:income applicant)))\n"
    "\n"
    ";; Classify a single applicant into a risk tier.\n"
    "(defn classify [applicant]\n"
    "  (let [score (:credit-score applicant)\n"
    "        ratio (dti applicant)\n"
    "        years (:years-employed applicant)]\n"
    "    (cond\n"
    "      (< score 580)              :denied\n"
    "      (> ratio 0.43)             :denied\n"
    "      (and (>= score 720)\n"
    "           (<= ratio 0.36)\n"
    "           (>= years 2))         :approved\n"
    "      :else                      :review)))\n"
    "\n"
    ";; Tag each applicant with their decision.\n"
    "(defn decide [applicant]\n"
    "  (assoc applicant :decision (classify applicant)))\n"
    "\n"
    ";; Process all applicants and group by decision.\n"
    "(defn process [applicants]\n"
    "  (->> applicants\n"
    "       (map decide)\n"
    "       (group-by :decision)\n"
    "       (map (fn [[tier apps]]\n"
    "              [tier (mapv :name apps)]))\n"
    "       (into (sorted-map))))\n"
    "\n"
    "(process applicants)\n";

/* ── Embed ─────────────────────────────────────────────────────────── */

int main()
{
    mino_state_t *S   = mino_state_new();
    mino_env_t   *env = mino_new(S);

    /* Build applicant data from the C++ side. */
    std::vector<Applicant> data = {
        {"Alice",   34, 750, 85000.0, 22000.0, 5},
        {"Bob",     28, 620, 52000.0, 28000.0, 1},
        {"Carol",   45, 810, 120000.0, 35000.0, 12},
        {"Dave",    22, 540, 38000.0, 15000.0, 0},
        {"Eve",     31, 690, 67000.0, 24000.0, 3},
    };

    /* Convert to a mino vector. Each record is rooted via mino_ref
     * so earlier records survive GC while later ones are allocated. */
    std::vector<mino_ref_t *> refs;
    for (auto &a : data)
        refs.push_back(mino_ref(S, make_applicant(S, a)));

    std::vector<mino_val_t *> records;
    for (auto *r : refs)
        records.push_back(mino_deref(r));
    mino_env_set(S, env, "applicants",
                 mino_vector(S, records.data(), records.size()));
    for (auto *r : refs)
        mino_unref(S, r);

    /* Evaluate the rules script. */
    mino_val_t *result = mino_eval_string(S, script, env);

    if (result) {
        printf("decisions: ");
        mino_println(S, result);
    } else {
        fprintf(stderr, "error: %s\n", mino_last_error(S));
    }

    mino_env_free(S, env);
    mino_state_free(S);
}
