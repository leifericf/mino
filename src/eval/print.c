/*
 * print.c -- value printer.
 *
 * Dynamic-variable resolution (print_dynvars_resolve / print_dynvars_restore)
 * lives in print_dynvars.c; declarations are in eval/internal.h (included
 * transitively via runtime/internal.h).
 */

#include "runtime/internal.h"
#include "runtime/host_threads.h"

/* ------------------------------------------------------------------------- */
/* Printer                                                                   */
/* ------------------------------------------------------------------------- */

static void print_string_escaped(FILE *out, const char *s, size_t len)
{
    size_t i;
    fputc('"', out);
    for (i = 0; i < len; i++) {
        unsigned char c = (unsigned char)s[i];
        switch (c) {
        case '"':  fputs("\\\"", out); break;
        case '\\': fputs("\\\\", out); break;
        case '\n': fputs("\\n",  out); break;
        case '\t': fputs("\\t",  out); break;
        case '\r': fputs("\\r",  out); break;
        case '\0': fputs("\\0",  out); break;
        default:   fputc((int)c, out); break;
        }
    }
    fputc('"', out);
}

/*
 * Cycle-safe print depth: when recursion exceeds this limit, the printer
 * emits #<...> instead of descending further. This prevents stack overflow
 * on deeply nested or self-referential structures (possible through mutable
 * cons tails in internal data, though the user-facing API is immutable).
 *
 * Kept equal to MINO_READER_MAX_DEPTH (read.c): anything the reader will
 * accept must print in a form the reader can read back, so pr-str round-
 * trips for every value `read` admits. The printer's per-frame cost is no
 * larger than the reader's, so the same ceiling sits the same safe margin
 * below the stack-overflow point. Deeper structures (reachable only via
 * internal mutable cons tails) still elide.
 */
#define MINO_PRINT_DEPTH_MAX 1024

/* Forward declaration: defined alongside print_map below; used by the
 * rb-tree namespace-stripped walker introduced for sorted maps. */
static void print_map_key_stripped(FILE *out, const mino_val *key,
                                   size_t ns_len);

static void print_rb_inorder(mino_state *S, FILE *out,
                             const mino_rb_node_t *n, int is_map, int *first)
{
    if (n == NULL) return;
    print_rb_inorder(S, out, n->left, is_map, first);
    if (!*first) fputs(is_map ? ", " : " ", out);
    *first = 0;
    mino_print_to(S, out, n->key);
    if (is_map) { fputc(' ', out); mino_print_to(S, out, n->val); }
    print_rb_inorder(S, out, n->right, is_map, first);
}

/* Walk an rb-tree in-order, accumulating the shared key namespace
 * (or detecting that one isn't possible). Mirrors map_common_key_ns
 * but works on the rb representation used by sorted maps. Sets
 * *out_ok = 0 the moment a key disqualifies; the caller checks that
 * before reading *out_ns / *out_ns_len. */
static void rb_collect_common_ns(const mino_rb_node_t *n,
                                 const char **out_ns,
                                 size_t *out_ns_len,
                                 int *out_ok)
{
    int t;
    size_t key_ns_len;
    if (n == NULL || !*out_ok) return;
    rb_collect_common_ns(n->left, out_ns, out_ns_len, out_ok);
    if (!*out_ok) return;
    t = mino_type_of(n->key);
    if (t != MINO_KEYWORD && t != MINO_SYMBOL) { *out_ok = 0; return; }
    key_ns_len = n->key->as.s.ns_len;
    if (key_ns_len == 0) { *out_ok = 0; return; }
    if (*out_ns == NULL) {
        *out_ns     = n->key->as.s.data;
        *out_ns_len = key_ns_len;
    } else if (key_ns_len != *out_ns_len
               || memcmp(n->key->as.s.data, *out_ns, *out_ns_len) != 0) {
        *out_ok = 0; return;
    }
    rb_collect_common_ns(n->right, out_ns, out_ns_len, out_ok);
}

static void print_rb_inorder_ns_stripped(mino_state *S, FILE *out,
                                          const mino_rb_node_t *n,
                                          size_t ns_len, int *first)
{
    if (n == NULL) return;
    print_rb_inorder_ns_stripped(S, out, n->left, ns_len, first);
    if (!*first) fputs(", ", out);
    *first = 0;
    print_map_key_stripped(out, n->key, ns_len);
    fputc(' ', out);
    mino_print_to(S, out, n->val);
    print_rb_inorder_ns_stripped(S, out, n->right, ns_len, first);
}

/* Per-tag printers. Each handles its case in isolation; the
 * dispatcher in mino_print_to is responsible for the NULL / NIL /
 * depth-cap prelude, picks the helper by tag, and lets the helper
 * write the entire form. Helpers may recurse through mino_print_to
 * and bump S->print_depth for the duration of any nested walk. */

/* Print a float (32-bit) or double (64-bit). is_float32 selects which
 * runtime type's round-trip rule applies: float32 prints the shortest
 * decimal that re-parses to the same `float` (mirroring JVM
 * Float.toString), double prints the shortest re-parsable `double`
 * (Double.toString). The two paths share the bracket-and-snprintf loop;
 * only the comparator and significand cap differ. */
static void print_float_typed(FILE *out, double x, int is_float32)
{
    char buf[64];
    int n, needs_dot, i, p, maxp, sci_cleaned;
    if (isnan(x)) { fputs("##NaN", out); return; }
    if (isinf(x)) {
        fputs(x > 0 ? "##Inf" : "##-Inf", out);
        return;
    }
    /* float32 carries ~9 significant digits; double carries ~17. The
     * loop tries widening precision until the round-trip matches, so
     * the cap just prevents an unbounded search. */
    maxp = is_float32 ? 9 : 17;
    {
        double absx = (x < 0.0) ? -x : x;
        int use_sci = (x != 0.0) && (absx < 1e-3 || absx >= 1e7);
        n = 0;
        if (use_sci) {
            for (p = 0; p <= maxp; p++) {
                /* %E -> uppercase exponent, matching JVM. */
                n = snprintf(buf, sizeof(buf), "%.*E", p, x);
                if (n < 0 || n >= (int)sizeof(buf)) { n = -1; break; }
                if (is_float32) {
                    float back = (float)strtod(buf, NULL);
                    if (back == (float)x) break;
                } else {
                    double back = strtod(buf, NULL);
                    if (back == x) break;
                }
            }
        } else {
            /* Fixed: %f precision is fractional-digit count. */
            for (p = 0; p <= maxp; p++) {
                n = snprintf(buf, sizeof(buf), "%.*f", p, x);
                if (n < 0 || n >= (int)sizeof(buf)) { n = -1; break; }
                if (is_float32) {
                    float back = (float)strtod(buf, NULL);
                    if (back == (float)x) break;
                } else {
                    double back = strtod(buf, NULL);
                    if (back == x) break;
                }
            }
        }
    }
    if (n < 0) { fputs("0.0", out); return; }
    /* snprintf("%E") emits "1.5E+02" / "1E+05". Strip the leading '+'
     * after E so the form matches JVM ("1.5E2", "1E5") -- JVM's
     * Double.toString never carries the '+'. Negative exponents keep
     * their '-'. */
    sci_cleaned = 0;
    for (i = 0; i + 2 < n; i++) {
        if (buf[i] == 'E' && buf[i + 1] == '+') {
            int j;
            for (j = i + 1; j + 1 < n; j++) buf[j] = buf[j + 1];
            n--;
            buf[n] = '\0';
            sci_cleaned = 1;
            break;
        }
    }
    /* JVM also strips a leading zero on a two-digit exponent: "E05" -> "E5".
     * The snprintf output keeps the leading zero on macOS / glibc;
     * trim a single leading zero in the exponent for parity. */
    (void)sci_cleaned;
    for (i = 0; i + 2 < n; i++) {
        if (buf[i] == 'E' &&
            (buf[i + 1] == '-' || (buf[i + 1] >= '0' && buf[i + 1] <= '9'))) {
            int eidx = i + 1;
            if (buf[eidx] == '-') eidx++;
            if (eidx + 1 < n && buf[eidx] == '0' &&
                buf[eidx + 1] >= '0' && buf[eidx + 1] <= '9') {
                int j;
                for (j = eidx; j + 1 < n; j++) buf[j] = buf[j + 1];
                n--;
                buf[n] = '\0';
            }
            break;
        }
    }
    /* Always include a decimal point so the printed form re-reads as
     * a float. For fixed, p=0 has no point; for scientific, the form
     * is like "1E5" or "1.5E2" -- if there's no '.' yet, insert one
     * before the 'E'. */
    needs_dot = 1;
    for (i = 0; i < n; i++) {
        if (buf[i] == '.') { needs_dot = 0; break; }
    }
    if (needs_dot) {
        int e_at = -1;
        for (i = 0; i < n; i++) {
            if (buf[i] == 'E') { e_at = i; break; }
        }
        if (e_at >= 0 && n + 2 < (int)sizeof(buf)) {
            int j;
            for (j = n; j > e_at; j--) buf[j + 1] = buf[j - 1];
            buf[e_at]     = '.';
            buf[e_at + 1] = '0';
            n += 2;
            buf[n] = '\0';
            needs_dot = 0;
        }
    }
    fputs(buf, out);
    if (needs_dot) fputs(".0", out);
}

static void print_float(FILE *out, double x)
{
    print_float_typed(out, x, 0);
}

static void print_float32(FILE *out, double x)
{
    print_float_typed(out, x, 1);
}

static void print_char(FILE *out, int cp)
{
    switch (cp) {
    case ' ':  fputs("\\space",     out); return;
    case '\n': fputs("\\newline",   out); return;
    case '\t': fputs("\\tab",       out); return;
    case '\r': fputs("\\return",    out); return;
    case '\b': fputs("\\backspace", out); return;
    case '\f': fputs("\\formfeed",  out); return;
    default:
        if (cp >= 0x21 && cp <= 0x7E) {
            fputc('\\', out);
            fputc(cp, out);
        } else if (cp <= 0xFFFF) {
            /* BMP codepoints print as the four-hex \uXXXX escape. */
            fprintf(out, "\\u%04X", (unsigned)cp);
        } else {
            /* The \uXXXX escape only spans four hex digits, so
             * astral codepoints print as the raw glyph -- the reader
             * accepts one full UTF-8 codepoint after the backslash. */
            unsigned char b[4];
            b[0] = (unsigned char)(0xF0u | ((unsigned)cp >> 18));
            b[1] = (unsigned char)(0x80u | (((unsigned)cp >> 12) & 0x3Fu));
            b[2] = (unsigned char)(0x80u | (((unsigned)cp >> 6) & 0x3Fu));
            b[3] = (unsigned char)(0x80u | ((unsigned)cp & 0x3Fu));
            fputc('\\', out);
            fwrite(b, 1, 4, out);
        }
        return;
    }
}

/* Emit a map carrying the :mino/instant metadata marker as its
 * #inst "..." reader literal so the documented literal round-trips
 * through pr/read. Returns 1 when the value was an instant map and
 * was printed; 0 to fall through to the generic map printer. The
 * component fields mirror clojure.instant/read-instant-date. */
static long long instant_field(mino_state *S, const mino_val *m,
                               const char *name, long long dflt)
{
    mino_val *v = map_get_val((mino_val *)m, mino_keyword(S, name));
    if (v == NULL || !mino_val_int_p(v)) return dflt;
    return mino_val_int_get(v);
}

static int print_instant_literal(mino_state *S, FILE *out,
                                 const mino_val *v)
{
    long long years, months, days, hours, minutes, seconds;
    long long nanos, osign, ohours, ominutes, millis;
    if (v->meta == NULL || mino_type_of(v->meta) != MINO_MAP) return 0;
    {
        mino_val *marker = map_get_val(v->meta,
            mino_keyword(S, "mino/instant"));
        if (marker == NULL || !mino_is_truthy(marker)) return 0;
    }
    years    = instant_field(S, v, "years", 1970);
    months   = instant_field(S, v, "months", 1);
    days     = instant_field(S, v, "days", 1);
    hours    = instant_field(S, v, "hours", 0);
    minutes  = instant_field(S, v, "minutes", 0);
    seconds  = instant_field(S, v, "seconds", 0);
    nanos    = instant_field(S, v, "nanoseconds", 0);
    osign    = instant_field(S, v, "offset-sign", 1);
    ohours   = instant_field(S, v, "offset-hours", 0);
    ominutes = instant_field(S, v, "offset-minutes", 0);
    millis   = nanos / 1000000;
    fprintf(out,
            "#inst \"%04lld-%02lld-%02lldT%02lld:%02lld:%02lld.%03lld%c%02lld:%02lld\"",
            years, months, days, hours, minutes, seconds, millis,
            osign < 0 ? '-' : '+', ohours, ominutes);
    return 1;
}

/* True when *print-level* has been resolved (>=0) and the current
 * print_depth is at or beyond the limit. Callers replace the open
 * collection with `#` per JVM Clojure's semantics. */
static int print_level_collapse(const mino_state *S)
{
    return S->print_level_limit >= 0
        && S->print_depth >= S->print_level_limit;
}

static void print_cons(mino_state *S, FILE *out, const mino_val *v)
{
    const mino_val *p = v;
    int printed = 0;
    if (print_level_collapse(S)) { fputc('#', out); return; }
    fputc('(', out);
    S->print_depth++;
    while (p != NULL && mino_type_of(p) == MINO_CONS) {
        if (printed > 0) fputc(' ', out);
        if (S->print_length_limit >= 0 && printed >= S->print_length_limit) {
            fputs("...", out);
            break;
        }
        mino_print_to(S, out, p->as.cons.car);
        printed++;
        p = p->as.cons.cdr;
        /* Force lazy tails so (cons x (lazy-seq ...)) prints as a list. */
        if (p != NULL && mino_type_of(p) == MINO_LAZY) {
            p = lazy_force(S, (mino_val *)p);
        }
        if (p != NULL && mino_type_of(p) != MINO_CONS
                      && mino_type_of(p) != MINO_NIL
                      && mino_type_of(p) != MINO_EMPTY_LIST) {
            fputs(" . ", out);
            mino_print_to(S, out, p);
            break;
        }
    }
    S->print_depth--;
    fputc(')', out);
}

static void print_vector(mino_state *S, FILE *out, const mino_val *v)
{
    size_t i;
    if (print_level_collapse(S)) { fputc('#', out); return; }
    fputc('[', out);
    S->print_depth++;
    for (i = 0; i < v->as.vec.len; i++) {
        if (i > 0) fputc(' ', out);
        if (S->print_length_limit >= 0 && (int)i >= S->print_length_limit) {
            fputs("...", out);
            break;
        }
        mino_print_to(S, out, vec_nth(v, i));
    }
    S->print_depth--;
    fputc(']', out);
}

/* If v's meta carries :mino/reader-conditional or :mino/tagged-literal,
 * print the canonical reader-syntax shape so pr-str / read-string
 * round-trip preserves the value. Returns 1 when the special-print
 * was emitted, 0 otherwise. */
static int print_map_meta_special(mino_state *S, FILE *out, const mino_val *v)
{
    mino_val *meta;
    mino_val *form_kw;
    mino_val *splicing_kw;
    mino_val *form;
    mino_val *splicing;
    mino_val *rc_marker;
    mino_val *tl_marker;
    mino_val *tag_kw;
    mino_val *tag;
    if (v == NULL) return 0;
    meta = (mino_val *)v->meta;
    if (meta == NULL || mino_type_of(meta) != MINO_MAP) return 0;

    rc_marker = map_get_val(meta, mino_keyword(S, "mino/reader-conditional"));
    if (rc_marker != NULL && rc_marker != mino_nil(S)
        && rc_marker != mino_false(S)) {
        form_kw     = mino_keyword(S, "form");
        splicing_kw = mino_keyword(S, "splicing?");
        form        = map_get_val(v, form_kw);
        splicing    = map_get_val(v, splicing_kw);
        if (splicing != NULL && splicing != mino_nil(S)
            && splicing != mino_false(S)) {
            fputs("#?@", out);
        } else {
            fputs("#?", out);
        }
        /* form is the body list (e.g. (:mino 1 :clj 2)). Print it as
         * a parenthesised body. */
        if (form != NULL && mino_is_cons(form)) {
            mino_print_to(S, out, form);
        } else if (form != NULL && mino_type_of(form) == MINO_EMPTY_LIST) {
            fputs("()", out);
        } else {
            /* Defensive: print whatever the form slot carries. */
            mino_print_to(S, out, form);
        }
        return 1;
    }

    tl_marker = map_get_val(meta, mino_keyword(S, "mino/tagged-literal"));
    if (tl_marker != NULL && tl_marker != mino_nil(S)
        && tl_marker != mino_false(S)) {
        tag_kw      = mino_keyword(S, "tag");
        form_kw     = mino_keyword(S, "form");
        tag         = map_get_val(v, tag_kw);
        form        = map_get_val(v, form_kw);
        fputc('#', out);
        mino_print_to(S, out, tag);
        fputc(' ', out);
        mino_print_to(S, out, form);
        return 1;
    }
    return 0;
}

/* When *print-namespace-maps* is true and every key in the map is a
 * keyword (or symbol) sharing one non-empty namespace, return a
 * pointer to that shared namespace string (interned, owned by the
 * symbol/keyword intern table; do not free). Length is written into
 * *out_len. Returns NULL when the map doesn't qualify. */
static const char *map_common_key_ns(const mino_val *v, size_t *out_len)
{
    size_t        i;
    const char   *ns      = NULL;
    size_t        ns_len  = 0;
    if (v->as.map.len == 0) return NULL;
    for (i = 0; i < v->as.map.len; i++) {
        mino_val *key = vec_nth(v->as.map.key_order, i);
        const char *key_data;
        size_t      key_full_len;
        size_t      key_ns_len;
        int         t = mino_type_of(key);
        if (t != MINO_KEYWORD && t != MINO_SYMBOL) return NULL;
        key_ns_len = key->as.s.ns_len;
        if (key_ns_len == 0) return NULL;
        key_data     = key->as.s.data;
        key_full_len = key->as.s.len;
        (void)key_full_len;
        if (ns == NULL) {
            ns     = key_data;
            ns_len = key_ns_len;
        } else if (key_ns_len != ns_len
                   || memcmp(key_data, ns, ns_len) != 0) {
            return NULL;
        }
    }
    *out_len = ns_len;
    return ns;
}

/* Print one key inside a #:ns{...} body. The key is a keyword or
 * symbol whose namespace prefix has already been stripped from the
 * emitted form. */
static void print_map_key_stripped(FILE *out, const mino_val *key,
                                   size_t ns_len)
{
    size_t skip = ns_len + 1; /* +1 for the '/' separator */
    int    t    = mino_type_of(key);
    if (t == MINO_KEYWORD) fputc(':', out);
    if (key->as.s.len > skip) {
        fwrite(key->as.s.data + skip, 1, key->as.s.len - skip, out);
    }
}

static void print_map(mino_state *S, FILE *out, const mino_val *v)
{
    size_t      i;
    const char *common_ns = NULL;
    size_t      common_ns_len = 0;
    if (print_map_meta_special(S, out, v)) return;
    if (print_level_collapse(S)) { fputc('#', out); return; }
    if (S->print_namespace_maps_flag == 1) {
        common_ns = map_common_key_ns(v, &common_ns_len);
    }
    if (common_ns != NULL) {
        fputs("#:", out);
        fwrite(common_ns, 1, common_ns_len, out);
    }
    fputc('{', out);
    S->print_depth++;
    for (i = 0; i < v->as.map.len; i++) {
        mino_val *key = vec_nth(v->as.map.key_order, i);
        if (i > 0) fputs(", ", out);
        if (S->print_length_limit >= 0 && (int)i >= S->print_length_limit) {
            fputs("...", out);
            break;
        }
        if (common_ns != NULL) {
            print_map_key_stripped(out, key, common_ns_len);
        } else {
            mino_print_to(S, out, key);
        }
        fputc(' ', out);
        mino_print_to(S, out, map_get_val(v, key));
    }
    S->print_depth--;
    fputc('}', out);
}

static void print_set(mino_state *S, FILE *out, const mino_val *v)
{
    size_t i;
    if (print_level_collapse(S)) { fputc('#', out); return; }
    fputs("#{", out);
    S->print_depth++;
    for (i = 0; i < v->as.set.len; i++) {
        if (i > 0) fputc(' ', out);
        if (S->print_length_limit >= 0 && (int)i >= S->print_length_limit) {
            fputs("...", out);
            break;
        }
        mino_print_to(S, out, vec_nth(v->as.set.key_order, i));
    }
    S->print_depth--;
    fputc('}', out);
}

static void print_sorted(mino_state *S, FILE *out, const mino_val *v,
                         int is_map)
{
    int first = 1;
    const char *common_ns     = NULL;
    size_t      common_ns_len = 0;
    int         ns_ok         = 1;
    if (print_level_collapse(S)) { fputc('#', out); return; }
    /* *print-namespace-maps* applies to sorted maps too. The detector
     * is a single in-order walk that short-circuits on the first
     * disqualifying key; non-maps and empty trees skip entirely. */
    if (is_map && S->print_namespace_maps_flag == 1
        && v->as.sorted.root != NULL) {
        rb_collect_common_ns(v->as.sorted.root, &common_ns,
                             &common_ns_len, &ns_ok);
        if (!ns_ok || common_ns == NULL) common_ns = NULL;
    }
    if (common_ns != NULL) {
        fputs("#:", out);
        fwrite(common_ns, 1, common_ns_len, out);
    }
    fputs(is_map ? "{" : "#{", out);
    S->print_depth++;
    /* Note: print-length isn't applied here -- print_rb_inorder is a
     * recursive in-order walk that doesn't carry a count. Sorted
     * collections are uncommon in user-facing prints; deferring the
     * length-limit on sorted to a follow-on if it surfaces in the
     * corpus. */
    if (common_ns != NULL) {
        print_rb_inorder_ns_stripped(S, out, v->as.sorted.root,
                                     common_ns_len, &first);
    } else {
        print_rb_inorder(S, out, v->as.sorted.root, is_map, &first);
    }
    S->print_depth--;
    fputc('}', out);
}

static void print_chunk(mino_state *S, FILE *out, const mino_val *v)
{
    /* Internal seq leaf; surface as `#chunk[…]` rather than
     * pretending to be a vector. */
    unsigned k;
    if (print_level_collapse(S)) { fputc('#', out); return; }
    fputs("#chunk[", out);
    S->print_depth++;
    for (k = 0; k < v->as.chunk.len; k++) {
        if (k > 0) fputc(' ', out);
        if (S->print_length_limit >= 0 && (int)k >= S->print_length_limit) {
            fputs("...", out);
            break;
        }
        mino_print_to(S, out, v->as.chunk.vals[k]);
    }
    S->print_depth--;
    fputc(']', out);
}

static void print_chunked_cons(mino_state *S, FILE *out, const mino_val *v)
{
    /* Print as a list. Walk the chunk from off..len-1, then recurse
     * into the more pointer (which may be cons / lazy / another
     * chunked-cons). */
    const mino_val *cur = v;
    int printed = 0;
    int truncated = 0;
    if (print_level_collapse(S)) { fputc('#', out); return; }
    fputc('(', out);
    S->print_depth++;
    while (cur != NULL && mino_type_of(cur) == MINO_CHUNKED_CONS) {
        const mino_val *ch = cur->as.chunked_cons.chunk;
        unsigned k;
        for (k = cur->as.chunked_cons.off; k < ch->as.chunk.len; k++) {
            if (printed > 0) fputc(' ', out);
            if (S->print_length_limit >= 0
                && printed >= S->print_length_limit) {
                fputs("...", out);
                truncated = 1;
                break;
            }
            mino_print_to(S, out, ch->as.chunk.vals[k]);
            printed++;
        }
        if (truncated) break;
        cur = cur->as.chunked_cons.more;
        if (cur != NULL && mino_type_of(cur) == MINO_LAZY) {
            cur = lazy_force(S, (mino_val *)cur);
        }
    }
    if (!truncated && cur != NULL && mino_type_of(cur) == MINO_CONS) {
        /* Reuse the cons walker by printing the tail inline. */
        while (cur != NULL && mino_type_of(cur) == MINO_CONS) {
            if (printed > 0) fputc(' ', out);
            if (S->print_length_limit >= 0
                && printed >= S->print_length_limit) {
                fputs("...", out);
                break;
            }
            mino_print_to(S, out, cur->as.cons.car);
            printed++;
            cur = cur->as.cons.cdr;
            if (cur != NULL && mino_type_of(cur) == MINO_LAZY) {
                cur = lazy_force(S, (mino_val *)cur);
            }
        }
    }
    S->print_depth--;
    fputc(')', out);
}

static void print_lazy(mino_state *S, FILE *out, const mino_val *v)
{
    /* Force the lazy seq and print the realized value. A lazy seq
     * that resolves to nil is the canonical empty seq — print as ()
     * so cross-type seq equality and printer agree. */
    mino_val *forced = lazy_force(S, (mino_val *)v);
    if (forced == NULL || mino_type_of(forced) == MINO_NIL
        || mino_type_of(forced) == MINO_EMPTY_LIST) {
        fputs("()", out);
        return;
    }
    mino_print_to(S, out, forced);
}

static void print_record(mino_state *S, FILE *out, const mino_val *v)
{
    /* #ns.Name{:f1 v1, :f2 v2, ...[ext...]} -- declared fields first
     * in declared order, then ext entries in insertion order. */
    const mino_val *t      = v->as.record.type;
    mino_val       *fields = t->as.record_type.fields;
    size_t            i, n_fields;
    int               first  = 1;
    fputc('#', out);
    if (t->as.record_type.ns != NULL) {
        fputs(t->as.record_type.ns, out);
        fputc('.', out);
    }
    if (t->as.record_type.name != NULL) {
        fputs(t->as.record_type.name, out);
    }
    fputc('{', out);
    S->print_depth++;
    n_fields = (fields != NULL) ? fields->as.vec.len : 0;
    for (i = 0; i < n_fields; i++) {
        if (!first) fputs(", ", out);
        first = 0;
        mino_print_to(S, out, vec_nth(fields, i));
        fputc(' ', out);
        mino_print_to(S, out, v->as.record.vals[i]);
    }
    if (v->as.record.ext != NULL) {
        const mino_val *e = v->as.record.ext;
        size_t k;
        for (k = 0; k < e->as.map.len; k++) {
            mino_val *key = vec_nth(e->as.map.key_order, k);
            if (!first) fputs(", ", out);
            first = 0;
            mino_print_to(S, out, key);
            fputc(' ', out);
            mino_print_to(S, out, map_get_val(e, key));
        }
    }
    S->print_depth--;
    fputc('}', out);
}

static void print_future(FILE *out, const mino_val *v)
{
    const mino_future *impl = v->as.future.impl;
    const char *st = "pending";
    if (impl != NULL) {
        switch (impl->state_tag) {
        case MINO_FUTURE_RESOLVED:  st = "resolved";  break;
        case MINO_FUTURE_FAILED:    st = "failed";    break;
        case MINO_FUTURE_CANCELLED: st = "cancelled"; break;
        default:                    st = "pending";   break;
        }
    }
    fputs("#<future:", out);
    fputs(st, out);
    fputc('>', out);
}

static void print_uuid(FILE *out, const mino_val *v)
{
    /* Canonical lowercase 8-4-4-4-12 form prefixed with #uuid so the
     * printed form roundtrips through the reader. */
    const unsigned char *b = v->as.uuid.bytes;
    char buf[64];
    snprintf(buf, sizeof(buf),
        "#uuid \"%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-"
        "%02x%02x%02x%02x%02x%02x\"",
        b[0],  b[1],  b[2],  b[3],  b[4],  b[5],  b[6],  b[7],
        b[8],  b[9],  b[10], b[11], b[12], b[13], b[14], b[15]);
    fputs(buf, out);
}

static void print_regex(FILE *out, const mino_val *v)
{
    /* Print as `#"source"` so the regex round-trips through the
     * reader. The source bytes pass through unescaped -- the reader's
     * regex literal accepts the same form. */
    const mino_val *src = v->as.regex.source;
    fputs("#\"", out);
    if (src != NULL && mino_type_of(src) == MINO_STRING) {
        fwrite(src->as.s.data, 1, src->as.s.len, out);
    }
    fputc('"', out);
}

static void print_host_array(mino_state *S, FILE *out, const mino_val *v)
{
    /* Mirror Clojure JVM's #object[...] form for arrays since arrays
     * don't round-trip through the reader. */
    static const char *kinds[] = {
        "Object", "int", "long", "short", "byte",
        "float",  "double", "char", "boolean"
    };
    unsigned k = v->as.host_array.element_kind;
    size_t   i;
    if (k >= sizeof(kinds) / sizeof(kinds[0])) k = 0;
    fprintf(out, "#object[\"[L%s;\" 0x%p {", kinds[k], (const void *)v);
    for (i = 0; i < v->as.host_array.len; i++) {
        if (i > 0) fputs(", ", out);
        mino_print_to(S, out, v->as.host_array.vals[i]);
    }
    fputs("}]", out);
}

void mino_print_to(mino_state *S, FILE *out, const mino_val *v)
{
    if (v == NULL || (MINO_IS_PTR(v) && mino_type_of(v) == MINO_NIL)) {
        fputs("nil", out);
        return;
    }
    if (S->print_depth >= MINO_PRINT_DEPTH_MAX) {
        fputs("#<...>", out);
        return;
    }
    /* *print-meta* prelude: when the dynvar is bound truthy and the
     * value carries non-nil meta, emit ^meta then a space before the
     * value's normal print form. The check requires MINO_IS_PTR(v):
     * inline-int / bool / nil / char values are tagged pointers, not
     * heap-allocated mino_val cells, so v->meta would be a garbage
     * deref. Tagged primitives can't carry meta in any case. The flag
     * is cleared around the meta print so the meta map's own meta
     * (if any) doesn't recurse on itself. */
    if (S->print_meta_flag == 1 && MINO_IS_PTR(v) && v->meta != NULL
        && mino_type_of(v->meta) != MINO_NIL) {
        int saved_meta = S->print_meta_flag;
        S->print_meta_flag = 0;
        fputc('^', out);
        mino_print_to(S, out, v->meta);
        fputc(' ', out);
        S->print_meta_flag = saved_meta;
    }
    switch (mino_type_of(v)) {
    case MINO_NIL:
        fputs("nil", out);
        return;
    case MINO_EMPTY_LIST:
        fputs("()", out);
        return;
    case MINO_BOOL:
        fputs(mino_val_bool_get(v) ? "true" : "false", out);
        return;
    case MINO_INT:
        fprintf(out, "%lld", mino_val_int_get(v));
        return;
    case MINO_BIGINT:
        mino_bigint_print(S, v, out);
        return;
    case MINO_RATIO:
        mino_ratio_print(S, v, out);
        return;
    case MINO_BIGDEC:
        mino_bigdec_print(S, v, out);
        return;
    case MINO_FLOAT:
        print_float(out, v->as.f);
        return;
    case MINO_FLOAT32:
        print_float32(out, v->as.f);
        return;
    case MINO_CHAR:
        if (S->print_readably_flag == 0) {
            /* Unreadable form: emit the UTF-8 bytes of the codepoint. */
            int cp = mino_val_char_get(v);
            unsigned char buf[4];
            int n = 0;
            if (cp < 0x80) {
                buf[n++] = (unsigned char)cp;
            } else if (cp < 0x800) {
                buf[n++] = (unsigned char)(0xC0u | (unsigned)(cp >> 6));
                buf[n++] = (unsigned char)(0x80u | ((unsigned)cp & 0x3Fu));
            } else if (cp < 0x10000) {
                buf[n++] = (unsigned char)(0xE0u | (unsigned)(cp >> 12));
                buf[n++] = (unsigned char)(0x80u | (((unsigned)cp >> 6) & 0x3Fu));
                buf[n++] = (unsigned char)(0x80u | ((unsigned)cp & 0x3Fu));
            } else {
                buf[n++] = (unsigned char)(0xF0u | (unsigned)(cp >> 18));
                buf[n++] = (unsigned char)(0x80u | (((unsigned)cp >> 12) & 0x3Fu));
                buf[n++] = (unsigned char)(0x80u | (((unsigned)cp >> 6) & 0x3Fu));
                buf[n++] = (unsigned char)(0x80u | ((unsigned)cp & 0x3Fu));
            }
            fwrite(buf, 1, (size_t)n, out);
        } else {
            print_char(out, mino_val_char_get(v));
        }
        return;
    case MINO_STRING:
        if (S->print_readably_flag == 0) {
            fwrite(v->as.s.data, 1, v->as.s.len, out);
        } else {
            print_string_escaped(out, v->as.s.data, v->as.s.len);
        }
        return;
    case MINO_SYMBOL:
        fwrite(v->as.s.data, 1, v->as.s.len, out);
        return;
    case MINO_KEYWORD:
        fputc(':', out);
        fwrite(v->as.s.data, 1, v->as.s.len, out);
        return;
    case MINO_CONS:
        print_cons(S, out, v);
        return;
    case MINO_VECTOR:
        print_vector(S, out, v);
        return;
    case MINO_MAP:
        if (print_instant_literal(S, out, v)) {
            return;
        }
        print_map(S, out, v);
        return;
    case MINO_SET:
        print_set(S, out, v);
        return;
    case MINO_SORTED_MAP:
        print_sorted(S, out, v, 1);
        return;
    case MINO_SORTED_SET:
        print_sorted(S, out, v, 0);
        return;
    case MINO_PRIM:
        fputs("#<prim ", out);
        if (v->as.prim.name != NULL) {
            fputs(v->as.prim.name, out);
        }
        fputc('>', out);
        return;
    case MINO_FN:
        fputs("#<fn>", out);
        return;
    case MINO_MACRO:
        fputs("#<macro>", out);
        return;
    case MINO_HANDLE:
        fputs("#<handle", out);
        if (v->as.handle.tag != NULL) {
            fputc(':', out);
            fputs(v->as.handle.tag, out);
        }
        fputc('>', out);
        return;
    case MINO_ATOM:
        fputs("#atom[", out);
        S->print_depth++;
        mino_print_to(S, out, v->as.atom.val);
        S->print_depth--;
        fputc(']', out);
        return;
    case MINO_VOLATILE:
        fputs("#volatile[", out);
        S->print_depth++;
        mino_print_to(S, out, v->as.volatile_.val);
        S->print_depth--;
        fputc(']', out);
        return;
    case MINO_LAZY: {
        /* Force the lazy seq and print the realized value. A lazy seq
         * that resolves to nil is the canonical empty seq — print as
         * () so cross-type seq equality and printer agree. */
        print_lazy(S, out, v);
        return;
    }
    case MINO_CHUNK:
        print_chunk(S, out, v);
        return;
    case MINO_CHUNKED_CONS:
        print_chunked_cons(S, out, v);
        return;
    case MINO_RECUR:
        /* Internal sentinel; should not escape to user-visible output. */
        fputs("#<recur>", out);
        return;
    case MINO_TAIL_CALL:
        fputs("#<tail-call>", out);
        return;
    case MINO_REDUCED:
        fputs("#<reduced ", out);
        mino_print_to(S, out, v->as.reduced.val);
        fputc('>', out);
        return;
    case MINO_VAR:
        fputs("#'", out);
        if (v->as.var.ns != NULL) {
            fputs(v->as.var.ns, out);
            fputc('/', out);
        }
        fputs(v->as.var.sym, out);
        return;
    case MINO_TRANSIENT:
        fputs("#<transient", out);
        if (v->as.transient.current != NULL) {
            fputc(':', out);
            switch (mino_type_of(v->as.transient.current)) {
            case MINO_VECTOR: fputs("vector", out); break;
            case MINO_MAP:    fputs("map",    out); break;
            case MINO_SET:    fputs("set",    out); break;
            default: fputs("?", out); break;
            }
        }
        if (!v->as.transient.valid) fputs(" sealed", out);
        fputc('>', out);
        return;
    case MINO_TYPE:
        if (v->as.record_type.ns != NULL) {
            fputs(v->as.record_type.ns, out);
            fputc('.', out);
        }
        if (v->as.record_type.name != NULL) {
            fputs(v->as.record_type.name, out);
        }
        return;
    case MINO_RECORD:
        print_record(S, out, v);
        return;
    case MINO_FUTURE:
        print_future(out, v);
        return;
    case MINO_UUID:
        print_uuid(out, v);
        return;
    case MINO_REGEX:
        print_regex(out, v);
        return;
    case MINO_MAP_ENTRY: {
        /* Print as `[k v]` so map-entry round-trips through equality
         * with a 2-vector and matches Clojure's pr/print form. */
        fputc('[', out);
        mino_print_to(S, out, v->as.map_entry.k);
        fputc(' ', out);
        mino_print_to(S, out, v->as.map_entry.v);
        fputc(']', out);
        return;
    }
    case MINO_TX_REF:
        /* `#ref[ID VAL]` -- not round-trippable, but consistent with
         * the rest of mino's identity-cell prints (atom, volatile). */
        fprintf(out, "#ref[0x%llx ",
                (unsigned long long)v->as.tx_ref.ref_id);
        S->print_depth++;
        mino_print_to(S, out, v->as.tx_ref.val);
        S->print_depth--;
        fputc(']', out);
        return;
    case MINO_AGENT:
        /* `#agent[ID VAL]` -- not round-trippable, but ID gives
         * each agent a stable identity in the print form so two
         * distinct agents holding the same value are still
         * distinguishable. Mirrors `#ref[ID VAL]`. */
        fprintf(out, "#agent[0x%llx ",
                (unsigned long long)v->as.agent.agent_id);
        S->print_depth++;
        mino_print_to(S, out, v->as.agent.val);
        S->print_depth--;
        fputc(']', out);
        return;
    case MINO_HOST_ARRAY:
        print_host_array(S, out, v);
        return;
    case MINO_CHAN:
        /* `#chan[0xPTR]` -- identity-based, like agents / refs. */
        fprintf(out, "#chan[%p]", (const void *)v);
        return;
    case MINO_QUEUE: {
        /* `#queue [a b c]` — Clojure JVM's print-method emits this
         * shape (modulo the JVM's `<-(...)-<` legacy form which is
         * not portable). The body is the queue's elements in deque
         * order. */
        mino_val *seqv = mino_queue_seq(S, v);
        fputs("#queue [", out);
        S->print_depth++;
        {
            mino_val *cur = seqv;
            int first = 1;
            while (cur != NULL && mino_is_cons(cur)) {
                if (!first) fputc(' ', out);
                mino_print_to(S, out, cur->as.cons.car);
                cur = cur->as.cons.cdr;
                first = 0;
            }
        }
        S->print_depth--;
        fputc(']', out);
        return;
    }
    case MINO_BYTES: {
        /* `#bytes "HEX..."` -- a tagged-literal-shaped form that
         * round-trips through read. Hex pairs match the bytes in
         * order; when bit_tail != 0, append `/N` to the printed
         * form to denote the trailing bit count. */
        size_t i;
        fputs("#bytes \"", out);
        for (i = 0; i < v->as.bytes.byte_len; i++) {
            fprintf(out, "%02x", (unsigned)v->as.bytes.data[i]);
        }
        if (v->as.bytes.bit_tail != 0) {
            fprintf(out, "/%u", (unsigned)v->as.bytes.bit_tail);
        }
        fputc('"', out);
        return;
    }
    }
}

void mino_print(mino_state *S, const mino_val *v)
{
    mino_print_to(S, stdout, v);
}

void mino_println(mino_state *S, const mino_val *v)
{
    mino_print_to(S, stdout, v);
    fputc('\n', stdout);
}

int mino_print_to_buf(mino_state *S, const mino_val *v,
                      char *buf, size_t n)
{
    FILE *out;
    size_t written;
    int rc = -1;
    if (buf == NULL || n == 0) return -1;
    out = tmpfile();
    if (out == NULL) {
        buf[0] = '\0';
        return -1;
    }
    mino_print_to(S, out, v);
    fflush(out);
    rewind(out);
    written = fread(buf, 1, n - 1, out);
    buf[written] = '\0';
    rc = (int)written;
    fclose(out);
    return rc;
}

