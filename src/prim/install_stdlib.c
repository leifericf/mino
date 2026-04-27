/*
 * install_stdlib.c -- per-namespace install hooks for the bundled
 * clojure.* stdlib. Each fn registers a static C-string source under
 * the canonical namespace name, so a subsequent (require '[<ns>])
 * loads the bundled source from memory instead of going to the disk
 * resolver.
 *
 * The C primitives that some of these namespaces layer over (e.g.
 * the clojure.string ns env's `lower-case` etc.) are installed
 * separately by `mino_install_core` -- this file is concerned only
 * with making the wrapper sources available.
 *
 * Embedders pick exactly the namespaces they want; the standalone
 * binary calls `mino_install_all` to register the full set.
 */

#include "prim/internal.h"

/* Suppress -Woverlength-strings: the generated headers are single
 * literals up to a few thousand bytes, comfortably above ANSI-C's
 * 509-char minimum but well within practical compiler limits. */
#if defined(__clang__)
#  pragma clang diagnostic push
#  pragma clang diagnostic ignored "-Woverlength-strings"
#elif defined(__GNUC__)
#  pragma GCC diagnostic push
#  pragma GCC diagnostic ignored "-Woverlength-strings"
#endif
#include "lib_clojure_string.h"
#include "lib_clojure_set.h"
#include "lib_clojure_walk.h"
#include "lib_clojure_edn.h"
#include "lib_clojure_pprint.h"
#include "lib_clojure_zip.h"
#include "lib_clojure_data.h"
#include "lib_clojure_test.h"
#include "lib_clojure_template.h"
#include "lib_clojure_repl.h"
#include "lib_clojure_stacktrace.h"
#include "lib_clojure_datafy.h"
#include "lib_clojure_core_protocols.h"
#include "lib_clojure_instant.h"
#include "lib_clojure_spec_alpha.h"
#include "lib_clojure_core_specs_alpha.h"
#if defined(__clang__)
#  pragma clang diagnostic pop
#elif defined(__GNUC__)
#  pragma GCC diagnostic pop
#endif

void mino_install_clojure_string(mino_state_t *S, mino_env_t *env)
{
    (void)env;
    mino_register_bundled_lib(S, "clojure.string", lib_clojure_string_src);
}

void mino_install_clojure_set(mino_state_t *S, mino_env_t *env)
{
    (void)env;
    mino_register_bundled_lib(S, "clojure.set", lib_clojure_set_src);
}

void mino_install_clojure_walk(mino_state_t *S, mino_env_t *env)
{
    (void)env;
    mino_register_bundled_lib(S, "clojure.walk", lib_clojure_walk_src);
}

void mino_install_clojure_edn(mino_state_t *S, mino_env_t *env)
{
    (void)env;
    mino_register_bundled_lib(S, "clojure.edn", lib_clojure_edn_src);
}

void mino_install_clojure_pprint(mino_state_t *S, mino_env_t *env)
{
    (void)env;
    mino_register_bundled_lib(S, "clojure.pprint", lib_clojure_pprint_src);
}

void mino_install_clojure_zip(mino_state_t *S, mino_env_t *env)
{
    (void)env;
    mino_register_bundled_lib(S, "clojure.zip", lib_clojure_zip_src);
}

void mino_install_clojure_data(mino_state_t *S, mino_env_t *env)
{
    (void)env;
    mino_register_bundled_lib(S, "clojure.data", lib_clojure_data_src);
}

/* clojure.test + clojure.template install together. clojure.template
 * is the substitution primitive historically used by `are`; mino's
 * own `are` is self-contained but user code that references
 * clojure.template directly expects it alongside clojure.test. */
void mino_install_clojure_test(mino_state_t *S, mino_env_t *env)
{
    (void)env;
    mino_register_bundled_lib(S, "clojure.test",     lib_clojure_test_src);
    mino_register_bundled_lib(S, "clojure.template", lib_clojure_template_src);
}

/* clojure.repl + clojure.stacktrace install together: the REPL pair
 * stays as a single embedder opt-in since one references the other
 * (clojure.repl/pst delegates to clojure.stacktrace's printer). */
void mino_install_clojure_repl(mino_state_t *S, mino_env_t *env)
{
    (void)env;
    mino_register_bundled_lib(S, "clojure.repl",       lib_clojure_repl_src);
    mino_register_bundled_lib(S, "clojure.stacktrace", lib_clojure_stacktrace_src);
}

/* clojure.datafy + clojure.core.protocols: datafy depends on the
 * protocols, so the pair installs together. */
void mino_install_clojure_datafy(mino_state_t *S, mino_env_t *env)
{
    (void)env;
    mino_register_bundled_lib(S, "clojure.datafy",          lib_clojure_datafy_src);
    mino_register_bundled_lib(S, "clojure.core.protocols",  lib_clojure_core_protocols_src);
}

void mino_install_clojure_instant(mino_state_t *S, mino_env_t *env)
{
    (void)env;
    mino_register_bundled_lib(S, "clojure.instant", lib_clojure_instant_src);
}

/* clojure.spec.alpha + clojure.core.specs.alpha: spec.alpha is the
 * predicate-and-data spec engine; core.specs.alpha holds the specs
 * for core macro forms (defn-args, binding-form). The latter requires
 * spec.alpha at load time, so the pair ships together. */
void mino_install_clojure_spec(mino_state_t *S, mino_env_t *env)
{
    (void)env;
    mino_register_bundled_lib(S, "clojure.spec.alpha",
                              lib_clojure_spec_alpha_src);
    mino_register_bundled_lib(S, "clojure.core.specs.alpha",
                              lib_clojure_core_specs_alpha_src);
}

void mino_install_all(mino_state_t *S, mino_env_t *env)
{
    mino_install_core(S, env);
    mino_install_io(S, env);
    mino_install_fs(S, env);
    mino_install_proc(S, env);
    mino_install_clojure_string(S, env);
    mino_install_clojure_set(S, env);
    mino_install_clojure_walk(S, env);
    mino_install_clojure_edn(S, env);
    mino_install_clojure_pprint(S, env);
    mino_install_clojure_zip(S, env);
    mino_install_clojure_data(S, env);
    mino_install_clojure_test(S, env);
    mino_install_clojure_repl(S, env);
    mino_install_clojure_datafy(S, env);
    mino_install_clojure_instant(S, env);
    mino_install_clojure_spec(S, env);
}
