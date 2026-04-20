/*
 * main.c — standalone entry point and interactive REPL.
 *
 * Reads forms from stdin one at a time, evaluates each in a persistent
 * environment, and prints the result. Multi-line forms are supported by
 * accumulating input until the reader produces a complete form. The prompt
 * is written to stderr so piped output on stdout stays clean.
 */

#include "mino_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define MINO_LINE_MAX 4096

/* ---- CWD-relative module resolver ---- */

static int file_exists(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (f == NULL) return 0;
    fclose(f);
    return 1;
}

/* Directory where the mino binary was invoked from. Used as fallback
 * for resolving lib/ modules even after chdir. */
static char initial_dir[4096] = "";

static int try_resolve(const char *name, size_t nlen, char *buf, size_t bufsz)
{
    static const char *exts[] = {".mino", ".cljc", ".clj", ".cljs"};
    size_t i;
    for (i = 0; i < 4; i++) {
        snprintf(buf, bufsz, "%s%s", name, exts[i]);
        if (file_exists(buf)) return 1;
    }
    /* Try lib/ prefix. */
    for (i = 0; i < 4; i++) {
        snprintf(buf, bufsz, "lib/%s%s", name, exts[i]);
        if (file_exists(buf)) return 1;
    }
    /* Try initial dir's lib/ as fallback. */
    if (initial_dir[0] != '\0') {
        for (i = 0; i < 4; i++) {
            snprintf(buf, bufsz, "%s/lib/%s%s", initial_dir, name, exts[i]);
            if (file_exists(buf)) return 1;
        }
    }
    (void)nlen;
    return 0;
}

static const char *cwd_resolve(const char *name, void *ctx)
{
    static char path_buf[4096];
    size_t nlen;
    (void)ctx;
    if (name == NULL) return NULL;
    nlen = strlen(name);
    if (nlen + 10 >= sizeof(path_buf)) return NULL;

    /* If name already has an extension, use as-is. */
    if ((nlen >= 5 && (strcmp(name + nlen - 5, ".mino") == 0
                    || strcmp(name + nlen - 5, ".cljc") == 0
                    || strcmp(name + nlen - 5, ".cljs") == 0))
     || (nlen >= 4 && strcmp(name + nlen - 4, ".clj") == 0)) {
        memcpy(path_buf, name, nlen + 1);
        if (file_exists(path_buf)) return path_buf;
        snprintf(path_buf, sizeof(path_buf), "lib/%s", name);
        if (file_exists(path_buf)) return path_buf;
        if (initial_dir[0] != '\0') {
            snprintf(path_buf, sizeof(path_buf), "%s/lib/%s", initial_dir, name);
            if (file_exists(path_buf)) return path_buf;
        }
        return NULL;
    }

    if (try_resolve(name, nlen, path_buf, sizeof(path_buf)))
        return path_buf;

    return NULL;
}

/* ---- helpers ---- */

static int is_unterminated_error(const char *msg)
{
    return msg != NULL && strstr(msg, "unterminated") != NULL;
}

static int has_only_whitespace(const char *s)
{
    while (*s) {
        unsigned char c = (unsigned char)*s++;
        if (c != ' ' && c != '\t' && c != '\n' && c != '\r' && c != ',') {
            return 0;
        }
    }
    return 1;
}

int main(int argc, char **argv)
{
    mino_state_t *S;
    getcwd(initial_dir, sizeof(initial_dir));
    S = mino_state_new();
    mino_env_t   *env = mino_env_new(S);
    char *buf  = NULL;
    size_t cap = 0;
    size_t len = 0;
    int    awaiting_continuation = 0;
    int    exit_code = 0;

    mino_install_core(S, env);
    mino_install_io(S, env);
    mino_install_fs(S, env);
    mino_install_proc(S, env);
    mino_set_resolver(S, cwd_resolve, NULL);

    /* File mode: evaluate a script and exit. */
    if (argc > 1) {
        mino_val_t *result = mino_load_file(S, argv[1], env);
        if (result == NULL) {
            const mino_diag_t *d = mino_last_diag(S);
            if (d != NULL) {
                char dbuf[2048];
                mino_render_diag(S, d, MINO_DIAG_RENDER_PRETTY,
                                 dbuf, sizeof(dbuf));
                fprintf(stderr, "%s", dbuf);
            } else {
                const char *err = mino_last_error(S);
                fprintf(stderr, "mino: %s\n", err ? err : "unknown error");
            }
            exit_code = 1;
        }
        mino_env_free(S, env);
        mino_state_free(S);
        return exit_code;
    }

    fputs("mino> ", stderr);
    fflush(stderr);

    for (;;) {
        char line[MINO_LINE_MAX];
        if (fgets(line, sizeof(line), stdin) == NULL) {
            break;
        }
        {
            size_t add = strlen(line);
            if (len + add + 1 > cap) {
                size_t new_cap = cap == 0 ? 256 : cap;
                while (new_cap < len + add + 1) {
                    new_cap *= 2;
                }
                buf = (char *)realloc(buf, new_cap);
                if (buf == NULL) {
                    fputs("mino: out of memory\n", stderr);
                    exit_code = 1;
                    goto cleanup;
                }
                cap = new_cap;
            }
            memcpy(buf + len, line, add + 1);
            len += add;
        }

        /* Drain as many complete forms as we have. */
        for (;;) {
            const char *cursor = buf;
            const char *end    = buf;
            mino_val_t *form;
            mino_val_t *result;

            if (has_only_whitespace(buf)) {
                /* Don't discard trailing whitespace: newlines in it feed
                 * the reader's line counter on the next call. */
                awaiting_continuation = 0;
                break;
            }

            /* Cache REPL input for diagnostic source snippets. */
            source_cache_store(S, S->reader_file, buf, len);

            form = mino_read(S, cursor, &end);
            if (form == NULL) {
                const char *err = mino_last_error(S);
                if (is_unterminated_error(err)) {
                    awaiting_continuation = 1;
                    break;
                }
                {
                    const mino_diag_t *d = mino_last_diag(S);
                    if (d != NULL) {
                        char dbuf[2048];
                        mino_render_diag(S, d, MINO_DIAG_RENDER_PRETTY,
                                         dbuf, sizeof(dbuf));
                        fprintf(stderr, "%s", dbuf);
                    } else {
                        fprintf(stderr, "read error: %s\n",
                                err ? err : "unknown");
                    }
                }
                len = 0;
                buf[0] = '\0';
                awaiting_continuation = 0;
                break;
            }

            result = mino_eval(S, form, env);
            if (result == NULL) {
                const mino_diag_t *d = mino_last_diag(S);
                if (d != NULL) {
                    char dbuf[2048];
                    mino_render_diag(S, d, MINO_DIAG_RENDER_PRETTY,
                                     dbuf, sizeof(dbuf));
                    fprintf(stderr, "%s", dbuf);
                } else {
                    const char *err = mino_last_error(S);
                    fprintf(stderr, "eval error: %s\n",
                            err ? err : "unknown");
                }
            } else {
                mino_println(S, result);
            }

            /* Shift unread bytes to the front of the buffer. */
            {
                size_t consumed = (size_t)(end - buf);
                size_t remaining = len - consumed;
                memmove(buf, end, remaining + 1);
                len = remaining;
            }
            awaiting_continuation = 0;
        }

        fputs(awaiting_continuation ? "...   " : "mino> ", stderr);
        fflush(stderr);
    }

    fputc('\n', stderr);

cleanup:
    free(buf);
    mino_env_free(S, env);
    mino_state_free(S);
    return exit_code;
}
