/*
 * install.c -- composes per-domain primitive tables into the core
 * environment.  Each prim/<domain>.c exports k_prims_<domain> + count;
 * mino_install_core walks k_core_domains[] and binds each entry through
 * prim_install_table.
 *
 * mino_install_io lives in prim/io.c next to its table; mino_install_fs,
 * mino_install_proc, mino_install_host, and mino_install_async likewise
 * stay next to their domain implementations.
 */

#include "prim/internal.h"

#include <string.h>

void prim_install_table(mino_state_t *S, mino_env_t *env, const char *ns_name,
                        const mino_prim_def *defs, size_t count)
{
    size_t i;
    for (i = 0; i < count; i++) {
        mino_val_t *pv = mino_prim(S, defs[i].name, defs[i].fn);
        mino_env_set(S, env, defs[i].name, pv);
        if (ns_name != NULL) {
            mino_val_t *var = var_intern(S, ns_name, defs[i].name);
            if (var != NULL) var_set_root(S, var, pv);
        }
        if (defs[i].doc != NULL) {
            meta_set(S, defs[i].name,
                     defs[i].doc, strlen(defs[i].doc), NULL);
        }
    }
}

#if defined(__clang__)
#  pragma clang diagnostic push
#  pragma clang diagnostic ignored "-Woverlength-strings"
#elif defined(__GNUC__)
#  pragma GCC diagnostic push
#  pragma GCC diagnostic ignored "-Woverlength-strings"
#endif
#include "core_mino.h"
#if defined(__clang__)
#  pragma clang diagnostic pop
#elif defined(__GNUC__)
#  pragma GCC diagnostic pop
#endif

/* Bootstraps lib/core by reading and evaluating embedded source on the
 * first call for a state, then caching the parsed AST so subsequent
 * mino_install_core calls into other envs can replay without re-parsing.
 *
 * Runs with current_ns set to "clojure.core" by the caller, so every def
 * in core.clj lands in the clojure.core ns env. */
static void install_core_mino(mino_state_t *S, mino_env_t *env)
{
    size_t i;

    if (S->core_forms == NULL) {
        const char  *src        = core_mino_src;
        const char  *saved_file = S->reader_file;
        int          saved_line = S->reader_line;
        size_t       cap        = 256;

        S->core_forms     = malloc(cap * sizeof(mino_val_t *));
        S->core_forms_len = 0;
        if (!S->core_forms) {
            /* Class I: init-time; no try-frame to recover through */
            fprintf(stderr, "core.clj: out of memory\n"); abort();
        }

        S->reader_file = intern_filename(S, "<core>");
        S->reader_line = 1;
        while (*src != '\0') {
            const char *end  = NULL;
            mino_val_t *form = mino_read(S, src, &end);
            if (form == NULL) {
                if (mino_last_error(S) != NULL) {
                    /* Class I: core library parse failure is unrecoverable */
                    fprintf(stderr, "core.clj parse error: %s\n",
                            mino_last_error(S));
                    abort();
                }
                if (end != NULL && end > src) {
                    src = end; /* reader conditional produced nothing; skip */
                    continue;
                }
                break;
            }
            if (S->core_forms_len >= cap) {
                cap *= 2;
                S->core_forms = realloc(S->core_forms,
                                        cap * sizeof(mino_val_t *));
                if (!S->core_forms) {
                    /* Class I: init-time; no try-frame to recover through */
                    fprintf(stderr, "core.clj: out of memory\n");
                    abort();
                }
            }
            S->core_forms[S->core_forms_len++] = form;
            if (mino_eval(S, form, env) == NULL) {
                /* Class I: core library eval failure is unrecoverable */
                fprintf(stderr, "core.clj eval error: %s\n",
                        mino_last_error(S));
                abort();
            }
            src = end;
        }
        S->reader_file = saved_file;
        S->reader_line = saved_line;
        return;
    }

    for (i = 0; i < S->core_forms_len; i++) {
        if (mino_eval(S, S->core_forms[i], env) == NULL) {
            /* Class I: cached core form eval failure is unrecoverable */
            fprintf(stderr, "core.clj eval error: %s\n", mino_last_error(S));
            abort();
        }
    }
}

/* k_core_domains -- ordered list of primitive domains installed into
 * every state's core env.  io_core carries pr-builtin and
 * set-print-method! so core.clj can wire its print-method multimethod
 * before any code calls pr.  Host and async domains install here even
 * though they expose mino_install_host / mino_install_async helpers, so
 * embedders bypassing mino_install_core can still install them directly.
 */
static const mino_prim_domain k_core_domains[] = {
    {"numeric",     k_prims_numeric,     &k_prims_numeric_count},
    {"meta",        k_prims_meta,        &k_prims_meta_count},
    {"collections", k_prims_collections, &k_prims_collections_count},
    {"sequences",   k_prims_sequences,   &k_prims_sequences_count},
    {"lazy",        k_prims_lazy,        &k_prims_lazy_count},
    {"string",      k_prims_string,      &k_prims_string_count},
    {"reflection",  k_prims_reflection,  &k_prims_reflection_count},
    {"regex",       k_prims_regex,       &k_prims_regex_count},
    {"stateful",    k_prims_stateful,    &k_prims_stateful_count},
    {"module",      k_prims_module,      &k_prims_module_count},
    {"ns",          k_prims_ns,          &k_prims_ns_count},
    {"bignum",      k_prims_bignum,      &k_prims_bignum_count},
    {"io_core",     k_prims_io_core,     &k_prims_io_core_count},
    {"host",        k_prims_host,        &k_prims_host_count},
    {"async",       k_prims_async,       &k_prims_async_count},
};

#define K_CORE_DOMAIN_COUNT \
    (sizeof(k_core_domains) / sizeof(k_core_domains[0]))

void mino_install_core(mino_state_t *S, mino_env_t *env)
{
    volatile char probe = 0;
    size_t i;
    mino_env_t  *core_env;
    const char  *saved_ns;

    gc_note_host_frame(S, (void *)&probe);
    (void)probe;

    /* All bundled-core bindings (primitives + core.clj defs) live in
     * the clojure.core ns env. Every other ns env (including the
     * embedder's default) chains its parent here so unqualified lookup
     * walks lexical → current-ns env → clojure.core. */
    core_env = ns_env_ensure(S, "clojure.core");

    mino_env_set(S, core_env, "math-pi", mino_float(S, 3.14159265358979323846));

    for (i = 0; i < K_CORE_DOMAIN_COUNT; i++) {
        prim_install_table(S, core_env, "clojure.core",
                           k_core_domains[i].defs,
                           *k_core_domains[i].count_ptr);
    }

    saved_ns      = S->current_ns;
    S->current_ns = "clojure.core";
    install_core_mino(S, core_env);
    S->current_ns = saved_ns;

    /* Install string operations into the clojure.string namespace,
     * matching the namespace name that Babashka, ClojureScript, and
     * Jank use. The wrappers in lib/clojure/string.clj layer
     * nil-handling on top. Putting these here instead of clojure.core
     * means a fresh user namespace doesn't accidentally inherit (and
     * shadow) names like `join` or `split` through :refer-clojure. */
    {
        mino_env_t *cs_env = ns_env_ensure(S, "clojure.string");
        if (cs_env != NULL) {
            prim_install_table(S, cs_env, "clojure.string",
                               k_prims_clojure_string,
                               k_prims_clojure_string_count);
        }
    }

    /* Install REPL helpers (`doc`, `source`, `apropos`) into the
     * clojure.repl namespace. Users opt in via
     * (require '[clojure.repl :refer [doc apropos source]]); a fresh
     * user namespace doesn't see them by default, matching canon. */
    {
        mino_env_t *cr_env = ns_env_ensure(S, "clojure.repl");
        if (cr_env != NULL) {
            prim_install_table(S, cr_env, "clojure.repl",
                               k_prims_clojure_repl,
                               k_prims_clojure_repl_count);
        }
    }

    /* The embedder's env stays parent-less. eval_symbol falls back to
     * current_ns_env after the lexical chain runs out, and per-ns envs
     * chain to clojure.core themselves -- so a namespace that detaches
     * its parent (e.g. via :refer-clojure :only) is properly isolated
     * instead of leaking core bindings through the embedder root.
     * clojure.core itself stays as-is. */
    (void)core_env;
    (void)env;

    /* Intern *ns* as a dynamic var so find-var on the qualified name
     * clojure.core followed by /-star-ns-star resolves and (deref ...)
     * tracks the user-visible namespace. The bare-symbol fast path in
     * eval/special.c stays as a fallback for embedders that look up
     * *ns* before the var is interned. */
    {
        mino_val_t *var = var_intern(S, "clojure.core", "*ns*");
        if (var != NULL) {
            var->as.var.dynamic = 1;
            if (S->current_ns == NULL) {
                S->current_ns = intern_filename(S, "user");
                (void)ns_env_ensure(S, "user");
            }
            mino_publish_current_ns(S);
        }
    }

    /* Intern *out*, *err*, and *in* as dynamic vars holding sentinel
     * keywords (:mino/stdout, :mino/stderr, :mino/stdin). The print
     * and read primitives consult these via dyn_lookup first, so a
     * (binding [*out* (atom "")] ...) wrap captures output into the
     * atom and a (binding [*in* (atom "1 2\n")] ...) wrap feeds
     * input from a string. Without a binding, the sentinel keyword
     * identifies the default FILE* source. */
    {
        mino_val_t *out_var = var_intern(S, "clojure.core", "*out*");
        mino_val_t *err_var = var_intern(S, "clojure.core", "*err*");
        mino_val_t *in_var  = var_intern(S, "clojure.core", "*in*");
        if (out_var != NULL) {
            out_var->as.var.dynamic = 1;
            var_set_root(S, out_var, mino_keyword(S, "mino/stdout"));
            mino_env_set(S, core_env, "*out*", out_var->as.var.root);
        }
        if (err_var != NULL) {
            err_var->as.var.dynamic = 1;
            var_set_root(S, err_var, mino_keyword(S, "mino/stderr"));
            mino_env_set(S, core_env, "*err*", err_var->as.var.root);
        }
        if (in_var != NULL) {
            in_var->as.var.dynamic = 1;
            var_set_root(S, in_var, mino_keyword(S, "mino/stdin"));
            mino_env_set(S, core_env, "*in*", in_var->as.var.root);
        }
    }
}
