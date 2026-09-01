/*
 * install_stdlib.c -- per-namespace install hooks for the bundled
 * clojure.* stdlib. Each fn registers a static C-string source under
 * the canonical namespace name, so a subsequent (require '[<ns>])
 * loads the bundled source from memory instead of going to the disk
 * resolver.
 *
 * The C primitives that some of these namespaces layer over (e.g.
 * the clojure.string ns env's `lower-case` etc.) are installed
 * separately by `mino_install_clojure_core` -- this file is
 * concerned only with making the wrapper sources available.
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
#include "lib_clojure_math.h"
#include "lib_clojure_walk.h"
#include "lib_clojure_edn.h"
#include "lib_clojure_pprint.h"
#include "lib_clojure_zip.h"
#include "lib_clojure_xml.h"
#include "lib_clojure_data.h"
#include "lib_clojure_data_json.h"
#include "lib_clojure_data_csv.h"
#include "lib_clojure_test.h"
#include "lib_clojure_template.h"
#include "lib_clojure_repl.h"
#include "lib_clojure_stacktrace.h"
#include "lib_clojure_datafy.h"
#include "lib_clojure_core_protocols.h"
#include "lib_clojure_core_reducers.h"
#include "lib_clojure_instant.h"
#include "lib_clojure_spec_alpha.h"
#include "lib_clojure_spec_gen_alpha.h"
#include "lib_clojure_spec_test_alpha.h"
#include "lib_clojure_core_specs_alpha.h"
#include "lib_clojure_core_unify.h"
#include "lib_clojure_core_cache.h"
#include "lib_clojure_core_memoize.h"
#include "lib_clojure_core_match.h"
#include "lib_clojure_core_logic.h"
#include "lib_clojure_core_logic_fd.h"
#include "lib_clojure_core_logic_nominal.h"
#include "lib_clojure_test_tap.h"
#include "lib_clojure_test_junit.h"
#include "lib_clojure_test_check_generators.h"
#include "lib_clojure_test_check_properties.h"
#include "lib_clojure_test_check.h"
#include "lib_mino_deps.h"
#include "lib_mino_tasks.h"
#include "lib_mino_tasks_builtin.h"
#include "lib_mino_store.h"
#include "lib_mino_http.h"
#include "lib_mino_time.h"
#include "lib_mino_path.h"
#include "lib_mino_cli.h"
#include "lib_mino_digest.h"
#include "lib_mino_env.h"
#include "lib_mino_term.h"
#include "lib_mino_log.h"
#include "lib_mino_toml.h"
#include "lib_mino_yaml.h"
#include "lib_mino_html.h"
#include "lib_mino_html_select.h"
#include "lib_mino_template.h"
#include "lib_mino_zip.h"
#include "lib_mino_shell.h"
#if defined(__clang__)
#  pragma clang diagnostic pop
#elif defined(__GNUC__)
#  pragma GCC diagnostic pop
#endif

void mino_install_clojure_string(mino_state *S, mino_env *env)
{
    (void)env;
    mino_register_bundled_lib(S, "clojure.string", lib_clojure_string_src);
}

void mino_install_clojure_set(mino_state *S, mino_env *env)
{
    (void)env;
    mino_register_bundled_lib(S, "clojure.set", lib_clojure_set_src);
}

void mino_install_clojure_math(mino_state *S, mino_env *env)
{
    (void)env;
    mino_register_bundled_lib(S, "clojure.math", lib_clojure_math_src);
}

void mino_install_clojure_walk(mino_state *S, mino_env *env)
{
    (void)env;
    mino_register_bundled_lib(S, "clojure.walk", lib_clojure_walk_src);
}

void mino_install_clojure_edn(mino_state *S, mino_env *env)
{
    (void)env;
    mino_register_bundled_lib(S, "clojure.edn", lib_clojure_edn_src);
}

void mino_install_clojure_pprint(mino_state *S, mino_env *env)
{
    (void)env;
    mino_register_bundled_lib(S, "clojure.pprint", lib_clojure_pprint_src);
}

void mino_install_clojure_zip(mino_state *S, mino_env *env)
{
    (void)env;
    mino_register_bundled_lib(S, "clojure.zip", lib_clojure_zip_src);
}

/* clojure.xml: the strict xml-parse reader prim installs under the XML
 * capability, with the JVM-mirror sugar layered over it in the bundled
 * source. Gated out of the floor so a minimal embed carries neither. */
void mino_install_clojure_xml(mino_state *S, mino_env *env)
{
    mino_env *core_env = ns_env_ensure(S, "clojure.core");
    (void)env;
    prim_install_table_with_capability(S, core_env, "clojure.core",
                                       k_prims_xml, k_prims_xml_count, "xml");
    mino_register_bundled_lib(S, "clojure.xml", lib_clojure_xml_src);
    S->caps_installed |= MINO_CAP_XML;
}

void mino_install_clojure_data(mino_state *S, mino_env *env)
{
    (void)env;
    mino_register_bundled_lib(S, "clojure.data", lib_clojure_data_src);
}

/* clojure.data.json: the json-parse reader prim installs under the JSON
 * capability (not the floor), so a minimal embed carries neither the
 * prim nor the bundled read-str / write-str sugar layered over it. The
 * bit is set here as well as by the capability dispatch loop, matching
 * mino_install_store, so a direct call sets it too. */
void mino_install_clojure_data_json(mino_state *S, mino_env *env)
{
    mino_env *core_env = ns_env_ensure(S, "clojure.core");
    (void)env;
    prim_install_table_with_capability(S, core_env, "clojure.core",
                                       k_prims_json, k_prims_json_count,
                                       "json");
    mino_register_bundled_lib(S, "clojure.data.json",
                              lib_clojure_data_json_src);
    S->caps_installed |= MINO_CAP_JSON;
}

/* clojure.data.csv: the csv-parse reader prim installs under the CSV
 * capability, mirroring the json path above. */
void mino_install_clojure_data_csv(mino_state *S, mino_env *env)
{
    mino_env *core_env = ns_env_ensure(S, "clojure.core");
    (void)env;
    prim_install_table_with_capability(S, core_env, "clojure.core",
                                       k_prims_csv, k_prims_csv_count,
                                       "csv");
    mino_register_bundled_lib(S, "clojure.data.csv",
                              lib_clojure_data_csv_src);
    S->caps_installed |= MINO_CAP_CSV;
}

/* clojure.test + clojure.template install together. clojure.template
 * is the substitution primitive historically used by `are`; mino's
 * own `are` is self-contained but user code that references
 * clojure.template directly expects it alongside clojure.test. */
void mino_install_clojure_test(mino_state *S, mino_env *env)
{
    (void)env;
    mino_register_bundled_lib(S, "clojure.test",     lib_clojure_test_src);
    mino_register_bundled_lib(S, "clojure.template", lib_clojure_template_src);
    /* clojure.test.tap + clojure.test.junit are pure-emission reporters
     * layered over clojure.test's report multimethod, so they ship with
     * the TEST capability. */
    mino_register_bundled_lib(S, "clojure.test.tap",   lib_clojure_test_tap_src);
    mino_register_bundled_lib(S, "clojure.test.junit", lib_clojure_test_junit_src);
}

/* clojure.repl + clojure.stacktrace install together: the REPL pair
 * stays as a single embedder opt-in since one references the other
 * (clojure.repl/pst delegates to clojure.stacktrace's printer). */
void mino_install_clojure_repl(mino_state *S, mino_env *env)
{
    (void)env;
    mino_register_bundled_lib(S, "clojure.repl",       lib_clojure_repl_src);
    mino_register_bundled_lib(S, "clojure.stacktrace", lib_clojure_stacktrace_src);
}

/* clojure.datafy + clojure.core.protocols: datafy depends on the
 * protocols, so the pair installs together. */
void mino_install_clojure_datafy(mino_state *S, mino_env *env)
{
    (void)env;
    mino_register_bundled_lib(S, "clojure.datafy",          lib_clojure_datafy_src);
    mino_register_bundled_lib(S, "clojure.core.protocols",  lib_clojure_core_protocols_src);
}

/* clojure.core.reducers: sequential transducer-layer wrapper. Parallel
 * fork/join is deferred until the multi-state OS-thread cycle. */
void mino_install_clojure_reducers(mino_state *S, mino_env *env)
{
    (void)env;
    mino_register_bundled_lib(S, "clojure.core.reducers",
                              lib_clojure_core_reducers_src);
}

void mino_install_clojure_instant(mino_state *S, mino_env *env)
{
    (void)env;
    mino_register_bundled_lib(S, "clojure.instant", lib_clojure_instant_src);
}

/* clojure.spec.alpha + clojure.core.specs.alpha: spec.alpha is the
 * predicate-and-data spec engine; core.specs.alpha holds the specs
 * for core macro forms (defn-args, binding-form). The latter requires
 * spec.alpha at load time, so the pair ships together. */
void mino_install_clojure_spec(mino_state *S, mino_env *env)
{
    (void)env;
    mino_register_bundled_lib(S, "clojure.spec.alpha",
                              lib_clojure_spec_alpha_src);
    mino_register_bundled_lib(S, "clojure.spec.gen.alpha",
                              lib_clojure_spec_gen_alpha_src);
    mino_register_bundled_lib(S, "clojure.spec.test.alpha",
                              lib_clojure_spec_test_alpha_src);
    mino_register_bundled_lib(S, "clojure.core.specs.alpha",
                              lib_clojure_core_specs_alpha_src);
}

/* clojure.core.unify: a small, pure first-order unifier over ordinary
 * data. No host dependency, so it stands alone under its own bit. */
void mino_install_clojure_unify(mino_state *S, mino_env *env)
{
    (void)env;
    mino_register_bundled_lib(S, "clojure.core.unify",
                              lib_clojure_core_unify_src);
}

/* clojure.core.cache + clojure.core.memoize: memoize is layered over the
 * cache protocol, so the dependent pair ships together under one bit. */
void mino_install_clojure_cache(mino_state *S, mino_env *env)
{
    (void)env;
    mino_register_bundled_lib(S, "clojure.core.cache",
                              lib_clojure_core_cache_src);
    mino_register_bundled_lib(S, "clojure.core.memoize",
                              lib_clojure_core_memoize_src);
}

/* clojure.core.match: pattern matching compiled to a decision structure.
 * Pure mino with no host dependency, so it stands alone under its bit. */
void mino_install_clojure_match(mino_state *S, mino_env *env)
{
    (void)env;
    mino_register_bundled_lib(S, "clojure.core.match",
                              lib_clojure_core_match_src);
}

/* clojure.core.logic + .fd + .nominal: relational logic programming and
 * its finite-domain and nominal companions ship together under one bit;
 * fd and nominal require the core, so all three register as a group. */
void mino_install_clojure_logic(mino_state *S, mino_env *env)
{
    (void)env;
    mino_register_bundled_lib(S, "clojure.core.logic",
                              lib_clojure_core_logic_src);
    mino_register_bundled_lib(S, "clojure.core.logic.fd",
                              lib_clojure_core_logic_fd_src);
    mino_register_bundled_lib(S, "clojure.core.logic.nominal",
                              lib_clojure_core_logic_nominal_src);
}

/* clojure.test.check + generators + properties: minimal property
 * runner used by clojure.spec.alpha's s/gen and s/exercise. The
 * three files install together so a single (require
 * '[clojure.test.check :as tc]) brings the full surface online. */
void mino_install_clojure_test_check(mino_state *S, mino_env *env)
{
    (void)env;
    mino_register_bundled_lib(S, "clojure.test.check.generators",
                              lib_clojure_test_check_generators_src);
    mino_register_bundled_lib(S, "clojure.test.check.properties",
                              lib_clojure_test_check_properties_src);
    mino_register_bundled_lib(S, "clojure.test.check",
                              lib_clojure_test_check_src);
}

/* mino.deps + mino.tasks + mino.tasks.builtin: the standalone-binary
 * tooling that backs the `mino deps` and `mino task` subcommands.
 * Bundled so brew/scoop installs without a lib/ on cwd still work. */
void mino_install_mino_tooling(mino_state *S, mino_env *env)
{
    (void)env;
    mino_register_bundled_lib(S, "mino.deps",          lib_mino_deps_src);
    mino_register_bundled_lib(S, "mino.tasks",         lib_mino_tasks_src);
    mino_register_bundled_lib(S, "mino.tasks.builtin", lib_mino_tasks_builtin_src);
}

void mino_install_mino_store(mino_state *S, mino_env *env)
{
    (void)env;
    mino_register_bundled_lib(S, "mino.store", lib_mino_store_src);
}

void mino_install_mino_http(mino_state *S, mino_env *env)
{
    (void)env;
    mino_register_bundled_lib(S, "mino.http", lib_mino_http_src);
}

/* Each of the pure-data / info-only namespaces below installs its C
 * prims under its own capability (so a minimal embed carries neither the
 * prim nor the bundled source), registers the bundled Clojure source,
 * and sets the bit for a direct call. The install order relative to
 * prerequisites (log needs time, html.select needs zip) is handled by
 * the capability-dependency closure in mino_install. */
void mino_install_mino_time(mino_state *S, mino_env *env)
{
    mino_env *core_env = ns_env_ensure(S, "clojure.core");
    (void)env;
    prim_install_table_with_capability(S, core_env, "clojure.core",
                                       k_prims_time, k_prims_time_count, "time");
    mino_register_bundled_lib(S, "mino.time", lib_mino_time_src);
    S->caps_installed |= MINO_CAP_TIME;
}

void mino_install_mino_path(mino_state *S, mino_env *env)
{
    mino_env *core_env = ns_env_ensure(S, "clojure.core");
    (void)env;
    prim_install_table_with_capability(S, core_env, "clojure.core",
                                       k_prims_path, k_prims_path_count, "path");
    mino_register_bundled_lib(S, "mino.path", lib_mino_path_src);
    S->caps_installed |= MINO_CAP_PATH;
}

void mino_install_mino_cli(mino_state *S, mino_env *env)
{
    (void)env;
    mino_register_bundled_lib(S, "mino.cli", lib_mino_cli_src);
    S->caps_installed |= MINO_CAP_CLI;
}

void mino_install_mino_digest(mino_state *S, mino_env *env)
{
    mino_env *core_env = ns_env_ensure(S, "clojure.core");
    (void)env;
    prim_install_table_with_capability(S, core_env, "clojure.core",
                                       k_prims_digest, k_prims_digest_count,
                                       "digest");
    mino_register_bundled_lib(S, "mino.digest", lib_mino_digest_src);
    S->caps_installed |= MINO_CAP_DIGEST;
}

void mino_install_mino_env(mino_state *S, mino_env *env)
{
    (void)env;
    mino_register_bundled_lib(S, "mino.env", lib_mino_env_src);
    S->caps_installed |= MINO_CAP_ENV;
}

void mino_install_mino_term(mino_state *S, mino_env *env)
{
    mino_env *core_env = ns_env_ensure(S, "clojure.core");
    (void)env;
    prim_install_table_with_capability(S, core_env, "clojure.core",
                                       k_prims_term, k_prims_term_count, "term");
    mino_register_bundled_lib(S, "mino.term", lib_mino_term_src);
    S->caps_installed |= MINO_CAP_TERM;
}

void mino_install_mino_log(mino_state *S, mino_env *env)
{
    (void)env;
    mino_register_bundled_lib(S, "mino.log", lib_mino_log_src);
    S->caps_installed |= MINO_CAP_LOG;
}

void mino_install_mino_toml(mino_state *S, mino_env *env)
{
    mino_env *core_env = ns_env_ensure(S, "clojure.core");
    (void)env;
    prim_install_table_with_capability(S, core_env, "clojure.core",
                                       k_prims_toml, k_prims_toml_count, "toml");
    mino_register_bundled_lib(S, "mino.toml", lib_mino_toml_src);
    S->caps_installed |= MINO_CAP_TOML;
}

void mino_install_mino_yaml(mino_state *S, mino_env *env)
{
    mino_env *core_env = ns_env_ensure(S, "clojure.core");
    (void)env;
    prim_install_table_with_capability(S, core_env, "clojure.core",
                                       k_prims_yaml, k_prims_yaml_count, "yaml");
    mino_register_bundled_lib(S, "mino.yaml", lib_mino_yaml_src);
    S->caps_installed |= MINO_CAP_YAML;
}

/* HTML installs the tolerant html-parse prim plus both bundled sources:
 * mino.html and the mino.html.select subset. The select surface leans on
 * clojure.zip, which the closure pulls in alongside HTML. */
void mino_install_mino_html(mino_state *S, mino_env *env)
{
    mino_env *core_env = ns_env_ensure(S, "clojure.core");
    (void)env;
    prim_install_table_with_capability(S, core_env, "clojure.core",
                                       k_prims_html, k_prims_html_count, "html");
    mino_register_bundled_lib(S, "mino.html", lib_mino_html_src);
    S->caps_installed |= MINO_CAP_HTML;
}

void mino_install_mino_html_select(mino_state *S, mino_env *env)
{
    (void)env;
    mino_register_bundled_lib(S, "mino.html.select",
                              lib_mino_html_select_src);
    S->caps_installed |= MINO_CAP_HTML;
}

void mino_install_mino_template(mino_state *S, mino_env *env)
{
    (void)env;
    mino_register_bundled_lib(S, "mino.template", lib_mino_template_src);
    S->caps_installed |= MINO_CAP_TEMPLATE;
}

void mino_install_mino_zip(mino_state *S, mino_env *env)
{
    mino_env *core_env = ns_env_ensure(S, "clojure.core");
    (void)env;
    prim_install_table_with_capability(S, core_env, "clojure.core",
                                       k_prims_archive, k_prims_archive_count,
                                       "archive");
    mino_register_bundled_lib(S, "mino.zip", lib_mino_zip_src);
    S->caps_installed |= MINO_CAP_ARCHIVE;
}

/* Prim-only capabilities: no bundled namespace, just the C prims tagged
 * with their group so (mino-capability 'sym) and the MNS002 diagnostic
 * report them. */
void mino_install_codec(mino_state *S, mino_env *env)
{
    mino_env *core_env = ns_env_ensure(S, "clojure.core");
    (void)env;
    prim_install_table_with_capability(S, core_env, "clojure.core",
                                       k_prims_url, k_prims_url_count, "codec");
    prim_install_table_with_capability(S, core_env, "clojure.core",
                                       k_prims_codec, k_prims_codec_count,
                                       "codec");
    S->caps_installed |= MINO_CAP_CODEC;
}

void mino_install_compress(mino_state *S, mino_env *env)
{
    mino_env *core_env = ns_env_ensure(S, "clojure.core");
    (void)env;
    prim_install_table_with_capability(S, core_env, "clojure.core",
                                       k_prims_gzip, k_prims_gzip_count,
                                       "compress");
    prim_install_table_with_capability(S, core_env, "clojure.core",
                                       k_prims_compress, k_prims_compress_count,
                                       "compress");
    S->caps_installed |= MINO_CAP_COMPRESS;
}

/* Pure scripting helpers: mino.shell, mino.retry, mino.wait, mino.mime.
 * No C prims; the namespaces are bundled mino-side source only. */
void mino_install_mino_util(mino_state *S, mino_env *env)
{
    (void)env;
    mino_register_bundled_lib(S, "mino.shell", lib_mino_shell_src);
    S->caps_installed |= MINO_CAP_UTIL;
}

void mino_install_all(mino_state *S, mino_env *env)
{
    mino_install(S, env, MINO_CAP_ALL);
}
