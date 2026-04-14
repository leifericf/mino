/*
 * main.c — standalone entry point and interactive REPL.
 *
 * Reads forms from stdin one at a time, evaluates each in a persistent
 * environment, and prints the result. Multi-line forms are supported by
 * accumulating input until the reader produces a complete form. The prompt
 * is written to stderr so piped output on stdout stays clean.
 */

#include "mino.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MINO_LINE_MAX 4096

/* ---- CWD-relative module resolver ---- */

static const char *cwd_resolve(const char *name, void *ctx)
{
    static char path_buf[4096];
    size_t nlen;
    (void)ctx;
    if (name == NULL) return NULL;
    nlen = strlen(name);
    /* If name already ends with ".mino", use as-is; otherwise append. */
    if (nlen >= 5 && strcmp(name + nlen - 5, ".mino") == 0) {
        if (nlen >= sizeof(path_buf)) return NULL;
        memcpy(path_buf, name, nlen + 1);
    } else {
        if (nlen + 5 >= sizeof(path_buf)) return NULL;
        memcpy(path_buf, name, nlen);
        memcpy(path_buf + nlen, ".mino", 6);
    }
    return path_buf;
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
    mino_env_t *env = mino_env_new();
    char *buf  = NULL;
    size_t cap = 0;
    size_t len = 0;
    int    awaiting_continuation = 0;
    int    exit_code = 0;

    mino_install_core(env);
    mino_install_io(env);
    mino_set_resolver(cwd_resolve, NULL);

    /* File mode: evaluate a script and exit. */
    if (argc > 1) {
        mino_val_t *result = mino_load_file(argv[1], env);
        if (result == NULL) {
            const char *err = mino_last_error();
            fprintf(stderr, "mino: %s\n", err ? err : "unknown error");
            exit_code = 1;
        }
        mino_env_free(env);
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

            form = mino_read(cursor, &end);
            if (form == NULL) {
                const char *err = mino_last_error();
                if (is_unterminated_error(err)) {
                    awaiting_continuation = 1;
                    break;
                }
                fprintf(stderr, "read error: %s\n", err ? err : "unknown");
                len = 0;
                buf[0] = '\0';
                awaiting_continuation = 0;
                break;
            }

            result = mino_eval(form, env);
            if (result == NULL) {
                const char *err = mino_last_error();
                fprintf(stderr, "eval error: %s\n", err ? err : "unknown");
            } else {
                mino_println(result);
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
    mino_env_free(env);
    return exit_code;
}
