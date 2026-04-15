/*
 * ref_test.c -- exercise mino_ref_t and handle finalizers.
 *
 * Build:
 *   cc -std=c99 -I.. -o ref_test ref_test.c ../mino.o ../re.o -lm
 * Run:
 *   ./ref_test
 */

#include "mino.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int finalized_count = 0;

static void test_finalizer(void *ptr, const char *tag)
{
    (void)tag;
    free(ptr);
    finalized_count++;
}

#define ASSERT(cond, msg) do { \
    if (!(cond)) { fprintf(stderr, "FAIL: %s\n", msg); return 1; } \
} while (0)

int main(void)
{
    mino_state_t *S   = mino_state_new();
    mino_env_t   *env = mino_new(S);

    /* ---- Test 1: ref keeps a value alive across GC ---- */
    {
        mino_val_t *v = mino_eval_string(S,
            "(vec (range 100))", env);
        ASSERT(v != NULL, "eval range failed");

        mino_ref_t *r = mino_ref(S, v);

        /* Force several GC cycles by allocating lots of garbage. */
        mino_eval_string(S,
            "(do (loop (i 0) (if (< i 500) "
            "  (do (vec (range 50)) (recur (+ i 1))))))",
            env);

        /* The ref'd value should still be alive and correct. */
        mino_val_t *got = mino_deref(r);
        ASSERT(got != NULL, "deref returned NULL");
        ASSERT(got == v, "deref returned different pointer");

        mino_unref(S, r);
    }

    /* ---- Test 2: multiple refs, unref order ---- */
    {
        mino_val_t *a = mino_int(S, 42);
        mino_val_t *b = mino_string(S, "hello");
        mino_val_t *c = mino_float(S, 3.14);

        mino_ref_t *ra = mino_ref(S, a);
        mino_ref_t *rb = mino_ref(S, b);
        mino_ref_t *rc = mino_ref(S, c);

        /* Unref middle first. */
        mino_unref(S, rb);

        ASSERT(mino_deref(ra) == a, "ra deref failed after rb unref");
        ASSERT(mino_deref(rc) == c, "rc deref failed after rb unref");

        mino_unref(S, ra);
        mino_unref(S, rc);
    }

    /* ---- Test 3: handle finalizer fires on GC ---- */
    {
        finalized_count = 0;
        char *data = (char *)malloc(64);
        strcpy(data, "test-data");

        /* Create a handle with a finalizer, but do NOT ref it. */
        mino_handle_ex(S, data, "test-handle", test_finalizer);

        /* Force GC by allocating garbage. The handle should be collected
         * and its finalizer called since nothing roots it. */
        mino_eval_string(S,
            "(do (loop (i 0) (if (< i 500) "
            "  (do (vec (range 50)) (recur (+ i 1))))))",
            env);

        ASSERT(finalized_count >= 1,
               "finalizer not called after GC");
    }

    /* ---- Test 4: ref'd handle finalizer does NOT fire ---- */
    {
        finalized_count = 0;
        char *data2 = (char *)malloc(64);
        strcpy(data2, "keep-alive");

        mino_val_t *h = mino_handle_ex(S, data2, "kept", test_finalizer);
        mino_ref_t *rh = mino_ref(S, h);

        /* Force GC. */
        mino_eval_string(S,
            "(do (loop (i 0) (if (< i 500) "
            "  (do (vec (range 50)) (recur (+ i 1))))))",
            env);

        ASSERT(finalized_count == 0,
               "finalizer fired despite ref");
        ASSERT(mino_handle_ptr(mino_deref(rh)) == data2,
               "handle pointer changed");

        mino_unref(S, rh);
        /* data2 will be freed when state is freed (finalizer fires during
         * state teardown sweep, or we accept the leak for this test). */
    }

    mino_env_free(S, env);
    mino_state_free(S);

    printf("all ref/finalizer tests passed\n");
    return 0;
}
