/*
 * prim_proc.c -- process execution primitives: sh, sh!.
 *
 * Uses popen(3) to capture stdout from external commands.  popen is
 * POSIX; Windows provides _popen with the same interface.
 */

#define _POSIX_C_SOURCE 200809L

#include "prim_internal.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifndef _WIN32
#include <sys/wait.h>
#endif

/* ---- shell-escape and command-string building ---- */

/* Append a shell-escaped token to buf. Returns new length, or 0 on overflow. */
static size_t append_escaped(char *buf, size_t pos, size_t cap,
                             const char *s, size_t slen)
{
    size_t i;
    if (pos + slen * 2 + 4 >= cap) return 0; /* conservative overflow check */
    buf[pos++] = '\'';
    for (i = 0; i < slen; i++) {
        if (s[i] == '\'') {
            /* end quote, escaped quote, restart quote: '\'' */
            if (pos + 4 >= cap) return 0;
            buf[pos++] = '\'';
            buf[pos++] = '\\';
            buf[pos++] = '\'';
            buf[pos++] = '\'';
        } else {
            buf[pos++] = s[i];
        }
    }
    buf[pos++] = '\'';
    buf[pos] = '\0';
    return pos;
}

/* Build a shell command string from a cons-list of string arguments.
 * Returns a malloc'd string or NULL on failure. */
static char *build_command(mino_state_t *S, mino_val_t *args)
{
    char *buf;
    size_t cap = 4096;
    size_t pos = 0;
    int first = 1;

    buf = (char *)malloc(cap);
    if (buf == NULL) return NULL;
    buf[0] = '\0';

    while (mino_is_cons(args)) {
        mino_val_t *arg = args->as.cons.car;
        size_t new_pos;
        if (arg == NULL || arg->type != MINO_STRING) {
            free(buf);
            prim_throw_classified(S, "eval/type", "MTY001",
                                  "sh: all arguments must be strings");
            return NULL;
        }
        if (!first) {
            if (pos + 1 >= cap) { free(buf); return NULL; }
            buf[pos++] = ' ';
        }
        new_pos = append_escaped(buf, pos, cap, arg->as.s.data, arg->as.s.len);
        if (new_pos == 0) {
            /* Grow buffer and retry. */
            cap = cap * 2 + arg->as.s.len * 4;
            buf = (char *)realloc(buf, cap);
            if (buf == NULL) return NULL;
            new_pos = append_escaped(buf, pos, cap,
                                     arg->as.s.data, arg->as.s.len);
            if (new_pos == 0) { free(buf); return NULL; }
        }
        pos = new_pos;
        first = 0;
        args = args->as.cons.cdr;
    }
    return buf;
}

/* ---- read all stdout from a popen handle ---- */

static char *read_all(FILE *fp, size_t *out_len)
{
    char *buf = NULL;
    size_t len = 0, cap = 0;
    char chunk[4096];
    size_t n;

    while ((n = fread(chunk, 1, sizeof(chunk), fp)) > 0) {
        if (len + n + 1 > cap) {
            cap = cap == 0 ? 4096 : cap * 2;
            while (cap < len + n + 1) cap *= 2;
            buf = (char *)realloc(buf, cap);
            if (buf == NULL) return NULL;
        }
        memcpy(buf + len, chunk, n);
        len += n;
    }
    if (buf == NULL) {
        buf = (char *)malloc(1);
        if (buf == NULL) return NULL;
    }
    buf[len] = '\0';
    *out_len = len;
    return buf;
}

/* ---- primitives ---- */

/* (sh cmd & args) -- run command, return {:exit n :out "..."} */
mino_val_t *prim_sh(mino_state_t *S, mino_val_t *args, mino_env_t *env)
{
    char *cmd;
    FILE *fp;
    char *out;
    size_t out_len;
    int status;
    mino_val_t *keys[2], *vals[2];
    mino_val_t *result;
    (void)env;

    if (!mino_is_cons(args)) {
        return prim_throw_classified(S, "eval/arity", "MAR001",
                                     "sh requires at least one argument");
    }

    cmd = build_command(S, args);
    if (cmd == NULL) return NULL; /* error already thrown */

    /* Redirect stderr to stdout so we capture both. */
    {
        size_t clen = strlen(cmd);
        char *cmd2 = (char *)realloc(cmd, clen + 16);
        if (cmd2 == NULL) { free(cmd); return NULL; }
        cmd = cmd2;
        memcpy(cmd + clen, " 2>&1", 6);
    }

    fp = popen(cmd, "r");
    free(cmd);
    if (fp == NULL) {
        return prim_throw_classified(S, "io", "MIO001",
                                     "sh: failed to execute command");
    }

    out = read_all(fp, &out_len);
    status = pclose(fp);
#ifndef _WIN32
    /* POSIX: extract exit code from wait status. */
    if (WIFEXITED(status)) status = WEXITSTATUS(status);
#endif

    if (out == NULL) {
        return prim_throw_classified(S, "io", "MIO001",
                                     "sh: out of memory reading output");
    }

    keys[0] = mino_keyword(S, "exit");
    keys[1] = mino_keyword(S, "out");
    vals[0] = mino_int(S, status);
    vals[1] = mino_string_n(S, out, out_len);
    free(out);

    result = mino_map(S, keys, vals, 2);
    return result;
}

/* (sh! cmd & args) -- run command, return stdout; throw on non-zero exit. */
mino_val_t *prim_sh_bang(mino_state_t *S, mino_val_t *args, mino_env_t *env)
{
    mino_val_t *result = prim_sh(S, args, env);
    mino_val_t *exit_val;
    mino_val_t *out_val;
    mino_val_t *exit_key;

    if (result == NULL) return NULL;

    exit_key = mino_keyword(S, "exit");
    exit_val = map_get_val(result, exit_key);

    if (exit_val != NULL && exit_val->type == MINO_INT
        && exit_val->as.i != 0) {
        out_val = map_get_val(result, mino_keyword(S, "out"));
        {
            char msg[512];
            snprintf(msg, sizeof(msg), "sh!: command exited with code %lld: %s",
                     exit_val->as.i,
                     (out_val && out_val->type == MINO_STRING)
                         ? out_val->as.s.data : "");
            return prim_throw_classified(S, "io", "MIO001", msg);
        }
    }

    return map_get_val(result, mino_keyword(S, "out"));
}

/* ---- install ---- */

void mino_install_proc(mino_state_t *S, mino_env_t *env)
{
    DEF_PRIM(env, "sh",  prim_sh,
             "Runs an external command. Returns {:exit n :out \"...\"}.");
    DEF_PRIM(env, "sh!", prim_sh_bang,
             "Runs an external command. Returns stdout; throws on non-zero exit.");
}
