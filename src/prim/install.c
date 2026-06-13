/*
 * install.c -- composes per-domain primitive tables into the core
 * environment.  Each prim/<domain>.c exports k_prims_<domain> + count;
 * mino_install_clojure_core walks k_core_domains[] and binds each entry
 * through prim_install_table.
 *
 * mino_install_io lives in prim/io.c next to its table; mino_install_fs,
 * mino_install_proc, mino_install_host, and mino_install_async likewise
 * stay next to their domain implementations.
 */

#include "prim/internal.h"
#include "mino.h"

#include <stdlib.h>
#include <string.h>

void prim_install_table(mino_state *S, mino_env *env, const char *ns_name,
                        const mino_prim_def *defs, size_t count)
{
    prim_install_table_with_capability(S, env, ns_name, defs, count, NULL);
}

void prim_install_table_with_capability(mino_state *S, mino_env *env,
                                        const char *ns_name,
                                        const mino_prim_def *defs,
                                        size_t count,
                                        const char *capability)
{
    size_t i;
    int has_cap = capability != NULL && capability[0] != '\0';
    for (i = 0; i < count; i++) {
        /* Each def carries one or both ABI pointers. fn2 takes
         * precedence when set; fn is the fallback cons-spine
         * dispatch path. Identity checks against a prim use its
         * stable registered name rather than these pointers. */
        mino_val *pv = (defs[i].fn2 != NULL)
                         ? mino_prim_argv(S, defs[i].name, defs[i].fn2)
                         : mino_prim(S, defs[i].name, defs[i].fn);
        if (defs[i].fn2 != NULL && defs[i].fn != NULL) {
            pv->as.prim.fn = defs[i].fn;
        }
        mino_env_set(S, env, defs[i].name, pv);
        if (ns_name != NULL) {
            mino_val *var = var_intern(S, ns_name, defs[i].name);
            if (var != NULL) var_set_root(S, var, pv);
        }
        if (defs[i].doc != NULL) {
            meta_set(S, defs[i].name,
                     defs[i].doc, strlen(defs[i].doc), NULL);
        } else if (has_cap) {
            /* Tagging requires a meta entry; create a stub one so the
             * capability label is reachable via meta_find. */
            meta_set(S, defs[i].name, "", 0, NULL);
        }
        if (has_cap) {
            meta_set_capability(S, defs[i].name, capability);
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
 * mino_install_clojure_core calls into other envs can replay without
 * re-parsing.
 *
 * Runs with current_ns set to "clojure.core" by the caller, so every def
 * in core.clj lands in the clojure.core ns env. */
static void install_core_mino(mino_state *S, mino_env *env)
{
    size_t i;

    if (S->core_forms == NULL) {
        const char  *src        = core_mino_src;
        const char  *saved_file = S->reader.reader_file;
        int          saved_line = S->reader.reader_line;
        size_t       cap        = 256;

        S->core_forms     = malloc(cap * sizeof(mino_val *));
        S->core_forms_len = 0;
        if (!S->core_forms) {
            /* Class I: init-time; no try-frame to recover through */
            fprintf(stderr, "core.clj: out of memory\n"); abort();
        }

        S->reader.reader_file = intern_filename(S, "<core>");
        S->reader.reader_line = 1;
        while (*src != '\0') {
            const char *end  = NULL;
            mino_val *form = mino_read(S, src, &end);
            if (form == NULL) {
                if (mino_last_error(S) != NULL) {
                    fprintf(stderr, "core.clj parse error: %s\n",
                            mino_last_error(S));
                    /* Class I: core parse failure is unrecoverable */
                    abort();
                }
                if (end != NULL && end > src) {
                    src = end; /* reader conditional produced nothing; skip */
                    continue;
                }
                break;
            }
            if (S->core_forms_len >= cap) {
                mino_val **tmp;
                cap *= 2;
                tmp = (mino_val **)realloc(S->core_forms,
                                           cap * sizeof(mino_val *));
                if (!tmp) {
                    fprintf(stderr, "core.clj: out of memory\n");
                    /* Class I: init-time OOM; no try-frame to recover through */
                    abort();
                }
                S->core_forms = tmp;
            }
            S->core_forms[S->core_forms_len++] = form;
            if (mino_eval(S, form, env) == NULL) {
                fprintf(stderr, "core.clj eval error: %s\n",
                        mino_last_error(S));
                /* Class I: core eval failure is unrecoverable */
                abort();
            }
            src = end;
        }
        S->reader.reader_file = saved_file;
        S->reader.reader_line = saved_line;
        return;
    }

    for (i = 0; i < S->core_forms_len; i++) {
        if (mino_eval(S, S->core_forms[i], env) == NULL) {
            fprintf(stderr, "core.clj eval error: %s\n", mino_last_error(S));
            /* Class I: cached core form eval failure is unrecoverable */
            abort();
        }
    }
}

/* k_core_domains -- ordered list of primitive domains installed into
 * every state's floor env. io_core carries pr-builtin and
 * set-print-method! so core.clj can wire its print-method multimethod
 * before any code calls pr.
 *
 * Capability-gated domains (regex, bignum, host, async, io, fs, proc,
 * stm, agent) are NOT in this list. They install via their own
 * `mino_install_<cap>` entry point; the standalone CLI brings them in
 * through `mino_install_clojure_core` / `mino_install_all`.
 */
static const mino_prim_domain k_core_domains[] = {
    {"numeric",     k_prims_numeric,     &k_prims_numeric_count},
    {"meta",        k_prims_meta,        &k_prims_meta_count},
    {"collections", k_prims_collections, &k_prims_collections_count},
    {"bits",        k_prims_bits,        &k_prims_bits_count},
    {"sequences",   k_prims_sequences,   &k_prims_sequences_count},
    {"lazy",        k_prims_lazy,        &k_prims_lazy_count},
    {"string",      k_prims_string,      &k_prims_string_count},
    {"reflection",  k_prims_reflection,  &k_prims_reflection_count},
    {"stateful",    k_prims_stateful,    &k_prims_stateful_count},
    {"module",      k_prims_module,      &k_prims_module_count},
    {"ns",          k_prims_ns,          &k_prims_ns_count},
    {"io_core",     k_prims_io_core,     &k_prims_io_core_count},
};

#define K_CORE_DOMAIN_COUNT \
    (sizeof(k_core_domains) / sizeof(k_core_domains[0]))

/* ------------------------------------------------------------------------- */
/* Capability registry                                                       */
/* ------------------------------------------------------------------------- */

static const mino_capability_info k_capability_info[] = {
    { "floor",        MINO_CAP_FLOOR,
      "Numeric, collections, sequences, printing, foundational macros." },
    { "regex",        MINO_CAP_REGEX,
      "re-pattern / re-find / re-matches / re-seq / re-matcher." },
    { "bignum",       MINO_CAP_BIGNUM,
      "Arbitrary-precision integers, ratios, decimals, primed arithmetic." },
    { "multimethods", MINO_CAP_MULTIMETHODS,
      "defmulti / defmethod / hierarchies / print-method dispatch." },
    { "protocols",    MINO_CAP_PROTOCOLS,
      "defprotocol / extend-protocol / extend-type / satisfies?." },
    { "transducers",  MINO_CAP_TRANSDUCERS,
      "transduce / sequence / xform-arity into / completing / halt-when." },
    { "io",           MINO_CAP_IO,
      "slurp / spit / read-line / time-ms / nano-time / file-seq." },
    { "fs",           MINO_CAP_FS,
      "file-exists? / directory? / mkdir-p / rm-rf." },
    { "proc",         MINO_CAP_PROC,
      "sh / sh! (external process execution)." },
    { "stm",          MINO_CAP_STM,
      "ref / dosync / alter / commute / ensure / ref-set." },
    { "agent",        MINO_CAP_AGENT,
      "agent / send / send-off / await / agent-error / restart-agent." },
    { "host",         MINO_CAP_HOST,
      "Host interop dispatcher (FFI registry for embedder values)." },
    { "async",        MINO_CAP_ASYNC,
      "chan / go / <! / >! / alts! / timeout (core.async surface)." },
    { "string-lib",   MINO_CAP_STRING_LIB,
      "clojure.string (capitalize / split / replace / join / ...)." },
    { "set-lib",      MINO_CAP_SET_LIB,
      "clojure.set (union / intersection / difference / project / ...)." },
    { "walk",         MINO_CAP_WALK,
      "clojure.walk (postwalk / prewalk / keywordize-keys / ...)." },
    { "edn",          MINO_CAP_EDN,
      "clojure.edn (read / read-string with EDN safety)." },
    { "pprint",       MINO_CAP_PPRINT,
      "clojure.pprint (pprint / cl-format / pprint-tabular)." },
    { "zip",          MINO_CAP_ZIP,
      "clojure.zip (vector / xml / tree zipper navigation)." },
    { "data",         MINO_CAP_DATA,
      "clojure.data (diff / EqualityPartition)." },
    { "test",         MINO_CAP_TEST,
      "clojure.test (deftest / is / are / testing) + test.check." },
    { "repl-lib",     MINO_CAP_REPL_LIB,
      "clojure.repl (doc / source / apropos)." },
    { "datafy",       MINO_CAP_DATAFY,
      "clojure.datafy + clojure.core.protocols (datafy / nav)." },
    { "instant",      MINO_CAP_INSTANT,
      "clojure.instant (read-instant-date / -calendar / -timestamp)." },
    { "spec",         MINO_CAP_SPEC,
      "clojure.spec.alpha (spec/def / spec/valid? / spec/explain / ...)." },
    { "tooling",      MINO_CAP_TOOLING,
      "mino.deps + mino.tasks (deps resolution, task runner)." },
    { "math-lib",     MINO_CAP_MATH_LIB,
      "clojure.math (sqrt / sin / log / floor / etc.)." },
    { "reducers",     MINO_CAP_REDUCERS,
      "clojure.core.reducers (sequential transducer-layer wrapper)." },
    { "unify",        MINO_CAP_UNIFY,
      "clojure.core.unify (first-order unification: unify / unifier / subst)." },
    { "cache",        MINO_CAP_CACHE,
      "clojure.core.cache + clojure.core.memoize (cache protocol, memoizers)." },
    { "match",        MINO_CAP_MATCH,
      "clojure.core.match (pattern matching: match / matchv / match-let)." },
    { NULL,           0u,                                                NULL },
};

const mino_capability_info *mino_capability_list(void)
{
    return k_capability_info;
}

unsigned int mino_capabilities(const mino_state *S)
{
    return (S != NULL) ? S->caps_installed : 0u;
}

int mino_capability_installed(const mino_state *S, unsigned int cap)
{
    if (S == NULL || cap == 0u) return 0;
    return (S->caps_installed & cap) == cap;
}

/* Hand-curated map of canonical Clojure-side names → capability. Used
 * by the eval_symbol MNS002 diagnostic so an unbound `defmulti` reports
 * "capability 'multimethods' disabled by host" instead of a bare
 * unbound-symbol error. Order doesn't matter; lookup is linear.
 *
 * Names that are C primitives (re-find, bigint, slurp, sh, ref, agent,
 * chan, ...) are NOT listed here -- they're discovered through their
 * capability label via a sweep of the per-capability prim tables. */
typedef struct {
    const char  *name;
    unsigned int cap;
} clj_name_capability_t;

static const clj_name_capability_t k_clj_capability_names[] = {
    /* multimethods */
    { "defmulti",         MINO_CAP_MULTIMETHODS },
    { "defmethod",        MINO_CAP_MULTIMETHODS },
    { "make-hierarchy",   MINO_CAP_MULTIMETHODS },
    { "derive",           MINO_CAP_MULTIMETHODS },
    { "underive",         MINO_CAP_MULTIMETHODS },
    { "isa?",             MINO_CAP_MULTIMETHODS },
    { "ancestors",        MINO_CAP_MULTIMETHODS },
    { "descendants",      MINO_CAP_MULTIMETHODS },
    { "prefer-method",    MINO_CAP_MULTIMETHODS },
    { "remove-method",    MINO_CAP_MULTIMETHODS },
    { "methods",          MINO_CAP_MULTIMETHODS },
    { "get-method",       MINO_CAP_MULTIMETHODS },
    { "prefers",          MINO_CAP_MULTIMETHODS },
    { "print-method",     MINO_CAP_MULTIMETHODS },
    /* protocols */
    { "defprotocol",      MINO_CAP_PROTOCOLS },
    { "extend-protocol",  MINO_CAP_PROTOCOLS },
    { "extend-type",      MINO_CAP_PROTOCOLS },
    { "extend",           MINO_CAP_PROTOCOLS },
    { "extends?",         MINO_CAP_PROTOCOLS },
    { "satisfies?",       MINO_CAP_PROTOCOLS },
    { "protocol-dispatch",MINO_CAP_PROTOCOLS },
    /* transducers */
    { "transduce",        MINO_CAP_TRANSDUCERS },
    { "sequence",         MINO_CAP_TRANSDUCERS },
    { "eduction",         MINO_CAP_TRANSDUCERS },
    { "completing",       MINO_CAP_TRANSDUCERS },
    { "halt-when",        MINO_CAP_TRANSDUCERS },
    { "cat",              MINO_CAP_TRANSDUCERS },
    { "internal-reduce",  MINO_CAP_TRANSDUCERS },
    /* regex helpers defined in core.clj */
    { "re-seq",           MINO_CAP_REGEX },
    { "re-matcher",       MINO_CAP_REGEX },
    { "re-groups",        MINO_CAP_REGEX },
    /* bignum primed arithmetic in core.clj */
    { "+'",               MINO_CAP_BIGNUM },
    { "-'",               MINO_CAP_BIGNUM },
    { "*'",               MINO_CAP_BIGNUM },
    { "inc'",             MINO_CAP_BIGNUM },
    { "dec'",             MINO_CAP_BIGNUM },
    { NULL,               0u },
};

/* Per-capability prim-table sweep registry. mino_capability_for_symbol
 * walks these tables looking for a name match. Order them by frequency
 * (regex / bignum first; agent / host last) for the common-case win on
 * partial-install diagnostics. */
typedef struct {
    unsigned int          cap;
    const mino_prim_def  *defs;
    const size_t         *count_ptr;
} cap_prim_table_t;

static const cap_prim_table_t k_cap_prim_tables[] = {
    { MINO_CAP_REGEX,  k_prims_regex,  &k_prims_regex_count  },
    { MINO_CAP_BIGNUM, k_prims_bignum, &k_prims_bignum_count },
    { MINO_CAP_IO,     k_prims_io,     &k_prims_io_count     },
    { MINO_CAP_FS,     k_prims_fs,     &k_prims_fs_count     },
    { MINO_CAP_PROC,   k_prims_proc,   &k_prims_proc_count   },
    { MINO_CAP_STM,    k_prims_stm,    &k_prims_stm_count    },
    { MINO_CAP_HOST,   k_prims_host,   &k_prims_host_count   },
    { MINO_CAP_ASYNC,  k_prims_async,  &k_prims_async_count  },
};

#define K_CAP_PRIM_TABLE_COUNT \
    (sizeof(k_cap_prim_tables) / sizeof(k_cap_prim_tables[0]))

static const mino_capability_info *capability_info_for_bit(unsigned int bit)
{
    const mino_capability_info *p;
    for (p = k_capability_info; p->name != NULL; p++) {
        if (p->bit == bit) return p;
    }
    return NULL;
}

const mino_capability_info *mino_capability_for_symbol(const char *name)
{
    size_t i, j;
    const clj_name_capability_t *cn;
    if (name == NULL || name[0] == '\0') return NULL;

    /* Hand-curated Clojure-side names first -- short list, fast hit. */
    for (cn = k_clj_capability_names; cn->name != NULL; cn++) {
        if (strcmp(cn->name, name) == 0) {
            return capability_info_for_bit(cn->cap);
        }
    }

    /* Sweep capability-tagged prim tables. Each prim's name is checked
     * directly; on hit, return the owning capability. */
    for (i = 0; i < K_CAP_PRIM_TABLE_COUNT; i++) {
        const mino_prim_def *defs = k_cap_prim_tables[i].defs;
        size_t               n    = *k_cap_prim_tables[i].count_ptr;
        for (j = 0; j < n; j++) {
            if (defs[j].name != NULL && strcmp(defs[j].name, name) == 0) {
                return capability_info_for_bit(k_cap_prim_tables[i].cap);
            }
        }
    }
    return NULL;
}

/* ------------------------------------------------------------------------- */
/* `mino-installed?` primitive                                               */
/* ------------------------------------------------------------------------- */

/* (mino-installed? cap) -- consults the runtime's capability bitmask.
 * cap may be a keyword, symbol, or string carrying the capability label
 * ("io", "regex", ...). Returns true / false. The floor always reports
 * true. Used by core.clj `(when (mino-installed? :cap) ...)` gates so
 * optional sections skip cleanly when their capability is off. */
static mino_val *prim_mino_installed_p(mino_state *S, mino_val *args,
                                          mino_env *env)
{
    mino_val *arg;
    const char *label = NULL;
    const mino_capability_info *p;
    (void)env;

    if (mino_args_parse(S, "mino-installed?", args, "v", &arg) != 0) {
        return NULL;
    }
    if (arg == NULL) return mino_false(S);
    switch (mino_type_of(arg)) {
    case MINO_KEYWORD: case MINO_STRING: case MINO_SYMBOL:
        label = arg->as.s.data;
        break;
    default:
        return prim_throw_classified(S, "eval/type", "MNS003",
            "mino-installed?: expected keyword, string, or symbol");
    }
    if (label == NULL) return mino_false(S);

    for (p = k_capability_info; p->name != NULL; p++) {
        if (strcmp(p->name, label) == 0) {
            return mino_capability_installed(S, p->bit)
                ? mino_true(S) : mino_false(S);
        }
    }
    /* Unknown capability label -- conservative: not installed. */
    return mino_false(S);
}

static const mino_prim_def k_prims_capability[] = {
    {"mino-installed?", prim_mino_installed_p,
     "Returns true if a named capability has been installed on this "
     "runtime. Argument is a keyword, symbol, or string; supported names "
     "include :floor :regex :bignum :multimethods :protocols :transducers "
     ":io :fs :proc :stm :agent :host :async. Used by core.clj sections "
     "to gate optional surface on the host's install picks."},
};

static const size_t k_prims_capability_count =
    sizeof(k_prims_capability) / sizeof(k_prims_capability[0]);

/* ------------------------------------------------------------------------- */
/* Internal: floor setup and finalize                                        */
/* ------------------------------------------------------------------------- */

/* Wire up the dynamic vars and ns scaffolding that the canonical
 * Clojure-core install needs. Used by both mino_install_minimal (where
 * core.clj is NOT evaluated) and the higher-tier installers so vars
 * like *ns* / *out* / *in* / *data-readers* are always available. */
static void install_floor_vars(mino_state *S, mino_env *core_env)
{
    {
        mino_val *var = var_intern(S, "clojure.core", "*ns*");
        if (var != NULL) {
            var->as.var.dynamic = 1;
            if (S->ns_vars.current_ns == NULL) {
                S->ns_vars.current_ns = intern_filename(S, "user");
                (void)ns_env_ensure(S, "user");
            }
            mino_publish_current_ns(S);
            /* Add the env binding so ns-publics and resolve can find *ns*.
             * The root was just set by mino_publish_current_ns above. */
            mino_env_set(S, core_env, "*ns*", var->as.var.root);
        }
    }
    {
        mino_val *out_var = var_intern(S, "clojure.core", "*out*");
        mino_val *err_var = var_intern(S, "clojure.core", "*err*");
        mino_val *in_var  = var_intern(S, "clojure.core", "*in*");
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
    {
        mino_val *dr_var  = var_intern(S, "clojure.core", "*data-readers*");
        mino_val *def_var = var_intern(S, "clojure.core",
                                          "*default-data-reader-fn*");
        if (dr_var != NULL) {
            dr_var->as.var.dynamic = 1;
            var_set_root(S, dr_var, mino_map(S, NULL, NULL, 0));
            mino_env_set(S, core_env, "*data-readers*", dr_var->as.var.root);
        }
        if (def_var != NULL) {
            def_var->as.var.dynamic = 1;
            var_set_root(S, def_var, mino_nil(S));
            mino_env_set(S, core_env, "*default-data-reader-fn*",
                         def_var->as.var.root);
        }
    }
    /* Mark clojure.core/pr dynamic so (binding [pr ...] ...) works.
     * The var was created by prim_install_table in floor_install_prim_tables;
     * this flag adjustment follows the same pattern used for *out* above. */
    {
        mino_val *pr_var = var_find(S, "clojure.core", "pr");
        if (pr_var != NULL) pr_var->as.var.dynamic = 1;
    }
}

static mino_env *floor_install_prim_tables(mino_state *S)
{
    size_t      i;
    mino_env *core_env = ns_env_ensure(S, "clojure.core");

    mino_env_set(S, core_env, "math-pi", mino_float(S, 3.14159265358979323846));

    for (i = 0; i < K_CORE_DOMAIN_COUNT; i++) {
        prim_install_table(S, core_env, "clojure.core",
                           k_core_domains[i].defs,
                           *k_core_domains[i].count_ptr);
    }
    /* The capability-introspection prims live in the floor so
     * `(mino-installed? :cap)` is always available, regardless of
     * what else the embedder did or did not install. */
    prim_install_table(S, core_env, "clojure.core",
                       k_prims_capability, k_prims_capability_count);

    /* clojure.string namespace (the wrappers in lib_clojure_string layer
     * over these). Always in the floor -- they're string ops, not I/O. */
    {
        mino_env *cs_env = ns_env_ensure(S, "clojure.string");
        if (cs_env != NULL) {
            prim_install_table(S, cs_env, "clojure.string",
                               k_prims_clojure_string,
                               k_prims_clojure_string_count);
        }
    }
    /* clojure.repl namespace -- doc, source, apropos. */
    {
        mino_env *cr_env = ns_env_ensure(S, "clojure.repl");
        if (cr_env != NULL) {
            prim_install_table(S, cr_env, "clojure.repl",
                               k_prims_clojure_repl,
                               k_prims_clojure_repl_count);
        }
    }
    return core_env;
}

/* ------------------------------------------------------------------------- */
/* Public install API                                                        */
/* ------------------------------------------------------------------------- */

void mino_install_minimal(mino_state *S, mino_env *env)
{
    volatile char probe = 0;
    mino_env   *core_env;

    gc_note_host_frame(S, (void *)&probe);
    (void)probe;
    (void)env;

    core_env = floor_install_prim_tables(S);
    install_floor_vars(S, core_env);
    S->caps_installed |= MINO_CAP_FLOOR;
}

void mino_install_multimethods(mino_state *S, mino_env *env)
{
    (void)env;
    /* No C-prim surface; capability is implemented in core.clj sections
     * gated on `(mino-installed? :multimethods)`. The bit must be set
     * before mino_install_clojure_core evaluates core.clj so the
     * section runs. */
    S->caps_installed |= MINO_CAP_MULTIMETHODS;
}

void mino_install_protocols(mino_state *S, mino_env *env)
{
    (void)env;
    S->caps_installed |= MINO_CAP_PROTOCOLS;
}

void mino_install_transducers(mino_state *S, mino_env *env)
{
    (void)env;
    S->caps_installed |= MINO_CAP_TRANSDUCERS;
}

void mino_install_clojure_core(mino_state *S, mino_env *env)
{
    volatile char probe = 0;
    mino_env   *core_env;
    const char   *saved_ns;

    gc_note_host_frame(S, (void *)&probe);
    (void)probe;

    /* Install the floor scaffold first (idempotent); the embedder is
     * responsible for having flipped the per-capability bits and
     * installed the related C prims via mino_install(S, env, caps)
     * before this call. The standalone CLI uses mino_install_all so
     * the canonical Clojure-core surface is up front. */
    if ((S->caps_installed & MINO_CAP_FLOOR) == 0) {
        mino_install_minimal(S, env);
    }
    core_env = ns_env_ensure(S, "clojure.core");

    /* JVM-static literal slash-name bindings (Long/MAX_VALUE,
     * Math/sqrt, java.util.UUID/randomUUID, ...). Installed before
     * core.clj runs so any future core.mino code that references them
     * resolves cleanly. */
    mino_install_jvm_statics(S, core_env);

    saved_ns      = S->ns_vars.current_ns;
    S->ns_vars.current_ns = "clojure.core";
    install_core_mino(S, core_env);
    S->ns_vars.current_ns = saved_ns;
    (void)env;
}

