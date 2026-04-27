/*
 * io.c -- I/O primitives: println, prn, slurp, spit, exit, time-ms,
 *              nano-time, file-seq, print_str_to helper.
 */

#include "prim/internal.h"
#include "path_buf.h"
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>

#ifdef _WIN32
#  define WIN32_LEAN_AND_MEAN
#  include <windows.h>
#elif defined(CLOCK_MONOTONIC)
#  include <time.h>
#endif

void print_str_to(mino_state_t *S, FILE *out, const mino_val_t *v)
{
    if (v != NULL && v->type == MINO_STRING) {
        fwrite(v->as.s.data, 1, v->as.s.len, out);
    } else {
        mino_print_to(S, out, v);
    }
}

/* Resolve the current sink for *out* / *err*. Looks up the dynamic-
 * binding stack first under both the bare and `clojure.core/`-
 * qualified names (syntax-quote in user macros tends to expand the
 * bare symbol to the qualified form), then falls back to the var's
 * root value. Returns NULL only when the var has never been interned
 * (the boot-time path before mino_install_core finishes). */
static mino_val_t *resolve_io_sink(mino_state_t *S, const char *name)
{
    mino_val_t *v;
    mino_val_t *var;
    char        qualified[64];
    if (S->dyn_stack != NULL) {
        v = dyn_lookup(S, name);
        if (v != NULL) return v;
        if (strlen(name) + sizeof("clojure.core/") < sizeof(qualified)) {
            int n = snprintf(qualified, sizeof(qualified),
                             "clojure.core/%s", name);
            if (n > 0 && (size_t)n < sizeof(qualified)) {
                v = dyn_lookup(S, qualified);
                if (v != NULL) return v;
            }
        }
    }
    var = var_find(S, "clojure.core", name);
    if (var != NULL && var->type == MINO_VAR && var->as.var.bound) {
        return var->as.var.root;
    }
    return NULL;
}

/* Append `buf` (len bytes) to the string-bearing atom `sink` if sink
 * is a MINO_ATOM holding a MINO_STRING. Returns 1 on capture, 0 if
 * sink is not a string-atom (caller falls back to a FILE*), and -1
 * on OOM. */
static int try_capture_to_atom(mino_state_t *S, mino_val_t *sink,
                               const char *buf, size_t len)
{
    mino_val_t *cur;
    mino_val_t *new_str;
    char       *combined;
    size_t      cur_len;
    if (sink == NULL || sink->type != MINO_ATOM) return 0;
    cur = sink->as.atom.val;
    if (cur == NULL || cur->type != MINO_STRING) return 0;
    cur_len = cur->as.s.len;
    combined = (char *)malloc(cur_len + len);
    if (combined == NULL) {
        prim_throw_classified(S, "internal", "MIN001",
            "*out*: out of memory");
        return -1;
    }
    memcpy(combined, cur->as.s.data, cur_len);
    memcpy(combined + cur_len, buf, len);
    new_str = mino_string_n(S, combined, cur_len + len);
    free(combined);
    gc_write_barrier(S, sink, sink->as.atom.val, new_str);
    sink->as.atom.val = new_str;
    return 1;
}

/* Emit `buf` to the sink named by `out_var_name` (`*out*` or `*err*`).
 * Routing:
 *   atom holding string  → append to atom
 *   :mino/stdout         → write to stdout
 *   :mino/stderr         → write to stderr
 *   other / unbound      → write to the default FILE* for the named
 *                          variable (stdout for *out*, stderr for *err*)
 * This means (binding [*out* *err*] (println "x")) routes through
 * stderr because *out* resolves to :mino/stderr.
 * Returns 0 on success, -1 on error. */
static int io_emit(mino_state_t *S, const char *out_var_name,
                   const char *buf, size_t len)
{
    mino_val_t *sink;
    int         captured;
    FILE       *fallback;
    sink     = resolve_io_sink(S, out_var_name);
    captured = try_capture_to_atom(S, sink, buf, len);
    if (captured < 0) return -1;
    if (captured == 1) return 0;
    fallback = (strcmp(out_var_name, "*err*") == 0) ? stderr : stdout;
    if (sink != NULL && sink->type == MINO_KEYWORD) {
        if (sink->as.s.len == 11
            && memcmp(sink->as.s.data, "mino/stdout", 11) == 0) {
            fallback = stdout;
        } else if (sink->as.s.len == 11
                   && memcmp(sink->as.s.data, "mino/stderr", 11) == 0) {
            fallback = stderr;
        }
    }
    fwrite(buf, 1, len, fallback);
    fflush(fallback);
    return 0;
}

/* Build a print-formatted chunk for one argument and append it to a
 * growing buffer. `readably` is non-zero for pr/prn (strings quoted,
 * chars escaped) and zero for print/println. Returns 0 on success,
 * -1 on error (caller frees buf). */
static int append_print_chunk(mino_state_t *S, mino_val_t *v,
                              char **buf, size_t *len, size_t *cap,
                              int readably)
{
    const char *src;
    size_t      slen;
    mino_val_t *formatted = NULL;
    if (!readably && v != NULL && v->type == MINO_STRING) {
        src  = v->as.s.data;
        slen = v->as.s.len;
    } else {
        formatted = print_to_string(S, v);
        if (formatted == NULL) return -1;
        src  = formatted->as.s.data;
        slen = formatted->as.s.len;
    }
    if (*len + slen > *cap) {
        size_t nc = *cap == 0 ? 64 : *cap;
        char  *nb;
        while (nc < *len + slen) nc *= 2;
        nb = (char *)realloc(*buf, nc);
        if (nb == NULL) {
            prim_throw_classified(S, "internal", "MIN001",
                "print: out of memory");
            return -1;
        }
        *buf = nb;
        *cap = nc;
    }
    memcpy(*buf + *len, src, slen);
    *len += slen;
    return 0;
}

/* Append a single byte to a growing buffer. */
static int append_byte(mino_state_t *S, char **buf, size_t *len,
                       size_t *cap, char c)
{
    if (*len + 1 > *cap) {
        size_t nc = *cap == 0 ? 64 : *cap * 2;
        char  *nb = (char *)realloc(*buf, nc);
        if (nb == NULL) {
            prim_throw_classified(S, "internal", "MIN001",
                "print: out of memory");
            return -1;
        }
        *buf = nb;
        *cap = nc;
    }
    (*buf)[(*len)++] = c;
    return 0;
}

/* Format one value through the print-method hook (if installed) or
 * the built-in C formatter, returning the bytes as a mino string.
 * Used by pr/prn when readably=1. Returns NULL on error. */
static mino_val_t *format_via_hook_or_builtin(mino_state_t *S,
                                              mino_val_t *v,
                                              mino_env_t *env)
{
    if (S->print_method_fn != NULL) {
        /* The hook calls pr-builtin (now routed through *out*) or
         * the user-supplied method. Capture its output by binding
         * *out* to a temporary string-atom for the duration of the
         * hook call, then return the captured string. */
        mino_val_t   *atom_str = mino_string_n(S, "", 0);
        mino_val_t   *atom_val;
        mino_val_t   *call_args;
        dyn_frame_t  *frame;
        dyn_binding_t *binding;
        atom_val = mino_atom(S, atom_str);
        if (atom_val == NULL) return NULL;
        binding = (dyn_binding_t *)malloc(sizeof(*binding));
        if (binding == NULL) {
            prim_throw_classified(S, "internal", "MIN001",
                "print: out of memory");
            return NULL;
        }
        binding->name = mino_symbol(S, "*out*")->as.s.data;
        binding->val  = atom_val;
        binding->next = NULL;
        frame = (dyn_frame_t *)malloc(sizeof(*frame));
        if (frame == NULL) {
            free(binding);
            prim_throw_classified(S, "internal", "MIN001",
                "print: out of memory");
            return NULL;
        }
        frame->bindings = binding;
        frame->prev     = S->dyn_stack;
        S->dyn_stack    = frame;
        call_args = mino_cons(S, v, mino_nil(S));
        (void)mino_call(S, S->print_method_fn, call_args, env);
        S->dyn_stack = frame->prev;
        free(binding);
        free(frame);
        if (mino_last_error(S) != NULL) return NULL;
        return atom_val->as.atom.val;
    }
    return print_to_string(S, v);
}

/* Build a space-separated chunk of formatted args, optionally with a
 * trailing newline, and emit through *out*. `readably` selects the
 * pr/prn family (strings quoted, chars escaped, print-method hook
 * consulted) versus the print/println family. */
static mino_val_t *print_args_to_out(mino_state_t *S, mino_val_t *args,
                                     mino_env_t *env,
                                     int readably, int newline)
{
    char   *buf   = NULL;
    size_t  len   = 0;
    size_t  cap   = 0;
    int     first = 1;
    while (mino_is_cons(args)) {
        mino_val_t *v = args->as.cons.car;
        if (!first) {
            if (append_byte(S, &buf, &len, &cap, ' ') < 0) {
                free(buf);
                return NULL;
            }
        }
        if (readably) {
            mino_val_t *formatted = format_via_hook_or_builtin(S, v, env);
            if (formatted == NULL) {
                free(buf);
                return NULL;
            }
            if (len + formatted->as.s.len > cap) {
                size_t nc = cap == 0 ? 64 : cap;
                char  *nb;
                while (nc < len + formatted->as.s.len) nc *= 2;
                nb = (char *)realloc(buf, nc);
                if (nb == NULL) {
                    free(buf);
                    prim_throw_classified(S, "internal", "MIN001",
                        "print: out of memory");
                    return NULL;
                }
                buf = nb;
                cap = nc;
            }
            memcpy(buf + len, formatted->as.s.data, formatted->as.s.len);
            len += formatted->as.s.len;
        } else {
            if (append_print_chunk(S, v, &buf, &len, &cap, 0) < 0) {
                free(buf);
                return NULL;
            }
        }
        first = 0;
        args  = args->as.cons.cdr;
    }
    if (newline) {
        if (append_byte(S, &buf, &len, &cap, '\n') < 0) {
            free(buf);
            return NULL;
        }
    }
    if (io_emit(S, "*out*", buf == NULL ? "" : buf, len) < 0) {
        free(buf);
        return NULL;
    }
    free(buf);
    return mino_nil(S);
}

mino_val_t *prim_println(mino_state_t *S, mino_val_t *args, mino_env_t *env)
{
    return print_args_to_out(S, args, env, 0, 1);
}

mino_val_t *prim_prn(mino_state_t *S, mino_val_t *args, mino_env_t *env)
{
    return print_args_to_out(S, args, env, 1, 1);
}

mino_val_t *prim_print(mino_state_t *S, mino_val_t *args, mino_env_t *env)
{
    return print_args_to_out(S, args, env, 0, 0);
}

mino_val_t *prim_pr(mino_state_t *S, mino_val_t *args, mino_env_t *env)
{
    return print_args_to_out(S, args, env, 1, 0);
}

/* (pr-builtin x) writes one value via the built-in C formatter, bypassing
 * the print-method hook. Used by print-method's :default method so the
 * default path does not recurse into itself. Routes through *out* so a
 * binding to a string-atom captures, and falls through to stdout
 * otherwise. */
mino_val_t *prim_pr_builtin(mino_state_t *S, mino_val_t *args, mino_env_t *env)
{
    mino_val_t *formatted;
    (void)env;
    if (!mino_is_cons(args) || mino_is_cons(args->as.cons.cdr)) {
        return prim_throw_classified(S, "eval/arity", "MAR001",
            "pr-builtin requires one argument");
    }
    formatted = print_to_string(S, args->as.cons.car);
    if (formatted == NULL) return NULL;
    if (io_emit(S, "*out*", formatted->as.s.data,
                formatted->as.s.len) < 0) {
        return NULL;
    }
    return mino_nil(S);
}

/* (set-print-method! fn) — install a late-binding hook for pr / prn.
 * Calling with nil removes the hook. The hook must be a fn that prints
 * its one argument to stdout. */
mino_val_t *prim_set_print_method_bang(mino_state_t *S, mino_val_t *args,
                                       mino_env_t *env)
{
    mino_val_t *fn;
    (void)env;
    if (!mino_is_cons(args) || mino_is_cons(args->as.cons.cdr)) {
        return prim_throw_classified(S, "eval/arity", "MAR001",
            "set-print-method! requires one argument");
    }
    fn = args->as.cons.car;
    if (fn == NULL || fn->type == MINO_NIL) {
        S->print_method_fn = NULL;
        return mino_nil(S);
    }
    if (fn->type != MINO_FN && fn->type != MINO_PRIM) {
        return prim_throw_classified(S, "eval/type", "MTY001",
            "set-print-method! argument must be a fn");
    }
    S->print_method_fn = fn;
    return mino_nil(S);
}

/* (newline) writes a single line separator. Returns nil. */
mino_val_t *prim_newline(mino_state_t *S, mino_val_t *args, mino_env_t *env)
{
    (void)env;
    if (mino_is_cons(args)) {
        return prim_throw_classified(S, "eval/arity", "MAR001",
            "newline takes no arguments");
    }
    if (io_emit(S, "*out*", "\n", 1) < 0) return NULL;
    return mino_nil(S);
}

/* (read-line) reads one line from *in*. Routing matches *out*:
 *   atom holding string  → consume up to next \n; update atom to
 *                          the remainder; return the line text
 *                          without the trailing \n
 *   :mino/stdin / unbound → read a line from stdin via fgets,
 *                           growing as needed for long lines
 * Returns nil on EOF. */
mino_val_t *prim_read_line(mino_state_t *S, mino_val_t *args, mino_env_t *env)
{
    mino_val_t *src;
    (void)env;
    if (mino_is_cons(args)) {
        return prim_throw_classified(S, "eval/arity", "MAR001",
            "read-line takes no arguments");
    }
    src = resolve_io_sink(S, "*in*");
    if (src != NULL && src->type == MINO_ATOM) {
        mino_val_t *cur = src->as.atom.val;
        size_t      i;
        size_t      llen;
        size_t      rstart;
        mino_val_t *line;
        mino_val_t *rem;
        if (cur == NULL || cur->type != MINO_STRING
            || cur->as.s.len == 0) {
            return mino_nil(S);
        }
        for (i = 0; i < cur->as.s.len; i++) {
            if (cur->as.s.data[i] == '\n') break;
        }
        llen   = i;
        rstart = (i < cur->as.s.len) ? i + 1 : i;
        line   = mino_string_n(S, cur->as.s.data, llen);
        rem    = mino_string_n(S, cur->as.s.data + rstart,
                               cur->as.s.len - rstart);
        gc_write_barrier(S, src, src->as.atom.val, rem);
        src->as.atom.val = rem;
        return line;
    }
    {
        char  *buf = NULL;
        size_t len = 0;
        size_t cap = 0;
        char   chunk[256];
        int    saw_any = 0;
        while (fgets(chunk, sizeof(chunk), stdin) != NULL) {
            size_t cl = strlen(chunk);
            int    has_nl = cl > 0 && chunk[cl - 1] == '\n';
            saw_any = 1;
            if (has_nl) cl -= 1;
            if (len + cl + 1 > cap) {
                size_t nc = cap == 0 ? 256 : cap;
                char  *nb;
                while (nc < len + cl + 1) nc *= 2;
                nb = (char *)realloc(buf, nc);
                if (nb == NULL) {
                    free(buf);
                    return prim_throw_classified(S, "internal", "MIN001",
                        "read-line: out of memory");
                }
                buf = nb;
                cap = nc;
            }
            memcpy(buf + len, chunk, cl);
            len += cl;
            if (has_nl) break;
        }
        if (!saw_any) {
            free(buf);
            return mino_nil(S);
        }
        {
            mino_val_t *result = mino_string_n(S, buf == NULL ? "" : buf, len);
            free(buf);
            return result;
        }
    }
}

/* (read) reads one form from *in*. Atom-bound *in* is a string
 * cursor: the form is parsed from the head, the atom is updated to
 * the unread tail. Stdin-backed *in* (default) is not supported;
 * use (read-string ...) on a captured input or with-in-str instead. */
mino_val_t *prim_read(mino_state_t *S, mino_val_t *args, mino_env_t *env)
{
    mino_val_t *src;
    (void)env;
    if (mino_is_cons(args)) {
        return prim_throw_classified(S, "eval/arity", "MAR001",
            "read takes no arguments in this build");
    }
    src = resolve_io_sink(S, "*in*");
    if (src != NULL && src->type == MINO_ATOM) {
        mino_val_t *cur = src->as.atom.val;
        const char *end = NULL;
        mino_val_t *form;
        mino_val_t *rem;
        size_t      consumed;
        if (cur == NULL || cur->type != MINO_STRING
            || cur->as.s.len == 0) {
            return mino_nil(S);
        }
        form = mino_read(S, cur->as.s.data, &end);
        if (form == NULL) {
            if (mino_last_error(S) != NULL) return NULL;
            /* Empty input or whitespace-only. */
            return mino_nil(S);
        }
        consumed = (end != NULL && end >= cur->as.s.data)
                 ? (size_t)(end - cur->as.s.data) : cur->as.s.len;
        if (consumed > cur->as.s.len) consumed = cur->as.s.len;
        rem = mino_string_n(S, cur->as.s.data + consumed,
                            cur->as.s.len - consumed);
        gc_write_barrier(S, src, src->as.atom.val, rem);
        src->as.atom.val = rem;
        return form;
    }
    return prim_throw_classified(S, "mino/unsupported", "MIO002",
        "read: stdin-backed *in* is not supported; use with-in-str or read-string");
}

/* (printf fmt & args) formats via the standard format primitive and
 * writes the resulting string to *out*. Equivalent to
 * (print (apply format fmt args)) but lives in C to keep the
 * boot-time core.clj footprint small. */
mino_val_t *prim_printf(mino_state_t *S, mino_val_t *args, mino_env_t *env)
{
    mino_val_t *formatted;
    (void)env;
    if (!mino_is_cons(args)) {
        return prim_throw_classified(S, "eval/arity", "MAR001",
            "printf requires at least a format string");
    }
    formatted = prim_format(S, args, env);
    if (formatted == NULL) return NULL;
    if (formatted->type != MINO_STRING) {
        return prim_throw_classified(S, "eval/type", "MTY001",
            "printf: format did not produce a string");
    }
    if (io_emit(S, "*out*", formatted->as.s.data,
                formatted->as.s.len) < 0) {
        return NULL;
    }
    return mino_nil(S);
}

/* (flush) flushes any pending output on *out* and *err*. For a
 * string-atom binding this is a no-op (writes are immediate); for
 * the FILE* fallback paths it calls fflush. */
mino_val_t *prim_flush(mino_state_t *S, mino_val_t *args, mino_env_t *env)
{
    (void)env;
    if (mino_is_cons(args)) {
        return prim_throw_classified(S, "eval/arity", "MAR001",
            "flush takes no arguments");
    }
    fflush(stdout);
    fflush(stderr);
    return mino_nil(S);
}

/* (slurp path) — read a file's entire contents as a string. I/O capability;
 * only installed by mino_install_io, not mino_install_core. */
mino_val_t *prim_slurp(mino_state_t *S, mino_val_t *args, mino_env_t *env)
{
    mino_val_t *path_val;
    const char *path;
    FILE       *f;
    long        sz;
    size_t      rd;
    char       *buf;
    mino_val_t *result;
    (void)env;
    if (!mino_is_cons(args) || mino_is_cons(args->as.cons.cdr)) {
        return prim_throw_classified(S, "eval/arity", "MAR001", "slurp requires one argument");
    }
    path_val = args->as.cons.car;
    if (path_val == NULL || path_val->type != MINO_STRING) {
        return prim_throw_classified(S, "eval/type", "MTY001", "slurp: argument must be a string");
    }
    path = path_val->as.s.data;
    f = fopen(path, "rb");
    if (f == NULL) {
        char msg[300];
        snprintf(msg, sizeof(msg), "slurp: cannot open file: %s", path);
        return prim_throw_classified(S, "host", "MHO001", msg);
    }
    fseek(f, 0, SEEK_END);
    sz = ftell(f);
    if (sz < 0) {
        fclose(f);
        return prim_throw_classified(S, "host", "MHO001", "slurp: cannot determine file size");
    }
    fseek(f, 0, SEEK_SET);
    buf = (char *)malloc((size_t)sz + 1);
    if (buf == NULL) {
        fclose(f);
        return prim_throw_classified(S, "host", "MHO001", "slurp: out of memory");
    }
    rd = fread(buf, 1, (size_t)sz, f);
    fclose(f);
    buf[rd] = '\0';
    result = mino_string_n(S, buf, rd);
    free(buf);
    return result;
}

mino_val_t *prim_spit(mino_state_t *S, mino_val_t *args, mino_env_t *env)
{
    mino_val_t *path_val;
    mino_val_t *content;
    const char *path;
    FILE       *f;
    (void)env;
    if (!mino_is_cons(args) || !mino_is_cons(args->as.cons.cdr)
        || mino_is_cons(args->as.cons.cdr->as.cons.cdr)) {
        return prim_throw_classified(S, "eval/arity", "MAR001", "spit requires two arguments");
    }
    path_val = args->as.cons.car;
    content  = args->as.cons.cdr->as.cons.car;
    if (path_val == NULL || path_val->type != MINO_STRING) {
        return prim_throw_classified(S, "eval/type", "MTY001", "spit: first argument must be a string path");
    }
    path = path_val->as.s.data;
    f = fopen(path, "wb");
    if (f == NULL) {
        char msg[300];
        snprintf(msg, sizeof(msg), "spit: cannot open file: %s", path);
        return prim_throw_classified(S, "host", "MHO001", msg);
    }
    if (content != NULL && content->type == MINO_STRING) {
        fwrite(content->as.s.data, 1, content->as.s.len, f);
    } else {
        mino_print_to(S, f, content);
    }
    fclose(f);
    return mino_nil(S);
}

/* (exit code) — terminate the process with the given exit code.
 * Defaults to 0 if no argument is given. */
mino_val_t *prim_exit(mino_state_t *S, mino_val_t *args, mino_env_t *env)
{
    int code = 0;
    (void)env;
    if (mino_is_cons(args)) {
        mino_val_t *v = args->as.cons.car;
        if (v != NULL && v->type == MINO_INT) {
            code = (int)v->as.i;
        } else if (v != NULL && v->type == MINO_FLOAT) {
            code = (int)v->as.f;
        }
    }
    exit(code);
    return mino_nil(S); /* unreachable */
}

/* (time-ms) — return process time in milliseconds as a float.
 * Uses ANSI C clock() for portability across all C99 platforms. */
mino_val_t *prim_time_ms(mino_state_t *S, mino_val_t *args, mino_env_t *env)
{
    (void)args;
    (void)env;
    if (mino_is_cons(args)) {
        return prim_throw_classified(S, "eval/arity", "MAR001", "time-ms takes no arguments");
    }
    return mino_float(S, (double)clock() / (double)CLOCKS_PER_SEC * 1000.0);
}

/* (nano-time) — return monotonic wall-clock time in nanoseconds as an integer. */
mino_val_t *prim_nano_time(mino_state_t *S, mino_val_t *args, mino_env_t *env)
{
    (void)env;
    if (mino_is_cons(args)) {
        return prim_throw_classified(S, "eval/arity", "MAR001",
                                     "nano-time takes no arguments");
    }
    return mino_int(S, mino_monotonic_ns());
}

/* (getcwd) -- return the current working directory as a string. */
mino_val_t *prim_getcwd(mino_state_t *S, mino_val_t *args, mino_env_t *env)
{
    char buf[PATH_BUF_CAP];
    (void)env;
    if (mino_is_cons(args)) {
        return prim_throw_classified(S, "eval/arity", "MAR001",
                                     "getcwd takes no arguments");
    }
    if (getcwd(buf, sizeof(buf)) == NULL) {
        return prim_throw_classified(S, "io", "MIO001",
                                     "getcwd: failed to get working directory");
    }
    return mino_string(S, buf);
}

/* (chdir path) -- change current working directory. Returns nil. */
mino_val_t *prim_chdir(mino_state_t *S, mino_val_t *args, mino_env_t *env)
{
    mino_val_t *path_val;
    (void)env;
    if (!mino_is_cons(args) || mino_is_cons(args->as.cons.cdr)) {
        return prim_throw_classified(S, "eval/arity", "MAR001",
                                     "chdir requires one argument");
    }
    path_val = args->as.cons.car;
    if (path_val == NULL || path_val->type != MINO_STRING) {
        return prim_throw_classified(S, "eval/type", "MTY001",
                                     "chdir: argument must be a string");
    }
    if (chdir(path_val->as.s.data) != 0) {
        return prim_throw_classified(S, "io", "MIO001",
                                     "chdir: directory not found");
    }
    return mino_nil(S);
}

/* (getenv name) -- return environment variable value or nil. */
mino_val_t *prim_getenv(mino_state_t *S, mino_val_t *args, mino_env_t *env)
{
    mino_val_t *name_val;
    const char *val;
    (void)env;
    if (!mino_is_cons(args) || mino_is_cons(args->as.cons.cdr)) {
        return prim_throw_classified(S, "eval/arity", "MAR001",
                                     "getenv requires one argument");
    }
    name_val = args->as.cons.car;
    if (name_val == NULL || name_val->type != MINO_STRING) {
        return prim_throw_classified(S, "eval/type", "MTY001",
                                     "getenv: argument must be a string");
    }
    val = getenv(name_val->as.s.data);
    if (val == NULL) return mino_nil(S);
    return mino_string(S, val);
}

/* ---- file-seq: recursive directory listing ---- */

static void file_seq_recurse(mino_state_t *S, const char *dir,
                             mino_val_t ***items, size_t *len, size_t *cap)
{
    DIR *d = opendir(dir);
    struct dirent *ent;
    if (d == NULL) return;
    while ((ent = readdir(d)) != NULL) {
        char path[PATH_BUF_CAP];
        struct stat st;
        if (ent->d_name[0] == '.') continue;
        snprintf(path, sizeof(path), "%s/%s", dir, ent->d_name);
        if (stat(path, &st) != 0) continue;
        if (S_ISDIR(st.st_mode)) {
            file_seq_recurse(S, path, items, len, cap);
        } else {
            if (*len == *cap) {
                size_t nc = *cap == 0 ? 64 : *cap * 2;
                mino_val_t **nb = (mino_val_t **)realloc(*items,
                                                          nc * sizeof(**items));
                if (nb == NULL) { closedir(d); return; }
                *items = nb;
                *cap = nc;
            }
            (*items)[*len] = mino_string(S, path);
            (*len)++;
        }
    }
    closedir(d);
}

mino_val_t *prim_file_seq(mino_state_t *S, mino_val_t *args, mino_env_t *env)
{
    mino_val_t *dir_val;
    const char *dir;
    mino_val_t **items = NULL;
    size_t len = 0, cap = 0;
    mino_val_t *result;
    (void)env;
    if (!mino_is_cons(args) || mino_is_cons(args->as.cons.cdr)) {
        return prim_throw_classified(S, "eval/arity", "MAR001",
                                     "file-seq requires one argument");
    }
    dir_val = args->as.cons.car;
    if (dir_val == NULL || dir_val->type != MINO_STRING) {
        return prim_throw_classified(S, "eval/type", "MTY001",
                                     "file-seq: argument must be a string");
    }
    dir = dir_val->as.s.data;
    file_seq_recurse(S, dir, &items, &len, &cap);
    result = mino_vector(S, items, len);
    free(items);
    return result;
}

/* k_prims_io_core -- print primitives and the printer hooks installed
 * during mino_install_core so the print-method multimethod and the
 * with-out-str / *out* surface in core.clj are available before any
 * code calls pr. Filesystem and process I/O (slurp, spit, exit, file-
 * seq, getenv, getcwd, chdir) stay in k_prims_io for capability-gated
 * installation by mino_install_io. */
const mino_prim_def k_prims_io_core[] = {
    {"pr-builtin",        prim_pr_builtin,
     "Prints a value readably via the built-in C formatter, bypassing print-method."},
    {"set-print-method!", prim_set_print_method_bang,
     "Installs a fn to dispatch pr / prn output; nil removes the hook."},
    {"println",           prim_println,
     "Prints the arguments to *out*, followed by a newline."},
    {"prn",               prim_prn,
     "Prints the arguments readably to *out*, followed by a newline."},
    {"print",             prim_print,
     "Prints the arguments space-separated to *out*, without a trailing newline."},
    {"pr",                prim_pr,
     "Prints the arguments readably to *out*, without a trailing newline."},
    {"newline",           prim_newline,
     "Writes a line separator to *out*."},
    {"flush",             prim_flush,
     "Flushes pending output on *out* and *err*. No-op for string-atom bindings."},
    {"read-line",         prim_read_line,
     "Reads one line from *in*. Returns the line without trailing newline, or nil at EOF."},
    {"read*",             prim_read,
     "Reads one form from *in*. Atom-bound *in* consumes from the head; stdin-backed *in* is unsupported. The user-facing `read` in core.clj dispatches on arity."},
    {"printf",            prim_printf,
     "Formats and prints to *out*: equivalent to (print (apply format fmt args))."},
};

const size_t k_prims_io_core_count =
    sizeof(k_prims_io_core) / sizeof(k_prims_io_core[0]);

const mino_prim_def k_prims_io[] = {
    {"slurp",    prim_slurp,
     "Reads the entire contents of a file as a string."},
    {"spit",     prim_spit,
     "Writes the string content to a file."},
    {"exit",     prim_exit,
     "Exits the process with the given status code."},
    {"time-ms",  prim_time_ms,
     "Returns the current time in milliseconds."},
    {"nano-time", prim_nano_time,
     "Returns monotonic wall-clock time in nanoseconds."},
    {"file-seq", prim_file_seq,
     "Returns a vector of all file paths under a directory, recursively."},
    {"getenv",   prim_getenv,
     "Returns the value of an environment variable, or nil."},
    {"getcwd",   prim_getcwd,
     "Returns the current working directory."},
    {"chdir",    prim_chdir,
     "Changes the current working directory."},
    {"gc-stats", prim_gc_stats,
     "Returns a map of GC statistics."},
    {"gc!",      prim_gc_bang,
     "Forces a full garbage collection. Returns nil."},
};

const size_t k_prims_io_count =
    sizeof(k_prims_io) / sizeof(k_prims_io[0]);

void mino_install_io(mino_state_t *S, mino_env_t *env)
{
    prim_install_table(S, env, "clojure.core", k_prims_io, k_prims_io_count);
}
