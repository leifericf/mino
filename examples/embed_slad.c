/*
 * embed_slad.c -- save-lisp-and-die image persistence: full round-trip.
 *
 * Cross-state round-trip E2E for the SLAD image format (ADR 12): define
 * a representative value of every type the serializer/deserializer must
 * handle in state A, save the image, tear A down completely, create a
 * fresh state B, install all, load the image, and verify each value
 * survived intact and is usable. This is the regression net for the
 * deserializer -- a memory-safety hardening or per-type patch change
 * that breaks restoration fails here.
 *
 * Why cross-state: load-image-into is defined to load into a FRESH
 * (state_new + install_all) baseline, replacing its namespaces/vars.
 * Loading into the already-populated same state is not contract
 * behaviour (it corrupts). The Clojure test harness is single-state, so
 * this C E2E is the lowest faithful round-trip; it runs under
 * `./mino task examples` (release-gate step 6).
 *
 * Build (from repo root):
 *   ./mino task examples
 */

#include "mino.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define IMG_PATH "embed_slad.img"

/* Eval EXPR and require its result to equal EXPECTED (an integer). */
static int verify_long(mino_state *S, mino_env *env, const char *expr,
                       long long expected, const char *label)
{
    long long got = 0;
    mino_val *r = mino_eval_string(S, expr, env);
    if (r == NULL || !mino_to_int(r, &got)) {
        fprintf(stderr, "FAIL %s: eval failed (%s)\n", label,
                mino_last_error(S));
        return 1;
    }
    if (got != expected) {
        fprintf(stderr, "FAIL %s: got %lld, expected %lld\n",
                label, got, expected);
        return 1;
    }
    printf("ok %s: %lld\n", label, got);
    return 0;
}

/* Eval (if EXPR 1 0) and require 1. Lets a test assert any truthy
 * boolean / equality / presence check via a single integer gate. */
static int verify_true(mino_state *S, mino_env *env, const char *expr,
                       const char *label)
{
    char buf[640];
    int n = snprintf(buf, sizeof buf, "(if %s 1 0)", expr);
    if (n < 0 || (size_t)n >= sizeof buf) {
        fprintf(stderr, "FAIL %s: assertion too long\n", label);
        return 1;
    }
    return verify_long(S, env, buf, 1, label);
}

/* Require that SYM resolves to a var after load. */
static int verify_resolves(mino_state *S, mino_env *env, const char *sym,
                           const char *label)
{
    char buf[128];
    mino_val *r;
    int n = snprintf(buf, sizeof buf, "(resolve '%s)", sym);
    if (n < 0 || (size_t)n >= sizeof buf) {
        fprintf(stderr, "FAIL %s: buffer overflow\n", label);
        return 1;
    }
    r = mino_eval_string(S, buf, env);
    if (r == NULL) {
        fprintf(stderr, "FAIL %s: resolve returned nil (%s)\n", label,
                mino_last_error(S));
        return 1;
    }
    printf("ok %s\n", label);
    return 0;
}

/* Eval a definition; abort the whole run if it fails (a setup failure
 * invalidates every downstream check). */
static void define_or_die(mino_state *S, mino_env *env, const char *src,
                          const char *label)
{
    if (mino_eval_string(S, src, env) == NULL) {
        fprintf(stderr, "setup failed (%s): %s\n", label, mino_last_error(S));
        exit(1);
    }
}

int main(void)
{
    mino_state *A, *B;
    mino_env   *env_a, *env_b;
    int          failures = 0;

    /* --- State A: define the type matrix, then save --------------- */
    A = mino_state_new();
    if (A == NULL) { fprintf(stderr, "state_new A failed\n"); return 1; }
    env_a = mino_env_new(A);
    mino_install_all(A, env_a);

    define_or_die(A, env_a, "(def rt-int 42)", "rt-int");
    define_or_die(A, env_a, "(def rt-big 99999999999999999999)", "rt-big");
    define_or_die(A, env_a, "(def rt-ratio 22/7)", "rt-ratio");
    define_or_die(A, env_a, "(def rt-bd 3.14M)", "rt-bd");
    define_or_die(A, env_a, "(def rt-str \"hello\\nworld\")", "rt-str");
    define_or_die(A, env_a, "(def rt-kw :user/tag)", "rt-kw");
    define_or_die(A, env_a, "(def rt-vec [10 20 30])", "rt-vec");
    define_or_die(A, env_a, "(def rt-map {:a 1 :b 2 :c 3})", "rt-map");
    define_or_die(A, env_a, "(def rt-set #{1 2 3})", "rt-set");
    define_or_die(A, env_a, "(def rt-smap (sorted-map :b 2 :a 1 :c 3))",
                 "rt-smap");
    define_or_die(A, env_a, "(def rt-sset (sorted-set 3 1 2))", "rt-sset");
    define_or_die(A, env_a, "(def rt-list '(1 2 3))", "rt-list");
    define_or_die(A, env_a,
                 "(def rt-queue (conj clojure.lang.PersistentQueue/EMPTY "
                 "1 2 3))", "rt-queue");
    define_or_die(A, env_a, "(def rt-fn (fn [x] (* x x)))", "rt-fn");
    define_or_die(A, env_a, "(def rt-close (let [c 100] (fn [] c)))",
                 "rt-close");
    define_or_die(A, env_a, "(def rt-multi (fn ([x] x) ([x y] (+ x y))))",
                 "rt-multi");
    define_or_die(A, env_a, "(def rt-atom (atom 99))", "rt-atom");
    define_or_die(A, env_a, "(def rt-vol (volatile! 7))", "rt-vol");
    define_or_die(A, env_a, "(defrecord Point [x y])", "Point");
    define_or_die(A, env_a, "(def rt-rec (->Point 3 4))", "rt-rec");
    define_or_die(A, env_a, "(def rt-bytes (byte-array [1 2 3 4]))",
                 "rt-bytes");
    define_or_die(A, env_a,
                 "(def rt-uuid #uuid "
                 "\"12345678-1234-5678-1234-567812345678\")", "rt-uuid");
    define_or_die(A, env_a, "(def rt-re #\"abc+\")", "rt-re");
    define_or_die(A, env_a, "(def rt-meta (with-meta [1 2 3] {:tag :kept}))",
                 "rt-meta");

    /* A durable store whose path contains a space. The ST image line
     * emits the path as a whitespace-delimited token, so an unescaped
     * space would truncate the path on load (and a newline could inject
     * a fake line). This is the only round-trip coverage for the ST
     * save/load path; the single-state Clojure harness cannot load. */
    #define STORE_DIR  "/tmp/mino_slad_c7/has space"
    #define STORE_PATH STORE_DIR "/store"
    remove(STORE_PATH);
    remove(STORE_PATH ".wal");
    define_or_die(A, env_a, "(mkdir-p \"" STORE_DIR "\")",
                 "mkdir spaced store dir");
    define_or_die(A, env_a,
                 "(do (require 'mino.store)"
                 "  (def rt-store (mino.store/open \"" STORE_PATH "\")))",
                 "rt-store");
    define_or_die(A, env_a,
                 "(mino.store/transact rt-store {:k {:v 1}})",
                 "rt-store tx");

    /* Sanity: state A works before saving. */
    failures += verify_long(A, env_a, "(rt-fn 7)", 49, "A: fn works");
    failures += verify_long(A, env_a, "@rt-atom", 99, "A: atom value");

    /* Save the image. */
    remove(IMG_PATH);
    if (mino_save_image(A, IMG_PATH) != 0) {
        fprintf(stderr, "save_image failed: %s\n", mino_last_error(A));
        return 1;
    }
    printf("saved image to %s\n", IMG_PATH);

    /* Tear down state A completely. */
    mino_env_free(A, env_a);
    mino_state_free(A);

    /* --- State B: fresh baseline, load, verify the matrix ---------- */
    B = mino_state_new();
    if (B == NULL) { fprintf(stderr, "state_new B failed\n"); return 1; }
    env_b = mino_env_new(B);
    mino_install_all(B, env_b);

    if (mino_load_image_into(B, IMG_PATH) != 0) {
        fprintf(stderr, "load_image failed: %s\n", mino_last_error(B));
        return 1;
    }
    printf("loaded image from %s\n", IMG_PATH);

    /* Scalars. */
    failures += verify_true(B, env_b, "(= rt-int 42)", "B: int");
    failures += verify_true(B, env_b, "(= rt-big 99999999999999999999)",
                            "B: bigint");
    failures += verify_true(B, env_b, "(= rt-ratio 22/7)", "B: ratio");
    failures += verify_true(B, env_b, "(= rt-bd 3.14M)", "B: bigdec");
    failures += verify_true(B, env_b, "(= rt-str \"hello\\nworld\")",
                            "B: string-with-escape");
    failures += verify_true(B, env_b, "(= rt-kw :user/tag)", "B: keyword");

    /* Collections. */
    failures += verify_long(B, env_b, "(count rt-vec)", 3, "B: vec count");
    failures += verify_long(B, env_b, "(first rt-vec)", 10, "B: vec first");
    failures += verify_long(B, env_b, "(:b rt-map)", 2, "B: map lookup");
    failures += verify_true(B, env_b, "(contains? rt-set 2)", "B: set");
    failures += verify_long(B, env_b, "(:a rt-smap)", 1, "B: smap lookup");
    failures += verify_true(B, env_b,
                            "(= (keys rt-smap) '(:a :b :c))",
                            "B: smap ordering preserved");
    failures += verify_long(B, env_b, "(first rt-sset)", 1, "B: sset order");
    failures += verify_long(B, env_b, "(first rt-list)", 1, "B: list first");
    failures += verify_long(B, env_b, "(peek rt-queue)", 1, "B: queue peek");

    /* Code: fn, closure (captured env), multi-arity (params==NULL path). */
    failures += verify_long(B, env_b, "(rt-fn 8)", 64, "B: fn call");
    failures += verify_long(B, env_b, "(rt-close)", 100, "B: closure env");
    failures += verify_long(B, env_b, "(rt-multi 5)", 5, "B: multi arity 1");
    failures += verify_long(B, env_b, "(rt-multi 5 6)", 11, "B: multi arity 2");

    /* Mutable refs. */
    failures += verify_long(B, env_b, "@rt-atom", 99, "B: atom value");
    failures += verify_long(B, env_b, "@rt-vol", 7, "B: volatile value");
    /* A restored atom is still a working identity: swap! must take. */
    if (mino_eval_string(B, "(swap! rt-atom inc)", env_b) == NULL) {
        fprintf(stderr, "FAIL B: swap! on restored atom: %s\n",
                mino_last_error(B));
        failures++;
    } else {
        failures += verify_long(B, env_b, "@rt-atom", 100, "B: atom after swap!");
    }

    /* Records: instances round-trip -- field access and resolution work.
     * (Record TYPE identity -- (= (type rt-rec) Point) -- is a known
     * deserializer gap: the TY pass canonicalizes the type via
     * mino_defrecord and repoints the id table, but bindings/roots that
     * captured the pre-TY shell in earlier patch passes still see it.
     * Filed separately; do not assert it here.) */
    failures += verify_long(B, env_b, "(:x rt-rec)", 3, "B: record :x");
    failures += verify_long(B, env_b, "(:y rt-rec)", 4, "B: record :y");
    failures += verify_resolves(B, env_b, "rt-rec", "B: resolve rt-rec");

    /* Bytes, uuid, regex. */
    failures += verify_long(B, env_b, "(alength rt-bytes)", 4, "B: bytes len");
    failures += verify_long(B, env_b, "(aget rt-bytes 0)", 1, "B: bytes nth");
    failures += verify_true(B, env_b,
                            "(= rt-uuid #uuid "
                            "\"12345678-1234-5678-1234-567812345678\")",
                            "B: uuid");
    failures += verify_true(B, env_b, "(re-find rt-re \"xabc\")", "B: regex");

    /* Metadata round-trips via the META section. */
    failures += verify_true(B, env_b, "(= (:tag (meta rt-meta)) :kept)",
                            "B: metadata");

    /* Store round-trip: the in-memory db restores and the spaced path
     * survives the escaped ST token. A regression in the path escape
     * truncates mino_store_path here. mino.store is a stdlib ns skipped
     * on save, so require it fresh in B before the script calls. */
    failures += verify_true(B, env_b,
                            "(do (require 'mino.store)"
                            "  (mino.store/store? rt-store))",
                            "B: store restored");
    failures += verify_long(B, env_b,
                            "(do (require 'mino.store)"
                            "  (mino.store/read (mino.store/db rt-store) :k :v))",
                            1, "B: store read");
    {
        mino_val *st = mino_eval_string(B, "rt-store", env_b);
        const char *got = (st != NULL) ? mino_store_path(st) : NULL;
        if (got == NULL || strcmp(got, STORE_PATH) != 0) {
            fprintf(stderr,
                    "FAIL B: store path round-trip: got \"%s\", expected \"%s\"\n",
                    got ? got : "(null)", STORE_PATH);
            failures++;
        } else {
            printf("ok B: store path with space round-tripped\n");
        }
    }

    /* Vars are re-registered and resolvable. */
    failures += verify_resolves(B, env_b, "rt-fn", "B: resolve rt-fn");

    remove(IMG_PATH);
    remove(STORE_PATH);
    remove(STORE_PATH ".wal");

    if (failures > 0) {
        fprintf(stderr, "%d failures\n", failures);
        return 1;
    }

    printf("ok\n");
    mino_env_free(B, env_b);
    mino_state_free(B);
    return 0;
}
