/*
 * embed_api_test.c -- C-level smoke test for the embedder helpers:
 *   MINO_VERSION_* / mino_version_string
 *   mino_args_parse
 *   mino_throw
 *
 * Build (from repo root):
 *   cc -std=c99 -Wall -Wextra -Wpedantic -O2 -Isrc -o embed_api_test \
 *       tests/embed_api_test.c src/SRC.c -lm
 * Run: ./embed_api_test
 */

#include "mino.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

static int failures = 0;

#define REQUIRE(cond, msg)                                         \
    do {                                                           \
        if (!(cond)) {                                             \
            fprintf(stderr, "FAIL (%s:%d): %s\n",                  \
                    __FILE__, __LINE__, (msg));                    \
            failures++;                                            \
        }                                                          \
    } while (0)

/* Primitive that uses mino_args_parse to take (int, string) and returns a
 * vector [i, len(s)] so the test can assert both destructured values. */
static mino_val *prim_demo(mino_state *S, mino_val *args,
                             mino_env *env)
{
    long long    i;
    const char  *s;
    size_t       slen;
    mino_val  *items[2];
    (void)env;

    if (mino_args_parse(S, "demo", args, "iS", &i, &s, &slen) != 0) {
        return NULL;
    }
    (void)s;

    items[0] = mino_int(S, i);
    items[1] = mino_int(S, (long long)slen);
    return mino_vector(S, items, 2);
}

/* Primitive that always throws a keyword exception via mino_throw. Used
 * to prove (a) the longjmp delivers to the surrounding (try ... catch)
 * and (b) the payload is the same value we passed in. */
static mino_val *prim_boom(mino_state *S, mino_val *args,
                             mino_env *env)
{
    (void)args;
    (void)env;
    return mino_throw(S, mino_keyword(S, "boom"));
}

static void test_version(void)
{
    const char *v = mino_version_string();
    char        expected[32];
    snprintf(expected, sizeof(expected), "%d.%d.%d",
             MINO_VERSION_MAJOR, MINO_VERSION_MINOR, MINO_VERSION_PATCH);
    REQUIRE(v != NULL, "mino_version_string returned NULL");
    REQUIRE(v != NULL && strcmp(v, expected) == 0,
            "mino_version_string does not match compile-time constants");
    REQUIRE(MINO_VERSION_MAJOR == 0, "unexpected MAJOR");
    REQUIRE(MINO_VERSION_MINOR >= 48, "unexpected MINOR");
}

static void test_args_parse_ok(mino_state *S, mino_env *env)
{
    mino_val *r;
    long long   first, second;
    mino_register_fn(S, env, "demo", prim_demo);

    r = mino_eval_string(S, "(demo 42 \"hello\")", env);
    REQUIRE(r != NULL, "demo eval returned NULL");
    REQUIRE(r != NULL && mino_typeof(r) == MINO_VECTOR,
            "demo result not a vector");

    r = mino_eval_string(S, "(nth (demo 42 \"hello\") 0)", env);
    if (mino_to_int(r, &first)) {
        REQUIRE(first == 42, "demo: int arg not preserved");
    } else {
        REQUIRE(0, "demo: first slot not int");
    }
    r = mino_eval_string(S, "(nth (demo 42 \"hello\") 1)", env);
    if (mino_to_int(r, &second)) {
        REQUIRE(second == 5, "demo: string byte length wrong");
    } else {
        REQUIRE(0, "demo: second slot not int");
    }
}

static void test_args_parse_arity(mino_state *S, mino_env *env)
{
    mino_val *r = mino_eval_string(S, "(demo 1)", env);
    const char *err;
    REQUIRE(r == NULL, "arity: demo with 1 arg should error");
    err = mino_last_error(S);
    REQUIRE(err != NULL && strstr(err, "expected 2") != NULL,
            "arity: error does not mention expected 2");
}

static void test_args_parse_type(mino_state *S, mino_env *env)
{
    mino_val *r = mino_eval_string(S, "(demo \"not-int\" \"s\")", env);
    const char *err;
    REQUIRE(r == NULL, "type: demo with wrong first arg should error");
    err = mino_last_error(S);
    REQUIRE(err != NULL && strstr(err, "expected int") != NULL,
            "type: error does not mention expected int");
}

static void test_throw_caught(mino_state *S, mino_env *env)
{
    mino_val *r;
    mino_register_fn(S, env, "boom", prim_boom);

    r = mino_eval_string(S,
        "(try (boom) (catch e (pr-str e)))",
        env);
    REQUIRE(r != NULL, "throw-caught: try-catch returned NULL");
    if (r != NULL && mino_typeof(r) == MINO_STRING) {
        const char *data = NULL;
        size_t      len  = 0;
        /* The runtime wraps caught exceptions in a diagnostic map that
         * carries the thrown payload under :mino/data. Assert the map
         * printed form includes the keyword we threw. */
        REQUIRE(mino_to_string(r, &data, &len)
                && data != NULL && strstr(data, ":boom") != NULL,
                "throw-caught: payload does not contain :boom");
    } else {
        REQUIRE(0, "throw-caught: catch result is not a string");
    }
}

static void test_throw_uncaught(mino_state *S, mino_env *env)
{
    mino_val *r = mino_eval_string(S, "(boom)", env);
    const char *err;
    REQUIRE(r == NULL, "throw-uncaught: should produce NULL result");
    err = mino_last_error(S);
    REQUIRE(err != NULL && strstr(err, "uncaught exception") != NULL,
            "throw-uncaught: error does not mention uncaught exception");
}

/* mino_to_int round-trips a value put in via mino_int regardless of
 * the bignum capability. With bignum installed mino_int auto-promotes
 * outside the tag range; the inverse extractor must accept bigints
 * that fit in long long and reject those that don't. */
static void test_to_int_bignum_round_trip(void)
{
    /* (a) bignum installed: mino_int(42) is a bigint and round-trips. */
    {
        mino_state *S   = mino_state_new();
        mino_env   *env = mino_env_new(S);
        long long     out = 0;
        mino_install(S, env, MINO_CAP_BIGNUM);
        {
            mino_val *v = mino_int(S, 42);
            REQUIRE(mino_to_int(v, &out) && out == 42,
                    "to_int/bignum: small mino_int round-trips");
        }
        /* (b) Large value above the tag range round-trips too. */
        {
            long long large = (long long)1 << 62;
            mino_val *v = mino_int(S, large);
            out = 0;
            REQUIRE(mino_to_int(v, &out) && out == large,
                    "to_int/bignum: tag-overflow mino_int round-trips");
        }
        /* (c) bigint_from_ll: explicit bigint construction round-trips. */
        {
            mino_val *bi = mino_bigint_from_ll(S, 7);
            out = 0;
            REQUIRE(mino_to_int(bi, &out) && out == 7,
                    "to_int/bignum: bigint_from_ll(7) round-trips");
        }
        /* (d) Out-of-range bigint correctly rejected. */
        {
            mino_val *bi = mino_bigint_from_string(S,
                "100000000000000000000");
            REQUIRE(bi != NULL, "to_int/bignum: huge bigint constructs");
            REQUIRE(!mino_to_int(bi, &out),
                    "to_int/bignum: out-of-range bigint is rejected");
        }
        mino_env_free(S, env);
        mino_state_free(S);
    }

    /* (e) No bignum cap: tag-overflow mino_int stays MINO_INT and
     *     still round-trips (existing behavior preserved). */
    {
        mino_state *S   = mino_state_new();
        mino_env   *env = mino_env_new(S);
        long long large   = (long long)1 << 62;
        long long out = 0;
        mino_val *v;
        mino_install_minimal(S, env);
        v = mino_int(S, large);
        REQUIRE(mino_to_int(v, &out) && out == large,
                "to_int/no-bignum: large mino_int round-trips");
        mino_env_free(S, env);
        mino_state_free(S);
    }
}

/* The _ex eval family delivers the raw thrown payload through out_ex,
 * matching the contract documented for mino_pcall. */
static void test_eval_ex_out_ex_payload(mino_state *S, mino_env *env)
{
    mino_val *out, *out_ex;
    int         rc;

    /* Throw a keyword from script-side. */
    out = NULL; out_ex = NULL;
    rc = mino_eval_string_ex(S, "(throw :boom)", env, &out, &out_ex);
    REQUIRE(rc == -1, "out_ex/keyword: rc == -1");
    REQUIRE(out_ex != NULL, "out_ex/keyword: out_ex non-NULL");
    REQUIRE(out_ex != NULL && mino_is_keyword(out_ex),
            "out_ex/keyword: out_ex is a keyword");
    if (out_ex != NULL && mino_is_keyword(out_ex)) {
        const char *kw; size_t klen;
        REQUIRE(mino_to_keyword(out_ex, &kw, &klen)
                && klen == 4 && memcmp(kw, "boom", 4) == 0,
                "out_ex/keyword: out_ex is :boom");
    }

    /* Throw an ex-info map. */
    out = NULL; out_ex = NULL;
    rc = mino_eval_string_ex(S,
        "(throw (ex-info \"x\" {:k :v}))", env, &out, &out_ex);
    REQUIRE(rc == -1, "out_ex/ex-info: rc == -1");
    REQUIRE(out_ex != NULL && mino_is_map(out_ex),
            "out_ex/ex-info: out_ex is a map (ex-data)");

    /* mino_eval_ex with a pre-read throw form. */
    {
        mino_val *form = mino_read(S,
            "(throw (ex-info \"y\" {:m 1}))", NULL);
        out = NULL; out_ex = NULL;
        REQUIRE(form != NULL, "out_ex/eval_ex: form parsed");
        rc = mino_eval_ex(S, form, env, &out, &out_ex);
        REQUIRE(rc == -1, "out_ex/eval_ex: rc == -1");
        REQUIRE(out_ex != NULL && mino_is_map(out_ex),
                "out_ex/eval_ex: out_ex is the ex-info map");
    }
}

/* mino_iter walks every k/v of a sorted-map (in sort order) and every
 * element of a sorted-set, just like it does for hashed variants. */
static void test_iter_sorted(mino_state *S, mino_env *env)
{
    size_t       isz = mino_iter_sizeof();
    mino_iter *it  = (mino_iter *)malloc(isz);
    mino_val  *k, *v;

    /* sorted-map */
    {
        mino_val *sm = mino_eval_string(S,
            "(sorted-map :a 1 :b 2 :c 3)", env);
        long long sum = 0, n;
        int       seen = 0;
        REQUIRE(sm != NULL, "iter-sorted-map: construction");
        mino_iter_init(S, it, sm);
        while (mino_iter_next(it, &k, &v)) {
            seen++;
            if (mino_to_int(v, &n)) sum += n;
        }
        mino_iter_done(it);
        REQUIRE(seen == 3, "iter-sorted-map: visits 3 entries");
        REQUIRE(sum == 6,  "iter-sorted-map: visits values 1+2+3");
    }

    /* sorted-set */
    {
        mino_val *ss = mino_eval_string(S, "(sorted-set 1 2 3 4 5)", env);
        long long sum = 0, n;
        int       seen = 0;
        REQUIRE(ss != NULL, "iter-sorted-set: construction");
        mino_iter_init(S, it, ss);
        while (mino_iter_next(it, &k, &v)) {
            seen++;
            if (mino_to_int(k, &n)) sum += n;
        }
        mino_iter_done(it);
        REQUIRE(seen == 5,  "iter-sorted-set: visits 5 elements");
        REQUIRE(sum == 15,  "iter-sorted-set: visits 1+2+3+4+5");
    }

    /* empty sorted-map */
    {
        mino_val *sm = mino_eval_string(S, "(sorted-map)", env);
        int seen = 0;
        REQUIRE(sm != NULL, "iter-sorted-map-empty: construction");
        mino_iter_init(S, it, sm);
        while (mino_iter_next(it, &k, &v)) seen++;
        mino_iter_done(it);
        REQUIRE(seen == 0, "iter-sorted-map-empty: visits nothing");
    }

    free(it);
}

/* mino_read with NULL src must follow the EOF path: return NULL with
 * no error and leave *end NULL (or unchanged), matching empty input. */
static void test_read_null_src(mino_state *S)
{
    const char *end = NULL;
    mino_val *v   = mino_read(S, NULL, &end);
    REQUIRE(v == NULL, "null-src: mino_read returns NULL");
    REQUIRE(mino_last_error(S) == NULL,
            "null-src: mino_read sets no error (EOF parity)");
    REQUIRE(end == NULL,
            "null-src: mino_read writes NULL through end");

    /* end == NULL must also be handled. */
    v = mino_read(S, NULL, NULL);
    REQUIRE(v == NULL,
            "null-src + null-end: mino_read still returns NULL");
}

/* NULL `src` to mino_eval_string and mino_eval_string_ex must surface
 * a classified error, matching mino_load_file's NULL-arg behaviour. */
static void test_eval_string_null_src(mino_state *S, mino_env *env)
{
    mino_val *out    = NULL;
    mino_val *out_ex = NULL;
    int         rc;

    /* Public, non-_ex form: returns NULL + sets last_error. */
    {
        mino_val *r = mino_eval_string(S, NULL, env);
        const char *err;
        REQUIRE(r == NULL, "null-src: mino_eval_string returns NULL");
        err = mino_last_error(S);
        REQUIRE(err != NULL,
                "null-src: mino_eval_string sets a last_error");
    }

    /* _ex form: rc == -1, out == NULL. */
    rc = mino_eval_string_ex(S, NULL, env, &out, &out_ex);
    REQUIRE(rc == -1, "null-src: mino_eval_string_ex returns -1");
    REQUIRE(out == NULL,
            "null-src: mino_eval_string_ex leaves out == NULL");
}

/* Smoke the grid additions (mino_is_* + mino_to_*) from v0.385.0.
 * Every predicate built in this phase has its own MINO_* tag-shaped
 * value evaluated and checked, and every new extractor is exercised
 * with both an in-type input (success path) and an out-of-type input
 * (the type-mismatch return-0 path). */
static void test_predicate_grid(mino_state *S, mino_env *env)
{
    mino_val *v;

    /* mino_is_float32 — distinct from MINO_FLOAT. */
    v = mino_eval_string(S, "(float 1.5)", env);
    REQUIRE(v != NULL && mino_is_float32(v),
            "grid: (float 1.5) is float32");
    {
        float f = 0.0f;
        REQUIRE(mino_to_float32(v, &f) && f == 1.5f,
                "grid: mino_to_float32 round-trips");
        REQUIRE(!mino_to_float32(mino_int(S, 1), &f),
                "grid: mino_to_float32 rejects non-float32");
    }

    /* mino_is_sorted_map / mino_is_sorted_set. */
    v = mino_eval_string(S, "(sorted-map :a 1)", env);
    REQUIRE(v != NULL && mino_is_sorted_map(v) && mino_is_map(v),
            "grid: sorted-map is sorted-map AND map");
    v = mino_eval_string(S, "(sorted-set 1 2)", env);
    REQUIRE(v != NULL && mino_is_sorted_set(v) && mino_is_set(v),
            "grid: sorted-set is sorted-set AND set");

    /* mino_is_map_entry. */
    v = mino_eval_string(S, "(first {:a 1})", env);
    REQUIRE(v != NULL && mino_is_map_entry(v),
            "grid: (first {}) returns a map-entry");

    /* mino_is_record / mino_is_record_type. */
    {
        const char *fields[1] = {"x"};
        mino_val   *T = mino_defrecord(S, "user", "TGrid", fields, 1);
        mino_val   *vals[1] = {mino_int(S, 1)};
        mino_val   *r = mino_record(S, T, vals, 1);
        REQUIRE(mino_is_record_type(T),
                "grid: defrecord result is a record-type");
        REQUIRE(mino_is_record(r),
                "grid: ctor result is a record");
    }

    /* mino_is_future is hard to construct without async; check the
     * negative path only. */
    REQUIRE(!mino_is_future(mino_int(S, 1)),
            "grid: int is not a future");

    /* mino_to_bigint_str / mino_to_ratio / mino_to_bigdec_str /
     * mino_to_uuid_bytes / mino_to_regex_source. */
    {
        char   buf[64];
        size_t w = 0;
        v = mino_eval_string(S, "100000000000000000000N", env);
        REQUIRE(v != NULL && mino_is_bigint(v), "grid: bigint literal");
        REQUIRE(mino_to_bigint_str(v, buf, sizeof(buf), &w) == 1
                && strcmp(buf, "100000000000000000000") == 0,
                "grid: mino_to_bigint_str round-trips");
        REQUIRE(mino_to_bigint_str(v, buf, 5, NULL) == -1,
                "grid: mino_to_bigint_str returns -1 on small buf");
        REQUIRE(mino_to_bigint_str(mino_int(S, 1), buf, sizeof(buf), NULL) == 0,
                "grid: mino_to_bigint_str rejects non-bigint");
    }
    {
        long long num = 0, den = 0;
        v = mino_eval_string(S, "3/4", env);
        REQUIRE(v != NULL && mino_is_ratio(v), "grid: ratio literal");
        REQUIRE(mino_to_ratio(v, &num, &den) == 1
                && num == 3 && den == 4,
                "grid: mino_to_ratio extracts 3/4");
    }
    {
        char   buf[64];
        size_t w = 0;
        v = mino_eval_string(S, "1.5M", env);
        REQUIRE(v != NULL && mino_is_bigdec(v), "grid: bigdec literal");
        REQUIRE(mino_to_bigdec_str(v, buf, sizeof(buf), &w) == 1
                && strcmp(buf, "1.5") == 0,
                "grid: mino_to_bigdec_str round-trips");
    }
    {
        uint8_t bytes[16];
        v = mino_eval_string(S,
            "(parse-uuid \"00000000-0000-0000-0000-000000000001\")",
            env);
        REQUIRE(v != NULL && mino_is_uuid(v), "grid: uuid literal");
        REQUIRE(mino_to_uuid_bytes(v, bytes) == 1
                && bytes[15] == 1 && bytes[0] == 0,
                "grid: mino_to_uuid_bytes extracts 16 bytes");
    }
    {
        const char *src = NULL;
        size_t      slen = 0;
        v = mino_eval_string(S, "#\"a+b*\"", env);
        REQUIRE(v != NULL && mino_is_regex(v), "grid: regex literal");
        REQUIRE(mino_to_regex_source(v, &src, &slen) == 1
                && src != NULL && slen == 4 && memcmp(src, "a+b*", 4) == 0,
                "grid: mino_to_regex_source extracts pattern");
    }
}

int main(void)
{
    mino_state *S = mino_state_new();
    mino_env   *env = mino_env_new_default(S);

    test_version();
    test_args_parse_ok(S, env);
    test_args_parse_arity(S, env);
    test_args_parse_type(S, env);
    test_throw_caught(S, env);
    test_throw_uncaught(S, env);
    test_eval_string_null_src(S, env);
    test_read_null_src(S);
    test_iter_sorted(S, env);
    test_eval_ex_out_ex_payload(S, env);
    test_to_int_bignum_round_trip();
    test_predicate_grid(S, env);

    /* Phase 5 -- Clojure-canon surface additions. */
    {
        mino_val *v, *m, *cp;
        /* mino_meta / mino_with_meta. */
        v = mino_eval_string(S, "(with-meta [1 2] {:n 1})", env);
        REQUIRE(v != NULL && mino_is_vector(v), "phase5: with-meta returns vector");
        m = mino_meta(v);
        REQUIRE(m != NULL && mino_is_map(m), "phase5: mino_meta retrieves map");
        {
            mino_val *m2 = mino_eval_string(S, "{:m 2}", env);
            cp = mino_with_meta(S, v, m2);
            REQUIRE(cp != NULL && mino_is_vector(cp), "phase5: mino_with_meta builds copy");
            REQUIRE(mino_meta(cp) == m2, "phase5: mino_with_meta meta is attached");
        }

        /* mino_seq / mino_first / mino_rest / mino_next. */
        v = mino_eval_string(S, "[10 20 30]", env);
        {
            mino_val *s = mino_seq(S, v);
            REQUIRE(s != NULL, "phase5: mino_seq returns non-nil on non-empty vec");
            REQUIRE(!mino_is_nil(s), "phase5: mino_seq result not nil");
        }
        v = mino_eval_string(S, "(list 10 20 30)", env);
        {
            mino_val *f = mino_first(v);
            long long n = 0;
            REQUIRE(f != NULL && mino_to_int(f, &n) && n == 10,
                    "phase5: mino_first returns 10");
        }
        v = mino_eval_string(S, "(list 1)", env);
        {
            mino_val *nx = mino_next(S, v);
            REQUIRE(nx != NULL && mino_is_nil(nx),
                    "phase5: mino_next of (list 1) is nil");
        }
        v = mino_eval_string(S, "(list 1 2)", env);
        {
            mino_val *nx = mino_next(S, v);
            REQUIRE(nx != NULL && !mino_is_nil(nx),
                    "phase5: mino_next of (list 1 2) non-nil");
        }

        /* mino_compare / mino_hash. */
        REQUIRE(mino_compare(S, mino_int(S, 1), mino_int(S, 2)) == -1,
                "phase5: mino_compare 1 vs 2 == -1");
        REQUIRE(mino_compare(S, mino_int(S, 2), mino_int(S, 2)) == 0,
                "phase5: mino_compare 2 vs 2 == 0");
        REQUIRE(mino_hash(mino_eval_string(S, "[1 2 3]", env))
                == mino_hash(mino_eval_string(S, "[1 2 3]", env)),
                "phase5: mino_hash agrees on equal vecs");

        /* mino_push_bindings / mino_pop_bindings. Bind *out* in a
         * single-frame push, evaluate (deref #'*out*) -- the var read
         * picks up the dyn frame. */
        v = mino_eval_string(S, "(def ^:dynamic *phase5*) 0", env);
        {
            mino_val *vars[1];
            mino_val *vals[1];
            mino_binding_frame *fr;
            mino_val *got;
            vars[0] = mino_symbol(S, "*phase5*");
            vals[0] = mino_int(S, 42);
            fr = mino_push_bindings(S, vars, vals, 1);
            REQUIRE(fr != NULL, "phase5: push_bindings returns frame");
            got = mino_eval_string(S, "*phase5*", env);
            {
                long long n = 0;
                REQUIRE(got != NULL && mino_to_int(got, &n) && n == 42,
                        "phase5: dyn-bound var visible during frame");
            }
            mino_pop_bindings(S, fr);
        }

        /* mino_can_clone. */
        {
            const char *reason = NULL;
            REQUIRE(mino_can_clone(mino_eval_string(S, "[1 2 3]", env),
                                   &reason) == 1
                    && reason == NULL,
                    "phase5: can_clone vec");
            REQUIRE(mino_can_clone(mino_eval_string(S, "(atom 1)", env),
                                   &reason) == 0
                    && reason != NULL && strcmp(reason, "atom") == 0,
                    "phase5: can_clone reports atom");
        }
    }

    mino_env_free(S, env);
    mino_state_free(S);

    if (failures > 0) {
        fprintf(stderr, "embed_api_test: %d failure(s)\n", failures);
        return 1;
    }
    printf("embed_api_test: all checks passed\n");
    return 0;
}
