/*
 * path.c -- path string algebra and the pure glob matcher (ADR 22).
 *
 * Vocabulary: paths are STRINGS, never a path type. The prims are
 * total byte-level functions: no filesystem contact, no locale,
 * no encoding assumptions (filenames are bytes). The one impure
 * resident is expand-home (a ~ lookup through the environment);
 * the glob walker joins this file in a later commit.
 *
 * Separators: '/' is canonical in every output. '\' is accepted in
 * every path input and folded at each prim's edge (the Windows v1
 * stance; no drive letters, no UNC). The pattern side of
 * path-glob-match never folds: patterns are user-written and '\'
 * in a pattern is an escape.
 *
 * Edge rules are the cross-language majority pinned by the
 * path-lib PoCs and ADR 22: extension carries the dot and dotfiles
 * answer "", join does not reset on an absolute segment, normalize
 * is lexical Go-Clean style (trailing separator strips, .. after
 * the root drops, leading .. on relative paths stays), basename is
 * the raw last segment (no .. resolution, node parity), dirname is
 * the cleaned prefix.
 *
 * Capabilities (ADR 22): k_prims_path (the algebra and the pure
 * matcher) installs in the floor like time -- string math reads
 * nothing and mutates nothing.
 */

#define _POSIX_C_SOURCE 200809L

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
#include <stdlib.h>
#include <string.h>

/* ---- shared helpers --------------------------------------------------- */

/* Folds '\' to '/' in place. Paths only, never patterns. */
static void fold_backslashes(char *s)
{
    for (; *s != '\0'; s++) {
        if (*s == '\\') *s = '/';
    }
}

/* Copies a mino string into a malloc'd, fold-normalized C string.
 * Returns NULL only on OOM (empty strings yield ""). */
static char *path_strdup_folded(const mino_val *v)
{
    size_t n = v->as.s.len;
    char *out = (char *)malloc(n + 1);
    if (out == NULL) return NULL;
    memcpy(out, v->as.s.data, n);
    out[n] = '\0';
    fold_backslashes(out);
    return out;
}

/* Exactly one argument, a string. On failure throws and returns
 * NULL with *ok cleared. */
static char *path_arg_string(mino_state *S, mino_val *args, const char *who,
                             int *ok)
{
    mino_val *v;
    if (!mino_is_cons(args) || mino_is_cons(args->as.cons.cdr)) {
        char msg[128];
        *ok = 0;
        snprintf(msg, sizeof(msg), "%s requires one argument", who);
        prim_throw_classified(S, "eval/arity", "MAR001", msg);
        return NULL;
    }
    v = args->as.cons.car;
    if (v == NULL || mino_type_of(v) != MINO_STRING) {
        char msg[128];
        *ok = 0;
        snprintf(msg, sizeof(msg), "%s: argument must be a string", who);
        prim_throw_classified(S, "eval/type", "MTY001", msg);
        return NULL;
    }
    *ok = 1;
    return path_strdup_folded(v);
}

/* ---- lexical clean (the normalize core) -------------------------------- */

/* Segment stack entry: offset and length into the source. */
typedef struct {
    size_t off, len;
} seg_span_t;

/* Lexical Clean over a folded path: collapse //, drop . segments,
 * cancel inner .. against a non-.. parent, drop .. after the root,
 * strip the trailing separator (root stays /). "" cleans to ".".
 * Writes into out (must hold strlen(src) + 2 bytes); the output is
 * never longer than the input after folding. Returns 0, or -1 on
 * bad caps (cannot happen with the +2 contract). */
static int path_clean(const char *src, size_t n, char *out, size_t cap)
{
    size_t stackbuf[64];
    size_t nsegs = 0, maxsegs, i = 0, o = 0, k;
    seg_span_t *segs;
    int absolute = (n > 0 && src[0] == '/');

    if (cap < n + 2) return -1;
    maxsegs = n / 2 + 1;
    segs = (maxsegs <= 64)
        ? (seg_span_t *)(void *)stackbuf
        : (seg_span_t *)malloc(maxsegs * sizeof(*segs));
    if (segs == NULL) return -1;

    while (i < n) {
        size_t start;
        while (i < n && src[i] == '/') i++;
        start = i;
        while (i < n && src[i] != '/') i++;
        if (i == start) break;
        if (i - start == 1 && src[start] == '.') continue;
        if (i - start == 2 && src[start] == '.' && src[start + 1] == '.') {
            if (nsegs > 0) {
                seg_span_t *top = &segs[nsegs - 1];
                int top_dd = top->len == 2 && src[top->off] == '.'
                             && src[top->off + 1] == '.';
                if (!top_dd) {
                    nsegs--;                 /* cancel the parent */
                    continue;
                }
            }
            if (absolute) continue;          /* .. after root drops */
        }
        if (nsegs == maxsegs) {              /* unreachable; bounds guard */
            if ((void *)segs != (void *)stackbuf) free(segs);
            return -1;
        }
        segs[nsegs].off = start;
        segs[nsegs].len = i - start;
        nsegs++;
    }

    if (absolute) out[o++] = '/';
    for (k = 0; k < nsegs; k++) {
        if (k > 0) out[o++] = '/';
        memcpy(out + o, src + segs[k].off, segs[k].len);
        o += segs[k].len;
    }
    if (o == 0) out[o++] = absolute ? '/' : '.';
    out[o] = '\0';
    if ((void *)segs != (void *)stackbuf) free(segs);
    return 0;
}

/* Clean into a stack buffer when it fits, else malloc. Sets *heap
 * when the buffer was malloc'd (caller frees). Returns NULL on
 * OOM. */
static char *path_clean_buf(const char *s, size_t n,
                            char *stack, size_t stack_cap, char **heap)
{
    size_t cap = n + 2;
    char *dst;
    *heap = NULL;
    if (cap <= stack_cap) {
        dst = stack;
    } else {
        dst = (char *)malloc(cap);
        *heap = dst;
        if (dst == NULL) return NULL;
    }
    if (path_clean(s, n, dst, cap) != 0) {
        if (*heap != NULL) { free(*heap); *heap = NULL; }
        return NULL;
    }
    return dst;
}

/* ---- pure prims -------------------------------------------------------- */

/* (path-normalize s) */
static mino_val *prim_path_normalize(mino_state *S, mino_val *args,
                                     mino_env *env)
{
    char stack[PATH_BUF_CAP];
    char *heap, *dst, *s;
    size_t n;
    mino_val *result;
    int ok = 1;
    (void)env;

    s = path_arg_string(S, args, "path-normalize", &ok);
    if (!ok) return NULL;
    n = strlen(s);
    dst = path_clean_buf(s, n, stack, sizeof(stack), &heap);
    if (dst == NULL) { free(s); return mino_nil(S); }
    result = mino_string(S, dst);
    free(heap);
    free(s);
    return result;
}

/* (path-join & parts) -- concat with / then clean. nil and empty
 * parts skip; an absolute part does not reset (ADR 22). */
static mino_val *prim_path_join(mino_state *S, mino_val *args, mino_env *env)
{
    size_t total = 0, o = 0;
    char *joined, stack[PATH_BUF_CAP], *heap, *dst;
    mino_val *a = args;
    mino_val *result;
    (void)env;

    while (a != NULL && mino_is_cons(a)) {
        mino_val *v = a->as.cons.car;
        if (v == NULL || mino_type_of(v) == MINO_NIL) {
            a = a->as.cons.cdr;
            continue;
        }
        if (mino_type_of(v) != MINO_STRING) {
            prim_throw_classified(S, "eval/type", "MTY001",
                                  "path-join: parts must be strings or nil");
            return NULL;
        }
        total += v->as.s.len + 1;
        a = a->as.cons.cdr;
    }
    if (total == 0) return mino_string(S, ".");
    joined = (char *)malloc(total + 1);
    if (joined == NULL) return mino_nil(S);

    a = args;
    while (a != NULL && mino_is_cons(a)) {
        mino_val *v = a->as.cons.car;
        size_t len, j;
        if (v == NULL || mino_type_of(v) == MINO_NIL) {
            a = a->as.cons.cdr;
            continue;
        }
        len = v->as.s.len;
        if (o > 0) joined[o++] = '/';
        for (j = 0; j < len; j++) {
            char c = v->as.s.data[j];
            joined[o++] = (c == '\\') ? '/' : c;
        }
        a = a->as.cons.cdr;
    }
    joined[o] = '\0';

    dst = path_clean_buf(joined, o, stack, sizeof(stack), &heap);
    free(joined);
    if (dst == NULL) return mino_nil(S);
    result = mino_string(S, dst);
    free(heap);
    return result;
}

/* (path-split s) -- raw segments, empty dropped, leading / kept
 * as a "/" element; "." and ".." pass through unresolved. */
static mino_val *prim_path_split(mino_state *S, mino_val *args, mino_env *env)
{
    char *s;
    size_t n, i = 0;
    mino_vec_builder *b;
    mino_val *result;
    int ok = 1;
    (void)env;

    s = path_arg_string(S, args, "path-split", &ok);
    if (!ok) return NULL;
    n = strlen(s);
    b = mino_vector_builder_new(S);
    if (b == NULL) { free(s); return mino_nil(S); }
    if (n > 0 && s[0] == '/') {
        mino_vector_builder_push(b, mino_string_n(S, "/", 1));
    }
    while (i < n) {
        size_t start;
        while (i < n && s[i] == '/') i++;
        start = i;
        while (i < n && s[i] != '/') i++;
        if (i > start) {
            mino_vector_builder_push(
                b, mino_string_n(S, s + start, i - start));
        }
    }
    result = mino_vector_builder_finish(b);
    free(s);
    return result;
}

/* (path-basename s) -- raw last non-empty segment; "/" answers
 * ""; "" answers ".". No .. resolution (node parity). */
static mino_val *prim_path_basename(mino_state *S, mino_val *args,
                                    mino_env *env)
{
    char *s;
    size_t n, end, start;
    mino_val *result;
    int ok = 1;
    (void)env;

    s = path_arg_string(S, args, "path-basename", &ok);
    if (!ok) return NULL;
    n = strlen(s);
    if (n == 0) { free(s); return mino_string(S, "."); }
    end = n;
    while (end > 0 && s[end - 1] == '/') end--;
    if (end == 0) { free(s); return mino_string(S, ""); }
    start = end;
    while (start > 0 && s[start - 1] != '/') start--;
    result = mino_string_n(S, s + start, end - start);
    free(s);
    return result;
}

/* (path-dirname s) -- the cleaned prefix: clean the path, cut at
 * the last separator. No separator answers "."; root answers "/". */
static mino_val *prim_path_dirname(mino_state *S, mino_val *args,
                                   mino_env *env)
{
    char *s, stack[PATH_BUF_CAP], *heap, *dst;
    size_t n, dn, cut;
    mino_val *result;
    int ok = 1;
    (void)env;

    s = path_arg_string(S, args, "path-dirname", &ok);
    if (!ok) return NULL;
    n = strlen(s);
    dst = path_clean_buf(s, n, stack, sizeof(stack), &heap);
    if (dst == NULL) { free(s); return mino_nil(S); }
    dn = strlen(dst);
    if (dn == 1 && dst[0] == '/') {
        result = mino_string(S, "/");
    } else {
        cut = dn;
        while (cut > 0 && dst[cut - 1] != '/') cut--;
        if (cut == 0) {
            result = mino_string(S, ".");
        } else if (cut == 1) {
            result = mino_string(S, "/");
        } else {
            result = mino_string_n(S, dst, cut - 1);
        }
    }
    free(heap);
    free(s);
    return result;
}

/* Last '.' inside the final segment, or n when there is no
 * extension. A leading run of dots belongs to the name (dotfiles,
 * ".", ".."), so the extension dot must sit after it. */
static size_t ext_dot_pos(const char *s, size_t n)
{
    size_t base_end = n, base_start, lead = 0, i, dot = n;
    while (base_end > 0 && s[base_end - 1] == '/') base_end--;
    base_start = base_end;
    while (base_start > 0 && s[base_start - 1] != '/') base_start--;
    if (base_end == base_start) return n;            /* "" basename */
    while (base_start + lead < base_end && s[base_start + lead] == '.') {
        lead++;
    }
    for (i = base_end; i-- > base_start + lead;) {
        if (s[i] == '.') { dot = i; break; }
    }
    if (dot == n) return n;                          /* none */
    return dot;
}

/* (path-extension s) -- ".gz" with the dot, last dot only,
 * dotfiles answer "". */
static mino_val *prim_path_extension(mino_state *S, mino_val *args,
                                     mino_env *env)
{
    char *s;
    size_t n, dot;
    mino_val *result;
    int ok = 1;
    (void)env;

    s = path_arg_string(S, args, "path-extension", &ok);
    if (!ok) return NULL;
    n = strlen(s);
    dot = ext_dot_pos(s, n);
    result = (dot == n) ? mino_string(S, "")
                        : mino_string_n(S, s + dot, n - dot);
    free(s);
    return result;
}

/* (path-split-ext s) -- [stem ext] over the whole path (os.path
 * shape); ext nil when absent (dotfiles included). */
static mino_val *prim_path_split_ext(mino_state *S, mino_val *args,
                                     mino_env *env)
{
    char *s;
    size_t n, dot;
    mino_val *items[2];
    int ok = 1;
    (void)env;

    s = path_arg_string(S, args, "path-split-ext", &ok);
    if (!ok) return NULL;
    n = strlen(s);
    dot = ext_dot_pos(s, n);
    items[0] = (dot == n) ? mino_string_n(S, s, n)
                          : mino_string_n(S, s, dot);
    items[1] = (dot == n) ? mino_nil(S)
                          : mino_string_n(S, s + dot, n - dot);
    free(s);
    return mino_vector(S, items, 2);
}

/* (path-stem s) -- basename minus the last extension (the pathlib
 * property as a plain function; ADR 22). */
static mino_val *prim_path_stem(mino_state *S, mino_val *args, mino_env *env)
{
    char *s;
    size_t n, base_end, base_start;
    mino_val *result;
    int ok = 1;
    (void)env;

    s = path_arg_string(S, args, "path-stem", &ok);
    if (!ok) return NULL;
    n = strlen(s);
    base_end = n;
    while (base_end > 0 && s[base_end - 1] == '/') base_end--;
    base_start = base_end;
    while (base_start > 0 && s[base_start - 1] != '/') base_start--;
    if (base_end == base_start) {
        /* no basename: "" answers ".", all-separator answers "" */
        result = (n == 0) ? mino_string(S, ".") : mino_string(S, "");
        free(s);
        return result;
    }
    {
        size_t lead = 0, d = base_end;
        /* the leading dot run (dotfiles, "..") belongs to the name */
        while (base_start + lead < base_end
               && s[base_start + lead] == '.') {
            lead++;
        }
        while (d-- > base_start + lead) {
            if (s[d] == '.') break;
        }
        /* d+1 == base_start + lead means no dot past the run:
         * the whole basename is the stem */
        if (d + 1 == base_start + lead) {
            result = mino_string_n(S, s + base_start,
                                   base_end - base_start);
        } else {
            result = mino_string_n(S, s + base_start, d - base_start);
        }
    }
    free(s);
    return result;
}

/* (path-absolute? s) -- leading / after folding. */
static mino_val *prim_path_absolute_p(mino_state *S, mino_val *args,
                                      mino_env *env)
{
    char *s;
    mino_val *result;
    int ok = 1;
    (void)env;

    s = path_arg_string(S, args, "path-absolute?", &ok);
    if (!ok) return NULL;
    result = (s[0] == '/') ? mino_true(S) : mino_false(S);
    free(s);
    return result;
}

/* (path-expand-home s) -- lone ~ or ~/ prefix through HOME
 * (POSIX) or USERPROFILE then HOMEDRIVE+HOMEPATH (Windows).
 * ~otheruser passes through (no pwd.h; Elixir parity). No ~
 * prefix or unset home: unchanged. */
static mino_val *prim_path_expand_home(mino_state *S, mino_val *args,
                                       mino_env *env)
{
    mino_val *v;
    const char *s;
    size_t n, hl, k, o;
    const char *home = NULL;
    char *home_buf = NULL, *out;
    mino_val *result;
    (void)env;

    if (!mino_is_cons(args) || mino_is_cons(args->as.cons.cdr)) {
        prim_throw_classified(S, "eval/arity", "MAR001",
                              "path-expand-home requires one argument");
        return NULL;
    }
    v = args->as.cons.car;
    if (v == NULL || mino_type_of(v) != MINO_STRING) {
        prim_throw_classified(S, "eval/type", "MTY001",
                              "path-expand-home: argument must be a string");
        return NULL;
    }
    s = v->as.s.data;
    n = v->as.s.len;
    if (n == 0 || s[0] != '~') return mino_string_n(S, s, n);
    if (n >= 2 && s[1] != '/' && s[1] != '\\') {
        return mino_string_n(S, s, n);   /* ~user passes through */
    }
#ifdef _WIN32
    home = getenv("USERPROFILE");
    if (home == NULL) {
        const char *drive = getenv("HOMEDRIVE");
        const char *dir = getenv("HOMEPATH");
        if (drive != NULL && dir != NULL) {
            size_t dl = strlen(drive), dnl = strlen(dir);
            home_buf = (char *)malloc(dl + dnl + 1);
            if (home_buf == NULL) return mino_nil(S);
            memcpy(home_buf, drive, dl);
            memcpy(home_buf + dl, dir, dnl);
            home_buf[dl + dnl] = '\0';
            home = home_buf;
        }
    }
#else
    home = getenv("HOME");
#endif
    if (home == NULL || home[0] == '\0') {
        free(home_buf);
        return mino_string_n(S, s, n);
    }
    hl = strlen(home);
    out = (char *)malloc(hl + n + 1);
    if (out == NULL) { free(home_buf); return mino_nil(S); }
    memcpy(out, home, hl);
    o = hl;
    k = 1;
    while (k < n && (s[k] == '/' || s[k] == '\\')) k++;
    if (k < n) {
        out[o++] = '/';
        for (; k < n; k++) {
            out[o++] = (s[k] == '\\') ? '/' : s[k];
        }
    }
    out[o] = '\0';
    /* the assembled path is a construction, so it cleans (the same
     * concat-then-clean rule join uses; handles "~/x//y") */
    {
        char stack[PATH_BUF_CAP], *heap, *dst;
        dst = path_clean_buf(out, o, stack, sizeof(stack), &heap);
        if (dst != NULL) {
            result = mino_string(S, dst);
            free(heap);
        } else {
            result = mino_string_n(S, out, o);
        }
    }
    free(out);
    free(home_buf);
    return result;
}


/* ---- pure glob matcher ------------------------------------------------- */

/* Byte matcher for one path: * (within a segment), ? (one char,
 * not /), [class] with ranges and ! negation, {a,b} alternation
 * (nested braces allowed, top-level commas split), backslash
 * escape, ** only as a whole segment (zero or more directories;
 * a trailing ** matches everything left). '/' never matches a
 * wildcard. Single pass, no compilation; recursion is bounded by
 * the 256-byte pattern cap and the depth guard. */

#define GLOB_MAX_PATTERN 256
#define GLOB_MAX_DEPTH   128

typedef struct {
    const char *p;      /* pattern cursor */
    const char *pend;
    const char *s;      /* string cursor */
    const char *send;
    const char *p0;     /* pattern start (for whole-segment checks) */
    int depth;
} glob_ctx;

static int glob_match_here(glob_ctx *g);

/* Full match of sub-pattern [sp, spend) against s[0..k); helper
 * for brace alternatives whose end position is ambiguous. */
static int glob_match_exact(const char *sp, const char *spend,
                            const char *s, size_t k, const char *p0,
                            int depth)
{
    glob_ctx sub;
    sub.p = sp;
    sub.pend = spend;
    sub.s = s;
    sub.send = s + k;
    sub.p0 = p0;
    sub.depth = depth;
    return glob_match_here(&sub) && sub.p == sub.pend && sub.s == sub.send;
}

/* Try the alternatives of a brace group whose body starts at body
 * and whose matching close brace is at close. g->p points at '{'.
 * On success g is left past '}' at the consumed position. */
static int glob_brace_try(glob_ctx *g, const char *body, const char *close)
{
    const char *alt_start = body;
    const char *p;
    const char *rest = close + 1;
    size_t maxlen = (size_t)(g->send - g->s);
    size_t k;
    int depth = 1;

    for (p = body; p <= close; p++) {
        int at_close = (p == close);
        if (!at_close) {
            if (*p == '\\') { p++; continue; }
            if (*p == '{') { depth++; continue; }
            if (*p == '}') { depth--; continue; }
            if (*p == ',' && depth == 1) {
                for (k = 0; k <= maxlen; k++) {
                    if (glob_match_exact(alt_start, p, g->s, k, g->p0,
                                         g->depth + 1)) {
                        const char *savep = g->p;
                        const char *saves = g->s;
                        g->p = rest;
                        g->s = g->s + k;
                        if (glob_match_here(g)) return 1;
                        g->p = savep;
                        g->s = saves;
                    }
                }
                alt_start = p + 1;
            }
        } else {
            for (k = 0; k <= maxlen; k++) {
                if (glob_match_exact(alt_start, close, g->s, k, g->p0,
                                     g->depth + 1)) {
                    const char *savep = g->p;
                    const char *saves = g->s;
                    g->p = rest;
                    g->s = g->s + k;
                    if (glob_match_here(g)) return 1;
                    g->p = savep;
                    g->s = saves;
                }
            }
            return 0;
        }
    }
    return 0;
}

/* Match a [...] class. g->p points at '['. On success advances
 * past ']' and consumes one non-'/' char. */
static int glob_class_match(glob_ctx *g)
{
    const char *p = g->p + 1;
    int negate = 0, matched = 0, first = 1;
    unsigned char c;

    if (g->s >= g->send) return 0;
    c = (unsigned char)*g->s;
    if (p < g->pend && *p == '!') { negate = 1; p++; }
    while (p < g->pend && (first || *p != ']')) {
        unsigned char lo, hi;
        first = 0;
        if (*p == '\\' && p + 1 < g->pend) {
            lo = (unsigned char)p[1];
            p += 2;
        } else {
            lo = (unsigned char)*p;
            p++;
        }
        if (p + 1 < g->pend && *p == '-' && p[1] != ']') {
            p++;
            if (*p == '\\' && p + 1 < g->pend) {
                hi = (unsigned char)p[1];
                p += 2;
            } else {
                hi = (unsigned char)*p;
                p++;
            }
        } else {
            hi = lo;
        }
        if (c >= lo && c <= hi) matched = 1;
    }
    if (p >= g->pend || *p != ']') return 0;   /* unterminated class */
    p++;                                       /* past ']' */
    if (negate) matched = !matched;
    if (c == '/') return 0;                    /* '/' never matches */
    if (!matched) return 0;
    g->p = p;
    g->s++;
    return 1;
}

static int glob_match_here(glob_ctx *g)
{
    if (g->depth > GLOB_MAX_DEPTH) return 0;
    while (g->p < g->pend) {
        char pc = *g->p;
        if (pc == '*') {
            const char *q = g->p;
            size_t stars = 0;
            const char *rest;
            while (q < g->pend && *q == '*') { stars++; q++; }
            if (stars >= 2
                && (g->p == g->p0 || g->p[-1] == '/')
                && (q == g->pend || *q == '/')) {
                /* whole-segment **: zero or more directories */
                rest = q;
                if (rest < g->pend && *rest == '/') rest++;
                if (rest == g->pend) return 1;  /* trailing ** eats all */
                {
                    const char *savep = g->p;
                    const char *saves = g->s;
                    const char *k;
                    g->p = rest;
                    if (glob_match_here(g)) return 1;
                    for (k = g->s; k < g->send; k++) {
                        if (*k == '/') {
                            g->p = rest;          /* reset per retry */
                            g->s = k + 1;
                            if (glob_match_here(g)) return 1;
                        }
                    }
                    g->p = savep;
                    g->s = saves;
                    return 0;
                }
            }
            /* single *: zero or more chars, not crossing '/'.
             * Each retry must reset both cursors: a failed
             * glob_match_here leaves them advanced. */
            {
                const char *savep = g->p;
                const char *saves = g->s;
                const char *after = q;
                g->p = after;
                for (q = g->s;; q++) {
                    g->p = after;
                    g->s = q;
                    if (glob_match_here(g)) return 1;
                    if (q >= g->send || *q == '/') break;
                }
                g->p = savep;
                g->s = saves;
                return 0;
            }
        } else if (pc == '?') {
            if (g->s >= g->send || *g->s == '/') return 0;
            g->p++;
            g->s++;
        } else if (pc == '[') {
            if (!glob_class_match(g)) return 0;
        } else if (pc == '{') {
            const char *p = g->p + 1;
            int depth = 1;
            const char *close = NULL;
            while (p < g->pend) {
                if (*p == '\\') { p += 2; continue; }
                if (*p == '{') depth++;
                else if (*p == '}') {
                    depth--;
                    if (depth == 0) { close = p; break; }
                }
                p++;
            }
            if (close == NULL) {
                /* unterminated '{': literal byte */
                if (g->s >= g->send || *g->s != '{') return 0;
                g->p++; g->s++;
            } else {
                if (!glob_brace_try(g, g->p + 1, close)) return 0;
            }
        } else if (pc == '\\') {
            if (g->p + 1 >= g->pend) {
                /* trailing backslash: literal backslash */
                if (g->s >= g->send || *g->s != '\\') return 0;
                g->p++; g->s++;
            } else {
                if (g->s >= g->send || *g->s != g->p[1]) return 0;
                g->p += 2; g->s++;
            }
        } else {
            if (g->s >= g->send || *g->s != pc) return 0;
            g->p++; g->s++;
        }
    }
    return g->s == g->send;
}

/* Shared entry: does pattern (already length-checked by the
 * caller) match s byte-for-byte under the glob syntax? */
static int glob_pattern_matches(const char *pat, size_t pat_len,
                                const char *s, size_t s_len)
{
    glob_ctx g;
    g.p = pat;
    g.pend = pat + pat_len;
    g.s = s;
    g.send = s + s_len;
    g.p0 = pat;
    g.depth = 0;
    return glob_match_here(&g);
}

/* (path-glob-match pattern s) -- pure matcher, walker policy
 * excluded: dotfile visibility is the walker's business. Pattern
 * cap 256 bytes throws :eval/bounds. */
static mino_val *prim_path_glob_match(mino_state *S, mino_val *args,
                                      mino_env *env)
{
    mino_val *pat_val, *s_val;
    int r;
    (void)env;

    if (!mino_is_cons(args) || !mino_is_cons(args->as.cons.cdr)
        || mino_is_cons(args->as.cons.cdr->as.cons.cdr)) {
        prim_throw_classified(S, "eval/arity", "MAR001",
                              "path-glob-match requires two arguments");
        return NULL;
    }
    pat_val = args->as.cons.car;
    s_val = args->as.cons.cdr->as.cons.car;
    if (pat_val == NULL || mino_type_of(pat_val) != MINO_STRING) {
        prim_throw_classified(S, "eval/type", "MTY001",
                              "path-glob-match: pattern must be a string");
        return NULL;
    }
    if (s_val == NULL || mino_type_of(s_val) != MINO_STRING) {
        prim_throw_classified(S, "eval/type", "MTY001",
                              "path-glob-match: path must be a string");
        return NULL;
    }
    if (pat_val->as.s.len > GLOB_MAX_PATTERN) {
        prim_throw_classified(S, "eval/bounds", "MBD001",
                              "path-glob-match: pattern longer than 256 "
                              "bytes");
        return NULL;
    }
    r = glob_pattern_matches(pat_val->as.s.data, pat_val->as.s.len,
                             s_val->as.s.data, s_val->as.s.len);
    return r ? mino_true(S) : mino_false(S);
}


/* ---- prim table ------------------------------------------------------- */

const mino_prim_def k_prims_path[] = {
    {"path-join", prim_path_join,
     "Joins path parts with / and normalizes the result. nil and "
      "empty parts skip; an absolute part does not reset the "
      "accumulation. All-empty answers \".\". Backslashes fold to /."},
    {"path-split", prim_path_split,
     "Splits a path into raw segments: empty segments drop, a "
      "leading / answers a \"/\" element, and . / .. pass through "
      "unresolved. \"\" answers []."},
    {"path-basename", prim_path_basename,
     "The last path segment, raw (no .. resolution): \"a/b/c.txt\" "
      "answers \"c.txt\"; \"/\" answers \"\"; \"\" answers \".\"."},
    {"path-dirname", prim_path_dirname,
     "The directory part: the cleaned path cut at the last "
      "separator. \"c\" answers \".\"; \"/\" answers \"/\"."},
    {"path-extension", prim_path_extension,
     "The extension with its dot and the last dot only: "
      "\"a.tar.gz\" answers \".gz\"; dotfiles and plain names "
      "answer \"\". A dot in a directory part never counts."},
    {"path-split-ext", prim_path_split_ext,
     "Splits a path into [stem extension] over the whole path "
      "(os.path shape); the extension is nil when absent "
      "(dotfiles included). (str stem extension) rebuilds the "
      "input."},
    {"path-stem", prim_path_stem,
     "The basename minus its last extension (the pathlib stem as "
      "a plain function): \"/a/b/c.tar.gz\" answers \"c.tar\"; "
      "\".bashrc\" answers \".bashrc\"."},
    {"path-normalize", prim_path_normalize,
     "Lexical clean: backslashes fold to /, duplicate separators "
      "collapse, . segments drop, inner .. cancels against a "
      "non-.. parent, .. after the root drops, leading .. on a "
      "relative path stays, the trailing separator strips. No "
      "filesystem contact."},
    {"path-absolute?", prim_path_absolute_p,
     "True when the path starts with / after backslash folding. "
      "~ paths are not absolute."},
    {"path-expand-home", prim_path_expand_home,
     "Expands a lone ~ or ~/ prefix through HOME (POSIX) or "
      "USERPROFILE then HOMEDRIVE+HOMEPATH (Windows). ~otheruser "
      "passes through unchanged (no pwd.h). Anything without a ~ "
      "prefix is returned as-is."},
    {"path-glob-match", prim_path_glob_match,
     "Pure glob matcher: does one path match one pattern? "
      "Syntax * ? [class] (ranges, ! negation) {a,b} (nested, "
      "comma-split) backslash-escape, ** as a whole segment "
      "matching zero or more directories (a trailing ** matches "
      "everything left). '/' never matches a wildcard; dotfile "
      "visibility is the walker's policy, not the matcher's. "
      "Patterns over 256 bytes throw :eval/bounds."},
};

const size_t k_prims_path_count =
    sizeof(k_prims_path) / sizeof(k_prims_path[0]);
