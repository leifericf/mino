/*
 * embed_proc.c -- host-side regression for proc `run` FD inheritance.
 *
 * proc `run` forks and execs a child capturing stdout/stderr. Before the
 * closefds fix the child inherited EVERY descriptor the host had open
 * (db handles, sockets, pipes from a surrounding capture, anything an
 * embedding application keeps open across a run). That is both an
 * info/capability leak into the subprocess and a correctness hazard.
 *
 * This harness is the only place that leak is observable: it plants a
 * known descriptor (fd 9) open in the HOST process the way an embedder
 * would, then asks the child whether it can still see it. The Clojure
 * test runner is a degenerate host with nothing beyond 0/1/2 open, so
 * the leak is invisible there -- the C harness stands in for a real
 * embedding application.
 *
 * Build (from repo root):
 *   ./mino task examples
 */

#include "mino.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if !defined(_WIN32)
#  include <unistd.h>
#  include <fcntl.h>
#  include <errno.h>
#endif

/* Eval EXPR and require its integer result to equal EXPECTED. */
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

int main(void)
{
    mino_state *S;
    mino_env   *env;
    int          failures = 0;

    S = mino_state_new();
    if (S == NULL) { fprintf(stderr, "state_new failed\n"); return 1; }
    env = mino_env_new(S);
    mino_install_all(S, env);

    /* Sanity: run's exit-code wiring works both ways (guards the
     * happy path so the closefds change cannot silently break capture). */
    failures += verify_long(S, env,
                            "(:exit (clojure.core/run \"true\"))", 0,
                            "run true exits 0");
    {
        mino_val *r = mino_eval_string(S,
            "(:exit (clojure.core/run \"false\"))", env);
        long long ec = 0;
        if (r == NULL || !mino_to_int(r, &ec) || ec == 0) {
            fprintf(stderr, "FAIL run false nonzero: got %lld\n", ec);
            failures++;
        } else {
            printf("ok run false exits nonzero: %lld\n", ec);
        }
    }

#if !defined(_WIN32)
    /* Plant a known descriptor in the HOST (fd 9) the way an embedder
     * holds a db/socket handle open across a run. dup2 onto a fixed low
     * number so the child probe (/dev/fd/9) targets it deterministically.
     * A tmpfile backs it so the fd is genuinely open and usable. */
    {
        int tfd = open("/tmp/mino_embed_proc_fd.tmp",
                       O_RDWR | O_CREAT | O_TRUNC, 0600);
        int planted = 0;
        if (tfd < 0) {
            fprintf(stderr, "setup failed: open tmpfile: %s\n",
                    strerror(errno));
            failures++;
        } else {
            if (dup2(tfd, 9) < 0) {
                fprintf(stderr, "setup failed: dup2 to fd 9: %s\n",
                        strerror(errno));
                failures++;
            } else {
                planted = 1;
            }
            if (tfd != 9) close(tfd);
        }
        /* Precondition: fd 9 is genuinely open in the host. Without this
         * the leak check below would pass vacuously. */
        if (planted && fcntl(9, F_GETFD) != -1) {
            printf("ok host fd 9 is open (precondition)\n");
        } else if (planted) {
            fprintf(stderr, "FAIL host fd 9 not open after plant\n");
            failures++;
        }

        if (planted) {
            /* Child probe: `test -e /dev/fd/9` is true ONLY while fd 9 is
             * open in the child. After closefds the child does not inherit
             * fd 9, so test fails and the child exits 0 (no leak). Before
             * closefds the child inherited fd 9, test succeeded, and the
             * child exited 1 (leak). /dev/fd is present on macOS and Linux. */
            failures += verify_long(S, env,
                "(:exit (clojure.core/run \"sh\" \"-c\" "
                "                       \"test -e /dev/fd/9 && exit 1 || exit 0\"))",
                0, "child did not inherit host fd 9");
        }
        close(9);
        remove("/tmp/mino_embed_proc_fd.tmp");
    }
#else
    printf("ok (fd-inheritance check skipped on Windows)\n");
#endif

    if (failures > 0) {
        fprintf(stderr, "%d failures\n", failures);
        mino_env_free(S, env);
        mino_state_free(S);
        return 1;
    }

    printf("ok\n");
    mino_env_free(S, env);
    mino_state_free(S);
    return 0;
}
