/*
 * runtime/capabilities.c -- data-driven capability install.
 *
 * The public surface is a single `mino_install(S, env, caps)` plus three
 * named presets. Embedders combine MINO_CAP_* bits and we dispatch
 * through the registry below to the per-capability install functions
 * (declared in mino_internal.h, defined alongside their domain in
 * the relevant src/prim/ source file).
 *
 * Order matters in two places:
 *   1. Per-capability C primitives must be registered before core.clj
 *      evaluates, because core.clj has (when (mino-installed? :cap) ...)
 *      sections that reference them. We set every capability's prim
 *      table and bit before invoking install_core_mino.
 *   2. Bundled-stdlib registrations (clojure.string et al.) only call
 *      mino_register_bundled_lib; they don't run code themselves. They
 *      can happen in any order, before or after core.clj.
 */

#include "runtime/internal.h"

/* mino_install_clojure_core lives in prim/install.c and is the entry
 * point that evaluates core.clj on the floor env. The floor-setup
 * function used by mino_install_minimal is exposed only as
 * mino_install_minimal — we delegate. */

typedef void (*mino_install_fn)(mino_state *S, mino_env *env);

/* Registry of capability bits paired with their install function. The
 * order here determines the order in which capabilities install when
 * `caps` is a bitmask; iteration walks this array in declaration order
 * and installs each capability whose bit is set in caps. */
typedef struct {
    uint64_t        bit;
    mino_install_fn install;
} cap_dispatch_t;

static const cap_dispatch_t k_cap_dispatch[] = {
    /* C-prim capabilities -- order doesn't matter functionally but we
     * keep "Clojure-core base" (regex, bignum, multimethods, protocols,
     * transducers) first so the picture matches how Clojure programmers
     * think about the runtime. */
    { MINO_CAP_REGEX,       mino_install_regex       },
    { MINO_CAP_BIGNUM,      mino_install_bignum      },
    { MINO_CAP_MULTIMETHODS,mino_install_multimethods},
    { MINO_CAP_PROTOCOLS,   mino_install_protocols   },
    { MINO_CAP_TRANSDUCERS, mino_install_transducers },
    { MINO_CAP_IO,          mino_install_io          },
    { MINO_CAP_FS,          mino_install_fs          },
    { MINO_CAP_PROC,        mino_install_proc        },
    { MINO_CAP_STM,         mino_install_stm         },
    { MINO_CAP_AGENT,       mino_install_agent       },
    { MINO_CAP_HOST,        mino_install_host        },
    { MINO_CAP_ASYNC,       mino_install_async       },
    /* Bundled-stdlib registrations. These run after C primitives are
     * in place so a (require '[clojure.string]) issued from core.clj's
     * gated sections resolves correctly. */
    { MINO_CAP_STRING_LIB,  mino_install_clojure_string },
    { MINO_CAP_SET_LIB,     mino_install_clojure_set    },
    { MINO_CAP_MATH_LIB,    mino_install_clojure_math   },
    { MINO_CAP_WALK,        mino_install_clojure_walk   },
    { MINO_CAP_EDN,         mino_install_clojure_edn    },
    { MINO_CAP_PPRINT,      mino_install_clojure_pprint },
    { MINO_CAP_ZIP,         mino_install_clojure_zip    },
    { MINO_CAP_DATA,        mino_install_clojure_data   },
    { MINO_CAP_TEST,        mino_install_clojure_test   },
    /* test.check piggy-backs on TEST -- handled below as a special
     * follow-on so embedders that pick TEST get the generator surface
     * too without a separate bit. */
    { MINO_CAP_REPL_LIB,    mino_install_clojure_repl    },
    { MINO_CAP_DATAFY,      mino_install_clojure_datafy  },
    { MINO_CAP_REDUCERS,    mino_install_clojure_reducers},
    { MINO_CAP_INSTANT,     mino_install_clojure_instant },
    { MINO_CAP_SPEC,        mino_install_clojure_spec    },
    { MINO_CAP_UNIFY,       mino_install_clojure_unify   },
    { MINO_CAP_CACHE,       mino_install_clojure_cache   },
    { MINO_CAP_MATCH,       mino_install_clojure_match   },
    { MINO_CAP_LOGIC,       mino_install_clojure_logic   },
    { MINO_CAP_TOOLING,     mino_install_mino_tooling    },
    /* Pure-data / info-only families. Ordered so a prerequisite installs
     * before its dependent when the closure pulls both in: codec before
     * net, time before log, zip (above) before html. */
    { MINO_CAP_CODEC,       mino_install_codec           },
    { MINO_CAP_RANDOM,      mino_install_random          },
    { MINO_CAP_TIME,        mino_install_mino_time       },
    { MINO_CAP_DIGEST,      mino_install_mino_digest     },
    { MINO_CAP_HTML,        mino_install_mino_html       },
    /* Same bit as html: the select subset ships alongside the reader. */
    { MINO_CAP_HTML,        mino_install_mino_html_select},
    { MINO_CAP_XML,         mino_install_clojure_xml     },
    { MINO_CAP_YAML,        mino_install_mino_yaml       },
    { MINO_CAP_TOML,        mino_install_mino_toml       },
    { MINO_CAP_COMPRESS,    mino_install_compress        },
    { MINO_CAP_ARCHIVE,     mino_install_mino_zip        },
    { MINO_CAP_TEMPLATE,    mino_install_mino_template   },
    { MINO_CAP_TERM,        mino_install_mino_term       },
    { MINO_CAP_ENV,         mino_install_mino_env        },
    { MINO_CAP_LOG,         mino_install_mino_log        },
    { MINO_CAP_CLI,         mino_install_mino_cli        },
    { MINO_CAP_PATH,        mino_install_mino_path       },
    { MINO_CAP_STORE,       mino_install_store           },
    { MINO_CAP_JSON,        mino_install_clojure_data_json },
    { MINO_CAP_CSV,         mino_install_clojure_data_csv  },
    { MINO_CAP_NET,         mino_install_net             },
    /* Same bit as net: TLS rides the net capability (ADR 20). */
    { MINO_CAP_NET,         mino_install_tls             },
    /* Same bit as net: the keep-alive pool is the net layer's. */
    { MINO_CAP_NET,         mino_install_pool            },
    /* Same bit as net: http-request composes the whole net stack. */
    { MINO_CAP_NET,         mino_install_http_client     },
};

#define K_CAP_DISPATCH_COUNT \
    (sizeof(k_cap_dispatch) / sizeof(k_cap_dispatch[0]))

/* Capability prerequisites. A key capability's bundled library leans on
 * a value capability's prims, so requesting the key implies the value.
 * The dispatch table above orders each value before its key, so a single
 * forward pass installs them in the right order. */
typedef struct {
    uint64_t cap;
    uint64_t needs;
} cap_requires_t;

static const cap_requires_t k_cap_requires[] = {
    { MINO_CAP_NET,  MINO_CAP_CODEC   }, /* mino.http: percent-encode / base64 / parse-url */
    { MINO_CAP_LOG,  MINO_CAP_TIME    }, /* mino.log: format-time on every line */
    { MINO_CAP_HTML, MINO_CAP_ZIP     }, /* mino.html.select: clojure.zip navigation */
    { MINO_CAP_TIME, MINO_CAP_INSTANT }, /* mino.time: #inst reader in its source */
};

#define K_CAP_REQUIRES_COUNT \
    (sizeof(k_cap_requires) / sizeof(k_cap_requires[0]))

/* Expand a requested capability set to include every prerequisite,
 * iterating to a fixpoint so transitive requirements are covered. */
static uint64_t cap_closure(uint64_t caps)
{
    uint64_t prev;
    do {
        size_t i;
        prev = caps;
        for (i = 0; i < K_CAP_REQUIRES_COUNT; i++) {
            if (caps & k_cap_requires[i].cap) {
                caps |= k_cap_requires[i].needs;
            }
        }
    } while (caps != prev);
    return caps;
}

void mino_install(mino_state *S, mino_env *env, uint64_t caps)
{
    size_t i;
    uint64_t wanted;
    int first_non_floor_install;

    if (S == NULL || env == NULL) return;

    /* Floor is always installed first; idempotent on a state that
     * already has the floor. */
    if ((S->caps_installed & MINO_CAP_FLOOR) == 0) {
        mino_install_minimal(S, env);
    }

    /* Pull in prerequisite capabilities so no bundled library installs
     * half-satisfied (e.g. net without the codec prims it calls). */
    caps = cap_closure(caps);

    if (caps == 0) {
        /* Floor-only path. */
        return;
    }

    /* Filter out bits already installed so we honor idempotency without
     * paying for redundant install_* calls. Bits not in the dispatch
     * table (e.g. reserved future bits) are simply ignored. */
    wanted = caps & ~S->caps_installed;

    /* Record whether this is the first time the state has any non-floor
     * capability installed. core.clj evaluates exactly once per state;
     * after that, additional capabilities install their C prims into
     * the existing env without re-running the script-side surface. */
    first_non_floor_install =
        ((S->caps_installed & ~(uint64_t)MINO_CAP_FLOOR) == 0);

    /* Install C primitives and register bundled sources for each bit
     * in `wanted`. After this loop every capability the embedder asked
     * for has its prim tables registered, so core.clj evaluates with
     * the right (mino-installed? :cap) answers. Bundled-stdlib install
     * functions don't track caps themselves, so set the bit here so
     * mino_capabilities() reports the full set after install. */
    for (i = 0; i < K_CAP_DISPATCH_COUNT; i++) {
        if (wanted & k_cap_dispatch[i].bit) {
            k_cap_dispatch[i].install(S, env);
            S->caps_installed |= k_cap_dispatch[i].bit;
        }
    }

    /* clojure.test pulls in test.check generators + properties; bundle
     * them so a host that asks for TEST gets the canonical Clojure-test
     * experience (deftest / is / are / test.check/quick-check). */
    if (wanted & MINO_CAP_TEST) {
        mino_install_clojure_test_check(S, env);
    }

    /* Evaluate core.clj on the floor env once we have any non-floor
     * capability. Skip on subsequent installs against the same state:
     * the script-side surface is already in place and re-evaluation
     * would redefine bindings already shadowed by user code. */
    if (first_non_floor_install && (caps & ~(uint64_t)MINO_CAP_FLOOR) != 0) {
        mino_install_clojure_core(S, env);
    }
}

void mino_install_sandbox(mino_state *S, mino_env *env)
{
    mino_install(S, env, MINO_CAP_DEFAULT);
}
