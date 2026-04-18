/*
 * automation.cpp — user-defined workflows over host APIs.
 *
 * A task automation system where C++ provides task primitives
 * (create, run, check status) and mino scripts define workflows
 * that compose those primitives. Workflows are data: lists of
 * steps that the host interprets. Higher-order functions and
 * threading macros keep workflow definitions concise.
 *
 * Build:
 *   make
 *   c++ -std=c++17 -Isrc -o examples/use-cases/automation \
 *       examples/use-cases/automation.cpp src/[a-z]*.o -lm
 */

#include "mino.h"
#include <cstdio>
#include <cstring>
#include <vector>
#include <string>

/* ── Expose ────────────────────────────────────────────────────────── */

/* Tasks have a name, status, and result. The host simulates
 * execution: each task transitions from `:pending` to `:done`. */

struct Task {
    std::string name;
    std::string status;  /* "pending", "running", "done", "failed" */
    std::string result;
};

static std::vector<Task> tasks;

/* Host function: create a task, return its index. */
static mino_val_t *host_create_task(mino_state_t *S, mino_val_t *args,
                                    mino_env_t *)
{
    const char *name;
    size_t len;
    if (!mino_is_cons(args) ||
        !mino_to_string(mino_car(args), &name, &len))
        return mino_nil(S);
    tasks.push_back({std::string(name, len), "pending", ""});
    return mino_int(S, (long long)(tasks.size() - 1));
}

/* Host function: run a task (simulated). */
static mino_val_t *host_run_task(mino_state_t *S, mino_val_t *args,
                                 mino_env_t *)
{
    long long id;
    if (!mino_is_cons(args) || !mino_to_int(mino_car(args), &id))
        return mino_nil(S);
    if (id < 0 || (size_t)id >= tasks.size())
        return mino_nil(S);
    tasks[(size_t)id].status = "done";
    tasks[(size_t)id].result = "ok";
    return mino_keyword(S, "done");
}

/* Host function: get task status as a map. */
static mino_val_t *host_task_status(mino_state_t *S, mino_val_t *args,
                                    mino_env_t *)
{
    long long id;
    if (!mino_is_cons(args) || !mino_to_int(mino_car(args), &id))
        return mino_nil(S);
    if (id < 0 || (size_t)id >= tasks.size())
        return mino_nil(S);
    auto &t = tasks[(size_t)id];
    mino_val_t *ks[3], *vs[3];
    ks[0] = mino_keyword(S, "name");   vs[0] = mino_string(S, t.name.c_str());
    ks[1] = mino_keyword(S, "status"); vs[1] = mino_keyword(S, t.status.c_str());
    ks[2] = mino_keyword(S, "result"); vs[2] = mino_string(S, t.result.c_str());
    return mino_map(S, ks, vs, 3);
}

/* ── Script ────────────────────────────────────────────────────────── */

/* Workflows are built by composing task primitives.
 * `step` takes a name, creates and runs a task, returns its status.
 * `pipeline` chains steps and collects results. */

static const char *script =
    ";; Execute one step: create, run, report.\n"
    "(defn step [name]\n"
    "  (let [id (create-task name)]\n"
    "    (run-task id)\n"
    "    (task-status id)))\n"
    "\n"
    ";; Run a sequence of named steps, collect results.\n"
    "(defn pipeline [step-names]\n"
    "  (->> step-names\n"
    "       (map step)\n"
    "       (map (fn [s] [(:name s) (:status s)]))\n"
    "       (into [])))\n"
    "\n"
    ";; Conditional step: only run if predicate holds.\n"
    "(defn when-step [pred name]\n"
    "  (when pred (step name)))\n"
    "\n"
    ";; A deployment workflow.\n"
    "(defn deploy [env-name]\n"
    "  (let [steps [\"lint\" \"test\" \"build\"\n"
    "               (str \"deploy-\" env-name)\n"
    "               \"notify\"]\n"
    "        results (pipeline steps)]\n"
    "    {:environment env-name\n"
    "     :steps       results\n"
    "     :all-done?   (every? (fn [[_ s]] (= s :done)) results)}))\n"
    "\n"
    "(deploy \"staging\")\n";

/* ── Embed ─────────────────────────────────────────────────────────── */

int main()
{
    mino_state_t *S   = mino_state_new();
    mino_env_t   *env = mino_new(S);

    /* Register task primitives. */
    mino_register_fn(S, env, "create-task",  host_create_task);
    mino_register_fn(S, env, "run-task",     host_run_task);
    mino_register_fn(S, env, "task-status",  host_task_status);

    mino_val_t *result = mino_eval_string(S, script, env);

    if (result) {
        printf("workflow result:\n");
        mino_println(S, result);

        /* Show all tasks the workflow created. */
        printf("\ntask log:\n");
        for (size_t i = 0; i < tasks.size(); i++)
            printf("  [%zu] %-20s %s\n", i,
                   tasks[i].name.c_str(), tasks[i].status.c_str());
    } else {
        fprintf(stderr, "error: %s\n", mino_last_error(S));
    }

    mino_env_free(S, env);
    mino_state_free(S);
}
