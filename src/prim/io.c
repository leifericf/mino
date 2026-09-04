/*
 * io.c -- I/O primitives: println, prn, slurp, spit, exit, time-ms,
 *              nano-time, file-seq, print_str_to helper.
 */

#include "prim/internal.h"
#include "mino.h"
#include "path_buf.h"
#if !defined(_MSC_VER)
#  include <dirent.h>
#  include <sys/stat.h>
#  include <unistd.h>
#else
#  include "win_dirent.h"
#endif
/* POSIX-only host-identity headers: absent on every Windows toolchain
 * (mingw included), so guard on _WIN32, not _MSC_VER. The prims that use
 * them (uname, user-name) carry their own _WIN32 branches below. */
#if !defined(_WIN32)
#  include <sys/utsname.h>
#  include <pwd.h>
#endif
#ifdef _WIN32
#  define WIN32_LEAN_AND_MEAN
#  include <windows.h>
#  include <direct.h>   /* _getcwd, _chdir */
#else
#  include <unistd.h>
#endif

#if !defined(_WIN32) && defined(CLOCK_MONOTONIC)
#  include <time.h>
#endif

void print_str_to(mino_state *S, FILE *out, const mino_val *v)
{
    if (v != NULL && mino_type_of(v) == MINO_STRING) {
        fwrite(v->as.s.data, 1, v->as.s.len, out);
    } else {
        mino_print_to(S, out, v);
    }
}

/* Resolve the current sink for *out* / *err*. Looks up the dynamic-
 * binding stack by the clojure.core var's identity (the stack is
 * keyed by canonical var, so any binding spelling lands there), then
 * falls back to the var's root value. The text probe covers var-less
 * bindings pushed before the var existed (boot-time embedder path).
 * Returns NULL only when the var has never been interned (before
 * mino_install_clojure_core finishes). */
static mino_val *resolve_io_sink(mino_state *S, const char *name)
{
    mino_val *v;
    mino_val *var = var_find(S, "clojure.core", name);
    if (mino_current_ctx(S)->dyn_stack != NULL) {
        v = (var != NULL) ? dyn_lookup_var_or_name(S, var, name)
                          : dyn_lookup(S, name);
        if (v != NULL) return v;
    }
    if (var != NULL && mino_type_of(var) == MINO_VAR && var->as.var.bound) {
        return var->as.var.root;
    }
    return NULL;
}

/* Append `buf` (len bytes) to the string-bearing atom `sink` if sink
 * is a MINO_ATOM holding a MINO_STRING. Returns 1 on capture, 0 if
 * sink is not a string-atom (caller falls back to a FILE*), and -1
 * on OOM. */
static int try_capture_to_atom(mino_state *S, mino_val *sink,
                               const char *buf, size_t len)
{
    mino_val *cur;
    mino_val *new_str;
    char       *combined;
    size_t      cur_len;
    if (sink == NULL || mino_type_of(sink) != MINO_ATOM) return 0;
    cur = sink->as.atom.val;
    if (cur == NULL || mino_type_of(cur) != MINO_STRING) return 0;
    cur_len = cur->as.s.len;
    if (len > SIZE_MAX - cur_len) {
        prim_throw_classified(S, "internal", "MIN001",
            "*out*: out of memory");
        return -1;
    }
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

/* Growable capture buffer behind with-out-str: appends are O(1)
 * amortized (capacity doubles on growth), so n prints into one
 * capture cost O(total bytes), not O(n * buffer). The buffer lives
 * in host memory behind an opaque handle; the string materializes
 * once, on out-buffer-str. */
#define OUT_BUF_TAG "mino/out-buffer"

typedef struct {
    char  *data;  /* host-owned bytes; NULL until the first append */
    size_t len;   /* bytes written */
    size_t cap;   /* bytes allocated */
} out_buf_t;

static void out_buf_finalize(void *ptr, const char *tag)
{
    out_buf_t *b = (out_buf_t *)ptr;
    (void)tag;
    if (b != NULL) {
        free(b->data);
        free(b);
    }
}

static int is_out_buf(const mino_val *v)
{
    return v != NULL && mino_type_of(v) == MINO_HANDLE
        && v->as.handle.tag != NULL
        && strcmp(v->as.handle.tag, OUT_BUF_TAG) == 0
        && v->as.handle.ptr != NULL;
}

/* Allocate a fresh out-buffer handle. Returns NULL with a pending
 * throw on OOM. */
static mino_val *out_buf_make(mino_state *S)
{
    mino_val *hv;
    out_buf_t  *b;
    hv = mino_handle_ex(S, NULL, OUT_BUF_TAG, out_buf_finalize);
    if (hv == NULL) return NULL;
    b = (out_buf_t *)calloc(1, sizeof(*b));
    if (b == NULL) {
        prim_throw_classified(S, "internal", "MIN001",
            "out-buffer: out of memory");
        return NULL;
    }
    hv->as.handle.ptr = b;
    return hv;
}

/* Append len bytes with amortized doubling. Returns 0 on success,
 * -1 with a pending throw on OOM or size overflow. */
static int out_buf_append(mino_state *S, out_buf_t *b,
                          const char *buf, size_t len)
{
    size_t need;
    if (len == 0) return 0;
    if (!checked_add_sz(b->len, len, &need)) goto oom;
    if (need > b->cap) {
        size_t nc = b->cap == 0 ? 256 : b->cap;
        char  *nd;
        while (nc < need) {
            if (!checked_double_sz(nc, &nc)) goto oom;
        }
        nd = (char *)realloc(b->data, nc);
        if (nd == NULL) goto oom;
        b->data = nd;
        b->cap  = nc;
    }
    memcpy(b->data + b->len, buf, len);
    b->len = need;
    return 0;
oom:
    prim_throw_classified(S, "internal", "MIN001",
        "*out*: out of memory");
    return -1;
}

/* Materialize the buffer's current contents as a mino string. The
 * handle must stay rooted by the caller across this call: the string
 * allocation can collect, and an unreachable handle's finalizer
 * would free the bytes mid-copy. */
static mino_val *out_buf_to_string(mino_state *S, mino_val *hv)
{
    out_buf_t *b = (out_buf_t *)hv->as.handle.ptr;
    if (b == NULL || b->len == 0) return mino_string_n(S, "", 0);
    return mino_string_n(S, b->data, b->len);
}

/* Append `buf` to the out-buffer handle `sink`. Returns 1 on capture,
 * 0 if sink is not an out-buffer, and -1 on OOM. */
static int try_capture_to_out_buf(mino_state *S, mino_val *sink,
                                  const char *buf, size_t len)
{
    if (!is_out_buf(sink)) return 0;
    if (out_buf_append(S, (out_buf_t *)sink->as.handle.ptr,
                       buf, len) < 0) {
        return -1;
    }
    return 1;
}

/* Emit `buf` to the sink named by `out_var_name` (`*out*` or `*err*`).
 * Routing:
 *   out-buffer handle    → amortized append (the with-out-str sink)
 *   atom holding string  → append to atom
 *   :mino/stdout         → write to stdout
 *   :mino/stderr         → write to stderr
 *   other / unbound      → write to the default FILE* for the named
 *                          variable (stdout for *out*, stderr for *err*)
 * This means (binding [*out* *err*] (println "x")) routes through
 * stderr because *out* resolves to :mino/stderr.
 * Returns 0 on success, -1 on error. */
static int io_emit(mino_state *S, const char *out_var_name,
                   const char *buf, size_t len)
{
    mino_val *sink;
    int         captured;
    FILE       *fallback;
    sink     = resolve_io_sink(S, out_var_name);
    captured = try_capture_to_out_buf(S, sink, buf, len);
    if (captured == 0) {
        captured = try_capture_to_atom(S, sink, buf, len);
    }
    if (captured < 0) return -1;
    if (captured == 1) return 0;
    fallback = (strcmp(out_var_name, "*err*") == 0) ? stderr : stdout;
    if (sink != NULL && mino_type_of(sink) == MINO_KEYWORD) {
        if (sink->as.s.len == 11
            && memcmp(sink->as.s.data, "mino/stdout", 11) == 0) {
            fallback = stdout;
        } else if (sink->as.s.len == 11
                   && memcmp(sink->as.s.data, "mino/stderr", 11) == 0) {
            fallback = stderr;
        }
    }
    if (len > 0) fwrite(buf, 1, len, fallback);
    /* *flush-on-newline*: when true (the JVM default), flush the
     * stream if the buffer contains a newline. When false, leave the
     * stream buffered so the OS write coalesces. The cached flag is
     * -1 (unresolved) outside a pr/print call; treat as the JVM
     * default in that boundary case. Guard the memchr scan against
     * buf == NULL (UBSan flags applying any offset to a null pointer
     * even for a zero-length read). */
    if (S->flush_on_newline_flag != 0) {
        if (S->flush_on_newline_flag == -1
            || (len > 0 && buf != NULL
                && memchr(buf, '\n', len) != NULL)) {
            fflush(fallback);
        }
    }
    return 0;
}

/* Format one value in print (non-readable) style as a mino string:
 * strings pass through unchanged, chars emit the bare codepoint as
 * UTF-8 (not the \name escape form), and everything else takes the
 * standard printer, which honors the cleared *print-readably* flag
 * for the nested walk. Returns NULL on error. */
static mino_val *print_plain_string(mino_state *S, mino_val *v)
{
    if (v != NULL && mino_type_of(v) == MINO_STRING) {
        return v;
    }
    if (v != NULL && mino_type_of(v) == MINO_CHAR) {
        char   char_buf[4];
        size_t slen;
        int    cp = mino_val_char_get(v);
        if (cp < 0x80) {
            char_buf[0] = (char)cp;
            slen = 1;
        } else if (cp < 0x800) {
            char_buf[0] = (char)(0xC0u | (unsigned)(cp >> 6));
            char_buf[1] = (char)(0x80u | ((unsigned)cp & 0x3Fu));
            slen = 2;
        } else if (cp < 0x10000) {
            char_buf[0] = (char)(0xE0u | (unsigned)(cp >> 12));
            char_buf[1] = (char)(0x80u | (((unsigned)cp >> 6) & 0x3Fu));
            char_buf[2] = (char)(0x80u | ((unsigned)cp & 0x3Fu));
            slen = 3;
        } else {
            char_buf[0] = (char)(0xF0u | (unsigned)(cp >> 18));
            char_buf[1] = (char)(0x80u | (((unsigned)cp >> 12) & 0x3Fu));
            char_buf[2] = (char)(0x80u | (((unsigned)cp >> 6)  & 0x3Fu));
            char_buf[3] = (char)(0x80u | ((unsigned)cp & 0x3Fu));
            slen = 4;
        }
        return mino_string_n(S, char_buf, slen);
    }
    return print_to_string(S, v);
}

/* Format one value through the print-method hook (if installed) or
 * the built-in C formatter, returning the bytes as a mino string.
 * Used by pr/prn when readably=1. Returns NULL on error. */
static mino_val *format_via_hook_or_builtin(mino_state *S,
                                              mino_val *v,
                                              mino_env *env)
{
    if (S->print_method_fn != NULL) {
        /* The hook calls pr-builtin (now routed through *out*) or
         * the user-supplied method. Capture its output by binding
         * *out* to a temporary out-buffer for the duration of the
         * hook call, then return the captured string. The pin keeps
         * the handle rooted through the extraction below: the string
         * allocation can collect, and an unreachable handle's
         * finalizer frees the bytes. */
        mino_val   *sink_buf;
        mino_val   *result;
        mino_val   *call_args;
        dyn_frame_t  *frame;
        dyn_binding_t *binding;
        sink_buf = out_buf_make(S);
        if (sink_buf == NULL) return NULL;
        gc_pin(sink_buf);
        binding = (dyn_binding_t *)malloc(sizeof(*binding));
        if (binding == NULL) {
            gc_unpin(1);
            prim_throw_classified(S, "internal", "MIN001",
                "print: out of memory");
            return NULL;
        }
        binding->var  = var_find(S, "clojure.core", "*out*");
        /* Use the var's interned name when available; fall back to the
         * literal so the pointer is never into a GC-managed buffer. */
        binding->name = (binding->var != NULL)
                        ? binding->var->as.var.sym
                        : "*out*";
        binding->val  = sink_buf;
        binding->next = NULL;
        frame = (dyn_frame_t *)calloc(1, sizeof(*frame));
        if (frame == NULL) {
            free(binding);
            gc_unpin(1);
            prim_throw_classified(S, "internal", "MIN001",
                "print: out of memory");
            return NULL;
        }
        frame->bindings = binding;
        frame->building = 0;
        frame->prev     = mino_current_ctx(S)->dyn_stack;
        mino_current_ctx(S)->dyn_stack    = frame;
        call_args = mino_cons(S, v, mino_nil(S));
        (void)mino_call(S, S->print_method_fn, call_args, env);
        mino_current_ctx(S)->dyn_stack = frame->prev;
        free(binding);
        free(frame);
        if (mino_last_error(S) != NULL) {
            gc_unpin(1);
            return NULL;
        }
        result = out_buf_to_string(S, sink_buf);
        gc_unpin(1);
        return result;
    }
    return print_to_string(S, v);
}

/* Join the formatted args, space-separated with an optional trailing
 * newline, into one GC-owned string. `readably` selects the pr/prn
 * family (strings quoted, chars escaped, print-method hook consulted)
 * versus the print/println family. Two rules keep a throwing
 * print-method (its longjmp lands on the enclosing try pad, skipping
 * every C frame here) from leaking: user code runs only from this
 * loop, and every byte accumulated lives in GC-owned storage (the
 * sink handle's finalizer frees its buffer), so an unwind leaves the
 * collector holding everything. Shared with pr-str (declared in
 * prim/internal.h) so the string form matches routed pr output byte
 * for byte. Returns NULL on error. */
mino_val *print_args_join(mino_state *S, mino_val *args,
                          mino_env *env,
                          int readably, int newline)
{
    mino_val *sink;
    int         first = 1;
    /* *print-readably* is a binding-time override of the entry-point's
     * choice: when the user binds *print-readably* to false, pr/prn
     * fall through to the print/println path. The reverse (a
     * print/println call inside (binding [*print-readably* true] ...))
     * still prints unreadably per canon (only readable forms set the
     * flag). */
    if (readably && !S->print_readably_flag) readably = 0;
    /* print/println bind unreadable printing for the WHOLE value walk,
     * not just top-level strings/chars: nested strings inside
     * collections emit raw content too. The caller's
     * print_dynvars_restore puts the cached flag back. */
    if (!readably) S->print_readably_flag = 0;
    /* One value, no newline: its formatted form is the result. */
    if (mino_is_cons(args) && !mino_is_cons(args->as.cons.cdr)
        && !newline) {
        return readably
            ? format_via_hook_or_builtin(S, args->as.cons.car, env)
            : print_plain_string(S, args->as.cons.car);
    }
    sink = out_buf_make(S);
    if (sink == NULL) return NULL;
    gc_pin(sink);
    while (mino_is_cons(args)) {
        mino_val *v = args->as.cons.car;
        mino_val *formatted = readably
            ? format_via_hook_or_builtin(S, v, env)
            : print_plain_string(S, v);
        if (formatted == NULL) {
            gc_unpin(1);
            return NULL;
        }
        if (!first
            && out_buf_append(S, (out_buf_t *)sink->as.handle.ptr,
                              " ", 1) < 0) {
            gc_unpin(1);
            return NULL;
        }
        if (out_buf_append(S, (out_buf_t *)sink->as.handle.ptr,
                           formatted->as.s.data,
                           formatted->as.s.len) < 0) {
            gc_unpin(1);
            return NULL;
        }
        first = 0;
        args  = args->as.cons.cdr;
    }
    if (newline
        && out_buf_append(S, (out_buf_t *)sink->as.handle.ptr,
                          "\n", 1) < 0) {
        gc_unpin(1);
        return NULL;
    }
    {
        mino_val *result = out_buf_to_string(S, sink);
        gc_unpin(1);
        return result;
    }
}

/* Format args as one chunk and emit through *out*; the single io_emit
 * keeps each pr/println call atomic with respect to sink routing. */
static mino_val *print_args_to_out(mino_state *S, mino_val *args,
                                     mino_env *env,
                                     int readably, int newline)
{
    mino_val *joined;
    print_dynvars_saved_t saved_dynvars;
    print_dynvars_resolve(S, env, &saved_dynvars);
    joined = print_args_join(S, args, env, readably, newline);
    if (joined == NULL) {
        print_dynvars_restore(S, &saved_dynvars);
        return NULL;
    }
    /* Pin across io_emit: a string-atom sink appends by allocating a
     * fresh string, and a collection there must not reclaim the
     * bytes mid-write. Empty joined: null+0 pointer math is UB. */
    gc_pin(joined);
    if (io_emit(S, "*out*",
                joined->as.s.len > 0 ? joined->as.s.data : "",
                joined->as.s.len) < 0) {
        gc_unpin(1);
        print_dynvars_restore(S, &saved_dynvars);
        return NULL;
    }
    gc_unpin(1);
    print_dynvars_restore(S, &saved_dynvars);
    return mino_nil(S);
}

static mino_val *prim_println(mino_state *S, mino_val *args, mino_env *env)
{
    return print_args_to_out(S, args, env, 0, 1);
}

static mino_val *prim_prn(mino_state *S, mino_val *args, mino_env *env)
{
    return print_args_to_out(S, args, env, 1, 1);
}

static mino_val *prim_print(mino_state *S, mino_val *args, mino_env *env)
{
    return print_args_to_out(S, args, env, 0, 0);
}

static mino_val *prim_pr(mino_state *S, mino_val *args, mino_env *env)
{
    return print_args_to_out(S, args, env, 1, 0);
}

/* (pr-builtin x) writes one value via the built-in C formatter, bypassing
 * the print-method hook. Used by print-method's :default method so the
 * default path does not recurse into itself. Routes through *out* so a
 * binding to a capture sink (out-buffer or string-atom) captures, and
 * falls through to stdout otherwise. */
static mino_val *prim_pr_builtin(mino_state *S, mino_val *args, mino_env *env)
{
    mino_val *formatted;
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

/* (out-buffer) — a fresh growable output sink for *out* / *err*.
 * with-out-str binds one, prints append O(1) amortized, and
 * out-buffer-str materializes the accumulated string once. */
static mino_val *prim_out_buffer(mino_state *S, mino_val *args,
                                   mino_env *env)
{
    (void)env;
    if (mino_is_cons(args)) {
        return prim_throw_classified(S, "eval/arity", "MAR001",
            "out-buffer takes no arguments");
    }
    return out_buf_make(S);
}

static mino_val *prim_out_buffer_p(mino_state *S, mino_val *args,
                                     mino_env *env)
{
    (void)env;
    if (!mino_is_cons(args) || mino_is_cons(args->as.cons.cdr)) {
        return prim_throw_classified(S, "eval/arity", "MAR001",
            "out-buffer? requires one argument");
    }
    return is_out_buf(args->as.cons.car) ? mino_true(S)
                                         : mino_false(S);
}

/* Shared argument check for the out-buffer accessors. */
static out_buf_t *out_buf_arg(mino_state *S, mino_val *args,
                              const char *who)
{
    char msg[80];
    if (!mino_is_cons(args) || mino_is_cons(args->as.cons.cdr)) {
        snprintf(msg, sizeof(msg), "%s requires one argument", who);
        prim_throw_classified(S, "eval/arity", "MAR001", msg);
        return NULL;
    }
    if (!is_out_buf(args->as.cons.car)) {
        snprintf(msg, sizeof(msg), "%s: argument must be an out-buffer",
                 who);
        prim_throw_classified(S, "eval/type", "MTY001", msg);
        return NULL;
    }
    return (out_buf_t *)args->as.cons.car->as.handle.ptr;
}

static mino_val *prim_out_buffer_str(mino_state *S, mino_val *args,
                                       mino_env *env)
{
    (void)env;
    if (out_buf_arg(S, args, "out-buffer-str") == NULL) return NULL;
    /* args stays rooted by the caller, so the handle cannot be
     * finalized during the string allocation. */
    return out_buf_to_string(S, args->as.cons.car);
}

/* (out-buffer-line-start? b) — true when the next byte written would
 * start a line: the buffer is empty or ends with a newline. Lets
 * fresh-line stay O(1) over a capture sink. */
static mino_val *prim_out_buffer_line_start_p(mino_state *S,
                                                mino_val *args,
                                                mino_env *env)
{
    out_buf_t *b;
    (void)env;
    b = out_buf_arg(S, args, "out-buffer-line-start?");
    if (b == NULL) return NULL;
    if (b->len == 0 || b->data[b->len - 1] == '\n') {
        return mino_true(S);
    }
    return mino_false(S);
}

/* (set-print-method! fn) — install a late-binding hook for pr / prn.
 * Calling with nil removes the hook. The hook must be a fn that prints
 * its one argument to stdout. */
static mino_val *prim_set_print_method_bang(mino_state *S, mino_val *args,
                                       mino_env *env)
{
    mino_val *fn;
    (void)env;
    if (!mino_is_cons(args) || mino_is_cons(args->as.cons.cdr)) {
        return prim_throw_classified(S, "eval/arity", "MAR001",
            "set-print-method! requires one argument");
    }
    fn = args->as.cons.car;
    if (fn == NULL || mino_type_of(fn) == MINO_NIL) {
        S->print_method_fn = NULL;
        return mino_nil(S);
    }
    if (mino_type_of(fn) != MINO_FN && mino_type_of(fn) != MINO_PRIM) {
        return prim_throw_classified(S, "eval/type", "MTY001",
            "set-print-method! argument must be a fn");
    }
    S->print_method_fn = fn;
    return mino_nil(S);
}

/* (newline) writes a single line separator. Returns nil. */
static mino_val *prim_newline(mino_state *S, mino_val *args, mino_env *env)
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
static mino_val *prim_read_line(mino_state *S, mino_val *args, mino_env *env)
{
    mino_val *src;
    (void)env;
    if (mino_is_cons(args)) {
        return prim_throw_classified(S, "eval/arity", "MAR001",
            "read-line takes no arguments");
    }
    src = resolve_io_sink(S, "*in*");
    if (src != NULL && mino_type_of(src) == MINO_ATOM) {
        mino_val *cur = src->as.atom.val;
        size_t      i;
        size_t      llen;
        size_t      rstart;
        mino_val *line;
        mino_val *rem;
        if (cur == NULL || mino_type_of(cur) != MINO_STRING
            || cur->as.s.len == 0) {
            return mino_nil(S);
        }
        for (i = 0; i < cur->as.s.len; i++) {
            if (cur->as.s.data[i] == '\n') break;
        }
        llen   = i;
        rstart = (i < cur->as.s.len) ? i + 1 : i;
        gc_pin(cur);
        line   = mino_string_n(S, cur->as.s.data, llen);
        if (line == NULL) { gc_unpin(1); return NULL; }
        gc_pin(line);
        rem    = mino_string_n(S, cur->as.s.data + rstart,
                               cur->as.s.len - rstart);
        gc_unpin(1); /* line */
        gc_unpin(1); /* cur */
        if (rem == NULL) return NULL;
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
            {
                size_t need;
                if (!checked_add_sz(len, cl, &need)
                    || !checked_add_sz(need, 1, &need)) {
                    free(buf);
                    return prim_throw_classified(S, "internal", "MIN001",
                        "read-line: buffer size overflow");
                }
                if (need > cap) {
                    size_t nc = cap == 0 ? 256 : cap;
                    char  *nb;
                    while (nc < need) {
                        if (!checked_double_sz(nc, &nc)) {
                            free(buf);
                            return prim_throw_classified(S, "internal", "MIN001",
                                "read-line: buffer size overflow");
                        }
                    }
                    nb = (char *)realloc(buf, nc);
                    if (nb == NULL) {
                        free(buf);
                        return prim_throw_classified(S, "internal", "MIN001",
                            "read-line: out of memory");
                    }
                    buf = nb;
                    cap = nc;
                }
            }
            if (buf != NULL && cl > 0) memcpy(buf + len, chunk, cl);
            len += cl;
            if (has_nl) break;
        }
        if (!saw_any) {
            free(buf);
            return mino_nil(S);
        }
        {
            mino_val *result = mino_string_n(S, buf == NULL ? "" : buf, len);
            free(buf);
            return result;
        }
    }
}

/* (read) reads one form from *in*. Atom-bound *in* is a string
 * cursor: the form is parsed from the head, the atom is updated to
 * the unread tail. Stdin-backed *in* (default) is not supported;
 * use (read-string ...) on a captured input or with-in-str instead. */
static mino_val *prim_read(mino_state *S, mino_val *args, mino_env *env)
{
    mino_val *src;
    (void)env;
    if (mino_is_cons(args)) {
        return prim_throw_classified(S, "eval/arity", "MAR001",
            "read takes no arguments in this build");
    }
    src = resolve_io_sink(S, "*in*");
    if (src != NULL && mino_type_of(src) == MINO_ATOM) {
        mino_val *cur = src->as.atom.val;
        const char *end = NULL;
        mino_val *form;
        mino_val *rem;
        size_t      consumed;
        if (cur == NULL || mino_type_of(cur) != MINO_STRING
            || cur->as.s.len == 0) {
            return mino_nil(S);
        }
        gc_pin(cur);
        form = mino_read(S, cur->as.s.data, &end);
        if (form == NULL) {
            gc_unpin(1);
            if (mino_last_error(S) != NULL) return NULL;
            /* Empty input or whitespace-only. */
            return mino_nil(S);
        }
        gc_pin(form);
        consumed = (end != NULL && end >= cur->as.s.data)
                 ? (size_t)(end - cur->as.s.data) : cur->as.s.len;
        if (consumed > cur->as.s.len) consumed = cur->as.s.len;
        rem = mino_string_n(S, cur->as.s.data + consumed,
                            cur->as.s.len - consumed);
        gc_unpin(1); /* form */
        gc_unpin(1); /* cur */
        if (rem == NULL) return NULL;
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
static mino_val *prim_printf(mino_state *S, mino_val *args, mino_env *env)
{
    mino_val *formatted;
    (void)env;
    if (!mino_is_cons(args)) {
        return prim_throw_classified(S, "eval/arity", "MAR001",
            "printf requires at least a format string");
    }
    formatted = prim_format(S, args, env);
    if (formatted == NULL) return NULL;
    if (mino_type_of(formatted) != MINO_STRING) {
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
static mino_val *prim_flush(mino_state *S, mino_val *args, mino_env *env)
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

/* (slurp path) — read a file's entire contents as a string. I/O
 * capability; only installed by mino_install(S, env, MINO_CAP_IO),
 * not the floor install. */
static mino_val *prim_slurp(mino_state *S, mino_val *args, mino_env *env)
{
    mino_val *path_val;
    const char *path;
    FILE       *f;
    long long   sz;
    size_t      rd;
    char       *buf;
    mino_val *result;
    (void)env;
    if (!mino_is_cons(args) || mino_is_cons(args->as.cons.cdr)) {
        return prim_throw_classified(S, "eval/arity", "MAR001", "slurp requires one argument");
    }
    path_val = args->as.cons.car;
    if (path_val == NULL || mino_type_of(path_val) != MINO_STRING) {
        return prim_throw_classified(S, "eval/type", "MTY001", "slurp: argument must be a string");
    }
    path = path_val->as.s.data;
    f = fopen(path, "rb");
    if (f == NULL) {
        char msg[300];
        snprintf(msg, sizeof(msg), "slurp: cannot open file: %s", path);
        return prim_throw_classified(S, "host", "MHO001", msg);
    }
    /* Seekable sources (regular files) take one sized read. A stream
     * that cannot probe its size (FIFO, pipe, /dev/stdin, process
     * substitution) is read to EOF in chunks instead. */
    sz = -1;
    if (fseek(f, 0, SEEK_END) == 0) {
#if defined(_WIN32) && defined(_MSC_VER)
        sz = _ftelli64(f);
#else
        sz = (long long)ftell(f);
#endif
        if (sz < 0) {
            /* Position is now undefined; rewind for the chunked path. */
            (void)fseek(f, 0, SEEK_SET);
        } else if (fseek(f, 0, SEEK_SET) != 0) {
            sz = -1;
        }
    }
    if (sz >= 0) {
        buf = (char *)malloc((size_t)sz + 1);
        if (buf == NULL) {
            fclose(f);
            return prim_throw_classified(S, "host", "MHO001",
                                         "slurp: out of memory");
        }
        rd = fread(buf, 1, (size_t)sz, f);
        fclose(f);
        buf[rd] = '\0';
        result = mino_string_n(S, buf, rd);
        free(buf);
        return result;
    }
    {
        size_t cap = 16384, len = 0, got;
        buf = (char *)malloc(cap);
        if (buf == NULL) {
            fclose(f);
            return prim_throw_classified(S, "host", "MHO001",
                                         "slurp: out of memory");
        }
        for (;;) {
            if (len + 4097 > cap) {
                char *grown = (char *)realloc(buf, cap * 2);
                if (grown == NULL) {
                    free(buf);
                    fclose(f);
                    return prim_throw_classified(S, "host", "MHO001",
                                                 "slurp: out of memory");
                }
                buf = grown;
                cap *= 2;
            }
            got = fread(buf + len, 1, 4096, f);
            len += got;
            if (got < 4096) {
                if (ferror(f)) {
                    free(buf);
                    fclose(f);
                    return prim_throw_classified(S, "host", "MHO001",
                                                 "slurp: read error");
                }
                break; /* EOF */
            }
        }
        fclose(f);
        buf[len] = '\0';
        result = mino_string_n(S, buf, len);
        free(buf);
        return result;
    }
}

/* Case-insensitive compare of a length-delimited string against a
 * NUL-terminated ASCII literal. Returns 0 on match. */
static int str_ieq_lit(const char *s, size_t len, const char *lit)
{
    size_t i;
    for (i = 0; i < len; i++) {
        char a = s[i], b = lit[i];
        if (b == '\0') return 1;
        if (a >= 'A' && a <= 'Z') a = (char)(a - 'A' + 'a');
        if (b >= 'A' && b <= 'Z') b = (char)(b - 'A' + 'a');
        if (a != b) return 1;
    }
    return lit[len] == '\0' ? 0 : 1;
}

static mino_val *prim_spit(mino_state *S, mino_val *args, mino_env *env)
{
    mino_val *path_val;
    mino_val *content;
    mino_val *opts;
    const char *path;
    FILE       *f;
    int         append = 0;
    (void)env;
    if (!mino_is_cons(args) || !mino_is_cons(args->as.cons.cdr)) {
        return prim_throw_classified(S, "eval/arity", "MAR001", "spit requires two arguments");
    }
    path_val = args->as.cons.car;
    content  = args->as.cons.cdr->as.cons.car;
    /* Trailing option pairs: :append <truthy> selects append mode;
     * :encoding is accepted only for "UTF-8" (the native encoding, so
     * accepting it is a no-op truth rather than a silent lie). Any
     * other key or encoding is rejected loudly. */
    opts = args->as.cons.cdr->as.cons.cdr;
    while (mino_is_cons(opts)) {
        mino_val *k = opts->as.cons.car;
        mino_val *v;
        if (!mino_is_cons(opts->as.cons.cdr)) {
            return prim_throw_dangling_key(S, k);
        }
        v    = opts->as.cons.cdr->as.cons.car;
        opts = opts->as.cons.cdr->as.cons.cdr;
        if (k != NULL && mino_type_of(k) == MINO_KEYWORD
            && k->as.s.len == 6 && memcmp(k->as.s.data, "append", 6) == 0) {
            append = mino_is_truthy(v);
        } else if (k != NULL && mino_type_of(k) == MINO_KEYWORD
                   && k->as.s.len == 8
                   && memcmp(k->as.s.data, "encoding", 8) == 0) {
            if (v == NULL || mino_type_of(v) != MINO_STRING
                || (str_ieq_lit(v->as.s.data, v->as.s.len, "UTF-8") != 0
                    && str_ieq_lit(v->as.s.data, v->as.s.len, "UTF8") != 0)) {
                return prim_throw_classified(S, "eval/contract", "MCT001",
                    "spit: only UTF-8 encoding is supported");
            }
        } else {
            return prim_throw_classified(S, "eval/contract", "MCT001",
                "spit: unknown option (supported: :append, :encoding)");
        }
    }
    if (path_val == NULL || mino_type_of(path_val) != MINO_STRING) {
        return prim_throw_classified(S, "eval/type", "MTY001", "spit: first argument must be a string path");
    }
    path = path_val->as.s.data;
    f = fopen(path, append ? "ab" : "wb");
    if (f == NULL) {
        char msg[300];
        snprintf(msg, sizeof(msg), "spit: cannot open file: %s", path);
        return prim_throw_classified(S, "host", "MHO001", msg);
    }
    if (content != NULL && mino_type_of(content) == MINO_STRING) {
        fwrite(content->as.s.data, 1, content->as.s.len, f);
    } else {
        mino_print_to(S, f, content);
    }
    fclose(f);
    return mino_nil(S);
}

/* (exit code) — terminate the process with the given exit code.
 * Defaults to 0 if no argument is given. */
mino_val *prim_exit(mino_state *S, mino_val *args, mino_env *env)
{
    int code = 0;
    (void)env;
    if (mino_is_cons(args)) {
        mino_val *v = args->as.cons.car;
        if (v != NULL && mino_val_int_p(v)) {
            code = (int)mino_val_int_get(v);
        } else if (v != NULL && mino_type_of(v) == MINO_FLOAT) {
            code = (int)v->as.f;
        }
    }
    /* Join outstanding host worker threads before libc teardown so
     * leaked threads don't trip TSan (or, on Windows, DllMain
     * teardown ordering). State teardown via mino_state_free also
     * calls quiesce, but `(exit ...)` used to bypass that path
     * entirely -- every finalizer-bearing value alive at exit
     * (sockets, chans, futures, bigint payloads) leaked to the OS
     * and LeakSanitizer reported the abandoned handles. Now the
     * drained path runs the full state teardown so finalizers fire
     * exactly as on a normal return from main.
     *
     * Use a *bounded* drain: a future whose body is an uninterruptible
     * tight C loop (e.g. a C-side reduce over a huge range) never
     * reaches a cooperative-cancel safepoint, so an unbounded join
     * here hangs the process forever -- the failure the per-host JIT
     * canary intermittently hit on slow/emulated runners. Well-behaved
     * workers observe the cancel and drain within milliseconds; only a
     * genuinely stuck worker hits the grace window. If one does, we
     * abandon it and _Exit: the process is terminating, so the OS
     * reclaims the thread, and skipping libc teardown avoids racing a
     * still-running worker against stdio/atexit teardown. Flush first
     * since _Exit does not. */
    if (mino_quiesce_threads_timed(S, 3000)) {
        mino_quiesce_threads(S); /* all drained; reap joinable threads */
        /* Drop this thread's recursive holds so state teardown can
         * destroy the state lock; the abandoned eval frames above
         * never resume, so the depth is not restored. */
        while (mino_current_ctx(S)->lock_depth > 0) {
            mino_state_lock_release(S);
            mino_current_ctx(S)->lock_depth--;
        }
        mino_state_free(S);
        exit(code);
    }
    fflush(stdout);
    fflush(stderr);
    _Exit(code);
    return mino_nil(S); /* unreachable */
}

/* (time-ms) — return monotonic wall-clock time in milliseconds as a
 * float. The `(time expr)` macro in core.clj builds on this to print
 * elapsed wall-clock; Clojure's `(time)` contract is wall-clock, and
 * task runners need wall-clock too (a thread-sleep that took 200ms
 * should report ~200ms, not ~0ms because the calling thread spent
 * no CPU time during the sleep).
 *
 * Previously this returned clock() / CLOCKS_PER_SEC, i.e. process
 * CPU time. That made `(time (thread-sleep 200))` print "0.194 ms"
 * and any wall-clock benchmarking that built on (time-ms) silently
 * undercounted by however long the thread spent blocked. */
static mino_val *prim_time_ms(mino_state *S, mino_val *args, mino_env *env)
{
    (void)args;
    (void)env;
    if (mino_is_cons(args)) {
        return prim_throw_classified(S, "eval/arity", "MAR001", "time-ms takes no arguments");
    }
    return mino_float(S, (double)mino_monotonic_ns() / 1.0e6);
}

/* (nano-time) — return monotonic wall-clock time in nanoseconds as an integer. */
mino_val *prim_nano_time(mino_state *S, mino_val *args, mino_env *env)
{
    (void)env;
    if (mino_is_cons(args)) {
        return prim_throw_classified(S, "eval/arity", "MAR001",
                                     "nano-time takes no arguments");
    }
    return mino_int(S, mino_monotonic_ns());
}

/* (uname) -- operating system identification in the os.uname shape:
 * a map of :sysname, :nodename, :release, :version, and :machine.
 * On Windows the release/version/machine fields are empty strings
 * (no stable query exists for them under the supported toolchain). */
static mino_val *prim_uname(mino_state *S, mino_val *args, mino_env *env)
{
    mino_val *ks[5], *vs[5];
    (void)env;
    if (mino_is_cons(args)) {
        return prim_throw_classified(S, "eval/arity", "MAR001",
                                     "uname takes no arguments");
    }
#ifdef _WIN32
    {
        char node[256] = "";
        DWORD node_len = (DWORD)sizeof(node);
        if (!GetComputerNameExA(ComputerNameDnsHostname,
                                node, &node_len)) {
            node[0] = '\0';
        }
        ks[0] = mino_keyword(S, "sysname");
        vs[0] = mino_string(S, "Windows");
        ks[1] = mino_keyword(S, "nodename");
        vs[1] = mino_string(S, node);
        ks[2] = mino_keyword(S, "release");
        vs[2] = mino_string(S, "");
        ks[3] = mino_keyword(S, "version");
        vs[3] = mino_string(S, "");
        ks[4] = mino_keyword(S, "machine");
        vs[4] = mino_string(S, "");
    }
#else
    {
        struct utsname u;
        if (uname(&u) != 0) {
            return prim_throw_classified(S, "host", "MHO001",
                                         "uname: failed to query host");
        }
        ks[0] = mino_keyword(S, "sysname");
        vs[0] = mino_string(S, u.sysname);
        ks[1] = mino_keyword(S, "nodename");
        vs[1] = mino_string(S, u.nodename);
        ks[2] = mino_keyword(S, "release");
        vs[2] = mino_string(S, u.release);
        ks[3] = mino_keyword(S, "version");
        vs[3] = mino_string(S, u.version);
        ks[4] = mino_keyword(S, "machine");
        vs[4] = mino_string(S, u.machine);
    }
#endif
    return mino_map(S, ks, vs, 5);
}

/* (user-name) -- the current effective user's login name. POSIX uses
 * the passwd database for the effective uid; Windows reads the
 * USERNAME environment variable. */
static mino_val *prim_user_name(mino_state *S, mino_val *args, mino_env *env)
{
    (void)env;
    if (mino_is_cons(args)) {
        return prim_throw_classified(S, "eval/arity", "MAR001",
                                     "user-name takes no arguments");
    }
#ifdef _WIN32
    {
        const char *u = getenv("USERNAME");
        if (u == NULL || u[0] == '\0') {
            return prim_throw_classified(S, "host", "MHO001",
                                         "user-name: USERNAME is not set");
        }
        return mino_string(S, u);
    }
#else
    {
        struct passwd *pw = getpwuid(geteuid());
        if (pw == NULL || pw->pw_name == NULL || pw->pw_name[0] == '\0') {
            return prim_throw_classified(S, "host", "MHO001",
                                         "user-name: cannot determine current user");
        }
        return mino_string(S, pw->pw_name);
    }
#endif
}

/* (getcwd) -- return the current working directory as a string. */
static mino_val *prim_getcwd(mino_state *S, mino_val *args, mino_env *env)
{
    char buf[PATH_BUF_CAP];
    (void)env;
    if (mino_is_cons(args)) {
        return prim_throw_classified(S, "eval/arity", "MAR001",
                                     "getcwd takes no arguments");
    }
#ifdef _WIN32
    if (_getcwd(buf, (int)sizeof(buf)) == NULL) {
#else
    if (getcwd(buf, sizeof(buf)) == NULL) {
#endif
        return prim_throw_classified(S, "io", "MIO001",
                                     "getcwd: failed to get working directory");
    }
    return mino_string(S, buf);
}

/* (chdir path) -- change current working directory. Returns nil. */
static mino_val *prim_chdir(mino_state *S, mino_val *args, mino_env *env)
{
    mino_val *path_val;
    (void)env;
    if (!mino_is_cons(args) || mino_is_cons(args->as.cons.cdr)) {
        return prim_throw_classified(S, "eval/arity", "MAR001",
                                     "chdir requires one argument");
    }
    path_val = args->as.cons.car;
    if (path_val == NULL || mino_type_of(path_val) != MINO_STRING) {
        return prim_throw_classified(S, "eval/type", "MTY001",
                                     "chdir: argument must be a string");
    }
#ifdef _WIN32
    if (_chdir(path_val->as.s.data) != 0) {
#else
    if (chdir(path_val->as.s.data) != 0) {
#endif
        return prim_throw_classified(S, "io", "MIO001",
                                     "chdir: directory not found");
    }
    return mino_nil(S);
}

/* (getenv name) -- return environment variable value or nil. */
mino_val *prim_getenv(mino_state *S, mino_val *args, mino_env *env)
{
    mino_val *name_val;
    const char *val;
    (void)env;
    if (!mino_is_cons(args) || mino_is_cons(args->as.cons.cdr)) {
        return prim_throw_classified(S, "eval/arity", "MAR001",
                                     "getenv requires one argument");
    }
    name_val = args->as.cons.car;
    if (name_val == NULL || mino_type_of(name_val) != MINO_STRING) {
        return prim_throw_classified(S, "eval/type", "MTY001",
                                     "getenv: argument must be a string");
    }
    val = getenv(name_val->as.s.data);
    if (val == NULL) return mino_nil(S);
    return mino_string(S, val);
}

/* ---- file-seq: recursive directory listing ---- */

/* file_seq_recurse -- accumulate file paths into a GC_T_VALARR buffer.
 * *items is a GC-allocated array (or NULL); the caller pins it before
 * each call so the GC can trace the already-stored strings. */
static void file_seq_recurse(mino_state *S, const char *dir,
                             mino_val ***items, size_t *len, size_t *cap)
{
    DIR *d = opendir(dir);
    struct dirent *ent;
    if (d == NULL) return;
    while ((ent = readdir(d)) != NULL) {
        char path[PATH_BUF_CAP];
        struct stat st;
        if (ent->d_name[0] == '.') continue;
        {
            int sn = snprintf(path, sizeof(path), "%s/%s", dir, ent->d_name);
            if (sn < 0 || (size_t)sn >= sizeof(path)) continue; /* truncated */
        }
#ifdef _WIN32
        if (stat(path, &st) != 0) continue;   /* lstat unavailable on Windows */
#else
        if (lstat(path, &st) != 0) continue;  /* don't follow symlinks */
#endif
        if (S_ISDIR(st.st_mode)) {
            file_seq_recurse(S, path, items, len, cap);
        } else {
            mino_val *str;
            if (*len == *cap) {
                size_t nc;
                mino_val **nb;
                size_t j;
                if (*cap == 0) {
                    nc = 64;
                } else if (!checked_double_sz(*cap, &nc)) {
                    closedir(d); return;
                }
                /* Pin old array across the allocation so its entries survive. */
                if (*items != NULL) gc_pin((mino_val *)*items);
                nb = (mino_val **)gc_alloc_typed(
                    S, GC_T_VALARR, nc * sizeof(*nb));
                if (*items != NULL) gc_unpin(1);
                if (nb == NULL) { closedir(d); return; }
                for (j = 0; j < *len; j++) nb[j] = (*items)[j];
                *items = nb;
                *cap = nc;
            }
            /* Pin the array across mino_string so already-stored entries
             * are reachable by the GC if a minor collection fires. */
            if (*items != NULL) gc_pin((mino_val *)*items);
            str = mino_string(S, path);
            if (*items != NULL) gc_unpin(1);
            if (str == NULL) { closedir(d); return; }
            gc_valarr_set(S, *items, *len, str);
            (*len)++;
        }
    }
    closedir(d);
}

static mino_val *prim_file_seq(mino_state *S, mino_val *args, mino_env *env)
{
    mino_val *dir_val;
    const char *dir;
    mino_val **items = NULL;
    size_t len = 0, cap = 0;
    mino_val *result;
    (void)env;
    if (!mino_is_cons(args) || mino_is_cons(args->as.cons.cdr)) {
        return prim_throw_classified(S, "eval/arity", "MAR001",
                                     "file-seq requires one argument");
    }
    dir_val = args->as.cons.car;
    if (dir_val == NULL || mino_type_of(dir_val) != MINO_STRING) {
        return prim_throw_classified(S, "eval/type", "MTY001",
                                     "file-seq: argument must be a string");
    }
    dir = dir_val->as.s.data;
    file_seq_recurse(S, dir, &items, &len, &cap);
    /* items is GC-owned (gc_alloc_typed); do not free it. */
    result = mino_vector(S, items, len);
    return result;
}

/* k_prims_io_core -- print primitives and the printer hooks installed
 * by the floor (FLOOR-domain) install so the print-method multimethod
 * and the with-out-str / *out* surface in core.clj are available
 * before any code calls pr. Filesystem and process I/O (slurp, spit,
 * exit, file-seq, getenv, getcwd, chdir) stay in k_prims_io for
 * capability-gated installation by mino_install(S, env, MINO_CAP_IO). */
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
    {"out-buffer",        prim_out_buffer,
     "Returns a fresh growable output sink for *out*; prints into it append in amortized constant time."},
    {"out-buffer?",       prim_out_buffer_p,
     "Returns true if x is an out-buffer sink."},
    {"out-buffer-str",    prim_out_buffer_str,
     "Returns the accumulated contents of an out-buffer as a string."},
    {"out-buffer-line-start?", prim_out_buffer_line_start_p,
     "Returns true when an out-buffer is empty or ends with a newline."},
    {"flush",             prim_flush,
     "Flushes pending output on *out* and *err*. No-op for capture sinks (out-buffers and string atoms)."},
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
    {"uname",    prim_uname,
     "Returns a map of host identity: :sysname, :nodename, :release, :version, :machine."},
    {"user-name", prim_user_name,
     "Returns the current effective user's login name."},
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

void mino_install_io(mino_state *S, mino_env *env)
{
    mino_env *core_env = ns_env_ensure(S, "clojure.core");
    (void)env;
    prim_install_table_with_capability(S, core_env, "clojure.core",
                                       k_prims_io, k_prims_io_count, "io");
    S->caps_installed |= MINO_CAP_IO;
}
