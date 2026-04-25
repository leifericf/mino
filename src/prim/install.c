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

void prim_install_table(mino_state_t *S, mino_env_t *env,
                        const mino_prim_def *defs, size_t count)
{
    size_t i;
    for (i = 0; i < count; i++) {
        mino_env_set(S, env, defs[i].name,
                     mino_prim(S, defs[i].name, defs[i].fn));
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
 * mino_install_core calls into other envs can replay without re-parsing. */
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
            fprintf(stderr, "core.mino: out of memory\n"); abort();
        }

        S->reader_file = intern_filename(S, "<core>");
        S->reader_line = 1;
        while (*src != '\0') {
            const char *end  = NULL;
            mino_val_t *form = mino_read(S, src, &end);
            if (form == NULL) {
                if (mino_last_error(S) != NULL) {
                    /* Class I: core library parse failure is unrecoverable */
                    fprintf(stderr, "core.mino parse error: %s\n",
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
                    fprintf(stderr, "core.mino: out of memory\n");
                    abort();
                }
            }
            S->core_forms[S->core_forms_len++] = form;
            if (mino_eval(S, form, env) == NULL) {
                /* Class I: core library eval failure is unrecoverable */
                fprintf(stderr, "core.mino eval error: %s\n",
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
            fprintf(stderr, "core.mino eval error: %s\n", mino_last_error(S));
            abort();
        }
    }
}

/* k_core_domains -- ordered list of primitive domains installed into
 * every state's core env.  io_core carries pr-builtin and
 * set-print-method! so core.mino can wire its print-method multimethod
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

    gc_note_host_frame(S, (void *)&probe);
    (void)probe;

    mino_env_set(S, env, "math-pi", mino_float(S, 3.14159265358979323846));

    for (i = 0; i < K_CORE_DOMAIN_COUNT; i++) {
        prim_install_table(S, env, k_core_domains[i].defs,
                           *k_core_domains[i].count_ptr);
    }

    install_core_mino(S, env);
}
