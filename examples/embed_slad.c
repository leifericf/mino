/*
 * embed_slad.c -- demonstrates save-lisp-and-die image persistence.
 *
 * Full round-trip test: define fns/atoms in state A, save image,
 * free A, create state B, install all, load image into B, verify
 * the fns/atoms survived.
 *
 * Build (from repo root):
 *   ./mino task examples
 */

#include "mino.h"

#include <stdio.h>
#include <string.h>

#define IMG_PATH "embed_slad.img"

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
        fprintf(stderr, "FAIL %s: resolve returned nil (%s)\n",
                label, mino_last_error(S));
        return 1;
    }
    printf("ok %s\n", label);
    return 0;
}

int main(void)
{
    mino_state *A, *B;
    mino_env   *env_a, *env_b;
    int          failures = 0;

    /* --- State A: set up state, define fns and atoms, save image --- */
    A = mino_state_new();
    if (A == NULL) { fprintf(stderr, "state_new A failed\n"); return 1; }
    env_a = mino_env_new(A);
    mino_install_all(A, env_a);

    /* Define a function */
    if (mino_eval_string(A,
        "(defn square [x] (* x x))", env_a) == NULL) {
        fprintf(stderr, "defn square failed: %s\n", mino_last_error(A));
        return 1;
    }

    /* Define an atom */
    if (mino_eval_string(A,
        "(def counter (atom 99))", env_a) == NULL) {
        fprintf(stderr, "def counter failed: %s\n", mino_last_error(A));
        return 1;
    }

    /* Define a var with a collection value */
    if (mino_eval_string(A,
        "(def items [10 20 30])", env_a) == NULL) {
        fprintf(stderr, "def items failed: %s\n", mino_last_error(A));
        return 1;
    }

    /* Verify state A works before saving */
    failures += verify_long(A, env_a, "(square 7)", 49, "A: square 7");
    failures += verify_long(A, env_a, "@counter", 99, "A: counter");

    /* Save the image */
    remove(IMG_PATH);
    if (mino_save_image(A, IMG_PATH) != 0) {
        fprintf(stderr, "save_image failed: %s\n", mino_last_error(A));
        return 1;
    }
    printf("saved image to %s\n", IMG_PATH);

    /* Tear down state A completely */
    mino_env_free(A, env_a);
    mino_state_free(A);

    /* --- State B: fresh state, install all, load image, verify --- */
    B = mino_state_new();
    if (B == NULL) { fprintf(stderr, "state_new B failed\n"); return 1; }
    env_b = mino_env_new(B);
    mino_install_all(B, env_b);

    /* Load the image into state B */
    if (mino_load_image_into(B, IMG_PATH) != 0) {
        fprintf(stderr, "load_image failed: %s\n", mino_last_error(B));
        return 1;
    }
    printf("loaded image from %s\n", IMG_PATH);

    /* Verify the function survived and is callable */
    failures += verify_long(B, env_b, "(square 8)", 64, "B: square 8");

    /* Verify the atom survived with its value */
    failures += verify_long(B, env_b, "@counter", 99, "B: counter");

    /* Verify the collection survived */
    failures += verify_long(B, env_b, "(count items)", 3, "B: items count");
    failures += verify_long(B, env_b, "(first items)", 10, "B: items first");

    /* Verify var is registered (resolvable) */
    failures += verify_resolves(B, env_b, "square", "B: resolve square");

    /* Verify we can transact into a restored atom */
    if (mino_eval_string(B, "(swap! counter inc)", env_b) == NULL) {
        fprintf(stderr, "swap! counter failed: %s\n", mino_last_error(B));
        failures++;
    } else {
        failures += verify_long(B, env_b, "@counter", 100, "B: after swap!");
    }

    remove(IMG_PATH);

    if (failures > 0) {
        fprintf(stderr, "%d failures\n", failures);
        return 1;
    }

    printf("ok\n");
    mino_env_free(B, env_b);
    mino_state_free(B);
    return 0;
}
