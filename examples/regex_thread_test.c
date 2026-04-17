/*
 * regex_thread_test.c -- thread-safety smoke test for the regex engine.
 *
 * Two threads compile and match independent patterns concurrently.
 * Build: make test-regex-thread
 * Requires pthreads (POSIX).
 */

#include "re.h"
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#define ITERATIONS 10000

static int thread_ok[2] = {1, 1};

static void *thread_digit(void *arg)
{
    int i;
    (void)arg;
    for (i = 0; i < ITERATIONS; i++) {
        int len = 0;
        re_t pat = re_compile("\\d+");
        if (pat == NULL) { thread_ok[0] = 0; break; }
        {
            int idx = re_matchp(pat, "abc123def", &len);
            if (idx < 0 || len != 3) { thread_ok[0] = 0; re_free(pat); break; }
        }
        {
            int idx = re_matchp(pat, "no digits here", &len);
            if (idx >= 0) { thread_ok[0] = 0; re_free(pat); break; }
        }
        re_free(pat);
    }
    return NULL;
}

static void *thread_alpha(void *arg)
{
    int i;
    (void)arg;
    for (i = 0; i < ITERATIONS; i++) {
        int len = 0;
        re_t pat = re_compile("[a-z]+");
        if (pat == NULL) { thread_ok[1] = 0; break; }
        {
            int idx = re_matchp(pat, "123hello456", &len);
            if (idx < 0 || len != 5) { thread_ok[1] = 0; re_free(pat); break; }
        }
        {
            int idx = re_matchp(pat, "123456", &len);
            if (idx >= 0) { thread_ok[1] = 0; re_free(pat); break; }
        }
        re_free(pat);
    }
    return NULL;
}

int main(void)
{
    pthread_t t1, t2;

    printf("Regex thread-safety smoke test (%d iterations per thread)\n", ITERATIONS);

    if (pthread_create(&t1, NULL, thread_digit, NULL) != 0) {
        fprintf(stderr, "failed to create thread 1\n");
        return 1;
    }
    if (pthread_create(&t2, NULL, thread_alpha, NULL) != 0) {
        fprintf(stderr, "failed to create thread 2\n");
        pthread_join(t1, NULL);
        return 1;
    }

    pthread_join(t1, NULL);
    pthread_join(t2, NULL);

    printf("  digit thread: %s\n", thread_ok[0] ? "OK" : "FAIL");
    printf("  alpha thread: %s\n", thread_ok[1] ? "OK" : "FAIL");

    if (thread_ok[0] && thread_ok[1]) {
        printf("PASSED\n");
        return 0;
    }
    printf("FAILED\n");
    return 1;
}
