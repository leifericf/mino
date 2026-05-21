/*
 * mino.h - public C API for the mino runtime.
 *
 * UNSTABLE until v1.0.0. Symbol names, types, and semantics may change.
 */

#ifndef MINO_H
#define MINO_H

#include <stddef.h>
#include <stdint.h>

/* ------------------------------------------------------------------------- */
/* Version                                                                   */
/* ------------------------------------------------------------------------- */

/*
 * Compile-time version constants, bumped by the release process. Host code
 * can check these at preprocessor time to guard against embedding against an
 * unexpected runtime:
 *
 *   #if MINO_VERSION_MAJOR == 0 && MINO_VERSION_MINOR < 48
 *   # error "embedded mino is too old; need >= 0.48"
 *   #endif
 *
 * The linked-in version (which may differ if the header was updated without
 * rebuilding the runtime) is available at runtime via mino_version_string().
 */
#define MINO_VERSION_MAJOR 0
#define MINO_VERSION_MINOR 410
#define MINO_VERSION_PATCH 0

/*
 * Human-readable version string of the *linked* runtime, e.g. "0.48.0".
 * The pointer is to a static string with program lifetime; callers must not
 * free it.
 */
const char *mino_version_string(void);

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------------- */
/* Value types                                                               */
/* ------------------------------------------------------------------------- */

typedef enum {
    MINO_NIL,
    MINO_BOOL,
    MINO_INT,
    MINO_FLOAT,    /* 64-bit IEEE 754 double. */
    MINO_FLOAT32,  /* 32-bit IEEE 754 single. Distinct tag so float?
                    * matches both tiers and double? matches only
                    * MINO_FLOAT. Stored in the same `as.f` field as
                    * MINO_FLOAT (the 32-bit cast precision narrowing
                    * happens at construction); arithmetic always
                    * promotes to MINO_FLOAT, matching JVM Clojure
                    * where Float arithmetic yields Double. */
    MINO_CHAR,       /* Unicode scalar value (codepoint) */
    MINO_STRING,
    MINO_SYMBOL,
    MINO_KEYWORD,
    MINO_EMPTY_LIST, /* singleton: the canonical empty list `()`.
                      * Distinct from nil: seq?-true, nil?-false,
                      * empty?-true. Walkers terminate on it because
                      * mino_is_cons returns false. Construct via
                      * mino_empty_list(S). */
    MINO_CONS,
    MINO_VECTOR,
    MINO_MAP,
    MINO_SET,        /* persistent set: HAMT of keys with sentinel values */
    MINO_SORTED_MAP, /* persistent sorted map: red-black tree */
    MINO_SORTED_SET, /* persistent sorted set: red-black tree */
    MINO_PRIM,
    MINO_FN,
    MINO_MACRO,   /* user-defined macro (shares the fn struct layout) */
    MINO_HANDLE,  /* opaque host object: pointer + type tag string */
    MINO_ATOM,    /* mutable reference cell: wraps a single value */
    MINO_VOLATILE, /* lightweight single-slot mutable cell. No watches,
                    * no validators, no atomic publish. Intended for
                    * transducer state where the reducing fn already
                    * implies single-thread access. Constructed via
                    * mino_volatile / `volatile!`. */
    MINO_LAZY,    /* lazy sequence: thunk body + env, cached on first force */
    MINO_CHUNK,   /* fixed-capacity buffer of values used as the leaf
                   * of a chunked seq. Constructed via `chunk-buffer`
                   * (mutable, for accumulation) and sealed via
                   * `chunk` (returns the same value with sealed=1).
                   * After sealing, chunk-append throws. */
    MINO_CHUNKED_CONS, /* chunked-seq cell: holds one MINO_CHUNK plus
                        * an offset into it and a `more` seq for what
                        * follows the chunk. first/next/rest treat
                        * this transparently as a seq; chunk-first /
                        * chunk-rest expose the underlying chunk so
                        * combinators can pull a whole chunk at a
                        * time and emit chunked output. Constructed
                        * via `chunk-cons`. */
    MINO_RECUR,   /* internal tail-call trampoline sentinel */
    MINO_TAIL_CALL, /* proper tail call: carries {fn, args} for trampoline */
    MINO_REDUCED, /* early termination wrapper for reduce */
    MINO_VAR,     /* first-class var: ns + name + root binding */
    MINO_TRANSIENT, /* mutable staging wrapper for batch mutation of a
                     * persistent vector, map, or set. Embedders call
                     * mino_transient / mino_persistent and the
                     * *_bang accessors below. */
    MINO_BIGINT,    /* arbitrary-precision signed integer. Backed by the
                     * vendored imath library (src/vendor/imath.c). The
                     * mpz_t struct is embedded in the value cell; the
                     * digit storage is owned by the cell and freed
                     * during GC sweep. See also `1N` literals and the
                     * `bigint` / `biginteger` constructors. */
    MINO_RATIO,     /* arbitrary-precision rational number, stored as
                     * a numerator/denominator pair of bigints. The
                     * representation is always reduced (gcd = 1) and
                     * the denominator is always positive (sign lives
                     * on the numerator). See also `1/2` literals and
                     * the `numerator` / `denominator` primitives. */
    MINO_BIGDEC,    /* arbitrary-precision decimal, stored as an
                     * unscaled bigint plus a non-negative integer
                     * scale: value = unscaled * 10^-scale. Constructed
                     * from `1.5M` / `1M` literals or via `(bigdec x)`. */
    MINO_TYPE,      /* first-class record type. Carries identity (ptr),
                     * defining namespace, unqualified name, and an
                     * ordered vector of declared field-name keywords.
                     * Method tables are not stored here; protocol
                     * dispatch lives in the protocol's namespace,
                     * keyed by what (type x) returns. Construct via
                     * mino_defrecord. */
    MINO_RECORD,    /* record value: pointer to its MINO_TYPE plus a
                     * malloc-owned array of field-value slots and an
                     * optional MINO_MAP for extension keys. Storage
                     * is field slots, not a backing map; map-iso
                     * behaviour (get, assoc, seq, count, ...) is a
                     * contract layered on top via primitive dispatch
                     * on this tag. Construct via mino_record. */
    MINO_FUTURE,    /* host-thread future: carries a result-or-exception
                     * cell, a state machine (PENDING, RESOLVED, FAILED,
                     * CANCELLED), a mutex+cond for cross-thread
                     * delivery, and a thread handle. Promises share
                     * this type — a promise is a future that exposes
                     * `deliver` and never has a worker thread. */
    MINO_UUID,      /* RFC 4122 UUID. 16 bytes inline. Equality is
                     * byte-wise; hash mixes the 16 bytes. The
                     * `#uuid "..."` reader literal and (random-uuid),
                     * (parse-uuid s) construct it. */
    MINO_REGEX,     /* Compiled-on-use regex pattern. Holds the source
                     * string only -- mino's regex engine compiles per
                     * call. Equality is identity (matches Clojure
                     * JVM's `(= #"x" #"x")` returning false); print
                     * form is `#"source"`. The reader's `#"..."`
                     * literal and `(re-pattern s)` construct it. */
    MINO_MAP_ENTRY, /* Map entry: a (k, v) pair returned by first/seq
                     * of a map. Distinct from MINO_VECTOR -- key and
                     * val accept only MAP_ENTRY, throwing on plain
                     * 2-vectors -- but vector-shaped: vector?, coll?,
                     * sequential?, indexed?, counted?, associative?,
                     * reversible? all true; equality with a 2-vector
                     * compares element-wise. count is 2; nth 0 / 1
                     * yield k / v; first/second/last/peek dispatch
                     * accordingly. Constructed by first/seq of a map
                     * and by `clojure.lang.MapEntry/create`. */
    MINO_HOST_ARRAY, /* JVM-style host array: a fixed-length container
                     * with element-kind tag (:object, :int, :long,
                     * etc.) for printing and zero-fill semantics.
                     * Distinct from MINO_VECTOR: predicates like
                     * vector?, coll?, sequential?, counted?,
                     * associative? all return false (matching JVM's
                     * Java arrays which don't implement
                     * IPersistentCollection). seq returns a chunked
                     * seq over the elements. Equality is identity.
                     * Constructed via object-array, int-array,
                     * long-array, etc. */
    MINO_TX_REF,    /* Software transactional memory ref: identity cell
                     * holding a single committed value plus per-cell
                     * watches and validator. Mutations are confined to
                     * `dosync` transactions and serialized through a
                     * single-version optimistic protocol (read-set
                     * validation at commit; conflicts trigger retry).
                     * Equality is identity, matching atoms.
                     * Constructed via `(ref v)`. The MINO_REF symbol
                     * was already taken by the embedder rooting handle
                     * (mino_ref), so the enum tag is MINO_TX_REF; the
                     * Clojure-level type keyword is `:ref`. */
    MINO_AGENT,     /* Asynchronous mutable cell with a per-state
                     * action run-queue. send / send-off enqueue
                     * (fn args) onto the queue and return the agent
                     * immediately; a worker thread drains the queue
                     * serially, applies actions under state_lock,
                     * and signals await waiters when the agent's
                     * in-flight count reaches zero. Watches and
                     * validators follow the atom/ref shape.
                     * Equality is identity. Constructed via
                     * `(agent v)`. The worker counts against
                     * thread_limit, so send / send-off throw MTH001
                     * if the host hasn't granted a thread budget
                     * (default thread_limit == 1 means no agents). */
    MINO_CHAN,      /* clojure.core.async channel. Owns its buffer
                     * (for :fixed / :dropping / :sliding kinds),
                     * pending-putters and pending-takers queues, a
                     * closed flag, and optional transducer + ex-
                     * handler hooks directly in C-side mutable
                     * slots. Replaces the previous atom-of-map
                     * shape so each offer!/poll!/put!/take! is one
                     * C call with no script-side state-map
                     * allocation. Equality is identity. Constructed
                     * via `(chan)` / `(chan n)` / `(chan n xform)` /
                     * `(promise-chan)`. */
    MINO_QUEUE      /* clojure.lang.PersistentQueue equivalent: a
                     * persistent FIFO with amortised O(1) enqueue
                     * (conj) and dequeue (pop). Backed by two cons
                     * lists: a front list (the next-to-pop side, in
                     * order) and a back list (the most-recently-conj
                     * side, in REVERSE order). When the front empties
                     * the back is reversed and becomes the new front.
                     * count is cached. Predicates / seq / conj /
                     * peek / pop dispatch element-wise; equality is
                     * sequence equality with other queues only.
                     * Constructed via clojure.lang.PersistentQueue/
                     * EMPTY plus conj, or the mino-native EMPTY-QUEUE
                     * binding. */
} mino_type;

typedef struct mino_val    mino_val;   /* opaque */
typedef struct mino_env    mino_env;   /* opaque */
typedef struct mino_future mino_future;/* opaque */
typedef struct mino_state  mino_state; /* opaque */
typedef struct mino_ref    mino_ref;   /* opaque */

/* Return the effective type of a value. Works for both heap-allocated
 * cells and tagged scalars (int, bool, nil, char). NULL is treated as
 * MINO_NIL. Use this to dispatch on type from C without reaching into
 * the value's internal layout. */
mino_type mino_typeof(const mino_val *v);

typedef mino_val *(*mino_prim_fn)(mino_state *S, mino_val *args,
                                    mino_env *env);
/* argv ABI: receives evaluated args as a flat C array instead of a
 * cons spine. Skips eval_args' per-call cons allocation for prims
 * registered with mino_prim_argv. argv slots are GC-rooted by the
 * conservative stack scan for the duration of the call. */
typedef mino_val *(*mino_prim_fn2)(mino_state *S,
                                     mino_val **argv, int argc,
                                     mino_env *env);
typedef void (*mino_finalizer_fn)(void *ptr, const char *tag);

/* Host interop callback. target is the handle (NULL for ctor/static),
 * args is a cons list of evaluated arguments. */
typedef mino_val *(*mino_host_fn)(mino_state *S, mino_val *target,
                                    mino_val *args, void *ctx);

/* ------------------------------------------------------------------------- */
/* Constructors                                                              */
/* ------------------------------------------------------------------------- */

/* Return the singleton nil value. */
mino_val *mino_nil(mino_state *S);

/* Return the singleton true value. */
mino_val *mino_true(mino_state *S);

/* Return the singleton false value. */
mino_val *mino_false(mino_state *S);

/* Return the singleton empty-list value `()`. Distinct from nil:
 * `seq?` is true, `nil?` is false, `empty?` is true, and `=` against
 * nil is false. Walkers using `mino_is_cons` terminate on it because
 * the type tag is MINO_EMPTY_LIST, not MINO_CONS. */
mino_val *mino_empty_list(mino_state *S);

/* Create an integer value from a signed long long. Total over the full
 * long long range. Values in [-2^60, 2^60 - 1] return a tag-encoded
 * pointer (no allocation). Values outside that band depend on the
 * MINO_CAP_BIGNUM capability: with bignum installed, the result is a
 * MINO_BIGINT carrying the same numeric value, so hosts that opt in
 * to arbitrary-precision arithmetic see one int family that grows
 * past 64 bits transparently; without bignum, the result is a boxed
 * MINO_INT cell that still holds the full 64-bit signed integer.
 *
 * Inside mino's runtime, deterministic operations such as the reader,
 * bitwise primitives, and the unchecked-* family never auto-promote;
 * they keep producing MINO_INT (boxed when outside the tag range) so
 * Clojure-style "long stays long" semantics are preserved.
 *
 * Capability-conditional behaviour
 * --------------------------------
 * The auto-promote of a tag-overflowing `mino_int` argument to a
 * MINO_BIGINT when MINO_CAP_BIGNUM is installed is the C-side peer
 * to JVM Clojure's `+'` / `inc'` / `*'` / `dec'` opt-in arithmetic.
 * An embedder that wants "long stays long" (regardless of magnitude)
 * uses `mino_install_minimal` plus the explicit `unchecked-*`
 * surface; an embedder that wants the JVM `BigInteger`-shaped
 * "promote silently" semantics installs the BIGNUM capability and
 * gets the auto-promote through this constructor. */
mino_val *mino_int(mino_state *S, long long n);

/* Create a floating-point value. */
mino_val *mino_float(mino_state *S, double f);

/* Create a 32-bit single-precision floating-point value. The double
 * argument is narrowed to float and stored back as a double so
 * equality / print see the rounded value; the type tag distinguishes
 * it from `MINO_FLOAT` so `double?` returns false on the result. */
mino_val *mino_float32(mino_state *S, double f);

/* Create a bigint from a signed long long. */
mino_val *mino_bigint_from_ll(mino_state *S, long long n);

/* Create a bigint by parsing a base-10 numeric string (optional leading
 * '+' or '-'). Returns NULL on parse failure. */
mino_val *mino_bigint_from_string(mino_state *S, const char *s);

/* Create a rational from numerator and denominator long longs. The result
 * is reduced (gcd = 1) and normalised so the denominator is positive.
 * If denom is zero, throws division-by-zero. If the result is integer,
 * returns a MINO_INT or MINO_BIGINT instead of a MINO_RATIO. */
mino_val *mino_ratio_from_ll(mino_state *S, long long num, long long denom);

/* Create a bigdec from a base-10 numeric string. Returns NULL on parse
 * failure. Recognises optional sign, fractional part, and 'e'-prefixed
 * exponent; the trailing 'M' suffix is optional in this entry point. */
mino_val *mino_bigdec_from_string(mino_state *S, const char *s);

/* Create a character value from a Unicode codepoint (0..0x10FFFF). */
mino_val *mino_char(mino_state *S, int codepoint);

/* Create a string from a NUL-terminated C string. The data is copied. */
mino_val *mino_string(mino_state *S, const char *s);

/* Create a string from a buffer of length len. The data is copied. */
mino_val *mino_string_n(mino_state *S, const char *s, size_t len);

/* Intern a symbol from a NUL-terminated C string. */
mino_val *mino_symbol(mino_state *S, const char *s);

/* Intern a symbol from a buffer of length len. */
mino_val *mino_symbol_n(mino_state *S, const char *s, size_t len);

/* Intern a keyword from a NUL-terminated C string (without the leading :). */
mino_val *mino_keyword(mino_state *S, const char *s);

/* Intern a keyword from a buffer of length len (without the leading :). */
mino_val *mino_keyword_n(mino_state *S, const char *s, size_t len);

/* Intern a keyword from explicit (ns, name) buffers. The resulting
 * keyword carries `ns_len` so `name` / `namespace` round-trip the
 * originally-passed segments — `(keyword "a/b" "c")` and `(keyword
 * "a" "b/c")` produce DISTINCT vals even though their printed flat
 * form is identical. ns may be NULL or ns_len == 0 for an unqualified
 * keyword. */
mino_val *mino_keyword_ns_n(mino_state *S,
                              const char *ns, size_t ns_len,
                              const char *name, size_t name_len);

/* Same shape for symbols. */
mino_val *mino_symbol_ns_n(mino_state *S,
                             const char *ns, size_t ns_len,
                             const char *name, size_t name_len);

/* Create a cons cell (list node) with the given car and cdr. */
mino_val *mino_cons(mino_state *S, mino_val *car, mino_val *cdr);

/* Create a persistent vector from a C array of values. */
mino_val *mino_vector(mino_state *S, mino_val **items, size_t len);

/* Create a persistent hash map from parallel key and value arrays. */
mino_val *mino_map(mino_state *S, mino_val **keys, mino_val **vals,
                     size_t len);

/* Create a persistent hash set from a C array of values. */
mino_val *mino_set(mino_state *S, mino_val **items, size_t len);

/* ------------------------------------------------------------------------- */
/* Collection builders                                                       */
/* ------------------------------------------------------------------------- */

/*
 * Builders are an embedder-friendly facade over transients. Use them
 * when you produce elements one at a time from C: open a builder,
 * push/put/add each element, finish to get the persistent value.
 *
 *   mino_vec_builder *b = mino_vector_builder_new(S);
 *   for (size_t i = 0; i < n; i++) {
 *       mino_vector_builder_push(b, mino_int(S, items[i]));
 *   }
 *   mino_val *v = mino_vector_builder_finish(b);
 *
 * The builder roots the in-flight collection across GC; _finish hands
 * back the persistent result and releases the builder.
 */
typedef struct mino_vec_builder mino_vec_builder;
typedef struct mino_map_builder mino_map_builder;
typedef struct mino_set_builder mino_set_builder;

mino_vec_builder *mino_vector_builder_new   (mino_state *S);
void                mino_vector_builder_push  (mino_vec_builder *b,
                                               mino_val *v);
mino_val         *mino_vector_builder_finish(mino_vec_builder *b);

mino_map_builder *mino_map_builder_new      (mino_state *S);
void                mino_map_builder_put      (mino_map_builder *b,
                                               mino_val *k, mino_val *v);
mino_val         *mino_map_builder_finish   (mino_map_builder *b);

mino_set_builder *mino_set_builder_new      (mino_state *S);
void                mino_set_builder_add      (mino_set_builder *b,
                                               mino_val *v);
mino_val         *mino_set_builder_finish   (mino_set_builder *b);

/* ------------------------------------------------------------------------- */
/* Collection iterator                                                       */
/* ------------------------------------------------------------------------- */

/*
 * One iterator type walks every sequential / associative collection
 * mino exposes: vectors, maps (hashed and sorted), sets (hashed and
 * sorted), cons lists, the empty-list singleton, lazy seqs, and
 * chunked seqs. Dispatch happens internally on each step.
 *
 * The iterator is opaque; the host allocates storage of
 * `mino_iter_sizeof()` bytes (typically on the C stack via `alloca`
 * or as a local struct) and passes a pointer into `mino_iter_init`.
 *
 * Usage:
 *
 *   mino_iter *it = alloca(mino_iter_sizeof());
 *   mino_iter_init(S, it, coll);
 *   mino_val *k, *v;
 *   while (mino_iter_next(it, &k, &v)) {
 *       // for vectors / sets / lists: k is the element, v is NULL
 *       // for maps: k is the key, v is the value
 *   }
 *   mino_iter_done(it);
 *
 * `mino_iter_init` roots `coll` for the iterator's lifetime so a GC
 * fired mid-walk cannot reclaim the cells the walker borrows pointers
 * into. `mino_iter_done` releases that root and must always be called
 * once, even if iteration ends early. Calling `_next` after it returns
 * 0 keeps returning 0; calling `_next` or `_done` on a NULL iterator
 * is harmless.
 */
typedef struct mino_iter mino_iter;

size_t       mino_iter_sizeof(void);
void         mino_iter_init  (mino_state *S, mino_iter *it,
                              mino_val *coll);
int          mino_iter_next  (mino_iter *it,
                              mino_val **out_k, mino_val **out_v);
void         mino_iter_done  (mino_iter *it);

/* Create a primitive function value from a C function pointer. */
mino_val *mino_prim(mino_state *S, const char *name, mino_prim_fn fn);
/* Create a primitive value backed by an argv-style C function. */
mino_val *mino_prim_argv(mino_state *S, const char *name, mino_prim_fn2 fn);

/* Wrap a host pointer as an opaque handle with a type tag. */
mino_val *mino_handle(mino_state *S, void *ptr, const char *tag);

/* Wrap a host pointer with a type tag and a finalizer called on GC. */
mino_val *mino_handle_ex(mino_state *S, void *ptr, const char *tag,
                           mino_finalizer_fn finalizer);

/* Create a mutable atom initialized with val. */
mino_val *mino_atom(mino_state *S, mino_val *val);

/* Return 1 if v is a handle, 0 otherwise. */
int         mino_is_handle(const mino_val *v);

/* Return the host pointer from a handle, or NULL if v is not a handle. */
void       *mino_handle_ptr(const mino_val *v);

/* Return the type tag from a handle, or NULL if v is not a handle. */
const char *mino_handle_tag(const mino_val *v);

/* Return 1 if v is an atom, 0 otherwise. */
int         mino_is_atom(const mino_val *v);

/* Return the current value of an atom. */
mino_val *mino_atom_deref(const mino_val *a);

/* Create a volatile cell initialized with val. */
mino_val *mino_volatile(mino_state *S, mino_val *val);

/* Return 1 if v is a volatile, 0 otherwise. */
int         mino_is_volatile(const mino_val *v);

/* Return the current value of a volatile, or NULL if v is not a volatile. */
mino_val *mino_volatile_deref(const mino_val *v);

/* Set the value of an atom. */
void        mino_atom_reset(mino_val *a, mino_val *val);

/* Construct a map entry holding (k, v). Used by first / seq of a map
 * to publish entries with the right type identity (so key / val can
 * type-check), and by `clojure.lang.MapEntry/create`. */
mino_val *mino_map_entry(mino_state *S, mino_val *k, mino_val *v);

/* Build an empty PersistentQueue. The mino-native canonical empty value
 * is bound to `clojure.lang.PersistentQueue/EMPTY` after a normal
 * mino_install; conj / peek / pop / count / seq behave canonically. */
mino_val *mino_queue_empty(mino_state *S);

/* Return 1 if v is a MINO_QUEUE, 0 otherwise. NULL-safe. */
int         mino_is_queue(const mino_val *v);

/* count / conj / peek / pop / seq on a PersistentQueue. NULL on type
 * mismatch except count which returns 0. conj appends to the back;
 * peek returns the next-to-pop element; pop returns the queue without
 * its head. The seq returns the elements in deque order. */
size_t      mino_queue_count(const mino_val *q);
mino_val *mino_queue_conj (mino_state *S, mino_val *q, mino_val *v);
mino_val *mino_queue_peek (const mino_val *q);
mino_val *mino_queue_pop  (mino_state *S, mino_val *q);
mino_val *mino_queue_seq  (mino_state *S, const mino_val *q);

/* Construct an STM ref holding the given committed value. The watches
 * map and validator slots start NULL; install them via add-watch /
 * set-validator! on the returned cell. The ref's identity (returned
 * monotonic ID) is unique within S. */
mino_val *mino_tx_ref(mino_state *S, mino_val *val);

/* Construct an asynchronous agent holding the given initial state.
 * Watches, validator, and error handler all start NULL; install them
 * via add-watch / set-validator! / set-error-handler! on the returned
 * cell. The agent's action queue is heap-allocated and freed via the
 * GC sweep finalizer when the agent becomes unreachable. */
mino_val *mino_agent(mino_state *S, mino_val *initial);

/* Return 1 if v is an agent, 0 otherwise. NULL-safe. */
int         mino_is_agent(const mino_val *v);

/* Return the agent's current state value (the most-recently committed
 * action result, or the initial value if no action has applied). Returns
 * NULL if v is not an agent. Mirrors mino_atom_deref / mino_tx_ref_deref. */
mino_val *mino_agent_deref(const mino_val *a);

/* Enqueue (fn current-value arg1 arg2 ...) onto the agent's POOLED
 * run-queue and return the agent immediately. `extra_args` is a cons
 * list (or nil) holding the extra arguments after the agent's
 * current value; the action fn is called as
 *   (fn current-value arg1 arg2 ...)
 *
 * Throws (and returns NULL) if:
 *   - agent is NULL or not an agent (MTY001),
 *   - agent was created in a different state (MST007),
 *   - agents have been shut down (MST008),
 *   - the agent is failed and its error mode is :fail (MST002),
 *   - the host has not granted enough thread budget to spawn the
 *     pool's worker (MTH001) -- the embedder thread does not count
 *     against the limit; raise via mino_set_thread_limit (>= 1 for
 *     one agent worker; >= 2 if both send and send-off are used
 *     concurrently; more if mixing with futures / host threads).
 *
 * Inside a transaction, the action is queued and only fires after a
 * successful commit, matching JVM canon. */
mino_val *mino_send(mino_state *S, mino_val *agent,
                      mino_val *fn, mino_val *extra_args);

/* Like mino_send but routes the action onto the SOLO pool. Same
 * error semantics. mino's per-state eval lock means actions across
 * the two pools still serialize, but the queues are independent. */
mino_val *mino_send_off(mino_state *S, mino_val *agent,
                          mino_val *fn, mino_val *extra_args);

/* Block the calling thread until each named agent's in-flight count
 * reaches zero. `agents` is a NULL-terminated array of MINO_AGENT
 * values (or NULL/empty for a no-op). Throws (and returns NULL) if
 * called from inside an agent action (MST002, would self-deadlock)
 * or if any agent is from a foreign state (MST007). Returns nil on
 * success. */
mino_val *mino_await(mino_state *S, mino_val **agents);

/* Like mino_await with a millisecond timeout. Returns 1 if every
 * named agent reached zero in-flight before the deadline, 0 on
 * timeout. Throws on cross-state misuse or self-await (returns 0
 * after the throw is published). */
int         mino_await_for(mino_state *S, long long timeout_ms,
                            mino_val **agents);

/* Return the agent's most recent captured exception value, or NULL
 * if the agent is in a clean state. Throws on cross-state misuse
 * (returns NULL). */
mino_val *mino_agent_error(mino_state *S, mino_val *agent);

/* Restart a failed agent: clears its captured error and resets its
 * value to `new_state`. With clear_actions=1, drops every queued
 * action targeting this agent across both pools. Returns the new
 * state on success, or NULL after throwing if:
 *   - agent is not an agent (MTY001),
 *   - agent is from a foreign state (MST007),
 *   - agent is not failed (MST002),
 *   - the validator (if any) rejects new_state (MCT001). */
mino_val *mino_restart_agent(mino_state *S, mino_val *agent,
                                mino_val *new_state,
                                int clear_actions);

/* Return 1 if v is an STM ref (MINO_TX_REF), 0 otherwise. NULL-safe. */
int         mino_is_tx_ref(const mino_val *v);

/* Read a ref. Outside any transaction: atomic load of the ref's
 * committed value. Inside a transaction on the calling thread: the
 * in-tx effective value (alter tentative, commute-log replay, or
 * committed) AND records the read for read-set validation. Equivalent
 * to Clojure's @ref. The caller must already be on a thread that has
 * either entered an outer mino_tx_run or is executing inside a Clojure
 * dosync; outside both, the committed value is returned without any
 * tx bookkeeping. Returns NULL if v is not a ref. */
mino_val *mino_tx_ref_deref(mino_state *S, mino_val *v);

/* Set the ref's in-transaction tentative value to val. Must be called
 * from inside a transaction (started either by a Clojure dosync or by
 * mino_tx_run); throws eval/state MST002 outside a tx. Throws MST002
 * "Can't set after commute" if commute was called on this ref earlier
 * in the same tx. Throws eval/type MTY001 if v is not a ref. Returns
 * val on success. Equivalent to Clojure's `(ref-set ref val)`. */
mino_val *mino_tx_ref_set(mino_state *S, mino_val *v, mino_val *val);

/* C-callable transformer used by mino_tx_alter_c / mino_tx_commute_c.
 * `cur` is the in-tx effective value at call time; `user` is the
 * caller-supplied opaque pointer. Returns the new value. May call
 * mino_call for nested Clojure dispatch and may throw via
 * prim_throw_classified -- both unwind through the enclosing tx
 * runner. The function pointer must remain valid for the lifetime of
 * the transaction (in particular: across retries and through commit-
 * time replay for commute). */
typedef mino_val *(*mino_tx_xform_fn)(mino_state *S, mino_val *cur,
                                         void *user, mino_env *env);

/* Apply (fn cur user) to ref's current in-tx value and store the
 * result. Records a read for read-set validation. Must be called from
 * inside a transaction. Returns the new value, or NULL if the
 * transformer threw. Equivalent to (alter ref #(fn % user)). */
mino_val *mino_tx_alter_c(mino_state *S, mino_val *v,
                            mino_tx_xform_fn fn, void *user,
                            mino_env *env);

/* Like mino_tx_alter_c but does NOT record a read (commute semantics).
 * The fn is invoked once eagerly to produce the call-site value; if
 * the ref did not also have an alter / ref-set in the same tx, the fn
 * is replayed at commit time against the latest committed value (the
 * fn must therefore be commutative). Otherwise the call-site value is
 * what gets committed (matching JVM, which skips commute-log replay
 * for refs in the write set). Returns the call-site value, or NULL if
 * the transformer threw. */
mino_val *mino_tx_commute_c(mino_state *S, mino_val *v,
                              mino_tx_xform_fn fn, void *user,
                              mino_env *env);

/* Pin the ref against any concurrent committer until this transaction
 * commits or aborts. Returns the in-tx effective value. Must be in a
 * transaction. Equivalent to (ensure ref). */
mino_val *mino_tx_ensure(mino_state *S, mino_val *v,
                           mino_env *env);

/* C-callable transaction body, used by mino_tx_run. Returns the
 * body's last value. The body may invoke any in-tx accessor
 * (mino_tx_ref_deref / mino_tx_alter_c / ...), call mino_call to
 * dispatch into Clojure code, and throw via prim_throw_classified.
 *
 * The body is re-invoked on commit-time conflict, up to 10000 retries
 * before throwing eval/state MST004. Any user state mutated through
 * `user` survives across retries; if clean retry semantics matter,
 * snapshot mutable state into a retry-local buffer at the top of the
 * body. mino_val * pointers captured in user state should not be
 * relied on across retries (the GC may relocate or finalize between
 * iterations); re-derive them from the ref / world on each call. */
typedef mino_val *(*mino_tx_body_fn)(mino_state *S, void *user,
                                        mino_env *env);

/* Run body inside a transaction. Returns body's result on commit,
 * or NULL if the body threw an unhandled exception or the retry cap
 * was exhausted. Nests cleanly: if a transaction is already running
 * on the calling thread (started by an outer dosync or mino_tx_run),
 * the inner call is absorbed into the outer's tx and runs once
 * without its own setjmp / retry frame. Equivalent to the host-level
 * (dosync (body)). */
mino_val *mino_tx_run(mino_state *S, mino_tx_body_fn body,
                        void *user, mino_env *env);

/* ------------------------------------------------------------------------- */
/* Record types                                                              */
/* ------------------------------------------------------------------------- */

/*
 * Define a record type. Returns the MINO_TYPE value. The pointer is
 * stable for the life of the state (record types are pinned), so type
 * identity is pointer equality.
 *
 * Idempotent: subsequent calls with the same ns/name return the
 * existing type so re-loading scripts is safe. Field shape on a
 * re-call is ignored (the existing type wins) — the script-layer
 * `defrecord` macro validates shape changes when it re-evaluates.
 *
 * The strings are copied into the state's intern table on first use,
 * so callers may pass stack-allocated names. Field names are
 * converted to keywords and stored in declared order.
 *
 * Mirrors the Clojure `defrecord` macro name to make the C/script
 * analogy obvious to a C/C++/Rust embedder.
 */
mino_val *mino_defrecord(mino_state *S,
                           const char *ns,
                           const char *name,
                           const char *const *field_names,
                           size_t n_fields);

/* Return 1 if v is a record type (MINO_TYPE), 0 otherwise. */
int mino_is_record_type(const mino_val *v);

/*
 * Build a record. type must be a MINO_TYPE returned by
 * mino_defrecord; n_vals must equal the type's declared field count;
 * vals[i] gives the value for the i-th declared field. The vals
 * array is copied into a fresh malloc-owned slot vector owned by the
 * returned record. Returns NULL on type mismatch or arity mismatch
 * (the caller should treat NULL as a runtime error).
 *
 * The new record has no extension keys; assoc with an undeclared key
 * lazily allocates the ext map.
 */
mino_val *mino_record(mino_state *S, mino_val *type,
                        mino_val **vals, size_t n_vals);

/*
 * Read a declared field by name. Returns the field value (borrowed)
 * or NULL if name is not a declared field of record's type.
 * Extension keys are not read by this entry point; embedders can
 * reach them via the script-level (get r :ext-key) path.
 */
mino_val *mino_record_field(const mino_val *record, const char *name);

/* Return 1 if v is a record (MINO_RECORD), 0 otherwise. */
int mino_is_record(const mino_val *v);

/* ------------------------------------------------------------------------- */
/* Transient (batch-mutation) API                                            */
/* ------------------------------------------------------------------------- */

/*
 * Transients give embedders an efficient batch-mutation path for
 * building vectors, maps, and sets without allocating a new
 * persistent value per step. After calling mino_persistent(t) the
 * transient is sealed; calling any *_bang on it returns NULL and sets
 * a runtime error.
 *
 * Current implementation wraps persistent ops rather than mutating
 * the underlying trie nodes in place. The public API shape is
 * stable; in-place fast-paths may replace the wrapper in a later
 * release without ABI change.
 */

/* Wrap a persistent vector, map, or set in a new transient. Throws
 * on other types. The returned value is reusable until
 * mino_persistent is called on it. */
mino_val *mino_transient(mino_state *S, mino_val *coll);

/* Extract the current persistent value and invalidate the transient.
 * Further *_bang calls on t will throw. */
mino_val *mino_persistent(mino_state *S, mino_val *t);

/* Transient mutators. Each returns t on success (after updating its
 * inner persistent value) and NULL on a classified error. The caller
 * must use the returned value instead of retaining its own handle to
 * the transient across the call, matching Clojure's convention. */
mino_val *mino_assoc_bang(mino_state *S, mino_val *t,
                            mino_val *key, mino_val *val);
mino_val *mino_conj_bang(mino_state *S, mino_val *t,
                           mino_val *val);
mino_val *mino_dissoc_bang(mino_state *S, mino_val *t,
                             mino_val *key);
mino_val *mino_disj_bang(mino_state *S, mino_val *t,
                           mino_val *key);
mino_val *mino_pop_bang(mino_state *S, mino_val *t);

/* Transient predicates and accessors. */
int         mino_is_transient(const mino_val *v);
size_t      mino_transient_count(const mino_val *t);

/* ------------------------------------------------------------------------- */
/* Predicates and accessors                                                  */
/* ------------------------------------------------------------------------- */

/* Type-predicate grid. Each returns 1 iff v has the given effective
 * type, 0 otherwise. NULL is treated as MINO_NIL. The predicates
 * are tag-aware: they work identically for inline scalars (int, bool,
 * char, nil) and boxed cells. mino_is_float matches both MINO_FLOAT
 * (double) and MINO_FLOAT32; mino_is_map / mino_is_set match the
 * sorted and unsorted variants. */
int mino_is_nil       (const mino_val *v);
int mino_is_truthy    (const mino_val *v);
int mino_is_bool      (const mino_val *v);
int mino_is_int       (const mino_val *v);
int mino_is_float     (const mino_val *v);
int mino_is_char      (const mino_val *v);
int mino_is_string    (const mino_val *v);
int mino_is_symbol    (const mino_val *v);
int mino_is_keyword   (const mino_val *v);
int mino_is_cons      (const mino_val *v);
int mino_is_empty_list(const mino_val *v);
int mino_is_vector    (const mino_val *v);
int mino_is_map       (const mino_val *v);
int mino_is_set       (const mino_val *v);
int mino_is_fn        (const mino_val *v);
int mino_is_macro     (const mino_val *v);
int mino_is_prim      (const mino_val *v);
int mino_is_lazy      (const mino_val *v);
int mino_is_var       (const mino_val *v);
int mino_is_bigint    (const mino_val *v);
int mino_is_ratio     (const mino_val *v);
int mino_is_bigdec    (const mino_val *v);
int mino_is_uuid      (const mino_val *v);
int mino_is_regex     (const mino_val *v);
int mino_is_float32   (const mino_val *v);
int mino_is_sorted_map(const mino_val *v);
int mino_is_sorted_set(const mino_val *v);
int mino_is_map_entry (const mino_val *v);
int mino_is_host_array(const mino_val *v);
int mino_is_record    (const mino_val *v);  /* also at mino_defrecord */
int mino_is_queue     (const mino_val *v);  /* also at mino_queue_empty */
int mino_is_record_type(const mino_val *v); /* also at mino_defrecord */
int mino_is_handle    (const mino_val *v);  /* also near mino_handle */
int mino_is_atom      (const mino_val *v);  /* also near mino_atom */
int mino_is_volatile  (const mino_val *v);  /* also near mino_volatile */
int mino_is_agent     (const mino_val *v);  /* also near mino_agent */
int mino_is_tx_ref    (const mino_val *v);  /* also near mino_tx_ref */
int mino_is_transient (const mino_val *v);  /* also near mino_transient */
int mino_is_future    (const mino_val *v);

/* Structural equality. Returns 1 if a and b are equal, 0 otherwise. */
int mino_eq(const mino_val *a, const mino_val *b);

/* Three-way comparison. Returns -1, 0, or 1 for orderable arguments
 * matching Clojure `compare`. Cross-type comparison throws via the
 * runtime's error path (the caller sees a NULL last_error change);
 * orderless types (`fn`, `prim`, `handle`, ...) likewise throw.
 * NULL is treated as nil; (compare nil nil) is 0. */
int mino_compare(mino_state *S, const mino_val *a, const mino_val *b);

/* Canonical 32-bit value hash matching Clojure's hash contract: for
 * any pair where `mino_eq(a, b)` is 1, `mino_hash(a) == mino_hash(b)`.
 * Tag-aware; NULL hashes the same as nil. */
uint32_t mino_hash(const mino_val *v);

/* Return the first element of a cons cell, or NULL. */
mino_val *mino_car(const mino_val *v);

/* Return the rest of a cons cell, or NULL. */
mino_val *mino_cdr(const mino_val *v);

/* Universal seq abstraction matching Clojure's `seq` / `first` /
 * `rest` / `next`:
 *
 *   mino_seq(S, coll)   -- coerce coll to a seq; returns NULL for an
 *                          eval error, mino_nil() for an empty input.
 *   mino_first(coll)    -- first element of the seq; mino_nil() if
 *                          empty.
 *   mino_rest(S, coll)  -- the seq of everything after the first; an
 *                          empty list when there's nothing left.
 *   mino_next(S, coll)  -- (seq (rest coll)): nil when empty, the
 *                          forced seq otherwise. Matches the
 *                          Clojure idiom for terminating while-let
 *                          loops.
 *
 * The cons-spine-only mino_car/mino_cdr above stay; this quartet
 * covers the universal walk across vec / map / set / lazy / chunked /
 * sorted variants. */
mino_val *mino_seq  (mino_state *S, mino_val *coll);
mino_val *mino_first(mino_val *coll);
mino_val *mino_rest (mino_state *S, mino_val *coll);
mino_val *mino_next (mino_state *S, mino_val *coll);

/* Metadata read / attach matching script-side `(meta x)` and
 * `(with-meta x m)`. `mino_with_meta` returns a fresh shallow-copy
 * with the new meta map; identity-shaped types (atom, agent) throw.
 * `m` must be a MINO_MAP or MINO_NIL. */
mino_val *mino_meta(const mino_val *v);
mino_val *mino_with_meta(mino_state *S, mino_val *v, mino_val *meta);

/* Dynamic-binding push/pop from C, peer to script-side
 * `(binding [...] ...)`. `vars` is an array of MINO_SYMBOL values
 * naming the dynamic vars to rebind; `vals` is the parallel array
 * of values. Returns an opaque frame handle the embedder pops with
 * `mino_pop_bindings` after evaluating the dynamically-scoped work.
 *
 * Nests correctly with script-side `binding` frames: a script-side
 * unwind through `throw` walks the dyn stack and clears any frames
 * still open above the catching frame, including ones the embedder
 * pushed.
 *
 * Returns NULL on allocation failure or invalid argument shape (any
 * non-symbol in `vars`, or attempting to bind a non-dynamic var). */
typedef struct mino_binding_frame mino_binding_frame;
mino_binding_frame *mino_push_bindings(mino_state *S,
                                        mino_val **vars,
                                        mino_val **vals,
                                        size_t n);
void                mino_pop_bindings (mino_state *S,
                                        mino_binding_frame *frame);

/* Cross-state transferability pre-flight. Returns 1 if the value
 * tree contains only transferable types (nil, bool, int, float,
 * string, symbol, keyword, cons, vector, map, set, sorted variants,
 * lazy, bigint, ratio, bigdec, uuid, regex source); returns 0 if
 * any identity-bearing leaf (atom, agent, handle, future, tx_ref,
 * fn, prim, etc.) is reached. When non-NULL, `*out_reason` names
 * the first non-transferable type encountered. Cheaper than
 * `mino_clone` because no copy is performed. */
int mino_can_clone(const mino_val *v, const char **out_reason);

/* Return the number of cons cells in a list. */
size_t mino_length(const mino_val *list);

/* Type-safe C extraction. Each returns 1 on success, 0 on type mismatch.
 * mino_to_bool uses truthiness (only nil and false are falsey). String,
 * keyword, and symbol extractors write the interned byte pointer through
 * `out` (NUL-terminated, mino-owned) and the byte length through `len`. */
int mino_to_int    (const mino_val *v, long long *out);
int mino_to_float  (const mino_val *v, double *out);
int mino_to_float32(const mino_val *v, float *out);
int mino_to_bool   (const mino_val *v);
int mino_to_char   (const mino_val *v, int *cp);
int mino_to_string (const mino_val *v, const char **out, size_t *len);
int mino_to_keyword(const mino_val *v, const char **out, size_t *len);
int mino_to_symbol (const mino_val *v, const char **out, size_t *len);

/* Bigint -> decimal-string serialiser. Writes into the caller-owned
 * `buf` of `n` bytes; the number of bytes written (not counting the
 * NUL terminator) lands in `*out_written` when non-NULL. Returns 1
 * on success, 0 on type mismatch, -1 if the buffer is too small to
 * hold the formatted result + NUL. */
int mino_to_bigint_str(const mino_val *v, char *buf, size_t n,
                       size_t *out_written);

/* Ratio extractor for ratios whose numerator and denominator both fit
 * in `long long`. Returns 1 on success, 0 on type mismatch or out-of-
 * range (bigint-backed ratios that overflow `long long`). */
int mino_to_ratio(const mino_val *v, long long *out_num,
                  long long *out_den);

/* Bigdec -> string serialiser (canonical Clojure-style printing,
 * e.g. "1.5M"-shaped value prints as "1.5"). Same buffer contract as
 * mino_to_bigint_str. */
int mino_to_bigdec_str(const mino_val *v, char *buf, size_t n,
                       size_t *out_written);

/* UUID extractor. Writes the 16 RFC 4122 bytes into the caller-owned
 * `out[16]` array. Returns 1 on success, 0 on type mismatch. */
int mino_to_uuid_bytes(const mino_val *v, uint8_t out[16]);

/* Regex source extractor. Writes the pattern source pointer through
 * `out` (mino-owned, NUL-terminated) and the byte length through
 * `len`. Returns 1 on success, 0 on type mismatch. */
int mino_to_regex_source(const mino_val *v, const char **out,
                         size_t *len);

/* ------------------------------------------------------------------------- */
/* Printer                                                                   */
/* ------------------------------------------------------------------------- */

#include <stdio.h>

/* Print a value to stdout in readable form. */
void mino_print(mino_state *S, const mino_val *v);

/* Print a value to stdout followed by a newline. */
void mino_println(mino_state *S, const mino_val *v);

/* Print a value to the given FILE stream. */
void mino_print_to(mino_state *S, FILE *out, const mino_val *v);

/* Print a value's readable form into a sized buffer (NUL-terminated).
 * Returns the number of bytes written excluding the trailing NUL, or
 * -1 on error (NULL buf, zero capacity, or I/O failure). When the
 * printed form is longer than n - 1 bytes, the output is truncated to
 * fit and the function still returns the truncated byte count. Use
 * this when the embedder routes output elsewhere than a FILE *
 * (server response buffer, plugin host, IDE panel). */
int  mino_print_to_buf(mino_state *S, const mino_val *v,
                       char *buf, size_t n);

/* ------------------------------------------------------------------------- */
/* Reader                                                                    */
/* ------------------------------------------------------------------------- */

/*
 * Read one form from `src`. On success returns a value and writes a pointer
 * just past the consumed input to `*end` (when `end` is non-NULL). On EOF
 * (only whitespace / comments remaining) returns NULL with `*end` advanced
 * past the trailing whitespace. On parse error returns NULL and writes a
 * human-readable message via `mino_last_error()`.
 */
mino_val *mino_read(mino_state *S, const char *src, const char **end);

/* Return the last error message, or NULL if no error occurred. */
const char *mino_last_error(mino_state *S);

/* Opaque structured diagnostic type. Use mino_last_diag() to retrieve. */
typedef struct mino_diag mino_diag;

/* Return the last structured diagnostic, or NULL if no error occurred.
 * The returned pointer is valid until the next error or clear_error. */
const mino_diag *mino_last_diag(mino_state *S);

/* Return the last error as a mino map with :mino/kind, :mino/code, etc.
 * Returns nil if no error occurred. The value is GC-owned and cached. */
mino_val *mino_last_error_map(mino_state *S);

/* Return the last error's classified kind (e.g. "eval/type", "eval/arity",
 * "name", "reader") or NULL when no error is current. The returned
 * pointer is valid until the next error or mino_clear_error call. */
const char *mino_error_kind(mino_state *S);

/* Return the last error's stable code (e.g. "MTY001", "MNS002") or NULL
 * when no error is current. Lifetimes match mino_error_kind. */
const char *mino_error_code(mino_state *S);

/* Clear the last error and diagnostic. After this call mino_last_error,
 * mino_error_kind, and mino_error_code all return NULL. */
void mino_clear_error(mino_state *S);

/* Diagnostic rendering modes. */
#define MINO_DIAG_RENDER_COMPACT 0
#define MINO_DIAG_RENDER_PRETTY  1

/* Render a diagnostic into buf. Returns bytes written (excl NUL).
 * mode is one of MINO_DIAG_RENDER_COMPACT or _PRETTY. */
int mino_render_diag(mino_state *S, const mino_diag *d,
                     int mode, char *buf, size_t n);

/* ------------------------------------------------------------------------- */
/* Runtime state                                                             */
/* ------------------------------------------------------------------------- */

/*
 * Create a new isolated runtime state. Each state owns its own GC, intern
 * tables, module cache, and singletons. Multiple states may coexist in the
 * same process; they share no mutable data.
 *
 * Fatal: aborts on allocation failure (no state exists to report through).
 */
mino_state *mino_state_new(void);

/*
 * Free a runtime state and all resources owned by it. All GC-managed objects,
 * intern tables, module caches, and metadata are released. Environments
 * created within this state become invalid.
 */
void mino_state_free(mino_state *S);

/*
 * JIT mode control (per-state).
 *
 *   AUTO  -- default. Eligible fns JIT after warming past the runtime's
 *            hot threshold (currently 100 calls; tune with
 *            mino_state_set_jit_hot_threshold). Cold fns stay on the
 *            interpreter.
 *   OFF   -- never JIT. The interpreter handles every call. Useful for
 *            embedding hosts that need predictable cold-start latency,
 *            deterministic resource usage, or that ship a security
 *            policy banning W^X violations.
 *   ON    -- JIT every eligible fn on its first call (no threshold).
 *            Useful for benchmarking or when the embedder knows ahead
 *            of time that JIT'd execution is wanted everywhere.
 *
 * mino-lean (the no-JIT distributable) ignores the mode and behaves
 * as if OFF; the call exists in both builds so embedders can write
 * portable initialisation code.
 *
 * Initial mode on state_new follows MINO_JIT env var (auto / off / on
 * case-insensitive), defaulting to AUTO.
 */
typedef enum {
    MINO_JIT_MODE_AUTO = 0,
    MINO_JIT_MODE_OFF  = 1,
    MINO_JIT_MODE_ON   = 2
} mino_jit_mode;

void            mino_state_set_jit_mode(mino_state *S, mino_jit_mode mode);
mino_jit_mode mino_state_jit_mode(const mino_state *S);

/*
 * JIT hot threshold (call count before AUTO mode triggers a compile).
 * Defaults to a runtime-internal value (currently 100). Embedders
 * tune it per-workload via this setter:
 *
 *   - Lower values JIT sooner (better steady-state throughput at
 *     the cost of more upfront compile time and a larger native
 *     footprint -- short-lived fns may compile then never run
 *     again).
 *   - Higher values delay JIT (less upfront work, but slower to
 *     warm).
 *
 * A value of 0 is clamped to 1 (compile on first call -- the same
 * behaviour as MINO_JIT_MODE_ON, but staying in AUTO so the OFF/ON
 * gate still applies). The threshold is irrelevant when the mode
 * is OFF (no compile happens regardless) or ON (compile fires on
 * call 1).
 *
 * Initial value follows MINO_JIT_HOT_THRESHOLD env var (positive
 * integer); unparseable / non-positive values fall back to the
 * default. NULL state is a no-op.
 */
void     mino_state_set_jit_hot_threshold(mino_state *S, unsigned threshold);
unsigned mino_state_jit_hot_threshold(const mino_state *S);

/*
 * JIT capability query. Returns a snapshot of the runtime's JIT
 * configuration so embedders can inspect what they got -- useful
 * for diagnostics, telemetry, and conditional embedding logic
 * that wants to behave differently when JIT is unavailable.
 *
 *   available  -- non-zero when this build was compiled with JIT
 *                 support AND the host arch / OS is one of the
 *                 supported targets. Always 0 on mino-lean.
 *   mode       -- the state's current mino_jit_mode.
 *   threshold  -- the state's current hot threshold.
 *   host_arch  -- "arm64" / "x86_64" / "unknown".
 *   host_os    -- "darwin" / "linux" / "windows" / "unknown".
 *
 * The host_arch and host_os strings live in static storage; the
 * caller does not free them.
 */
typedef struct {
    int             available;
    mino_jit_mode mode;
    unsigned        threshold;
    const char     *host_arch;
    const char     *host_os;
} mino_jit_capability;

mino_jit_capability mino_state_jit_capability(const mino_state *S);

/* ------------------------------------------------------------------------- */
/* Environment and evaluator                                                 */
/* ------------------------------------------------------------------------- */

/*
 * Allocate a fresh root environment and register it with the collector so
 * every value reachable through it survives collection. The runtime holds
 * the returned env weakly: mino_env_free unregisters it and lets the next
 * sweep reclaim the frame and any closures that were reachable only from
 * within it. The host does not free any mino-owned pointers directly.
 *
 * Returns NULL on allocation failure (check mino_last_error).
 */
mino_env *mino_env_new(mino_state *S);
/* Free an environment and unregister it from the collector. */
void        mino_env_free(mino_state *S, mino_env *env);

/*
 * Clone an environment: allocate a new root environment and copy all
 * bindings from the source. Values are shared (not deep-copied), so
 * the clone is only meaningful within the same state. Useful for
 * snapshotting a session before independent evaluation in each copy.
 */
mino_env *mino_env_clone(mino_state *S, mino_env *env);

/*
 * Convenience: allocate a new env and install the sandbox preset.
 * Equivalent to:
 *
 *   mino_env *env = mino_env_new(S);
 *   mino_install_sandbox(S, env);
 *
 * Use this when getting a runnable env in one line matters and the
 * sandbox surface is the right contract. For a custom tier, build the
 * env explicitly and pass a MINO_CAP_* bitmask to mino_install.
 */
mino_env *mino_env_new_default(mino_state *S);

/* Define or replace a binding in `env`. */
void        mino_env_set(mino_state *S, mino_env *env, const char *name,
                         mino_val *val);
/* Look up `name`. Returns NULL if unbound. */
mino_val *mino_env_get(mino_env *env, const char *name);

/*
 * Evaluate one form. Returns NULL on error and writes a message via
 * mino_last_error(). Returns mino_nil() for an explicit nil result.
 *
 * A top-level try frame catches out-of-memory conditions and unhandled
 * exceptions during evaluation, surfacing them as NULL + error message
 * rather than aborting the process. The state remains usable after an
 * OOM return.
 */
mino_val *mino_eval(mino_state *S, mino_val *form, mino_env *env);

/*
 * Read and evaluate all forms in `src`. Returns the value of the last
 * form, or NULL on error. An empty string returns mino_nil().
 *
 * Installs its own try frame: OOM during read or eval returns NULL
 * with an error message rather than aborting.
 */
mino_val *mino_eval_string(mino_state *S, const char *src,
                             mino_env *env);

/*
 * Read a file at `path` and evaluate all forms. Returns the value of the
 * last form, or NULL on error (file I/O failures and parse/eval errors).
 */
mino_val *mino_load_file(mino_state *S, const char *path,
                           mino_env *env);

/*
 * Protected variants of mino_eval / mino_eval_string / mino_load_file.
 * Return 0 on success (writing the result to *out) or -1 on error.
 *
 * Unlike the unsuffixed variants, the _ex form disambiguates "real nil
 * result" from "error": a 0 return with *out == mino_nil() is genuine
 * nil; a -1 return means a throw / OOM / parse failure was caught. If
 * out_ex is non-NULL, on error *out_ex is set to the raw thrown payload
 * (matching mino_pcall's contract). *out_ex is NULL on success or when
 * out_ex is NULL.
 *
 * The _ex variants do NOT publish to mino_last_error on a caught throw;
 * embedders that want a diagnostic must inspect *out_ex or call
 * mino_last_error explicitly.
 */
int mino_eval_ex       (mino_state *S, mino_val *form, mino_env *env,
                        mino_val **out, mino_val **out_ex);
int mino_eval_string_ex(mino_state *S, const char *src, mino_env *env,
                        mino_val **out, mino_val **out_ex);
int mino_load_file_ex  (mino_state *S, const char *path, mino_env *env,
                        mino_val **out, mino_val **out_ex);

/*
 * Shorthand: bind a C function as a primitive in `env`.
 * Equivalent to mino_env_set(S, env, name, mino_prim(S, name, fn)).
 */
void mino_register_fn(mino_state *S, mino_env *env, const char *name,
                      mino_prim_fn fn);

/*
 * Bulk-register an array of C primitives. The array is terminated by
 * a sentinel row `{NULL, NULL, NULL, 0}`. Each row's `argv` field
 * picks the ABI: 0 = cons-list (`fn` is mino_prim_fn), 1 = argv
 * (`fn2` is mino_prim_fn2). Either `fn` or `fn2` must be set
 * (matching the chosen ABI), the other can be NULL. Matches the peer
 * convention used by other embedded scripting C APIs for bulk
 * registration.
 *
 * Example:
 *   static mino_val *prim_a(mino_state *S, mino_val *args, mino_env *e) {...}
 *   static mino_val *prim_b(mino_state *S, mino_val **argv, int n, mino_env *e) {...}
 *   static const mino_reg my_prims[] = {
 *       {"a", prim_a, NULL,    0},
 *       {"b", NULL,   prim_b,  1},
 *       {NULL, NULL,  NULL,    0},
 *   };
 *   mino_register_fns(S, env, my_prims);
 */
typedef struct {
    const char    *name;
    mino_prim_fn   fn;
    mino_prim_fn2  fn2;
    int            argv;   /* 0 = cons ABI (use fn), 1 = argv ABI (use fn2) */
} mino_reg;

void mino_register_fns(mino_state *S, mino_env *env, const mino_reg *regs);

/*
 * Call a callable value (fn, macro, prim) with an argument list.
 * Returns the result, or NULL on error (via mino_last_error).
 */
mino_val *mino_call(mino_state *S, mino_val *fn, mino_val *args,
                      mino_env *env);

/*
 * Protected call: same as mino_call but returns 0 on success (writing
 * the result to *out) or -1 on error. The error message is available
 * via mino_last_error(). *out is set to NULL on error.
 *
 * If out_ex is non-NULL, on error *out_ex is set to the raw thrown
 * exception value (the cell passed to (throw ...) by the inner code).
 * This is the original payload, not a diagnostic map -- useful for
 * captures-and-stores callers like agent dispatch and STM validator
 * handling that want to surface the user's ex-info / map / etc.
 * unchanged. *out_ex is set to NULL on success or if out_ex is NULL.
 */
int mino_pcall(mino_state *S, mino_val *fn, mino_val *args,
               mino_env *env, mino_val **out, mino_val **out_ex);

/* ------------------------------------------------------------------------- */
/* Exceptions                                                                */
/* ------------------------------------------------------------------------- */

/*
 * Raise a mino exception carrying `ex` as the payload. When called from a
 * C primitive that is executing inside a mino (try ... (catch ...)) frame,
 * the payload is delivered to the matching catch binding. When called
 * outside any try frame, the exception surfaces as a classified error
 * through mino_last_error (kind "user", code "MUS001") and the function
 * returns NULL.
 *
 * Inside a try frame this call does NOT return; control transfers to the
 * try handler via longjmp. Callers should treat it as "returns NULL" for
 * typing purposes and not rely on any code after the call:
 *
 *   return mino_throw(S, mino_keyword(S, "bad-arg"));
 *
 * The payload can be any mino value: a keyword, a string, or an
 * information-carrying map built via mino_map(). This is the C-side
 * analogue of the (throw ex) mino primitive.
 */
mino_val *mino_throw(mino_state *S, mino_val *ex);

/* ------------------------------------------------------------------------- */
/* Argument parsing                                                          */
/* ------------------------------------------------------------------------- */

/*
 * Type-check and destructure a primitive's argument list into C variables.
 * The format string lists one character per expected positional argument;
 * each variadic pointer receives the corresponding extracted value:
 *
 *   "i"  long long *        -- MINO_INT
 *   "f"  double *           -- MINO_FLOAT or MINO_INT (promoted)
 *   "s"  const char **      -- MINO_STRING; pointer into mino-owned data
 *   "S"  const char **,     -- MINO_STRING; pointer plus byte length
 *        size_t *              (write both in order: "S" consumes two ptrs)
 *   "k"  const char **      -- MINO_KEYWORD name (without the leading :)
 *   "y"  const char **      -- MINO_SYMBOL name
 *   "b"  int *              -- MINO_BOOL (0 or 1)
 *   "c"  int *              -- MINO_CHAR codepoint (0..0x10FFFF)
 *   "v"  mino_val **      -- any value (no type check)
 *   "V"  mino_val **      -- MINO_VECTOR
 *   "M"  mino_val **      -- MINO_MAP
 *   "L"  mino_val **      -- MINO_CONS or MINO_NIL (a list)
 *   "H"  mino_val **      -- MINO_HANDLE
 *   "A"  mino_val **      -- MINO_ATOM
 *
 * Returns 0 on success, -1 on arity or type error. On failure the current
 * error is set via mino_last_error with kind "eval/arity" or "eval/type"
 * so the caller can just `return NULL;` after a non-zero return. The name
 * argument is used in error messages ("foo: expected int, got string").
 *
 * Extra arguments beyond the format string cause an arity error; missing
 * arguments do the same.
 */
int mino_args_parse(mino_state *S, const char *name, mino_val *args,
                    const char *fmt, ...);

/* ------------------------------------------------------------------------- */
/* Host interop                                                              */
/* ------------------------------------------------------------------------- */

/*
 * Enable interop dispatch. By default all host primitives return
 * "interop disabled". Call this after registration to activate them.
 */
void mino_host_enable(mino_state *S);

/*
 * Register host capabilities. String arguments must outlive the state.
 * Call before eval -- the registry is intended to be immutable after init.
 *
 * arity: expected argument count, or -1 for variadic.
 */
void mino_host_register_ctor(mino_state *S, const char *type_key,
                              int arity, mino_host_fn fn, void *ctx);
void mino_host_register_method(mino_state *S, const char *type_key,
                                const char *method_key, int arity,
                                mino_host_fn fn, void *ctx);
void mino_host_register_static(mino_state *S, const char *type_key,
                                const char *method_key, int arity,
                                mino_host_fn fn, void *ctx);
void mino_host_register_getter(mino_state *S, const char *type_key,
                                const char *field_key, mino_host_fn fn,
                                void *ctx);

/* ------------------------------------------------------------------------- */
/* Modules                                                                   */
/* ------------------------------------------------------------------------- */

/*
 * Module resolver callback. Given a module name (the argument to `require`),
 * return a file path to load, or NULL on failure. `ctx` is the opaque
 * pointer passed to mino_set_resolver.
 */
typedef const char *(*mino_resolve_fn)(const char *name, void *ctx);

/*
 * Register a module resolver. When mino code calls (require "name"), the
 * resolver is invoked to map the name to a file path. The file is loaded
 * once; subsequent requires of the same name return the cached value.
 * Pass NULL to remove the resolver.
 */
void mino_set_resolver(mino_state *S, mino_resolve_fn fn, void *ctx);

/*
 * Register a bundled-stdlib source under `name`. Subsequent calls to
 * (require '<name>) load this in-memory source instead of going to the
 * disk resolver, so embeds and brew/scoop installs without a lib/
 * directory still resolve their bundled namespaces.
 *
 * The source pointer must outlive the state -- typically a static
 * C-string literal in a generated header (e.g. lib_clojure_string.h).
 * The name is copied and freed at state teardown. Re-registering the
 * same name silently replaces the previous source.
 *
 * Use mino_install_clojure_<name> wrappers (declared below) when an
 * install hook fits the embedder's group; this raw API is exposed for
 * embedders that bundle their own non-clojure namespaces (mirroring
 * mino_set_resolver for the disk path).
 */
void mino_register_bundled_lib(mino_state *S, const char *name,
                                const char *source);

/* ------------------------------------------------------------------------- */
/* Capability-gated install API                                              */
/* ------------------------------------------------------------------------- */

/*
 * Capabilities are addressable as bits. Pass a bitmask to mino_install to
 * install exactly the subset the host wants:
 *
 *   mino_install(S, env, MINO_CAP_IO | MINO_CAP_FS | MINO_CAP_REGEX);
 *
 * Or pick one of the named presets:
 *
 *   mino_install_minimal(S, env)  -> floor only, no core.clj
 *   mino_install_sandbox(S, env)  -> floor + canonical Clojure-core + safe libs;
 *                                    no I/O, FS, PROC, STM, AGENT, HOST, ASYNC
 *   mino_install_all(S, env)      -> every capability and every bundled lib
 *
 * The floor (MINO_CAP_FLOOR) is always installed by any mino_install* call
 * — numeric, collections, sequences, printing, foundational macros, plus
 * the *ns* / *out* / *in* dynamic vars. Passing caps = 0 to mino_install
 * is equivalent to mino_install_minimal.
 *
 * Capabilities are independent; mino_install does not chain implicit
 * dependencies. Embedders who want canonical bundles use MINO_CAP_DEFAULT
 * or MINO_CAP_ALL. mino_install is idempotent: re-installing a capability
 * is a no-op.
 *
 * Calling mino_install with any non-floor capability also evaluates
 * core.clj on the floor env so the capability-gated sections fire.
 */
#define MINO_CAP_FLOOR         (1u <<  0)
#define MINO_CAP_REGEX         (1u <<  1)
#define MINO_CAP_BIGNUM        (1u <<  2)
#define MINO_CAP_MULTIMETHODS  (1u <<  3)
#define MINO_CAP_PROTOCOLS     (1u <<  4)
#define MINO_CAP_TRANSDUCERS   (1u <<  5)
#define MINO_CAP_IO            (1u <<  6)
#define MINO_CAP_FS            (1u <<  7)
#define MINO_CAP_PROC          (1u <<  8)
#define MINO_CAP_STM           (1u <<  9)
#define MINO_CAP_AGENT         (1u << 10)
#define MINO_CAP_HOST          (1u << 11)
#define MINO_CAP_ASYNC         (1u << 12)
#define MINO_CAP_STRING_LIB    (1u << 13)  /* clojure.string */
#define MINO_CAP_SET_LIB       (1u << 14)  /* clojure.set */
#define MINO_CAP_WALK          (1u << 15)  /* clojure.walk */
#define MINO_CAP_EDN           (1u << 16)  /* clojure.edn */
#define MINO_CAP_PPRINT        (1u << 17)  /* clojure.pprint */
#define MINO_CAP_ZIP           (1u << 18)  /* clojure.zip */
#define MINO_CAP_DATA          (1u << 19)  /* clojure.data */
#define MINO_CAP_TEST          (1u << 20)  /* clojure.test (+ clojure.test.check) */
#define MINO_CAP_REPL_LIB      (1u << 21)  /* clojure.repl */
#define MINO_CAP_DATAFY        (1u << 22)  /* clojure.datafy + core.protocols */
#define MINO_CAP_INSTANT       (1u << 23)  /* clojure.instant */
#define MINO_CAP_SPEC          (1u << 24)  /* clojure.spec.alpha */
#define MINO_CAP_TOOLING       (1u << 25)  /* mino.deps + mino.tasks */
#define MINO_CAP_MATH_LIB      (1u << 26)  /* clojure.math */
#define MINO_CAP_REDUCERS      (1u << 27)  /* clojure.core.reducers */

/* The sandbox preset: floor + Clojure-core (multimethods, protocols,
 * transducers, regex, bignum) + the bundled libraries that have no I/O
 * surface (string, set, walk, edn, data, test, datafy). Excludes IO, FS,
 * PROC, HOST, STM, AGENT, ASYNC. */
#define MINO_CAP_DEFAULT (MINO_CAP_FLOOR | MINO_CAP_REGEX | MINO_CAP_BIGNUM | \
                          MINO_CAP_MULTIMETHODS | MINO_CAP_PROTOCOLS | \
                          MINO_CAP_TRANSDUCERS | MINO_CAP_STRING_LIB | \
                          MINO_CAP_SET_LIB | MINO_CAP_WALK | MINO_CAP_EDN | \
                          MINO_CAP_DATA | MINO_CAP_TEST | MINO_CAP_DATAFY | \
                          MINO_CAP_MATH_LIB | MINO_CAP_REDUCERS)

/* Every defined capability bit. */
#define MINO_CAP_ALL     0xFFFFFFFFu

/*
 * Install the given set of capabilities into env. FLOOR is always
 * installed implicitly. Passing 0 installs just the floor (no core.clj),
 * matching mino_install_minimal. Idempotent: bits already installed are
 * skipped silently.
 */
void mino_install(mino_state *S, mino_env *env, unsigned int caps);

/*
 * Floor-only convenience. Installs the foundational C primitives plus
 * the unconditional sections of core.clj's prelude (defn / when / cond /
 * and / or / threading / lazy seqs / basic collections / printing). Does
 * NOT evaluate core.clj. Use when targeting Lua-class cold start in
 * embedded mode.
 */
void mino_install_minimal(mino_state *S, mino_env *env);

/*
 * Sandbox preset: equivalent to mino_install(S, env, MINO_CAP_DEFAULT).
 * Names the recipe for "safe untrusted-script env" so embedders don't
 * reinvent the threat model.
 */
void mino_install_sandbox(mino_state *S, mino_env *env);

/*
 * Install every capability and every bundled stdlib namespace the
 * standalone binary ships with. Equivalent to
 * mino_install(S, env, MINO_CAP_ALL).
 */
void mino_install_all(mino_state *S, mino_env *env);

/* Inspect installed capabilities. Bit set per MINO_CAP_*. */
unsigned int mino_capabilities(const mino_state *S);
int          mino_capability_installed(const mino_state *S, unsigned int cap);

/*
 * Enumerate the full capability registry. The returned array is static,
 * NULL-terminated, and ordered by install tier. Each entry names the
 * capability, its bit, and a one-line UX summary. Use this to render a
 * "supported capabilities" list to the host's user.
 */
typedef struct {
    const char  *name;     /* canonical label ("io", "regex", ...) */
    unsigned int bit;      /* MINO_CAP_* */
    const char  *summary;  /* one-line UX description */
} mino_capability_info;

const mino_capability_info *mino_capability_list(void);

/*
 * Look up the capability that owns a given symbol name, if any. Returns
 * the capability info pointer or NULL when the name is not a known
 * gateable primitive or canonical core.clj definition. Used by the
 * eval_symbol MNS002 diagnostic to enrich "unbound symbol" errors.
 */
const mino_capability_info *mino_capability_for_symbol(const char *name);

/* ------------------------------------------------------------------------- */
/* Execution limits                                                          */
/* ------------------------------------------------------------------------- */

#define MINO_LIMIT_STEPS  1   /* max eval steps per mino_eval/mino_eval_string */
#define MINO_LIMIT_HEAP   2   /* max bytes under GC management                */

/*
 * Set a global execution limit. Pass 0 to disable a limit. Step limits
 * are reset at the start of each mino_eval or mino_eval_string call.
 * When a limit is exceeded, the current eval returns NULL and
 * mino_last_error() reports the cause.
 */
void mino_set_limit(mino_state *S, int kind, size_t value);

/*
 * Request interruption of a running eval. Sets a flag that the eval loop
 * checks on each step. Safe to call from a different thread than the one
 * running eval. The flag is cleared at the start of the next eval call.
 */
void mino_interrupt(mino_state *S);

/* ------------------------------------------------------------------------- */
/* Host thread grant                                                         */
/* ------------------------------------------------------------------------- */

/*
 * Threading is a per-state runtime capability, not a build-time toggle.
 * Each `mino_state` starts with `thread_limit = 1`; while the limit is
 * <= 1, `(future ...)`, `(promise)` / `deliver` / `realized?`,
 * `(thread ...)`, and the blocking core.async ops `<!!` / `>!!` /
 * `alts!!` throw `:mino/unsupported` with a message that names the policy
 * and points the host at this surface.
 *
 * Standalone `./mino` grants `thread_limit = <cpu_count>` right after
 * `mino_install_all`, so REPL/script users get the canonical surface
 * working out of the box. Embedders opt in per state by calling
 * `mino_set_thread_limit` with a value > 1.
 */

/*
 * Set the per-state thread limit. n=0 or n=1 disables host threads;
 * n>1 grants the runtime permission to spawn that many concurrent
 * worker threads. Calling with the same value twice is a no-op.
 *
 * Calling with n>1 BEFORE any `(future ...)` is in flight is the
 * supported sequence; lowering the limit while threads are running
 * does not interrupt them, but new spawns will respect the new ceiling.
 */
void mino_set_thread_limit(mino_state *S, int n);

/*
 * Read the current thread limit. Returns 1 by default (single-threaded).
 */
int  mino_get_thread_limit(mino_state *S);

/*
 * Return the count of host threads currently spawned by this state.
 * Decremented as threads complete and join. Returns 0 in single-
 * threaded mode.
 */
int  mino_thread_count(mino_state *S);

/*
 * Wait for all in-flight host threads to finish. Intended to be called
 * by the embedder before `mino_state_free`; calling it from a worker
 * thread that this state spawned is undefined behaviour. Safe to call
 * when no threads are in flight (returns immediately).
 */
void mino_quiesce_threads(mino_state *S);

/* ------------------------------------------------------------------------- */
/* Host thread pool, factory, stack-size knobs [MINO_UNSTABLE_THREADPOOL]    */
/* ------------------------------------------------------------------------- */
/*
 * UNSTABLE: this section is provisional and stays UNSTABLE through the
 * 0.x alpha series. The pool ABI, factory callback shape, and stack-
 * size knobs may change in subsequent releases. Symbols outside this
 * section aim for source stability; symbols inside this block do not.
 *
 * The default model is "spawn-per-future": each `(future ...)` calls
 * pthread_create / CreateThread and the resulting OS thread runs the
 * future body. Three knobs let embedders shape that:
 *
 *   1. mino_set_thread_pool — delegate worker management to a host
 *      pool (Tokio runtime, libuv, ASIO, custom pthread pool). Mino
 *      submits work items via the pool's submit_fn; the pool decides
 *      scheduling, threading, and recycling. The same pool may be
 *      bound to multiple `mino_state` for multi-tenant patterns
 *      (game-engine-per-NPC, chat-bot-fleet, IDE-per-buffer-linter):
 *      the pool's N workers fan out across all of them.
 *
 *   2. mino_set_thread_factory — install start/end callbacks that
 *      fire on each mino-spawned worker. Use for naming, CPU
 *      affinity, priority class, or tracing-context inheritance.
 *      Spawn-per-future path only; pool workers are owned by the
 *      pool and run under its own lifecycle hooks.
 *
 *   3. mino_set_thread_stack_size — set the per-worker stack size for
 *      the spawn-per-future path. Pool-managed workers ignore this.
 */

/*
 * Function-pointer interface a host thread pool implements. Mino
 * calls submit_fn for each `(future ...)` while a pool is registered;
 * the pool MUST eventually call (*work)(ctx) on a worker thread. The
 * pool MUST NOT call work synchronously on the calling thread —
 * that defeats the parallelism mino is asking for.
 */
typedef struct mino_thread_pool {
    /* Submit a work item. Return 0 on success, non-zero on
     * pool-full / shut-down / refusal. A non-zero return makes mino
     * throw `:mino/thread-limit-exceeded`. */
    int (*submit_fn)(struct mino_thread_pool *pool,
                     void (*work)(void *), void *ctx);
    /* User data for the pool implementation; mino doesn't touch. */
    void *user_data;
} mino_thread_pool;

/*
 * Register a host thread pool for this state. Pass NULL to revert
 * to spawn-per-future. The pool's `submit_fn` is consulted for each
 * `(future ...)`. The pool may be shared across multiple states; mino
 * does not enforce single-state ownership. The thread_limit still
 * applies — submission is rejected when the per-state count is full.
 */
void mino_set_thread_pool(mino_state *S, mino_thread_pool *pool);

/*
 * Per-thread factory hooks for the spawn-per-future path. start_fn
 * runs on the worker thread before the body executes; end_fn runs
 * after the body returns (whether normally or via uncaught throw).
 * ctx is opaque to mino. Pass NULL fns to clear. Ignored entirely
 * when a pool is registered (the pool owns thread lifecycle).
 */
typedef void (*mino_thread_lifecycle_fn)(mino_state *S, void *ctx);

void mino_set_thread_factory(mino_state *S,
                             mino_thread_lifecycle_fn start_fn,
                             mino_thread_lifecycle_fn end_fn,
                             void *ctx);

/*
 * Per-worker stack size for the spawn-per-future path. n=0 means
 * platform default. Ignored when a pool is registered.
 */
void mino_set_thread_stack_size(mino_state *S, size_t n);

/* ------------------------------------------------------------------------- */
/* Garbage collector control [MINO_UNSTABLE_GC]                              */
/* ------------------------------------------------------------------------- */

/*
 * UNSTABLE: GC tuning, kind enum, phase constants, and the stats
 * struct stay UNSTABLE through the 0.x alpha series. The collector is
 * still evolving (generational + incremental layout, threshold
 * heuristics) and this section will track those changes. Pin behavior
 * through explicit mino_gc_collect calls at quiescent points; do not
 * rely on tuning parameter ranges or the stats struct layout across
 * releases.
 *
 * Kinds of collection the host can request. Use at quiescent points such
 * as between REPL turns, after bulk import, or before long-idle periods.
 *
 *   MINO_GC_MINOR -- nursery collection only; cheapest, bounded by young set
 *                   size. Safe at any time including mid-major.
 *   MINO_GC_MAJOR -- drain any in-flight incremental major cycle and sweep,
 *                   then start a fresh STW major if no cycle is active.
 *                   Reclaims old-gen dead; leaves young-gen alone.
 *   MINO_GC_FULL  -- minor, then finish or run a fresh STW major.
 *                   Strongest reclamation; highest pause.
 */
typedef enum {
    MINO_GC_MINOR = 1,
    MINO_GC_MAJOR = 2,
    MINO_GC_FULL  = 3
} mino_gc_kind;

void mino_gc_collect(mino_state *S, mino_gc_kind kind);

/*
 * Tunable parameters. Values out of range are rejected; mino_gc_set_param
 * returns 0 on success and -1 on a bad parameter or out-of-range value.
 *
 *   NURSERY_BYTES       young-gen trigger; minor fires when young exceeds.
 *                       Default 1 MiB. Range 64 KiB .. 256 MiB.
 *   MAJOR_GROWTH_TENTHS old-gen growth multiplier in tenths (15 = 1.5x).
 *                       Default 15. Range 11 .. 40.
 *   PROMOTION_AGE       minor-survival count before YOUNG->OLD promotion.
 *                       Default 1. Range 1 .. 8.
 *   INCREMENTAL_BUDGET  headers popped per incremental major slice.
 *                       Default 4096. Range 64 .. 65536.
 *   STEP_ALLOC_BYTES    bytes allocated between automatic slices.
 *                       Default 16 KiB. Range 1 KiB .. 16 MiB.
 */
typedef enum {
    MINO_GC_NURSERY_BYTES       = 1,
    MINO_GC_MAJOR_GROWTH_TENTHS = 2,
    MINO_GC_PROMOTION_AGE       = 3,
    MINO_GC_INCREMENTAL_BUDGET  = 4,
    MINO_GC_STEP_ALLOC_BYTES    = 5
} mino_gc_param;

int mino_gc_set_param(mino_state *S, mino_gc_param p, size_t value);

/* Phase tag returned in mino_gc_stats_out.phase. */
#define MINO_GC_PHASE_IDLE        0
#define MINO_GC_PHASE_MINOR       1
#define MINO_GC_PHASE_MAJOR_MARK  2
#define MINO_GC_PHASE_MAJOR_SWEEP 3

/*
 * Collector statistics. Populated by mino_gc_stats. No allocation is
 * performed; the host owns the out struct. Counters are cumulative
 * since state creation except where noted.
 *
 * Note: bytes_alloc is NOT a monotonic total. It tracks bytes live in
 * the bump path; minor GC decrements it by the bytes it sweeps and
 * major GC resets it to bytes_live. To recover a true allocation
 * total over a window, sum (delta bytes_alloc + delta bytes_freed);
 * bytes_freed IS monotonic. This is the formula the perf-gate
 * allocation tracker uses.
 */
typedef struct {
    size_t collections_minor;
    size_t collections_major;
    size_t bytes_live;         /* young + old */
    size_t bytes_young;
    size_t bytes_old;
    size_t bytes_alloc;        /* current; reset by major, decremented by minor */
    size_t bytes_freed;        /* monotonic: total ever swept */
    size_t total_gc_ns;
    size_t max_gc_ns;
    /* Per-phase timers (cumulative ns since state creation). minor_mark
     * counts time inside the minor's mark phase (precise roots + remset
     * + conservative stack scan + drains); minor_sweep counts time in
     * gc_minor_sweep, which also performs age-based promotion. major_mark
     * sums begin + every incremental step + remark; major_sweep counts
     * gc_major_sweep_phase. root_scan is a sub-timer of the mark phases
     * that measures just precise-root enumeration (gc_mark_roots) across
     * both collectors and overlaps with minor_mark_ns + major_mark_ns
     * rather than adding to them. */
    size_t minor_mark_ns;
    size_t minor_sweep_ns;
    size_t major_mark_ns;
    size_t major_sweep_ns;
    size_t root_scan_ns;
    /* Write-barrier hit counters (cumulative since state creation).
     * barrier_satb_pushes ticks for each old-value snapshot push
     * during MAJOR_MARK (Yuasa half of the hybrid barrier);
     * barrier_dijkstra_pushes ticks for each new-value insertion
     * push (the half that catches edges whose snapshot path was
     * overwritten in the same store). mark_stack_overflows counts
     * silent-drop events when gc_mark_stack_push_raw could not grow
     * the stack -- the collector then leans on conservative stack
     * scan as a backstop, so the count is also a hint that the cycle
     * may have over-paid for scanning. */
    size_t barrier_satb_pushes;
    size_t barrier_dijkstra_pushes;
    size_t mark_stack_overflows;
    /* Generational promotion bookkeeping. bytes_promoted_minor is a
     * cumulative running total of YOUNG -> OLD byte volume across
     * minor cycles. young_age_bucket[i] increments once per minor-
     * survivor in bucket i = clamp(log2(age+1), 0..7); high-bucket
     * counts identify candidates for promotion-age / nursery tuning. */
    size_t   bytes_promoted_minor;
    uint64_t young_age_bucket[8];
    /* Per-GC-tag allocation counter. Indexed 1..10 (GC_T_RAW..
     * GC_T_BC); 0 and the slack tail are reserved. Always-on; ticked
     * once per gc_alloc_typed call. The named indices are exposed via
     * `(gc-stats)`'s `:alloc-by-tag` map for the script-side view. */
    uint64_t alloc_by_tag[16];
    size_t remset_entries;     /* current remembered-set size */
    size_t remset_cap;         /* remembered-set capacity */
    size_t remset_high_water;  /* peak remset size this state */
    size_t mark_stack_cap;     /* mark-stack capacity */
    size_t mark_stack_high_water; /* peak mark-stack depth this state */
    int    phase;              /* MINO_GC_PHASE_* */
} mino_gc_stats_out;

void mino_gc_stats(mino_state *S, mino_gc_stats_out *out);

/*
 * Pause-time distribution accessors. The GC keeps a circular buffer
 * of the last 256 pause durations (one entry per minor / major slice
 * / fully-STW major) and a log2 histogram covering buckets
 * [2^i, 2^(i+1)) ns from i = 0 (1 ns) to i = 23 (>= 8.4 ms).
 *
 * `mino_gc_stats_pauses` populates the four percentile out-pointers
 * from the recent-window ring (any of which may be NULL to skip).
 * Percentiles are computed by sorting a temporary copy of the active
 * ring entries; computed values are saturated at UINT32_MAX ns.
 *
 * `mino_gc_pause_hist` copies the 24-bucket lifetime histogram into
 * the host-owned out array. `out_count` returns the number of valid
 * entries currently in the ring (0..256).
 */
void mino_gc_stats_pauses(mino_state *S,
                          uint64_t *out_p50_ns,
                          uint64_t *out_p95_ns,
                          uint64_t *out_p99_ns,
                          uint64_t *out_max_ns);
void mino_gc_pause_hist(mino_state *S,
                        uint32_t out_buckets[24],
                        unsigned *out_count);

/* CPU and allocation safepoint-sampler dumps (mino_sampler_dump,
 * mino_alloc_sampler_dump) are runtime diagnostic helpers, not part
 * of the embedder surface. They're declared in mino_internal.h and
 * called from runtime/state.c::quiesce and from the mino-side
 * (alloc-site-summary) primitive in prim/reflection.c. */

/* ------------------------------------------------------------------------- */
/* Allocation profiler [MINO_UNSTABLE_ALLOC_PROFILE]                         */
/* ------------------------------------------------------------------------- */

/*
 * UNSTABLE: the allocation profiler is opt-in (compile-time gated on
 * -DMINO_ALLOC_PROFILE=1) and stays UNSTABLE through the 0.x alpha
 * series; its output format is in flux. The functions below are part
 * of the public surface for parity with tooling that needs profile
 * data, but their shape may change in subsequent releases.
 *
 * Reports 1 when the binary was built with -DMINO_ALLOC_PROFILE=1, else 0.
 * The recording paths and dump output are only meaningful in profile builds.
 */
int  mino_alloc_profile_enabled(void);

/*
 * Reset the profile counters to zero. No-op in non-profile builds.
 */
void mino_alloc_profile_reset(mino_state *S);

/*
 * Dump the top `top_n` call sites by allocation count to `out` (stderr
 * if NULL). Pass top_n <= 0 to dump every recorded site. Format is a
 * fixed-width table: rank, count, bytes, tag, file:line. In non-profile
 * builds emits a single "rebuild with MINO_ALLOC_PROFILE=1" line.
 */
void mino_alloc_profile_dump_top(mino_state *S, FILE *out, int top_n);

/* ------------------------------------------------------------------------- */
/* In-process REPL handle                                                    */
/* ------------------------------------------------------------------------- */

/*
 * Return codes for mino_repl_feed.
 */
#define MINO_REPL_OK     0   /* form evaluated; result written to *out      */
#define MINO_REPL_MORE   1   /* line accepted; more input needed            */
#define MINO_REPL_ERROR  2   /* parse or eval error; see mino_last_error()  */

typedef struct mino_repl mino_repl;

/*
 * Create a REPL handle that evaluates forms in `env`. The handle owns
 * an internal line buffer; the host drives it by feeding one line at a
 * time via mino_repl_feed. No thread is required: the host controls
 * the call cadence entirely.
 *
 * `env` must outlive the REPL handle.
 */
mino_repl *mino_repl_new(mino_state *S, mino_env *env);

/*
 * Feed one line of input to the REPL. Returns:
 *   MINO_REPL_OK    - a complete form was read and evaluated. The result
 *                      is written to *out (when out is non-NULL).
 *   MINO_REPL_MORE  - the line was accumulated; more input is needed to
 *                      complete the current form.
 *   MINO_REPL_ERROR - a parse or eval error occurred. The error message
 *                      is available via mino_last_error(). The buffer is
 *                      reset so the next feed starts a fresh form.
 *
 * Multiple complete forms on one line: only the first is evaluated per
 * call. Feed an empty line (or call again with "") to drain remaining
 * buffered forms.
 */
int mino_repl_feed(mino_repl *repl, const char *line, mino_val **out);

/*
 * Free the REPL handle and its internal buffer. Does not free `env`.
 */
void mino_repl_free(mino_repl *repl);

/* ------------------------------------------------------------------------- */
/* Value retention (refs)                                                     */
/* ------------------------------------------------------------------------- */

/*
 * Values returned by constructors and eval are borrowed: they survive until
 * the next GC cycle but are not pinned. A ref roots a value so it survives
 * collection indefinitely. The host must call mino_unref when the value is
 * no longer needed.
 *
 *   mino_ref *r = mino_ref_new(S, val);   // root val
 *   mino_val *v = mino_deref(r);      // get the value
 *   mino_unref(S, r);                   // release the root
 *
 * Refs are owned by the state that created them and freed when the state
 * is freed, but the host should unref explicitly to avoid holding objects
 * longer than necessary.
 */
mino_ref *mino_ref_new(mino_state *S, mino_val *val);
mino_val *mino_deref(const mino_ref *ref);
void        mino_unref(mino_state *S, mino_ref *ref);

/* ------------------------------------------------------------------------- */
/* Value cloning (cross-state transfer)                                      */
/* ------------------------------------------------------------------------- */

/*
 * Deep-copy a value from one state into another. Transferable types:
 * nil, bool, int, float, string, symbol, keyword, cons, vector, map, set.
 *
 * Returns NULL (and sets an error on dst) if the value contains a
 * non-transferable type (fn, macro, prim, handle, atom, lazy-seq).
 * Nested collections are cloned recursively; a non-transferable element
 * anywhere in the tree causes the entire clone to fail.
 */
mino_val *mino_clone(mino_state *dst, mino_state *src, mino_val *val);

#ifdef __cplusplus
}
#endif

#endif /* MINO_H */
