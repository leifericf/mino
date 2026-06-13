/*
 * mino_internal.h -- private companion header to mino.h.
 *
 * Holds everything that is implementation-detail-but-needed-by-the-runtime:
 *
 *   - the body of struct mino_val (union arms, internal cache fields),
 *   - the pointer-tag encoding (MINO_TAG_*, MINO_MAKE_*, MINO_*_VAL),
 *   - opaque-to-embedders typedefs (mino_vec_node_t, mino_hamt_node_t,
 *     mino_rb_node_t, struct mino_bc_fn),
 *   - the host-array element-kind enum,
 *   - test-only fault-injection helpers,
 *   - chunked-seq and host-array constructors that are runtime-internal.
 *
 * Internal to the runtime. Embedders should only include mino.h. The
 * shape of struct mino_val, the tag scheme, and the helpers declared
 * here are NOT part of the public API contract and can change between
 * patch releases without notice.
 */

#ifndef MINO_INTERNAL_H
#define MINO_INTERNAL_H

#include "mino.h"
#include "values/layout.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------------- */
/* Host-array element kind                                                   */
/* ------------------------------------------------------------------------- */

/* JVM-style host array element kind. Used for printing and zero-fill
 * semantics on (int-array n) etc. Pure-object arrays nil-fill; the
 * primitive-element variants fill with their type's zero value. */
typedef enum {
    HOST_ARRAY_OBJECT = 0,
    HOST_ARRAY_INT,
    HOST_ARRAY_LONG,
    HOST_ARRAY_SHORT,
    HOST_ARRAY_BYTE,
    HOST_ARRAY_FLOAT,
    HOST_ARRAY_DOUBLE,
    HOST_ARRAY_CHAR,
    HOST_ARRAY_BOOLEAN
} host_array_kind_t;


/* ------------------------------------------------------------------------- */
/* Runtime-internal constructors                                             */
/* ------------------------------------------------------------------------- */

/* Create a host-style array of the given length, fill-initialized
 * according to the element kind. JVM-shape mimicry surface; not part
 * of the public embedding API. */
mino_val *mino_host_array_new(mino_state *S, size_t len,
                                host_array_kind_t kind);
mino_val *mino_host_array_from_coll(mino_state *S, mino_val *coll,
                                      host_array_kind_t kind);

/* Chunked-seq constructors. Internal: lazy.c and sequences.c use these
 * to build chunked seqs from sequences and ranges. */
mino_val *mino_chunk_buffer(mino_state *S, unsigned cap);
int         mino_chunk_append(mino_val *buf, mino_val *elem);
mino_val *mino_chunk_seal(mino_val *buf);
mino_val *mino_chunked_cons(mino_state *S, mino_val *chunk,
                              mino_val *more);
mino_val *mino_chunked_cons_advance(mino_state *S, const mino_val *cs);

/* Wraparound int constructor: identical to mino_int when the value
 * fits the 61-bit tag, but always boxes the overflow path as MINO_INT
 * (never auto-promotes to MINO_BIGINT, regardless of MINO_CAP_BIGNUM).
 * Used by the unchecked-* family in numeric.c so the documented
 * two's-complement-wrap semantics produce an int, not a bigint. */
mino_val *mino_int_wrap(mino_state *S, long long n);

/* ------------------------------------------------------------------------- */
/* Per-capability install functions (internal-only)                          */
/* ------------------------------------------------------------------------- */

/* Each function registers the C primitives for its capability and sets
 * the corresponding MINO_CAP_* bit on the state. These are dispatched
 * from the capability registry in runtime/capabilities.c; embedders use
 * mino_install(S, env, caps) and never call these directly. */

void mino_install_regex       (mino_state *S, mino_env *env);
void mino_install_bignum      (mino_state *S, mino_env *env);
void mino_install_multimethods(mino_state *S, mino_env *env);
void mino_install_protocols   (mino_state *S, mino_env *env);
void mino_install_transducers (mino_state *S, mino_env *env);
void mino_install_io          (mino_state *S, mino_env *env);
void mino_install_fs          (mino_state *S, mino_env *env);
void mino_install_proc        (mino_state *S, mino_env *env);
void mino_install_stm         (mino_state *S, mino_env *env);
void mino_install_agent       (mino_state *S, mino_env *env);
void mino_install_host        (mino_state *S, mino_env *env);
void mino_install_async       (mino_state *S, mino_env *env);

/* Bundled-stdlib registration hooks. Each registers the in-binary source
 * for its namespace via mino_register_bundled_lib so a subsequent
 * (require '[<ns>]) loads it from memory. Pairs that depend on each
 * other ship as a single hook (clojure.repl + clojure.stacktrace;
 * clojure.datafy + clojure.core.protocols; clojure.test +
 * clojure.test.check). */
void mino_install_clojure_string    (mino_state *S, mino_env *env);
void mino_install_clojure_set       (mino_state *S, mino_env *env);
void mino_install_clojure_math      (mino_state *S, mino_env *env);
void mino_install_clojure_walk      (mino_state *S, mino_env *env);
void mino_install_clojure_edn       (mino_state *S, mino_env *env);
void mino_install_clojure_pprint    (mino_state *S, mino_env *env);
void mino_install_clojure_zip       (mino_state *S, mino_env *env);
void mino_install_clojure_data      (mino_state *S, mino_env *env);
void mino_install_clojure_test      (mino_state *S, mino_env *env);
void mino_install_clojure_test_check(mino_state *S, mino_env *env);
void mino_install_clojure_repl      (mino_state *S, mino_env *env);
void mino_install_clojure_datafy    (mino_state *S, mino_env *env);
void mino_install_clojure_reducers  (mino_state *S, mino_env *env);
void mino_install_clojure_instant   (mino_state *S, mino_env *env);
void mino_install_clojure_spec      (mino_state *S, mino_env *env);
void mino_install_clojure_unify     (mino_state *S, mino_env *env);
void mino_install_clojure_cache     (mino_state *S, mino_env *env);
void mino_install_clojure_match     (mino_state *S, mino_env *env);
void mino_install_mino_tooling      (mino_state *S, mino_env *env);

/* Internal: evaluate core.clj on the floor env. Used by mino_install
 * after capability bits are set; idempotent. */
void mino_install_clojure_core(mino_state *S, mino_env *env);

/* ------------------------------------------------------------------------- */
/* Safepoint-sampler dumps (diagnostic, runtime-internal)                    */
/* ------------------------------------------------------------------------- */

#include <stdio.h>

/*
 * CPU sampler dump. When MINO_SAMPLE=1 was set in the environment,
 * the runtime records (fn, pc, op) tuples into a ring at each
 * safepoint hit (default every 1000 safepoints; MINO_SAMPLE_PERIOD
 * tunes the rate). mino_sampler_dump writes a per-PC histogram of
 * the current ring contents to `out`. Returns the number of distinct
 * (fn, pc) pairs written. No-op when sampling is off or the ring is
 * empty.
 */
unsigned mino_sampler_dump(mino_state *S, FILE *out);

/*
 * Allocation-site sampler dump. When MINO_ALLOC_SAMPLE=1 was set in
 * the environment, the runtime records one (return-address, tag,
 * size-bucket) tuple for every MINO_ALLOC_SAMPLE_RATE-th allocation
 * (default 4096) into a tiny ring (4096 entries × 16 B = 64 KB).
 * Writes a top-N table aggregated by site to `out` and returns the
 * number of distinct sites written. No-op when sampling is off.
 */
unsigned mino_alloc_sampler_dump(mino_state *S, FILE *out);

/* ------------------------------------------------------------------------- */
/* Fault injection (test-only)                                               */
/* ------------------------------------------------------------------------- */

/* Schedule a simulated OOM after the next `n` GC-managed allocations. */
void mino_set_fail_alloc_at(mino_state *S, long n);
/* Schedule a simulated OOM after `n` raw (non-GC) allocations. */
void mino_set_fail_raw_at(mino_state *S, long n);
/* Check-and-decrement the raw fault-injection counter. */
int  mino_fi_should_fail_raw(mino_state *S);

#ifdef __cplusplus
}
#endif

#endif /* MINO_INTERNAL_H */
