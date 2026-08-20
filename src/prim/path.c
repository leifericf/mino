/*
 * path.c -- path string algebra, the pure glob matcher, and the
 * glob walker (ADR 22).
 *
 * Vocabulary: paths are STRINGS, never a path type. The pure prims
 * are total byte-level functions: no filesystem contact, no locale,
 * no encoding assumptions (filenames are bytes). The one impure
 * algebra resident is expand-home (a ~ lookup through the
 * environment); the one filesystem reader is the glob walker.
 *
 * Separators: '/' is canonical in every output. '\' is accepted in
 * every path input and folded at each prim's edge (the Windows v1
 * stance; no drive letters, no UNC). The pattern side of
 * path-glob-match and glob never folds: patterns are user-written
 * and '\' in a pattern is an escape.
 *
 * Edge rules are the cross-language majority pinned by the
 * path-lib PoCs and ADR 22: extension carries the dot and dotfiles
 * answer "", join does not reset on an absolute segment, normalize
 * is lexical Go-Clean style (trailing separator strips, .. after
 * the root drops, leading .. on relative paths stays), basename is
 * the raw last segment (no .. resolution, node parity), dirname is
 * the cleaned prefix.
 *
 * Capabilities (ADR 22): k_prims_path (the algebra + the pure
 * matcher) installs in the floor like time -- string math reads
 * nothing. The glob walker reads directory contents, so it lives
 * in k_prims_path_fs and installs with the fs capability through
 * mino_install_fs (fs.c), the file-seq precedent: a directory
 * walker rides a capability gate.
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


/* ---- glob walker ------------------------------------------------------- */

typedef struct {
    int match_dot;
    int follow_links;
    int recursive;
    long long max_depth;
} glob_opts_t;

/* Result accumulator: C strings, sorted and deduped at the end. */
typedef struct {
    char **items;
    size_t len, cap;
} strvec_t;

static int strvec_cmp(const void *a, const void *b)
{
    return strcmp(*(const char *const *)a, *(const char *const *)b);
}

typedef struct {
    glob_opts_t opts;
    strvec_t results;
    const char *prefix;    /* rendered root ("" means no prefix) */
    size_t prefix_len;
} glob_walk_t;

/* Entry visibility under the dotfile policy: hidden names need
 * match-dot or a pattern segment that itself starts with a dot. */
static int glob_visible(const char *name, const char *seg, size_t seg_len,
                        const glob_opts_t *opts)
{
    if (name[0] != '.') return 1;
    if (opts->match_dot) return 1;
    return seg_len > 0 && seg[0] == '.';
}

/* Is child (dir + "/" + name) a directory? lstat unless
 * follow-links; stat on Windows (no lstat there). */
static int glob_is_dir(const char *child, const glob_opts_t *opts)
{
    struct stat st;
#ifdef _WIN32
    (void)opts;
    return stat(child, &st) == 0 && S_ISDIR(st.st_mode);
#else
    return (opts->follow_links ? stat(child, &st)
                               : lstat(child, &st)) == 0
           && S_ISDIR(st.st_mode);
#endif
}

/* Directory test through symlinks (stat). Used for segments the
 * pattern names literally: the pattern's own structure is the
 * user's explicit path (find -P semantics: -P governs discovered
 * entries, not explicitly named operands), so /tmp/x works on
 * macOS where /tmp is a symlink while ** keeps its no-follow
 * default for wildcard-discovered directories. */
static int glob_is_dir_follow(const char *child)
{
    struct stat st;
    return stat(child, &st) == 0 && S_ISDIR(st.st_mode);
}

/* A segment with no wildcard metacharacters matches exactly one
 * name the user spelled out. */
static int seg_is_literal(const char *seg, size_t len)
{
    size_t i;
    for (i = 0; i < len; i++) {
        char c = seg[i];
        if (c == '*' || c == '?' || c == '[' || c == '{'
            || c == '\\') {
            return 0;
        }
    }
    return 1;
}

/* Push one result: prefix + "/" + rel, or bare rel when the
 * prefix is empty (the default "." root renders unprefixed). A
 * prefix ending in "/" (the absolute-pattern "/") does not get a
 * second separator. */
static void glob_push(glob_walk_t *w, const char *rel, size_t rel_len)
{
    size_t sep = (w->prefix_len > 0
                  && w->prefix[w->prefix_len - 1] != '/') ? 1 : 0;
    size_t need = w->prefix_len + sep + rel_len + 1;
    char *out = (char *)malloc(need);
    if (out == NULL) return;
    if (w->prefix_len == 0) {
        memcpy(out, rel, rel_len);
        out[rel_len] = '\0';
    } else {
        memcpy(out, w->prefix, w->prefix_len);
        if (sep) out[w->prefix_len] = '/';
        memcpy(out + w->prefix_len + sep, rel, rel_len);
        out[w->prefix_len + sep + rel_len] = '\0';
    }
    if (w->results.len == w->results.cap) {
        size_t nc = w->results.cap ? w->results.cap * 2 : 64;
        char **nb = (char **)realloc(w->results.items,
                                     nc * sizeof(*nb));
        if (nb == NULL) { free(out); return; }
        w->results.items = nb;
        w->results.cap = nc;
    }
    w->results.items[w->results.len++] = out;
}

/* Expand pattern segments segs[i..) against directory dir; rel is
 * the malloc'd path of dir relative to the walk root ("" at the
 * top). Every directory level descended increments depth; the
 * ** recursion never exceeds opts->max_depth. */
static void glob_walk_dir(glob_walk_t *w, const char *dir,
                          const char **segs, const size_t *seg_lens,
                          size_t nsegs, size_t i, const char *rel,
                          size_t rel_len, long long depth)
{
    DIR *d;
    struct dirent *ent;
    const glob_opts_t *opts = &w->opts;

    if (i >= nsegs) return;
    if (depth > opts->max_depth) return;

    /* ** zero-level case first, once per directory: the segment
     * after ** matches entries of this very directory. */
    if (seg_lens[i] == 2 && segs[i][0] == '*' && segs[i][1] == '*'
        && opts->recursive && i + 1 < nsegs) {
        glob_walk_dir(w, dir, segs, seg_lens, nsegs, i + 1, rel,
                      rel_len, depth);
    }

    d = opendir(dir);
    if (d == NULL) return;
    while ((ent = readdir(d)) != NULL) {
        const char *name = ent->d_name;
        size_t name_len = strlen(name);
        const char *seg = segs[i];
        size_t seg_len = seg_lens[i];
        size_t dir_len = strlen(dir);
        char *child, *rel2;
        size_t child_len, rel2_len;

        if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0) continue;
        if (name[0] == '\0') continue;

        child_len = dir_len + 1 + name_len + 1;
        child = (char *)malloc(child_len);
        if (child == NULL) continue;
        memcpy(child, dir, dir_len);
        child[dir_len] = '/';
        memcpy(child + dir_len + 1, name, name_len);
        child[child_len - 1] = '\0';

        rel2_len = rel_len + (rel_len ? 1 : 0) + name_len + 1;
        rel2 = (char *)malloc(rel2_len);
        if (rel2 == NULL) { free(child); continue; }
        {
            size_t o = rel_len;
            memcpy(rel2, rel, rel_len);
            if (o > 0) rel2[o++] = '/';
            memcpy(rel2 + o, name, name_len);
            o += name_len;
            rel2[o] = '\0';
            rel2_len = o;
        }

        if (seg_len == 2 && seg[0] == '*' && seg[1] == '*'
            && opts->recursive) {
            /* ** descend: every visible directory continues with **
             * in effect; as the final segment it also emits every
             * visible descendant. */
            if (glob_visible(name, seg, seg_len, opts)) {
                if (i + 1 == nsegs) {
                    glob_push(w, rel2, rel2_len);
                }
                if (glob_is_dir(child, opts)) {
                    glob_walk_dir(w, child, segs, seg_lens, nsegs, i, rel2,
                                  rel2_len, depth + 1);
                }
            }
        } else {
            const char *match_seg = seg;
            size_t match_len = seg_len;
            if (seg_len == 2 && seg[0] == '*' && seg[1] == '*') {
                /* non-recursive **: behaves as a single * */
                match_seg = "*";
                match_len = 1;
            }
            if (glob_visible(name, match_seg, match_len, opts)
                && glob_pattern_matches(match_seg, match_len, name,
                                        name_len)) {
                if (i + 1 == nsegs) {
                    glob_push(w, rel2, rel2_len);
                } else if (glob_is_dir(child, opts)
                           || (seg_is_literal(seg, seg_len)
                               && glob_is_dir_follow(child))) {
                    glob_walk_dir(w, child, segs, seg_lens, nsegs, i + 1,
                                  rel2, rel2_len, depth + 1);
                }
            }
        }
        free(child);
        free(rel2);
    }
    closedir(d);
}

/* (glob pattern root? opts?) -- the one walker (ADR 22). Sorted
 * (byte order) vector of strings rendered as-given; dotfiles
 * hidden unless {:match-dot true}; symlinks not followed unless
 * {:follow-links true}; {:recursive false} caps ** to one level;
 * {:max-depth n} bounds the walk (default 128). Unreadable or
 * missing directories answer []. */
static mino_val *prim_glob(mino_state *S, mino_val *args, mino_env *env)
{
    mino_val *pat_val, *root_val = NULL, *opts_val = NULL;
    size_t nargs = 0, pat_len, nsegs = 0, i, k;
    mino_val *a, *result;
    char *pat = NULL, *root_folded = NULL;
    const char **segs = NULL;
    size_t *seg_lens = NULL;
    glob_opts_t opts;
    glob_walk_t w;
    mino_vec_builder *b;
    int pattern_absolute;
    char *top_dir_heap = NULL;
    const char *walk_root;

    opts.match_dot = 0;
    opts.follow_links = 0;
    opts.recursive = 1;
    opts.max_depth = 128;

    for (a = args; a != NULL && mino_is_cons(a); a = a->as.cons.cdr) nargs++;
    if (nargs < 1 || nargs > 3) {
        prim_throw_classified(S, "eval/arity", "MAR001",
                              "glob requires one to three arguments");
        return NULL;
    }
    pat_val = args->as.cons.car;
    if (nargs >= 2) root_val = args->as.cons.cdr->as.cons.car;
    if (nargs >= 3) opts_val = args->as.cons.cdr->as.cons.cdr->as.cons.car;

    if (pat_val == NULL || mino_type_of(pat_val) != MINO_STRING
        || pat_val->as.s.len == 0) {
        prim_throw_classified(S, "eval/type", "MTY001",
                              "glob: pattern must be a non-empty string");
        return NULL;
    }
    if (pat_val->as.s.len > GLOB_MAX_PATTERN) {
        prim_throw_classified(S, "eval/bounds", "MBD001",
                              "glob: pattern longer than 256 bytes");
        return NULL;
    }
    if (nargs >= 2 && root_val != NULL
        && mino_type_of(root_val) != MINO_STRING
        && mino_type_of(root_val) != MINO_NIL) {
        prim_throw_classified(S, "eval/type", "MTY001",
                              "glob: root must be a string");
        return NULL;
    }
    if (nargs >= 3 && opts_val != NULL
        && mino_type_of(opts_val) != MINO_MAP
        && mino_type_of(opts_val) != MINO_NIL) {
        prim_throw_classified(S, "eval/type", "MTY001",
                              "glob: opts must be a map");
        return NULL;
    }
    if (opts_val != NULL && mino_type_of(opts_val) == MINO_MAP) {
        static const struct { const char *key; int is_bool; } kbools[] = {
            { "match-dot", 1 }, { "follow-links", 1 }, { "recursive", 1 },
        };
        size_t bi;
        for (bi = 0; bi < sizeof(kbools) / sizeof(kbools[0]); bi++) {
            mino_val *v = map_get_val(opts_val, mino_keyword(S,
                                                             kbools[bi].key));
            if (v == NULL || mino_type_of(v) == MINO_NIL) continue;
            if (mino_type_of(v) != MINO_BOOL) {
                char msg[96];
                snprintf(msg, sizeof(msg),
                         "glob: :%s must be a boolean", kbools[bi].key);
                prim_throw_classified(S, "eval/contract", "MCT001", msg);
                goto fail;
            }
            {
                int on = mino_val_bool_get(v);
                if (bi == 0) opts.match_dot = on;
                else if (bi == 1) opts.follow_links = on;
                else opts.recursive = on;
            }
        }
        {
            mino_val *v = map_get_val(opts_val, mino_keyword(S,
                                                             "max-depth"));
            long long md;
            if (v != NULL && mino_type_of(v) != MINO_NIL) {
                if (mino_type_of(v) != MINO_INT || !as_long(v, &md) || md < 1) {
                    prim_throw_classified(S, "eval/contract", "MCT001",
                                          "glob: :max-depth must be a "
                                          "positive integer");
                    goto fail;
                }
                opts.max_depth = md;
            }
        }
    }

    /* Pattern: split on '/' (pattern never folds; '\' escapes). */
    pat_len = pat_val->as.s.len;
    pat = (char *)malloc(pat_len + 1);
    if (pat == NULL) goto fail;
    memcpy(pat, pat_val->as.s.data, pat_len);
    pat[pat_len] = '\0';
    pattern_absolute = (pat[0] == '/');
    segs = (const char **)malloc((pat_len / 2 + 2) * sizeof(*segs));
    seg_lens = (size_t *)malloc((pat_len / 2 + 2) * sizeof(*seg_lens));
    if (segs == NULL || seg_lens == NULL) goto fail;
    {
        size_t j = 0;
        while (j < pat_len) {
            size_t start;
            while (j < pat_len && pat[j] == '/') j++;
            start = j;
            while (j < pat_len && pat[j] != '/') j++;
            if (j > start) {
                segs[nsegs] = pat + start;
                seg_lens[nsegs] = j - start;
                nsegs++;
            }
        }
    }
    if (nsegs == 0) {
        /* pattern was only separators: nothing to match */
        result = mino_vector(S, NULL, 0);
        goto done;
    }

    /* Root: as-given prefix; the default (and an explicit "." or
     * "") renders unprefixed, so relative patterns answer relative
     * results. An absolute pattern walks from / and ignores root. */
    if (pattern_absolute) {
        w.prefix = "/";
        w.prefix_len = 1;
        walk_root = "/";
    } else {
        const char *root_str = ".";
        size_t root_len = 1;
        if (nargs >= 2 && root_val != NULL
            && mino_type_of(root_val) == MINO_STRING
            && root_val->as.s.len > 0) {
            root_str = root_val->as.s.data;
            root_len = root_val->as.s.len;
        }
        root_folded = (char *)malloc(root_len + 1);
        if (root_folded == NULL) goto fail;
        memcpy(root_folded, root_str, root_len);
        root_folded[root_len] = '\0';
        fold_backslashes(root_folded);
        while (root_len > 1 && root_folded[root_len - 1] == '/') root_len--;
        root_folded[root_len] = '\0';
        if (root_folded[0] == '/') {
            w.prefix = root_folded;
            w.prefix_len = root_len;
        } else if (root_len == 1 && root_folded[0] == '.') {
            w.prefix = "";
            w.prefix_len = 0;
        } else {
            w.prefix = root_folded;
            w.prefix_len = root_len;
        }
        walk_root = root_folded;
        if (w.prefix_len == 0) {
            /* unprefixed walk still needs a real directory to open */
            if (top_dir_heap != NULL) free(top_dir_heap);
            top_dir_heap = (char *)malloc(2);
            if (top_dir_heap == NULL) goto fail;
            top_dir_heap[0] = '.'; top_dir_heap[1] = '\0';
            walk_root = top_dir_heap;
        }
    }

    w.opts = opts;
    w.results.items = NULL;
    w.results.len = 0;
    w.results.cap = 0;

    glob_walk_dir(&w, walk_root, segs, seg_lens, nsegs, 0, "", 0, 0);

    if (w.results.len > 1) {
        qsort(w.results.items, w.results.len, sizeof(char *), strvec_cmp);
    }
    b = mino_vector_builder_new(S);
    if (b == NULL) goto fail;
    for (k = 0; k < w.results.len; k++) {
        if (k > 0 && strcmp(w.results.items[k], w.results.items[k - 1]) == 0)
            continue;                         /* dedupe ambiguous ** */
        mino_vector_builder_push(b, mino_string(S, w.results.items[k]));
    }
    result = mino_vector_builder_finish(b);
    for (i = 0; i < w.results.len; i++) free(w.results.items[i]);
    free(w.results.items);

done:
    free(top_dir_heap);
    free(root_folded);
    free(segs);
    free(seg_lens);
    free(pat);
    return result;

fail:
    free(top_dir_heap);
    free(root_folded);
    free(segs);
    free(seg_lens);
    free(pat);
    return NULL;
}


/* ---- prim tables ------------------------------------------------------ */

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

/* The glob walker rides the fs capability (ADR 22): it reads
 * directory contents, exactly the surface file-seq gates under
 * io. Installed by mino_install_fs in fs.c. */
const mino_prim_def k_prims_path_fs[] = {
    {"glob", prim_glob,
     "Walks directories matching a glob pattern and answers a "
      "sorted (byte order) vector of path strings rendered "
      "as-given: a relative pattern with no root (or root \".\") "
      "answers relative paths. Syntax as path-glob-match. "
      "Dotfiles are hidden unless the segment itself starts with "
      "a dot or {:match-dot true}. Symlinked directories found by "
      "wildcards are not followed unless {:follow-links true}; "
      "segments the pattern names literally are the user's "
      "explicit path and resolve through symlinks (so /tmp/x "
      "works where /tmp is one). {:recursive false} caps ** to "
      "one level; {:max-depth n} bounds the walk (default 128). "
      "Missing or unreadable directories answer []. An absolute "
      "pattern walks from / and ignores root. Requires the fs "
      "capability."},
};

const size_t k_prims_path_fs_count =
    sizeof(k_prims_path_fs) / sizeof(k_prims_path_fs[0]);

