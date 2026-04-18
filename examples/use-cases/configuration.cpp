/*
 * configuration.cpp — sandboxed configuration with computed values.
 *
 * A build system loads its configuration from a mino script. The script
 * can reference host-provided environment values (platform, build mode)
 * but has no file or network access. Configuration values are computed
 * at load time using conditionals and string operations.
 *
 * Build:
 *   make
 *   c++ -std=c++17 -Isrc -o examples/use-cases/configuration \
 *       examples/use-cases/configuration.cpp src/[a-z]*.o -lm
 */

#include "mino.h"
#include <cstdio>

/* ── Expose ────────────────────────────────────────────────────────── */

/* The C++ engineer wraps host environment queries as typed handles.
 * The `BuildEnv` handle exposes platform, mode, and version as
 * read-only getters. The script sees structured data without
 * touching the process environment directly. */

struct BuildEnv {
    const char *platform;
    const char *mode;
    int         version_major;
    int         version_minor;
};

static mino_val_t *env_new(mino_state_t *S, mino_val_t *,
                           mino_val_t *, void *)
{
    auto *env = new BuildEnv{"linux", "release", 2, 7};
    return mino_handle_ex(S, env, "BuildEnv",
        [](void *p, const char *) { delete static_cast<BuildEnv *>(p); });
}

static mino_val_t *env_platform(mino_state_t *S, mino_val_t *target,
                                mino_val_t *, void *)
{
    auto *env = static_cast<BuildEnv *>(mino_handle_ptr(target));
    return mino_keyword(S, env->platform);
}

static mino_val_t *env_mode(mino_state_t *S, mino_val_t *target,
                            mino_val_t *, void *)
{
    auto *env = static_cast<BuildEnv *>(mino_handle_ptr(target));
    return mino_keyword(S, env->mode);
}

static mino_val_t *env_version(mino_state_t *S, mino_val_t *target,
                               mino_val_t *, void *)
{
    auto *env = static_cast<BuildEnv *>(mino_handle_ptr(target));
    mino_val_t *items[2];
    items[0] = mino_int(S, env->version_major);
    items[1] = mino_int(S, env->version_minor);
    return mino_vector(S, items, 2);
}

/* ── Script ────────────────────────────────────────────────────────── */

static const char *script =
    ";; Build configuration with computed values.\n"
    ";;\n"
    ";; The script queries the host environment through a BuildEnv\n"
    ";; handle and computes paths, flags, and feature toggles.\n"
    ";; No file access, no network, no ambient globals.\n"
    "\n"
    "(let [env    (new BuildEnv)\n"
    "      plat   (.-platform env)\n"
    "      mode   (.-mode env)\n"
    "      ver    (.-version env)\n"
    "      debug? (= mode :debug)]\n"
    "  {:output-dir (str \"build/\" (name plat) \"/\" (name mode))\n"
    "   :version    (str (first ver) \".\" (nth ver 1))\n"
    "   :opt-level  (if debug? 0 2)\n"
    "   :features   (cond-> #{:core :logging}\n"
    "                 debug?             (conj :profiler)\n"
    "                 (= plat :linux)    (conj :epoll)\n"
    "                 (= plat :macos)    (conj :kqueue))\n"
    "   :defines    (->> [(when debug? \"DEBUG=1\")\n"
    "                     (str \"VERSION=\" (first ver) (nth ver 1))]\n"
    "                    (remove nil?)\n"
    "                    (into []))})\n";

/* ── Embed ─────────────────────────────────────────────────────────── */

int main()
{
    /* Create a sandboxed runtime: core bindings only, no I/O. */
    mino_state_t *S   = mino_state_new();
    mino_env_t   *env = mino_env_new(S);
    mino_install_core(S, env);

    /* Register the BuildEnv type. */
    mino_host_enable(S);
    mino_host_register_ctor(S, "BuildEnv", 0, env_new, nullptr);
    mino_host_register_getter(S, "BuildEnv", "platform", env_platform, nullptr);
    mino_host_register_getter(S, "BuildEnv", "mode", env_mode, nullptr);
    mino_host_register_getter(S, "BuildEnv", "version", env_version, nullptr);

    /* Evaluate the config script. */
    mino_val_t *cfg = mino_eval_string(S, script, env);

    if (cfg) {
        printf("config: ");
        mino_println(S, cfg);

        /* Extract a single value for the C++ side. */
        mino_env_set(S, env, "__cfg", cfg);
        mino_val_t *dir = mino_eval_string(S, "(get __cfg :output-dir)", env);
        if (dir) {
            const char *s;
            size_t n;
            if (mino_to_string(dir, &s, &n))
                printf("output-dir: %.*s\n", (int)n, s);
        }
    } else {
        fprintf(stderr, "error: %s\n", mino_last_error(S));
    }

    mino_env_free(S, env);
    mino_state_free(S);
}
