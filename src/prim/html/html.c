/*
 * html.c -- native HTML/XML reader, two modes over one core (ADR 28).
 *
 * Single-pass byte-cursor tokenizer and tree assembler backing the
 * html-parse prim (the mino.html facade rides it): hickory-shaped
 * node maps {:type :element|:comment|:document-type :tag :attrs
 * :content} constructed directly in C in the json.c/toml.c lineage;
 * no event objects cross the boundary. The xml-parse prim (the
 * clojure.xml mirror rides it) runs the strict mode over the same
 * machinery: JVM clojure.xml nodes without :type, the strict
 * entity rule (five predefined plus numeric only, D3), CDATA
 * capture, mandatory attribute quoting, single-root and prolog
 * checks, and its own positioned error classes.
 *
 * The tolerance tier is the enumerated set pinned in the campaign's
 * technical design: unclosed tags auto-balance at EOF, stray end
 * tags drop, misnested end tags pop-until, the WHATWG void list, the
 * trailing solidus honored on every start tag, tolerant attributes,
 * script/style RAWTEXT with title/textarea RCDATA, character
 * references through the generated table (html_entities.c, the
 * python3 html.entities oracle), bogus comments, first-DOCTYPE-only,
 * name lowercasing, PLAINTEXT rest-of-input, NUL-to-U+FFFD in text,
 * and the 256-deep open-element cap (:max-depth, the only
 * non-recovering edge besides eof-in-tag). The fixup layer adds
 * like-tag implied closes bounded by scope barriers, the verbatim
 * p-closing list, and simplified html/head/body synthesis in
 * document mode; parse-fragment skips the synthesis.
 *
 * The prim returns the tree, or an error descriptor
 * [:html/error "reason" line col "text"] the facade converts to
 * ex-info. Rooting follows toml.c: in-flight values live in C
 * locals (this struct) under the conservative stack scan; decode
 * scratch buffers are GC_T_RAW memory held under a gc_depth guard.
 */

#include "prim/internal.h"
#include "mino.h"
#include "html_entities.h"

#include <string.h>

#define HP_MAX_DEPTH 256

/* Wrapper state machine: 0 none, 1 implied-open, 2 explicit-open,
 * 3 closed. */
#define HP_W_NONE 0
#define HP_W_IMPL 1
#define HP_W_EXPL 2
#define HP_W_CLOSED 3

typedef struct {
    mino_val *acc;    /* transient vector of children */
    mino_val *tag;    /* interned lowercase keyword (borrowed) */
    mino_val *attrs;  /* persistent map */
    int       is_impl;
} hp_open_t;

typedef struct {
    mino_state          *S;
    mino_env            *env;
    const unsigned char *p;
    const unsigned char *end;
    /* line bookkeeping: synced up to h->synced only */
    const unsigned char *line_start;
    const unsigned char *synced;
    size_t               line;
    int                  fragment;
    int                  failed;
    const char          *err_code;
    size_t               err_line;
    size_t               err_col;
    const unsigned char *err_tok;  /* token start for the position */
    mino_val            *doc;      /* transient: document children */
    hp_open_t            stack[HP_MAX_DEPTH];
    int                  depth;
    int                  html_st;
    int                  head_st;
    int                  body_st;
    int                  seen_doctype;
    int                  plain;
    /* pending text run: a raw segment [pend_from, p) plus an optional
     * carry buffer left by skipped </> tokens inside the run (the
     * oracle merges the data around them into one event) */
    const unsigned char *pend_from;
    unsigned char       *pend_buf; /* NULL until a </> splits a run */
    size_t               pend_cap;
    size_t               pend_w;
    /* XML mode state (ADR 28: one core, two modes). pend_buf is
     * reused as the XML character-data carry (pend_w its length);
     * the html pending fields above are untouched in XML mode.
     * seen_doctype is shared: the html rule-10 flag and the xml
     * one-doctype rule never run in the same parse. */
    int                  xml;
    int                  seen_root;
    const unsigned char *xml_start; /* doc start after BOM strip */
    mino_val            *xml_root;  /* the root element node */
} hp_t;

static int hp_run(hp_t *h);
static int hp_pop(hp_t *h);
static int hp_pop_until(hp_t *h, const char *name);

/* ---- small predicates over raw name spans ---- */

static int hp_lower(unsigned char c)
{
    if (c >= 'A' && c <= 'Z') return c + 32;
    return c;
}

static int hp_is_ws(unsigned char c)
{
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == 0x0C;
}

/* The oracle's str whitespace (str.strip / regex \s), used only for
 * DOCTYPE text trimming; tag scanning keeps the WHATWG set above. */
static int hp_is_trim_ws(unsigned char c)
{
    return c == ' ' || c == '\t' || c == '\n' || c == '\r'
        || c == 0x0B || c == 0x0C || c == 0x1C || c == 0x1D
        || c == 0x1E || c == 0x1F;
}

static int hp_is_alpha(unsigned char c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
}

static int hp_is_alnum(unsigned char c)
{
    return hp_is_alpha(c) || (c >= '0' && c <= '9');
}

static int hp_is_digit(unsigned char c)
{
    return c >= '0' && c <= '9';
}

static int hp_hex_val(unsigned char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

/* Case-insensitive equality between a raw span and a lowercase
 * literal. */
static int hp_span_ci_eq(const unsigned char *s, size_t len,
                         const char *lit)
{
    size_t n = strlen(lit);
    size_t i;
    if (n != len) return 0;
    for (i = 0; i < n; i++) {
        if (hp_lower(s[i]) != (unsigned char)lit[i]) return 0;
    }
    return 1;
}

/* Exact equality between an interned keyword's bytes and a
 * lowercase literal (keywords are stored lowercase here). */
static int hp_kw_is(const mino_val *kw, const char *lit)
{
    size_t n = strlen(lit);
    return kw->as.s.len == n && memcmp(kw->as.s.data, lit, n) == 0;
}

/* Byte equality between two interned keywords (tag names). */
static int hp_kw_eq(const mino_val *a, const mino_val *b)
{
    return a == b
        || (a->as.s.len == b->as.s.len
            && memcmp(a->as.s.data, b->as.s.data, a->as.s.len) == 0);
}

static int hp_kw_void(const mino_val *kw)
{
    static const char *const k_void[] = {
        "area", "base", "br", "col", "embed", "hr", "img", "input",
        "link", "meta", "param", "source", "track", "wbr", NULL
    };
    int i;
    for (i = 0; k_void[i] != NULL; i++) {
        if (hp_kw_is(kw, k_void[i])) return 1;
    }
    return 0;
}

static int hp_kw_rawtext(const mino_val *kw)
{
    return hp_kw_is(kw, "script") || hp_kw_is(kw, "style");
}

static int hp_kw_rcdata(const mino_val *kw)
{
    return hp_kw_is(kw, "title") || hp_kw_is(kw, "textarea");
}

static int hp_kw_head_elem(const mino_val *kw)
{
    /* Tier rule 17: elements head receives before body content. */
    static const char *const k_head[] = {
        "base", "basefont", "bgsound", "link", "meta", "noframes",
        "script", "style", "template", "title", NULL
    };
    int i;
    for (i = 0; k_head[i] != NULL; i++) {
        if (hp_kw_is(kw, k_head[i])) return 1;
    }
    return 0;
}

static int hp_kw_r15(const mino_val *kw)
{
    /* Tier rule 15 like-tag close set. */
    static const char *const k_r15[] = {
        "li", "dd", "dt", "option", "optgroup", "p", "rb", "rp",
        "rt", "rtc", "tr", "td", "th", "tbody", "thead", "tfoot",
        "caption", "colgroup", NULL
    };
    int i;
    for (i = 0; k_r15[i] != NULL; i++) {
        if (hp_kw_is(kw, k_r15[i])) return 1;
    }
    return 0;
}

static int hp_kw_p_closer(const mino_val *kw)
{
    /* Tier rule 16, the verbatim WHATWG 13.2.6.3 list. */
    static const char *const k_pc[] = {
        "address", "article", "aside", "blockquote", "center",
        "details", "dialog", "dir", "div", "dl", "fieldset",
        "figcaption", "figure", "footer", "form", "h1", "h2", "h3",
        "h4", "h5", "h6", "header", "hgroup", "hr", "main", "menu",
        "nav", "ol", "p", "pre", "search", "section", "summary",
        "table", "ul", NULL
    };
    int i;
    for (i = 0; k_pc[i] != NULL; i++) {
        if (hp_kw_is(kw, k_pc[i])) return 1;
    }
    return 0;
}

static int hp_kw_barrier(const mino_val *kw)
{
    /* Simplified scope barriers (rule 15). */
    static const char *const k_bar[] = {
        "table", "td", "th", "caption", "template", "html", NULL
    };
    int i;
    for (i = 0; k_bar[i] != NULL; i++) {
        if (hp_kw_is(kw, k_bar[i])) return 1;
    }
    return 0;
}

static int hp_kw_heading(const mino_val *kw)
{
    return hp_kw_is(kw, "h1") || hp_kw_is(kw, "h2")
        || hp_kw_is(kw, "h3") || hp_kw_is(kw, "h4")
        || hp_kw_is(kw, "h5") || hp_kw_is(kw, "h6");
}

/* ---- errors ---- */

static void hp_sync_line(hp_t *h)
{
    /* Advance the line counters over everything consumed since the
     * last sync. */
    const unsigned char *q = h->synced;
    while (q < h->p) {
        if (*q == '\n') {
            h->line++;
            h->line_start = q + 1;
        }
        q++;
    }
    h->synced = h->p;
}

static void hp_fail(hp_t *h, const char *code)
{
    if (!h->failed) {
        h->failed = 1;
        h->err_code = code;
        h->err_line = h->line;
        h->err_col = (size_t)(h->err_tok - h->line_start) + 1;
    }
}

/* ---- constructors ---- */

static mino_val *hp_empty_map(hp_t *h)
{
    return mino_map(h->S, NULL, NULL, 0);
}

static mino_val *hp_empty_transient(hp_t *h)
{
    mino_val *v = mino_vector(h->S, NULL, 0);
    if (v == NULL) return NULL;
    return mino_transient(h->S, v);
}

/* Lowercased keyword from a raw span. */
static mino_val *hp_name_keyword(hp_t *h, const unsigned char *s,
                                 size_t len)
{
    unsigned char lbuf[64];
    unsigned char *lp;
    mino_val *kw;
    size_t i;
    if (len <= sizeof(lbuf)) {
        lp = lbuf;
    } else {
        int saved = mino_current_ctx(h->S)->gc_depth;
        mino_current_ctx(h->S)->gc_depth = saved + 1;
        lp = (unsigned char *)gc_alloc_typed_inner(h->S, GC_T_RAW, len);
        mino_current_ctx(h->S)->gc_depth = saved;
        if (lp == NULL) return NULL;
    }
    for (i = 0; i < len; i++) {
        lp[i] = (unsigned char)hp_lower(s[i]);
    }
    kw = mino_keyword_n(h->S, (const char *)lp, len);
    return kw;
}

/* Child target: the top element's accumulator, or the document's. */
static int hp_add_child(hp_t *h, mino_val *child)
{
    mino_val *acc;
    mino_val *next;
    if (h->depth == 0) {
        acc = h->doc;
    } else {
        acc = h->stack[h->depth - 1].acc;
    }
    next = mino_conj_bang(h->S, acc, child);
    if (next == NULL) return -1;
    if (h->depth == 0) {
        h->doc = next;
    } else {
        h->stack[h->depth - 1].acc = next;
    }
    return 0;
}

/* Push one open element. */
static int hp_push(hp_t *h, mino_val *tag, mino_val *attrs, int impl)
{
    hp_open_t *o;
    if (h->depth >= HP_MAX_DEPTH) {
        hp_fail(h, "max-depth");
        return -1;
    }
    o = &h->stack[h->depth];
    o->acc = hp_empty_transient(h);
    o->tag = tag;
    o->attrs = attrs;
    o->is_impl = impl;
    if (o->acc == NULL) return -1;
    h->depth++;
    return 0;
}

static mino_val *hp_kw_type(hp_t *h)
{
    return mino_keyword(h->S, "type");
}

static mino_val *hp_kw_tag(hp_t *h)
{
    return mino_keyword(h->S, "tag");
}

static mino_val *hp_kw_attrs(hp_t *h)
{
    return mino_keyword(h->S, "attrs");
}

static mino_val *hp_kw_content(hp_t *h)
{
    return mino_keyword(h->S, "content");
}

static mino_val *hp_kw_element(hp_t *h)
{
    return mino_keyword(h->S, "element");
}

/* Build a one-string-content node ({:type t :content [s]}). */
static mino_val *hp_string_node(hp_t *h, const char *type,
                                mino_val *s)
{
    mino_state *S = h->S;
    mino_val *items[1];
    mino_val *content;
    mino_val *keys[2];
    mino_val *vals[2];
    items[0] = s;
    content = mino_vector(S, items, 1);
    if (content == NULL) return NULL;
    keys[0] = hp_kw_type(h);
    keys[1] = hp_kw_content(h);
    vals[0] = mino_keyword(S, type);
    vals[1] = content;
    return mino_map(S, keys, vals, 2);
}

/* Close the top element, materializing its node. */
static int hp_pop(hp_t *h)
{
    mino_state *S = h->S;
    hp_open_t *o;
    mino_val *content;
    mino_val *keys[4];
    mino_val *vals[4];
    mino_val *node;
    h->depth--;
    o = &h->stack[h->depth];
    content = mino_persistent(S, o->acc);
    if (content == NULL) return -1;
    keys[0] = hp_kw_type(h);
    keys[1] = hp_kw_tag(h);
    keys[2] = hp_kw_attrs(h);
    keys[3] = hp_kw_content(h);
    vals[0] = hp_kw_element(h);
    vals[1] = o->tag;
    vals[2] = o->attrs;
    vals[3] = content;
    node = mino_map(S, keys, vals, 4);
    if (node == NULL) return -1;
    if (hp_add_child(h, node) != 0) return -1;
    if (hp_kw_is(o->tag, "html")) {
        h->html_st = HP_W_CLOSED;
    } else if (hp_kw_is(o->tag, "head")) {
        h->head_st = HP_W_CLOSED;
    } else if (hp_kw_is(o->tag, "body")) {
        h->body_st = HP_W_CLOSED;
    }
    return 0;
}

/* Close every element through (and including) the nearest open one
 * whose tag matches the lowercase literal. A no-op when absent. */
static int hp_pop_until(hp_t *h, const char *name)
{
    int i;
    int hit = -1;
    for (i = h->depth - 1; i >= 0; i--) {
        if (hp_kw_is(h->stack[i].tag, name)) {
            hit = i;
            break;
        }
    }
    if (hit < 0) return 0;
    while (h->depth > hit) {
        if (hp_pop(h) != 0) return -1;
    }
    return 0;
}

/* Index of the nearest open tag matching the literal, or -1. */
static int hp_find_open(hp_t *h, const char *name)
{
    int i;
    for (i = h->depth - 1; i >= 0; i--) {
        if (hp_kw_is(h->stack[i].tag, name)) {
            return i;
        }
    }
    return -1;
}

/* ---- document-mode wrapper synthesis (rule 17) ---- */

static int hp_ensure_html(hp_t *h)
{
    if (h->html_st == HP_W_NONE || h->html_st == HP_W_CLOSED) {
        mino_val *attrs = hp_empty_map(h);
        if (attrs == NULL) return -1;
        if (hp_push(h, mino_keyword(h->S, "html"), attrs, 1) != 0) {
            return -1;
        }
        h->html_st = HP_W_IMPL;
    }
    return 0;
}

static int hp_push_lit(hp_t *h, const char *name, mino_val *attrs,
                       int impl, int *st, int st_val)
{
    if (hp_push(h, mino_keyword(h->S, name), attrs, impl) != 0) {
        return -1;
    }
    if (st != NULL) {
        *st = st_val;
    }
    return 0;
}

/* Open the body (implied), synthesizing/closing the head first so
 * html always ends up with exactly head-then-body shape. */
static int hp_start_body(hp_t *h)
{
    if (h->body_st == HP_W_IMPL || h->body_st == HP_W_EXPL) {
        return 0;
    }
    if (hp_ensure_html(h) != 0) return -1;
    if (h->head_st == HP_W_NONE) {
        /* synthesize the empty head so it always exists */
        if (hp_push_lit(h, "head", hp_empty_map(h), 1,
                        &h->head_st, HP_W_IMPL) != 0) {
            return -1;
        }
        if (hp_pop(h) != 0) return -1;
    } else if (h->head_st == HP_W_IMPL || h->head_st == HP_W_EXPL) {
        if (hp_pop_until(h, "head") != 0) return -1;
    }
    return hp_push_lit(h, "body", hp_empty_map(h), 1,
                       &h->body_st, HP_W_IMPL);
}

/* Is the insertion position top-level for synthesis purposes: no
 * element open, or the open element is the html/head wrapper. */
static int hp_at_wrapper_level(hp_t *h)
{
    if (h->depth == 0) return 1;
    return hp_kw_is(h->stack[h->depth - 1].tag, "html")
        || hp_kw_is(h->stack[h->depth - 1].tag, "head");
}

/* ---- character references (tier rule 8) ---- */

/* Windows-1252 remap for numeric refs 0x80..0x9F (the python oracle
 * table, CPython html/__init__ _invalid_charrefs). */
static const unsigned short k_hp_win1252[32] = {
    0x20AC, 0x0081, 0x201A, 0x0192, 0x201E, 0x2026, 0x2020, 0x2021,
    0x02C6, 0x2030, 0x0160, 0x2039, 0x0152, 0x008D, 0x017D, 0x008F,
    0x0090, 0x2018, 0x2019, 0x201C, 0x201D, 0x2022, 0x2013, 0x2014,
    0x02DC, 0x2122, 0x0161, 0x203A, 0x0153, 0x009D, 0x017E, 0x0178
};

/* Code points the oracle drops outright (CPython
 * _invalid_codepoints): C0 controls, 0x7F, and the noncharacters. */
static int hp_dropped_cp(unsigned long cp)
{
    if ((cp >= 0x01 && cp <= 0x08) || (cp >= 0x0E && cp <= 0x1F)
        || cp == 0x0B || cp == 0x7F
        || (cp >= 0xFDD0 && cp <= 0xFDEF)) {
        return 1;
    }
    if (cp >= 0xFFFE && cp <= 0x10FFFF) {
        unsigned long lo = cp & 0xFFFF;
        if (lo == 0xFFFE || lo == 0xFFFF) return 1;
    }
    return 0;
}

static size_t hp_utf8(unsigned long cp, unsigned char *out)
{
    if (cp < 0x80) {
        out[0] = (unsigned char)cp;
        return 1;
    }
    if (cp < 0x800) {
        out[0] = (unsigned char)(0xC0 | (cp >> 6));
        out[1] = (unsigned char)(0x80 | (cp & 0x3F));
        return 2;
    }
    if (cp < 0x10000) {
        out[0] = (unsigned char)(0xE0 | (cp >> 12));
        out[1] = (unsigned char)(0x80 | ((cp >> 6) & 0x3F));
        out[2] = (unsigned char)(0x80 | (cp & 0x3F));
        return 3;
    }
    out[0] = (unsigned char)(0xF0 | (cp >> 18));
    out[1] = (unsigned char)(0x80 | ((cp >> 12) & 0x3F));
    out[2] = (unsigned char)(0x80 | ((cp >> 6) & 0x3F));
    out[3] = (unsigned char)(0x80 | (cp & 0x3F));
    return 4;
}

/* Emit one numeric code point per the oracle policing; returns the
 * new write index. */
static long long hp_emit_cp(unsigned long cp, unsigned char *buf, long long w)
{
    if (cp == 0) {
        cp = 0xFFFD;
    } else if (cp == 0x0D) {
        /* identity: carriage return passes */
    } else if (cp >= 0x80 && cp <= 0x9F) {
        cp = k_hp_win1252[cp - 0x80];
    } else if (cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) {
        cp = 0xFFFD;
    } else if (hp_dropped_cp(cp)) {
        return w;
    }
    return w + (long long)hp_utf8(cp, buf + w);
}

static int hp_ent_cmp(const void *a, const void *b)
{
    return strcmp((const char *)a,
                  ((const mino_html_entity_t *)b)->name);
}

/* Exact-table lookup of a candidate name (already NUL-terminated in
 * cand). Returns the entry or NULL. */
static const mino_html_entity_t *hp_ent_lookup(const char *cand)
{
    return (const mino_html_entity_t *)bsearch(
        cand, k_html_entities, k_html_entities_count,
        sizeof(k_html_entities[0]), hp_ent_cmp);
}

/* Decode the reference at *pp (pointing at '&') against [to).
 * Returns the new write index and advances *pp on match; returns -1
 * when nothing matches (the caller emits a literal '&'). attr
 * selects the tier rule 8 attribute mode: named references require
 * the semicolon. */
static long long hp_ref(const unsigned char **pp, const unsigned char *to,
                   int attr, unsigned char *buf, long long w)
{
    const unsigned char *r = *pp + 1;
    size_t alen;
    int has_semi;
    if (r >= to) return -1;
    if (*r == '#') {
        const unsigned char *d = r + 1;
        int hex = 0;
        unsigned long cp = 0;
        int ndig = 0;
        if (d < to && (*d == 'x' || *d == 'X')) {
            hex = 1;
            d++;
        }
        while (d < to) {
            int dv = hex ? hp_hex_val(*d)
                         : (hp_is_digit(*d) ? *d - '0' : -1);
            if (dv < 0) break;
            if (cp <= 0x10FFFF) {
                cp = cp * (unsigned long)(hex ? 16 : 10)
                     + (unsigned long)dv;
            }
            ndig++;
            d++;
        }
        if (ndig == 0) return -1;
        if (d < to && *d == ';') d++;
        w = hp_emit_cp(cp, buf, w);
        *pp = d;
        return w;
    }
    if (!hp_is_alnum(*r)) return -1;
    alen = 0;
    while (r + alen < to && alen < 32 && hp_is_alnum(r[alen])) {
        alen++;
    }
    has_semi = (r + alen < to && r[alen] == ';');
    if (attr) {
        char cand[34];
        const mino_html_entity_t *e;
        size_t vlen;
        if (!has_semi) return -1;
        memcpy(cand, r, alen);
        cand[alen] = ';';
        cand[alen + 1] = '\0';
        e = hp_ent_lookup(cand);
        if (e == NULL) return -1;
        vlen = strlen(e->value);
        memcpy(buf + w, e->value, vlen);
        *pp = r + alen + 1;
        return w + (long long)vlen;
    }
    /* text mode: exact semicolon form, then bare prefixes from the
     * longest down (the oracle's longest-match rule) */
    if (has_semi) {
        char cand[34];
        const mino_html_entity_t *e;
        size_t vlen;
        memcpy(cand, r, alen);
        cand[alen] = ';';
        cand[alen + 1] = '\0';
        e = hp_ent_lookup(cand);
        if (e != NULL) {
            vlen = strlen(e->value);
            memcpy(buf + w, e->value, vlen);
            *pp = r + alen + 1;
            return w + (long long)vlen;
        }
    }
    {
        size_t L;
        char cand[33];
        for (L = alen; L >= 2; L--) {
            const mino_html_entity_t *e;
            size_t vlen;
            memcpy(cand, r, L);
            cand[L] = '\0';
            e = hp_ent_lookup(cand);
            if (e != NULL) {
                vlen = strlen(e->value);
                memcpy(buf + w, e->value, vlen);
                *pp = r + L;
                return w + (long long)vlen;
            }
        }
    }
    return -1;
}

/* ---- text runs ---- */

/* Decode [from,to) into buf at w (entities per the text rule, NUL to
 * U+FFFD). Returns the new write index or -1. buf is grown to fit by
 * the caller. */
static long long hp_decode_into(const unsigned char *from,
                           const unsigned char *to, unsigned char *buf,
                           long long w)
{
    const unsigned char *r = from;
    while (r < to) {
        if (*r == '&') {
            long long nw = hp_ref(&r, to, 0, buf, w);
            if (nw < 0) {
                buf[w++] = '&';
                r++;
            } else {
                w = nw;
            }
        } else if (*r == 0) {
            buf[w++] = 0xEF;
            buf[w++] = 0xBF;
            buf[w++] = 0xBD;
            r++;
        } else {
            buf[w++] = *r++;
        }
    }
    return w;
}

/* Build a text string over [from,to): entities decode per mode,
 * NUL becomes U+FFFD in text/RCDATA; raw keeps bytes verbatim.
 * mode: 0 text, 1 raw. */
static mino_val *hp_text_val(hp_t *h, const unsigned char *from,
                             const unsigned char *to, int raw)
{
    mino_state *S = h->S;
    const unsigned char *q;
    if (raw) {
        return mino_string_n(S, (const char *)from,
                             (size_t)(to - from));
    }
    for (q = from; q < to; q++) {
        if (*q == '&' || *q == 0) break;
    }
    if (q == to) {
        return mino_string_n(S, (const char *)from,
                             (size_t)(to - from));
    }
    {
        /* worst growth: NUL to three bytes, entities under two */
        size_t cap = (size_t)(to - from) * 3 + 16;
        unsigned char *buf;
        long long w;
        int saved = mino_current_ctx(S)->gc_depth;
        mino_val *out;
        mino_current_ctx(S)->gc_depth = saved + 1;
        buf = (unsigned char *)gc_alloc_typed_inner(S, GC_T_RAW, cap);
        if (buf == NULL) {
            mino_current_ctx(S)->gc_depth = saved;
            return NULL;
        }
        w = hp_decode_into(from, to, buf, 0);
        out = mino_string_n(S, (const char *)buf, (size_t)w);
        mino_current_ctx(S)->gc_depth = saved;
        return out;
    }
}

/* Route a completed text string per the document-mode rules and add
 * it as a child. */
static int hp_add_text_val(hp_t *h, mino_val *s)
{
    if (!h->fragment) {
        size_t len = s->as.s.len;
        const unsigned char *d = (const unsigned char *)s->as.s.data;
        int ws_only = 1;
        size_t i;
        for (i = 0; i < len; i++) {
            if (!hp_is_ws(d[i])) {
                ws_only = 0;
                break;
            }
        }
        if (h->depth == 0) {
            if (h->body_st == HP_W_NONE) {
                if (ws_only) return 0; /* leading whitespace drops */
                if (hp_start_body(h) != 0) return -1;
            } else if (h->body_st == HP_W_CLOSED) {
                if (hp_start_body(h) != 0) return -1;
            }
        } else if (hp_kw_is(h->stack[h->depth - 1].tag, "head")
                   && !ws_only) {
            /* non-whitespace text ends the head phase (rule 17) */
            if (hp_start_body(h) != 0) return -1;
        }
    }
    return hp_add_child(h, s);
}

/* Add a raw (verbatim) text range as a child; the caller guarantees
 * the placement rules already hold (plaintext tail, raw-text scan). */
static int hp_add_raw_range(hp_t *h, const unsigned char *from,
                            const unsigned char *to)
{
    mino_val *s;
    if (from >= to) return 0;
    s = hp_text_val(h, from, to, 1);
    if (s == NULL) return -1;
    return hp_add_child(h, s);
}

/* Grow the pending carry buffer to at least need bytes. */
static int hp_pend_grow(hp_t *h, size_t need)
{
    unsigned char *nb;
    size_t cap;
    if (h->pend_buf != NULL && need <= h->pend_cap) return 0;
    cap = (h->pend_cap == 0) ? 64 : h->pend_cap * 2;
    if (cap < need) cap = need;
    {
        int saved = mino_current_ctx(h->S)->gc_depth;
        mino_current_ctx(h->S)->gc_depth = saved + 1;
        nb = (unsigned char *)gc_alloc_typed_inner(h->S, GC_T_RAW,
                                                   cap);
        mino_current_ctx(h->S)->gc_depth = saved;
        if (nb == NULL) return -1;
    }
    if (h->pend_buf != NULL) {
        memcpy(nb, h->pend_buf, h->pend_w);
    }
    h->pend_buf = nb;
    h->pend_cap = cap;
    return 0;
}

/* Fold the pending raw segment [pend_from, upto) into the carry
 * buffer (creating it if needed); called when a </> splits the run. */
static int hp_pend_fold(hp_t *h, const unsigned char *upto)
{
    size_t w = h->pend_w;
    int saved;
    long long nw;
    if (h->pend_from == NULL || h->pend_from >= upto) {
        h->pend_from = NULL;
        return 0;
    }
    if (hp_pend_grow(h, w + (size_t)(upto - h->pend_from) * 3 + 16)
        != 0) {
        return -1;
    }
    saved = mino_current_ctx(h->S)->gc_depth;
    mino_current_ctx(h->S)->gc_depth = saved + 1;
    nw = hp_decode_into(h->pend_from, upto, h->pend_buf,
                        (long long)w);
    mino_current_ctx(h->S)->gc_depth = saved;
    if (nw < 0) return -1;
    h->pend_w = (size_t)nw;
    h->pend_from = NULL;
    return 0;
}

/* Emit the pending text run (carry plus segment) as one child. */
static int hp_flush_pending(hp_t *h)
{
    mino_state *S = h->S;
    mino_val *s;
    if (h->pend_buf != NULL) {
        int saved;
        if (hp_pend_fold(h, h->p) != 0) return -1;
        saved = mino_current_ctx(S)->gc_depth;
        mino_current_ctx(S)->gc_depth = saved + 1;
        s = mino_string_n(S, (const char *)h->pend_buf, h->pend_w);
        mino_current_ctx(S)->gc_depth = saved;
        h->pend_buf = NULL;
        h->pend_w = 0;
        h->pend_cap = 0;
        if (s == NULL) return -1;
        return hp_add_text_val(h, s);
    }
    if (h->pend_from != NULL && h->pend_from < h->p) {
        const unsigned char *from = h->pend_from;
        h->pend_from = NULL;
        s = hp_text_val(h, from, h->p, 0);
        if (s == NULL) return -1;
        return hp_add_text_val(h, s);
    }
    h->pend_from = NULL;
    return 0;
}

/* ---- RAWTEXT / RCDATA scanning (tier rule 7) ---- */

/* Does [s,s+len) ci-match the keyword's bytes? */
static int hp_span_matches_kw(const unsigned char *s, size_t len,
                              const mino_val *kw)
{
    size_t i;
    if (len != kw->as.s.len) return 0;
    for (i = 0; i < len; i++) {
        if (hp_lower(s[i]) != (unsigned char)kw->as.s.data[i]) {
            return 0;
        }
    }
    return 1;
}

/* Scan raw/rcdata content for the just-pushed element; consumes the
 * close tag when found, else the run reaches EOF (auto-balance). */
static int hp_scan_cdata(hp_t *h, int rcdata)
{
    const mino_val *kw = h->stack[h->depth - 1].tag;
    size_t nlen = kw->as.s.len;
    const unsigned char *start = h->p;
    const unsigned char *content_end = h->end;
    const unsigned char *lt = h->p;
    for (;;) {
        const unsigned char *cand;
        const unsigned char *after;
        lt = (const unsigned char *)memchr(lt, '<',
                                           (size_t)(h->end - lt));
        if (lt == NULL) break;
        cand = lt + 1;
        if (cand < h->end && *cand == '/'
            && (size_t)(h->end - (cand + 1)) >= nlen
            && hp_span_matches_kw(cand + 1, nlen, kw)) {
            after = cand + 1 + nlen;
            if (after < h->end
                && (hp_is_ws(*after) || *after == '/' || *after == '>')) {
                /* a real close: content ends before '<' */
                content_end = lt;
                /* consume the end tag through '>' (attrs ignored) */
                h->p = after;
                while (h->p < h->end && *h->p != '>') {
                    h->p++;
                }
                if (h->p < h->end) {
                    h->p++; /* the '>' */
                } else {
                    /* unterminated end tag: absorbed to EOF */
                    content_end = h->end;
                }
                goto done;
            }
        }
        lt++;
    }
    h->p = h->end;
done:
    {
        mino_val *s;
        if (start >= content_end) {
            s = NULL; /* an empty raw/rcdata run is no child */
        } else {
            s = hp_text_val(h, start, content_end, !rcdata);
        }
        if (s != NULL && hp_add_child(h, s) != 0) return -1;
        return hp_pop(h);
    }
}

/* ---- tag scanning ---- */

static int hp_assoc_attr(hp_t *h, mino_val **attrs, mino_val *k,
                         mino_val *v)
{
    mino_state *S = h->S;
    mino_val *tr;
    mino_val *tr2;
    mino_val *p;
    if (map_get_val(*attrs, k) != NULL) {
        return 0; /* duplicates keep the first (rule 6) */
    }
    tr = mino_transient(S, *attrs);
    if (tr == NULL) return -1;
    tr2 = mino_assoc_bang(S, tr, k, v);
    if (tr2 == NULL) return -1;
    p = mino_persistent(S, tr2);
    if (p == NULL) return -1;
    *attrs = p;
    return 0;
}

/* Attribute value entity decode (attr mode). */
static mino_val *hp_attr_value(hp_t *h, const unsigned char *from,
                               const unsigned char *to)
{
    mino_state *S = h->S;
    const unsigned char *q;
    for (q = from; q < to; q++) {
        if (*q == '&' || *q == 0) break;
    }
    if (q == to) {
        return mino_string_n(S, (const char *)from,
                             (size_t)(to - from));
    }
    {
        size_t cap = (size_t)(to - from) * 3 + 16;
        unsigned char *buf;
        const unsigned char *r = from;
        long long w = 0;
        int saved = mino_current_ctx(S)->gc_depth;
        mino_val *out;
        mino_current_ctx(S)->gc_depth = saved + 1;
        buf = (unsigned char *)gc_alloc_typed_inner(S, GC_T_RAW, cap);
        if (buf == NULL) {
            mino_current_ctx(S)->gc_depth = saved;
            return NULL;
        }
        while (r < to) {
            if (*r == '&') {
                long long nw = hp_ref(&r, to, 1, buf, w);
                if (nw < 0) {
                    buf[w++] = '&';
                    r++;
                } else {
                    w = nw;
                }
            } else if (*r == 0) {
                buf[w++] = 0xEF;
                buf[w++] = 0xBF;
                buf[w++] = 0xBD;
                r++;
            } else {
                buf[w++] = *r++;
            }
        }
        out = mino_string_n(S, (const char *)buf, (size_t)w);
        mino_current_ctx(S)->gc_depth = saved;
        return out;
    }
}

/* Parse a start tag at '<'. Returns 0 handled, 1 dropped
 * (eof-in-tag), -1 hard failure. */
static int hp_start_tag(hp_t *h)
{
    mino_state *S = h->S;
    const unsigned char *ns;
    size_t nlen;
    mino_val *tag;
    mino_val *attrs;
    int self_close = 0;
    h->p++; /* past '<' */
    ns = h->p;
    while (h->p < h->end && !hp_is_ws(*h->p) && *h->p != '/'
           && *h->p != '>') {
        h->p++;
    }
    nlen = (size_t)(h->p - ns);
    tag = hp_name_keyword(h, ns, nlen);
    if (tag == NULL) return -1;
    attrs = hp_empty_map(h);
    if (attrs == NULL) return -1;
    /* attributes */
    for (;;) {
        const unsigned char *ans;
        size_t anlen;
        mino_val *akw;
        const unsigned char *vs = NULL;
        const unsigned char *ve = NULL;
        if (h->p >= h->end) return 1; /* eof-in-tag drops the tag */
        if (hp_is_ws(*h->p)) {
            h->p++;
            continue;
        }
        if (*h->p == '>') {
            h->p++;
            break;
        }
        if (*h->p == '/') {
            if (h->p + 1 < h->end && h->p[1] == '>') {
                self_close = 1;
                h->p += 2;
                break;
            }
            h->p++; /* stray solidus: separator, not a close */
            continue;
        }
        ans = h->p;
        if (*h->p == '=') {
            /* unexpected-equals before a name: '=' begins the name */
            h->p++;
        }
        while (h->p < h->end && !hp_is_ws(*h->p) && *h->p != '/'
               && *h->p != '>' && *h->p != '=') {
            h->p++;
        }
        anlen = (size_t)(h->p - ans);
        if (anlen == 0) {
            /* defensive: cannot happen ('>' handled above) */
            h->p++;
            continue;
        }
        akw = hp_name_keyword(h, ans, anlen);
        if (akw == NULL) return -1;
        /* optional value, whitespace-flexible around '=' */
        {
            const unsigned char *q = h->p;
            while (q < h->end && hp_is_ws(*q)) q++;
            if (q < h->end && *q == '=') {
                q++;
                while (q < h->end && hp_is_ws(*q)) q++;
                if (q >= h->end) return 1; /* eof-in-tag */
                if (*q == '"' || *q == '\'') {
                    unsigned char quote = *q;
                    vs = q + 1;
                    ve = vs;
                    while (ve < h->end && *ve != quote) ve++;
                    if (ve >= h->end) return 1; /* eof-in-tag */
                    h->p = ve + 1;
                } else {
                    vs = q;
                    ve = q;
                    while (ve < h->end && !hp_is_ws(*ve) && *ve != '>') {
                        ve++;
                    }
                    h->p = ve;
                }
                {
                    mino_val *v = hp_attr_value(h, vs, ve);
                    if (v == NULL) return -1;
                    if (hp_assoc_attr(h, &attrs, akw, v) != 0) {
                        return -1;
                    }
                }
                continue;
            }
        }
        /* valueless attribute normalizes to "" (rule 6) */
        {
            mino_val *v = mino_string_n(S, "", 0);
            if (v == NULL) return -1;
            if (hp_assoc_attr(h, &attrs, akw, v) != 0) return -1;
        }
    }
    /* tier rules 15/16 before placement (both modes) */
    if (hp_kw_r15(tag)) {
        int i;
        for (i = h->depth - 1; i >= 0; i--) {
            if (hp_kw_eq(h->stack[i].tag, tag)) {
                if (hp_pop_until(h, tag->as.s.data) != 0) return -1;
                break;
            }
            if (hp_kw_barrier(h->stack[i].tag)) break;
        }
    }
    if (hp_kw_p_closer(tag)) {
        int i;
        for (i = h->depth - 1; i >= 0; i--) {
            if (hp_kw_is(h->stack[i].tag, "p")) {
                if (hp_pop_until(h, "p") != 0) return -1;
                break;
            }
            if (hp_kw_barrier(h->stack[i].tag)) break;
        }
    }
    if (hp_kw_heading(tag) && h->depth > 0
        && hp_kw_heading(h->stack[h->depth - 1].tag)) {
        if (hp_pop(h) != 0) return -1;
    }
    /* placement */
    if (!h->fragment) {
        if (hp_kw_is(tag, "html")) {
            if (h->html_st == HP_W_NONE || h->html_st == HP_W_IMPL
                || h->html_st == HP_W_CLOSED) {
                if (hp_ensure_html(h) != 0) return -1;
                /* explicit wins: adopt attrs onto the open html,
                 * which sits at the stack bottom while open */
                h->stack[0].attrs = attrs;
                h->html_st = HP_W_EXPL;
                if (self_close && hp_pop_until(h, "html") != 0) {
                    return -1;
                }
            }
            /* an already-explicit html start tag is dropped */
            return 0;
        }
        if (hp_kw_is(tag, "head")) {
            if (h->head_st == HP_W_NONE && h->body_st == HP_W_NONE) {
                if (hp_ensure_html(h) != 0) return -1;
                if (hp_push_lit(h, "head", attrs, 0,
                                &h->head_st, HP_W_EXPL) != 0) {
                    return -1;
                }
                if (self_close && hp_pop_until(h, "head") != 0) {
                    return -1;
                }
                return 0;
            }
            if (h->head_st == HP_W_IMPL) {
                int idx = hp_find_open(h, "head");
                if (idx >= 0) {
                    h->stack[idx].attrs = attrs;
                }
                h->head_st = HP_W_EXPL;
                if (self_close && hp_pop_until(h, "head") != 0) {
                    return -1;
                }
                return 0;
            }
            return 0; /* duplicate or post-body head: dropped */
        }
        if (hp_kw_is(tag, "body")) {
            if (h->body_st == HP_W_NONE || h->body_st == HP_W_CLOSED) {
                if (hp_start_body(h) != 0) return -1;
                /* adopt attrs onto the just-opened body */
                {
                    int idx = hp_find_open(h, "body");
                    if (idx >= 0) {
                        h->stack[idx].attrs = attrs;
                    }
                }
                h->body_st = HP_W_EXPL;
                if (self_close && hp_pop_until(h, "body") != 0) {
                    return -1;
                }
                return 0;
            }
            return 0; /* duplicate body: dropped */
        }
        if (hp_kw_head_elem(tag) && h->body_st == HP_W_NONE
            && h->head_st != HP_W_CLOSED && hp_at_wrapper_level(h)) {
            if (hp_ensure_html(h) != 0) return -1;
            if (h->head_st == HP_W_NONE) {
                if (hp_push_lit(h, "head", hp_empty_map(h), 1,
                                &h->head_st, HP_W_IMPL) != 0) {
                    return -1;
                }
            }
        } else if (hp_at_wrapper_level(h)) {
            if (hp_start_body(h) != 0) return -1;
        }
    }
    if (hp_push(h, tag, attrs, 0) != 0) return -1;
    if (hp_kw_void(tag) || self_close) {
        return hp_pop(h);
    }
    if (hp_kw_rawtext(tag)) {
        return hp_scan_cdata(h, 0);
    }
    if (hp_kw_rcdata(tag)) {
        return hp_scan_cdata(h, 1);
    }
    if (hp_kw_is(tag, "plaintext")) {
        h->plain = 1;
    }
    return 0;
}

/* Parse an end tag at '</'. Returns 0 handled, 1 dropped, -1 hard. */
static int hp_end_tag(hp_t *h)
{
    const unsigned char *ns;
    size_t nlen;
    mino_val *tag;
    h->p += 2; /* past '</' */
    ns = h->p;
    while (h->p < h->end && !hp_is_ws(*h->p) && *h->p != '/'
           && *h->p != '>') {
        h->p++;
    }
    nlen = (size_t)(h->p - ns);
    /* skip attributes / junk to '>' (they are ignored on end tags) */
    while (h->p < h->end && *h->p != '>') {
        h->p++;
    }
    if (h->p >= h->end) return 1; /* eof-in-tag drops it */
    h->p++;
    tag = hp_name_keyword(h, ns, nlen);
    if (tag == NULL) return -1;
    if (!h->fragment
        && (hp_kw_is(tag, "html") || hp_kw_is(tag, "head")
            || hp_kw_is(tag, "body"))) {
        int idx = hp_find_open(h, tag->as.s.data);
        if (idx >= 0 && h->stack[idx].is_impl) {
            return 0; /* implied wrappers swallow their end tags */
        }
    }
    if (hp_kw_void(tag)) {
        return 0; /* rule 4: void end tags drop */
    }
    if (hp_find_open(h, tag->as.s.data) < 0) {
        return 0; /* rule 2: stray end tags drop */
    }
    /* rule 3: pop-until through the match */
    if (hp_pop_until(h, tag->as.s.data) != 0) return -1;
    return 0;
}

/* Scan a comment whose content starts at the cursor: close at the
 * first --> or --!> (the oracle's commentclose), else an abrupt -?>
 * right at the content start, else EOF where the oracle strips a
 * trailing --! / -- / - suffix. */
static int hp_comment(hp_t *h)
{
    const unsigned char *start = h->p;
    const unsigned char *content_end = h->end;
    const unsigned char *q = h->p;
    mino_val *s;
    mino_val *node;
    int closed = 0;
    for (;;) {
        q = (const unsigned char *)memchr(q, '-',
                                          (size_t)(h->end - q));
        if (q == NULL) break;
        if (h->end - q >= 3 && q[1] == '-') {
            if (q[2] == '>') {
                content_end = q;
                h->p = q + 3;
                closed = 1;
                break;
            }
            if (q[2] == '!' && h->end - q >= 4 && q[3] == '>') {
                content_end = q;
                h->p = q + 4;
                closed = 1;
                break;
            }
        }
        q++;
    }
    if (!closed) {
        /* abrupt close: content starting with -?> or > */
        if (h->p < h->end && *h->p == '>'
            && h->p + 1 <= h->end) {
            content_end = h->p;
            h->p++;
            closed = 1;
        } else if (h->p + 1 < h->end && *h->p == '-'
                   && h->p[1] == '>') {
            content_end = h->p;
            h->p += 2;
            closed = 1;
        }
    }
    if (!closed) {
        /* EOF: the oracle strips one trailing --! / -- / - suffix */
        h->p = h->end;
        while (content_end > start) {
            if (content_end - start >= 3
                && content_end[-3] == '-' && content_end[-2] == '-'
                && content_end[-1] == '!') {
                content_end -= 3;
            } else if (content_end - start >= 2
                       && content_end[-2] == '-'
                       && content_end[-1] == '-') {
                content_end -= 2;
            } else if (content_end[-1] == '-') {
                content_end -= 1;
            } else {
                break;
            }
            break;
        }
    }
    s = mino_string_n(h->S, (const char *)start,
                      (size_t)(content_end - start));
    if (s == NULL) return -1;
    node = hp_string_node(h, "comment", s);
    if (node == NULL) return -1;
    return hp_add_child(h, node);
}

/* Bogus comment: content from the cursor to the first '>' or EOF
 * (tier rule 9; the WHATWG bogus-comment recovery). */
static int hp_bogus_comment(hp_t *h)
{
    const unsigned char *start = h->p;
    const unsigned char *gt = (const unsigned char *)memchr(
        h->p, '>', (size_t)(h->end - h->p));
    mino_val *s;
    mino_val *node;
    if (gt == NULL) {
        h->p = h->end;
        gt = h->end;
    } else {
        h->p = gt + 1;
    }
    s = mino_string_n(h->S, (const char *)start,
                      (size_t)(gt - start));
    if (s == NULL) return -1;
    node = hp_string_node(h, "comment", s);
    if (node == NULL) return -1;
    return hp_add_child(h, node);
}

/* DOCTYPE at '<!DOCTYPE' (ci). Rule 10: first declaration captured,
 * later ones dropped; text mirrors the oracle dump: the ci 'doctype'
 * word plus its whitespace run strips when whitespace follows, then
 * both ends trim. */
static int hp_doctype(hp_t *h)
{
    const unsigned char *start = h->p + 2; /* after '<!' */
    const unsigned char *gt = (const unsigned char *)memchr(
        start, '>', (size_t)(h->end - start));
    const unsigned char *to;
    const unsigned char *s;
    mino_val *sv;
    mino_val *node;
    if (gt == NULL) {
        to = h->end;
        h->p = h->end;
    } else {
        to = gt;
        h->p = gt + 1;
    }
    s = start;
    if ((size_t)(to - s) > 7
        && hp_span_ci_eq(s, 7, "doctype") && hp_is_trim_ws(s[7])) {
        s += 8;
        while (s < to && hp_is_trim_ws(*s)) s++;
    }
    while (to > s && hp_is_trim_ws(to[-1])) to--;
    if (!h->fragment && h->seen_doctype) {
        return 0; /* rule 10: later declarations drop (document mode) */
    }
    h->seen_doctype = 1;
    sv = mino_string_n(h->S, (const char *)s, (size_t)(to - s));
    if (sv == NULL) return -1;
    node = hp_string_node(h, "document-type", sv);
    if (node == NULL) return -1;
    return hp_add_child(h, node);
}

/* ---- main loop ---- */

static int hp_run(hp_t *h)
{
    while (h->p < h->end) {
        unsigned char c1;
        int r;
        if (h->plain) {
            const unsigned char *from = h->p;
            h->p = h->end;
            if (hp_add_raw_range(h, from, h->end) != 0) return -1;
            break;
        }
        if (*h->p != '<') {
            /* extend the pending text run to the next '<'; the run
             * flushes only when a real construct starts (a lone '<'
             * stays text and merges, like the oracle's data events) */
            const unsigned char *lt = (const unsigned char *)memchr(
                h->p, '<', (size_t)(h->end - h->p));
            if (h->pend_from == NULL) {
                h->pend_from = h->p;
            }
            if (lt == NULL) {
                h->p = h->end;
            } else {
                h->p = lt;
            }
            continue;
        }
        c1 = (h->p + 1 < h->end) ? h->p[1] : 0;
        if (c1 == '/' && h->p + 2 < h->end && h->p[2] == '>') {
            /* '</>' emits nothing: fold the pending segment into the
             * carry so the surrounding data merges (oracle behavior) */
            if (hp_pend_fold(h, h->p) != 0) return -1;
            h->p += 3;
            continue;
        }
        if (!(hp_is_alpha(c1) || c1 == '/' || c1 == '!'
              || c1 == '?')) {
            /* '<' that starts nothing: it stays text; keep the run
             * pending so it merges with what follows */
            if (h->pend_from == NULL) {
                h->pend_from = h->p;
            }
            h->p++;
            continue;
        }
        /* a real construct starts: flush the pending run up to '<' */
        if (hp_flush_pending(h) != 0) return -1;
        hp_sync_line(h);
        h->err_tok = h->p;
        if (hp_is_alpha(c1)) {
            r = hp_start_tag(h);
        } else if (c1 == '/') {
            if (h->p + 2 < h->end && hp_is_alpha(h->p[2])) {
                r = hp_end_tag(h);
            } else if (h->p + 2 >= h->end) {
                /* '</' at EOF is text */
                if (h->pend_from == NULL) {
                    h->pend_from = h->p;
                }
                h->p++;
                r = 0;
            } else {
                h->p += 2; /* bogus comment after '</' */
                r = hp_bogus_comment(h);
            }
        } else if (c1 == '!') {
            if (h->end - h->p >= 4 && h->p[2] == '-'
                && h->p[3] == '-') {
                h->p += 4;
                r = hp_comment(h);
            } else if (h->end - h->p >= 9
                       && hp_span_ci_eq(h->p + 2, 7, "doctype")) {
                r = hp_doctype(h);
            } else {
                h->p += 2;
                r = hp_bogus_comment(h);
            }
        } else {
            h->p += 2; /* '<?' bogus comment */
            r = hp_bogus_comment(h);
        }
        if (r < 0) return -1;
        if (r == 1) {
            /* eof-in-tag: the dropped tag swallows the rest */
            h->p = h->end;
        }
    }
    /* flush any trailing pending text */
    if (hp_flush_pending(h) != 0) return -1;
    if (!h->fragment && h->body_st == HP_W_NONE) {
        if (hp_start_body(h) != 0) return -1;
    }
    while (h->depth > 0) {
        if (hp_pop(h) != 0) return -1;
    }
    return 0;
}

/* ---- XML mode (ADR 28: one core, two modes; the strict delta) ----
 *
 * xml-parse over the shared machinery: case-sensitive names, the
 * five predefined entities plus numeric references only (D3), CDATA
 * capture with comments/PIs/doctype dropped at the tree, mandatory
 * attribute quoting with 3.3.3 whitespace normalization, single
 * root and prolog checks, no implied closes, the shared 256-deep
 * cap. Errors carry byte positions at the failing token (design
 * D11: the failing byte, never python's event boundaries).
 */

static int hp_xml_run(hp_t *h);

/* XML whitespace: exactly space, tab, CR, LF (no \f). */
static int hp_x_ws(unsigned char c)
{
    return c == ' ' || c == '\t' || c == '\n' || c == '\r';
}

/* Simplified XML NameStartChar: letters, '_', ':', and any
 * non-ASCII byte (the full production's exotic exclusions are not
 * policed; the JVM qname behavior this mirrors does not care). */
static int hp_x_name_start(unsigned char c)
{
    if (hp_is_alpha(c) || c == '_' || c == ':') return 1;
    return c >= 0x80;
}

static int hp_x_name_char(unsigned char c)
{
    if (hp_x_name_start(c)) return 1;
    if (hp_is_digit(c)) return 1;
    return c == '-' || c == '.';
}

/* Case-preserving keyword from a raw span; a QName's first colon
 * splits into namespace and name (the JVM (keyword "ns:name")
 * behavior, so :dc/creator from "dc:creator"). A colon at either
 * end stays flat. */
static mino_val *xp_name_keyword(hp_t *h, const unsigned char *s,
                                 size_t len)
{
    size_t i;
    for (i = 0; i < len; i++) {
        if (s[i] == ':') break;
    }
    if (i == 0 || i >= len) {
        return mino_keyword_n(h->S, (const char *)s, len);
    }
    return mino_keyword_ns_n(h->S, (const char *)s, i,
                             (const char *)s + i + 1, len - i - 1);
}

/* Fail with the error position at tok: line counters advance only
 * up to tok so multi-line constructs report the failing byte's own
 * line and column. */
static void xp_fail(hp_t *h, const char *code,
                    const unsigned char *tok)
{
    if (!h->failed) {
        const unsigned char *q = h->synced;
        while (q < tok) {
            if (*q == '\n') {
                h->line++;
                h->line_start = q + 1;
            }
            q++;
        }
        h->synced = q;
        h->failed = 1;
        h->err_code = code;
        h->err_line = h->line;
        h->err_col = (size_t)(tok - h->line_start) + 1;
        h->err_tok = tok;
    }
}

/* The five predefined entities (design D3), ';' included in the
 * pattern so the semicolon stays mandatory. */
static const struct { const char *name; const char *value; }
    k_xp_ents[5] = {
        {"amp;", "&"}, {"lt;", "<"}, {"gt;", ">"},
        {"quot;", "\""}, {"apos;", "'"},
};

/* Decode the reference at *pp (the '&') under the strict rule.
 * Returns the UTF-8 byte count written to out (max 4) and advances
 * *pp past the ';', or -1 with the error set at the '&'. */
static long xp_ref(hp_t *h, const unsigned char **pp,
                   const unsigned char *to, unsigned char *out)
{
    const unsigned char *r = *pp + 1;
    size_t i;
    if (r >= to || to - r < 2) {
        xp_fail(h, "undefined-entity", *pp);
        return -1;
    }
    if (*r == '#') {
        const unsigned char *d = r + 1;
        int hex = 0;
        unsigned long cp = 0;
        int ndig = 0;
        if (*d == 'x') {
            hex = 1;
            d++;
        }
        while (d < to) {
            int dv = hex ? hp_hex_val(*d)
                         : (hp_is_digit(*d) ? *d - '0' : -1);
            if (dv < 0) break;
            if (cp <= 0x10FFFF) {
                cp = cp * (unsigned long)(hex ? 16 : 10)
                     + (unsigned long)dv;
            }
            ndig++;
            d++;
        }
        /* Semicolon mandatory; 'X' is not a hex marker (XML 1.0). */
        if (ndig == 0 || d >= to || *d != ';') {
            xp_fail(h, "undefined-entity", *pp);
            return -1;
        }
        /* XML Char policing: #x9 #xA #xD, #x20-#xD7FF,
         * #xE000-#xFFFD, #x10000-#x10FFFF. */
        if (!((cp == 0x9 || cp == 0xA || cp == 0xD)
              || (cp >= 0x20 && cp <= 0xD7FF)
              || (cp >= 0xE000 && cp <= 0xFFFD)
              || (cp >= 0x10000 && cp <= 0x10FFFF))) {
            xp_fail(h, "undefined-entity", *pp);
            return -1;
        }
        *pp = d + 1;
        return (long)hp_utf8(cp, out);
    }
    for (i = 0; i < 5; i++) {
        size_t n = strlen(k_xp_ents[i].name);
        const char *v = k_xp_ents[i].value;
        size_t vl = strlen(v);
        if ((size_t)(to - r) >= n
            && memcmp(r, k_xp_ents[i].name, n) == 0) {
            memcpy(out, v, vl);
            *pp = r + n;
            return (long)vl;
        }
    }
    xp_fail(h, "undefined-entity", *pp);
    return -1;
}

/* Append bytes to the pending character-data carry. */
static int xp_put(hp_t *h, const unsigned char *from, size_t n)
{
    if (n == 0) return 0;
    if (hp_pend_grow(h, h->pend_w + n) != 0) return -1;
    memcpy(h->pend_buf + h->pend_w, from, n);
    h->pend_w += n;
    return 0;
}

/* Append a verbatim span with line-ending normalization (CR LF and
 * lone CR to LF; XML 2.11, CDATA included). */
static int xp_put_raw_cr(hp_t *h, const unsigned char *from,
                         const unsigned char *to)
{
    while (from < to) {
        const unsigned char *q = from;
        while (q < to && *q != '\r') q++;
        if (xp_put(h, from, (size_t)(q - from)) != 0) return -1;
        if (q >= to) break;
        if (xp_put(h, (const unsigned char *)"\n", 1) != 0) return -1;
        from = q + 1;
        if (from < to && *from == '\n') from++;
    }
    return 0;
}

/* Decode one text span into the pending carry: strict references,
 * line-ending normalization, NUL and the literal ]]> rejected. */
static int xp_decode_span(hp_t *h, const unsigned char *from,
                          const unsigned char *to)
{
    const unsigned char *r = from;
    while (r < to) {
        unsigned char c = *r;
        if (c == '&') {
            unsigned char ent[4];
            long n = xp_ref(h, &r, to, ent);
            if (n < 0) return -1;
            if (xp_put(h, ent, (size_t)n) != 0) return -1;
        } else if (c == '\r') {
            if (xp_put(h, (const unsigned char *)"\n", 1) != 0) {
                return -1;
            }
            r++;
            if (r < to && *r == '\n') r++;
        } else if (c == 0) {
            xp_fail(h, "unexpected-token", r);
            return -1;
        } else {
            const unsigned char *q = r;
            while (q < to && *q != '&' && *q != '\r' && *q != 0
                   && *q != ']') {
                q++;
            }
            if (q < to && *q == ']') {
                /* ']' is data unless it starts ]]>; the span ends
                 * at '<' so a ]]> can never straddle the boundary */
                if (to - q >= 3 && q[1] == ']' && q[2] == '>') {
                    xp_fail(h, "unexpected-token", q);
                    return -1;
                }
                q++; /* this one ']' is data */
            }
            if (xp_put(h, r, (size_t)(q - r)) != 0) return -1;
            r = q;
        }
    }
    return 0;
}

/* Materialize the pending character-data carry as one string child
 * (the oracle merges text, CDATA, comments, and PIs into one run). */
static int xp_flush(hp_t *h)
{
    mino_state *S = h->S;
    mino_val *s;
    int saved;
    if (h->pend_w == 0) return 0;
    saved = mino_current_ctx(S)->gc_depth;
    mino_current_ctx(S)->gc_depth = saved + 1;
    s = mino_string_n(S, (const char *)h->pend_buf, h->pend_w);
    mino_current_ctx(S)->gc_depth = saved;
    h->pend_w = 0;
    if (s == NULL) return -1;
    return hp_add_child(h, s);
}

/* Close the top element as a JVM clojure.xml node (no :type). */
static int hp_xml_pop(hp_t *h)
{
    mino_state *S = h->S;
    hp_open_t *o;
    mino_val *content;
    mino_val *keys[3];
    mino_val *vals[3];
    mino_val *node;
    if (xp_flush(h) != 0) return -1;
    h->depth--;
    o = &h->stack[h->depth];
    content = mino_persistent(S, o->acc);
    if (content == NULL) return -1;
    keys[0] = hp_kw_tag(h);
    keys[1] = hp_kw_attrs(h);
    keys[2] = hp_kw_content(h);
    vals[0] = o->tag;
    vals[1] = o->attrs;
    vals[2] = content;
    node = mino_map(S, keys, vals, 3);
    if (node == NULL) return -1;
    if (hp_add_child(h, node) != 0) return -1;
    if (h->depth == 0) {
        h->xml_root = node;
    }
    return 0;
}

/* assoc with duplicate detection (strict XML has no keep-first). */
static int xp_assoc_attr(hp_t *h, mino_val **attrs, mino_val *k,
                         mino_val *v, const unsigned char *name_pos)
{
    mino_state *S = h->S;
    mino_val *tr;
    mino_val *tr2;
    mino_val *p;
    if (map_get_val(*attrs, k) != NULL) {
        xp_fail(h, "duplicate-attribute", name_pos);
        return -1;
    }
    tr = mino_transient(S, *attrs);
    if (tr == NULL) return -1;
    tr2 = mino_assoc_bang(S, tr, k, v);
    if (tr2 == NULL) return -1;
    p = mino_persistent(S, tr2);
    if (p == NULL) return -1;
    *attrs = p;
    return 0;
}

/* Decode a quoted attribute-value span: strict references; literal
 * tab, LF, and CR (CR LF together) become one space each (XML 1.0
 * 3.3.3 CDATA-type normalization); character references pass
 * through verbatim. */
static mino_val *xp_attr_value(hp_t *h, const unsigned char *from,
                               const unsigned char *to)
{
    mino_state *S = h->S;
    size_t cap = (size_t)(to - from) + 16;
    unsigned char *buf;
    size_t w = 0;
    int saved = mino_current_ctx(S)->gc_depth;
    mino_val *out;
    const unsigned char *r = from;
    mino_current_ctx(S)->gc_depth = saved + 1;
    buf = (unsigned char *)gc_alloc_typed_inner(S, GC_T_RAW, cap);
    if (buf == NULL) {
        mino_current_ctx(S)->gc_depth = saved;
        return NULL;
    }
    while (r < to) {
        unsigned char c = *r;
        if (c == '&') {
            unsigned char ent[4];
            long n = xp_ref(h, &r, to, ent);
            if (n < 0) {
                mino_current_ctx(S)->gc_depth = saved;
                return NULL;
            }
            memcpy(buf + w, ent, (size_t)n);
            w += (size_t)n;
        } else if (c == ' ' || c == '\t' || c == '\n') {
            buf[w++] = ' ';
            r++;
        } else if (c == '\r') {
            buf[w++] = ' ';
            r++;
            if (r < to && *r == '\n') r++;
        } else {
            const unsigned char *q = r;
            while (q < to && *q != '&' && *q != ' ' && *q != '\t'
                   && *q != '\n' && *q != '\r') {
                q++;
            }
            memcpy(buf + w, r, (size_t)(q - r));
            w += (size_t)(q - r);
            r = q;
        }
    }
    out = mino_string_n(S, (const char *)buf, w);
    mino_current_ctx(S)->gc_depth = saved;
    return out;
}

/* A strict start tag at '<'. */
static int hp_xml_start_tag(hp_t *h)
{
    const unsigned char *ts = h->p;
    const unsigned char *ns;
    size_t nlen;
    mino_val *tag;
    mino_val *attrs;
    int self_close = 0;
    h->p++;
    ns = h->p;
    while (h->p < h->end && hp_x_name_char(*h->p)) {
        h->p++;
    }
    nlen = (size_t)(h->p - ns);
    tag = xp_name_keyword(h, ns, nlen);
    if (tag == NULL) return -1;
    attrs = hp_empty_map(h);
    if (attrs == NULL) return -1;
    for (;;) {
        const unsigned char *ans;
        const unsigned char *name_end;
        mino_val *akw;
        if (h->p >= h->end) {
            xp_fail(h, "unexpected-eof", h->end);
            return -1;
        }
        if (hp_x_ws(*h->p)) {
            h->p++;
            continue;
        }
        if (*h->p == '>') {
            h->p++;
            break;
        }
        if (*h->p == '/') {
            if (h->p + 1 < h->end && h->p[1] == '>') {
                self_close = 1;
                h->p += 2;
                break;
            }
            xp_fail(h, "unexpected-token", h->p);
            return -1;
        }
        ans = h->p;
        if (!hp_x_name_start(*h->p)) {
            xp_fail(h, "invalid-name", h->p);
            return -1;
        }
        h->p++;
        while (h->p < h->end && hp_x_name_char(*h->p)) {
            h->p++;
        }
        name_end = h->p;
        akw = xp_name_keyword(h, ans, (size_t)(name_end - ans));
        if (akw == NULL) return -1;
        {
            const unsigned char *q = h->p;
            while (q < h->end && hp_x_ws(*q)) q++;
            if (q >= h->end) {
                xp_fail(h, "unexpected-eof", h->end);
                return -1;
            }
            if (*q != '=') {
                xp_fail(h, "unexpected-token", q);
                return -1;
            }
            q++;
            while (q < h->end && hp_x_ws(*q)) q++;
            if (q >= h->end) {
                xp_fail(h, "unexpected-eof", h->end);
                return -1;
            }
            if (*q != '"' && *q != '\'') {
                xp_fail(h, "unexpected-token", q);
                return -1;
            }
            {
                unsigned char quote = *q;
                const unsigned char *vs = q + 1;
                const unsigned char *ve = vs;
                while (ve < h->end && *ve != quote && *ve != '<'
                       && *ve != 0) {
                    ve++;
                }
                if (ve >= h->end) {
                    xp_fail(h, "unexpected-eof", h->end);
                    return -1;
                }
                if (*ve != quote) {
                    xp_fail(h, "unexpected-token", ve);
                    return -1;
                }
                h->p = ve + 1;
                {
                    mino_val *v = xp_attr_value(h, vs, ve);
                    if (v == NULL) return -1;
                    if (xp_assoc_attr(h, &attrs, akw, v, ans) != 0) {
                        return -1;
                    }
                }
                /* whitespace (or the tag end) must follow a value */
                if (h->p < h->end && !hp_x_ws(*h->p)
                    && *h->p != '>' && *h->p != '/') {
                    xp_fail(h, "unexpected-token", h->p);
                    return -1;
                }
            }
        }
    }
    if (h->depth == 0) {
        if (h->seen_root) {
            xp_fail(h, "multiple-roots", ts);
            return -1;
        }
        h->seen_root = 1;
    } else if (xp_flush(h) != 0) {
        return -1;
    }
    if (h->depth >= HP_MAX_DEPTH) {
        xp_fail(h, "max-depth", ts);
        return -1;
    }
    if (hp_push(h, tag, attrs, 0) != 0) {
        if (!h->failed) xp_fail(h, "max-depth", ts);
        return -1;
    }
    if (self_close) {
        return hp_xml_pop(h);
    }
    return 0;
}

/* A strict end tag at '</': must match the open element exactly. */
static int hp_xml_end_tag(hp_t *h)
{
    const unsigned char *ts = h->p;
    const unsigned char *ns;
    size_t nlen;
    mino_val *tag;
    h->p += 2;
    ns = h->p;
    while (h->p < h->end && hp_x_name_char(*h->p)) {
        h->p++;
    }
    nlen = (size_t)(h->p - ns);
    while (h->p < h->end && hp_x_ws(*h->p)) {
        h->p++;
    }
    if (h->p >= h->end) {
        xp_fail(h, "unexpected-eof", h->end);
        return -1;
    }
    if (*h->p != '>') {
        xp_fail(h, "unexpected-token", h->p);
        return -1;
    }
    h->p++;
    tag = xp_name_keyword(h, ns, nlen);
    if (tag == NULL) return -1;
    if (h->depth == 0 || !hp_kw_eq(h->stack[h->depth - 1].tag, tag)) {
        xp_fail(h, "mismatched-end-tag", ts);
        return -1;
    }
    return hp_xml_pop(h);
}

/* A comment at '<!--' consumed: close at the first -->; any other
 * '--' inside the content is not well-formed. Comments drop. */
static int hp_xml_comment(hp_t *h)
{
    const unsigned char *q = h->p;
    for (;;) {
        while (q < h->end && *q != '-') q++;
        if (h->end - q < 2) {
            xp_fail(h, "unexpected-eof", h->end);
            return -1;
        }
        if (q[1] != '-') {
            q += 2; /* a lone hyphen is data */
            continue;
        }
        if (h->end - q >= 3 && q[2] == '>') {
            h->p = q + 3;
            return 0;
        }
        if (h->end - q < 3) {
            xp_fail(h, "unexpected-eof", h->end);
            return -1;
        }
        xp_fail(h, "unexpected-token", q);
        return -1;
    }
}

/* CDATA at '<![CDATA[': content verbatim (line endings normalized)
 * into the pending carry; ends at the first ]]>; never parsed. */
static int hp_xml_cdata(hp_t *h)
{
    const unsigned char *start = h->p;
    const unsigned char *q = h->p;
    for (;;) {
        while (q < h->end && *q != ']' && *q != 0) q++;
        if (q >= h->end) {
            xp_fail(h, "unexpected-eof", h->end);
            return -1;
        }
        if (*q == 0) {
            xp_fail(h, "unexpected-token", q);
            return -1;
        }
        if (h->end - q >= 3 && q[1] == ']' && q[2] == '>') {
            if (xp_put_raw_cr(h, start, q) != 0) return -1;
            h->p = q + 3;
            return 0;
        }
        q++;
    }
}

/* A PI at '<?' with the cursor past it: target name, then either
 * the reserved-xml handling or a skip to '?>'. PIs drop. */
static int hp_xml_pi(hp_t *h)
{
    const unsigned char *ts = h->p - 2;
    const unsigned char *tstart = h->p;
    size_t tlen;
    if (h->p >= h->end || !hp_x_name_start(*h->p)) {
        xp_fail(h, "invalid-name",
                h->p < h->end ? h->p : h->end);
        return -1;
    }
    while (h->p < h->end && hp_x_name_char(*h->p)) {
        h->p++;
    }
    tlen = (size_t)(h->p - tstart);
    if (tlen == 3 && hp_lower(tstart[0]) == 'x'
        && hp_lower(tstart[1]) == 'm' && hp_lower(tstart[2]) == 'l') {
        /* reserved target: the declaration only, only at the very
         * start, only exactly lowercase */
        if (ts != h->xml_start
            || !(tstart[0] == 'x' && tstart[1] == 'm'
                 && tstart[2] == 'l')) {
            xp_fail(h, "invalid-prolog", ts);
            return -1;
        }
        /* the XML declaration: quoted pseudo-attributes, version
         * required (value not policed; python accepts "2.0") */
        {
            int seen_version = 0;
            for (;;) {
                const unsigned char *ans;
                const unsigned char *name_end;
                if (h->p >= h->end) {
                    xp_fail(h, "unexpected-eof", h->end);
                    return -1;
                }
                if (hp_x_ws(*h->p)) {
                    h->p++;
                    continue;
                }
                if (*h->p == '?') {
                    if (h->p + 1 < h->end && h->p[1] == '>') {
                        if (!seen_version) {
                            xp_fail(h, "invalid-prolog", ts);
                            return -1;
                        }
                        h->p += 2;
                        return 0;
                    }
                    xp_fail(h, "invalid-prolog", h->p);
                    return -1;
                }
                ans = h->p;
                if (!hp_x_name_start(*h->p)) {
                    xp_fail(h, "invalid-prolog", h->p);
                    return -1;
                }
                h->p++;
                while (h->p < h->end && hp_x_name_char(*h->p)) {
                    h->p++;
                }
                name_end = h->p;
                if (name_end - ans == 7
                    && memcmp(ans, "version", 7) == 0) {
                    seen_version = 1;
                }
                {
                    const unsigned char *q = h->p;
                    while (q < h->end && hp_x_ws(*q)) q++;
                    if (q >= h->end) {
                        xp_fail(h, "unexpected-eof", h->end);
                        return -1;
                    }
                    if (*q != '=') {
                        xp_fail(h, "invalid-prolog", q);
                        return -1;
                    }
                    q++;
                    while (q < h->end && hp_x_ws(*q)) q++;
                    if (q >= h->end) {
                        xp_fail(h, "unexpected-eof", h->end);
                        return -1;
                    }
                    if (*q != '"' && *q != '\'') {
                        xp_fail(h, "invalid-prolog", q);
                        return -1;
                    }
                    {
                        unsigned char quote = *q;
                        q++;
                        while (q < h->end && *q != quote) q++;
                        if (q >= h->end) {
                            xp_fail(h, "unexpected-eof", h->end);
                            return -1;
                        }
                        h->p = q + 1;
                    }
                }
            }
        }
    }
    /* a normal PI: anything to '?>' */
    {
        const unsigned char *q = h->p;
        for (;;) {
            while (q < h->end && *q != '?') q++;
            if (q >= h->end) {
                xp_fail(h, "unexpected-eof", h->end);
                return -1;
            }
            if (q + 1 < h->end && q[1] == '>') {
                h->p = q + 2;
                return 0;
            }
            q++;
        }
    }
}

/* A DOCTYPE at '<!DOCTYPE' (the keyword is case-sensitive): scanned
 * and dropped. Brackets, quoted strings, comments, and PIs inside
 * are skipped correctly; any <!ENTITY declaration inside the
 * internal subset throws :unsupported-doctype (design D3: nothing
 * is ever honored or fetched). */
static int hp_xml_doctype(hp_t *h)
{
    const unsigned char *ts = h->p;
    const unsigned char *q = h->p + 9;
    int brackets = 0;
    if (h->seen_root || h->seen_doctype) {
        xp_fail(h, "invalid-prolog", ts);
        return -1;
    }
    h->seen_doctype = 1;
    if (q >= h->end || !hp_x_ws(*q)) {
        xp_fail(h, "unexpected-token", q < h->end ? q : h->end);
        return -1;
    }
    while (q < h->end) {
        unsigned char c = *q;
        if (c == '"' || c == '\'') {
            unsigned char qc = c;
            q++;
            while (q < h->end && *q != qc) q++;
            if (q >= h->end) break;
            q++;
            continue;
        }
        if (c == '[') {
            brackets = 1;
            q++;
            continue;
        }
        if (c == ']') {
            brackets = 0;
            q++;
            continue;
        }
        if (c == '>') {
            if (brackets) {
                q++;
                continue;
            }
            h->p = q + 1;
            return 0;
        }
        if (c == '<') {
            if (h->end - q >= 4 && q[1] == '!' && q[2] == '-'
                && q[3] == '-') {
                q += 4;
                while (q < h->end
                       && !(h->end - q >= 3 && q[0] == '-'
                            && q[1] == '-' && q[2] == '>')) {
                    q++;
                }
                if (q >= h->end) break;
                q += 3;
                continue;
            }
            if (h->end - q >= 8 && q[1] == '!'
                && hp_span_ci_eq(q + 2, 6, "entity")
                && (q + 8 >= h->end || hp_x_ws(q[8])
                    || q[8] == '%')) {
                xp_fail(h, "unsupported-doctype", q);
                return -1;
            }
            if (h->end - q >= 2 && q[1] == '?') {
                q += 2;
                while (q < h->end
                       && !(q + 1 < h->end && q[0] == '?'
                            && q[1] == '>')) {
                    q++;
                }
                if (q >= h->end) break;
                q += 2;
                continue;
            }
            if (h->end - q >= 2 && q[1] == '!') {
                /* ELEMENT/ATTLIST/NOTATION: to the decl's '>' */
                q += 2;
                while (q < h->end && *q != '>') {
                    if (*q == '"' || *q == '\'') {
                        unsigned char qc = *q;
                        q++;
                        while (q < h->end && *q != qc) q++;
                    }
                    q++;
                }
                if (q >= h->end) break;
                q++;
                continue;
            }
            q++;
            continue;
        }
        q++;
    }
    xp_fail(h, "unexpected-eof", h->end);
    return -1;
}

/* The strict main loop: prolog, single root, epilogue. Character
 * data merges across comments, PIs, and CDATA into one string per
 * position (the oracle shape); whitespace outside the root drops. */
static int hp_xml_run(hp_t *h)
{
    if (h->end - h->p >= 3 && h->p[0] == 0xEF && h->p[1] == 0xBB
        && h->p[2] == 0xBF) {
        h->p += 3; /* the UTF-8 BOM drops (the oracle behavior) */
        h->line_start = h->p;
        h->synced = h->p;
    }
    h->xml_start = h->p;
    while (h->p < h->end) {
        unsigned char c1;
        if (*h->p != '<') {
            if (h->depth > 0) {
                const unsigned char *lt =
                    (const unsigned char *)memchr(
                        h->p, '<', (size_t)(h->end - h->p));
                const unsigned char *to = (lt != NULL) ? lt : h->end;
                if (xp_decode_span(h, h->p, to) != 0) return -1;
                h->p = to;
            } else {
                if (hp_x_ws(*h->p)) {
                    h->p++;
                    continue;
                }
                xp_fail(h, "content-before-root", h->p);
                return -1;
            }
            continue;
        }
        c1 = (h->p + 1 < h->end) ? h->p[1] : 0;
        if (c1 == '?') {
            h->p += 2;
            if (hp_xml_pi(h) != 0) return -1;
            continue;
        }
        if (c1 == '!') {
            if (h->end - h->p >= 4 && h->p[2] == '-'
                && h->p[3] == '-') {
                h->p += 4;
                if (hp_xml_comment(h) != 0) return -1;
                continue;
            }
            if (h->end - h->p >= 9
                && memcmp(h->p + 2, "DOCTYPE", 7) == 0) {
                if (hp_xml_doctype(h) != 0) return -1;
                continue;
            }
            if (h->end - h->p >= 9
                && memcmp(h->p + 2, "[CDATA[", 7) == 0) {
                if (h->depth == 0) {
                    /* character data outside the root */
                    xp_fail(h, "content-before-root", h->p);
                    return -1;
                }
                h->p += 9;
                if (hp_xml_cdata(h) != 0) return -1;
                continue;
            }
            xp_fail(h, "unexpected-token", h->p);
            return -1;
        }
        if (c1 == '/') {
            if (h->p + 2 < h->end
                && hp_x_name_start(h->p[2])) {
                if (hp_xml_end_tag(h) != 0) return -1;
                continue;
            }
            if (h->p + 2 >= h->end) {
                xp_fail(h, "unexpected-eof", h->end);
            } else {
                xp_fail(h, "invalid-name", h->p + 2);
            }
            return -1;
        }
        if (hp_x_name_start(c1)) {
            if (hp_xml_start_tag(h) != 0) return -1;
            continue;
        }
        xp_fail(h, "unexpected-token", h->p);
        return -1;
    }
    if (h->depth > 0 || !h->seen_root) {
        xp_fail(h, "unexpected-eof", h->end);
        return -1;
    }
    return 0;
}

/* ---- prim ---- */

static mino_val *prim_html_parse(mino_state *S, mino_val *args,
                                 mino_env *env)
{
    mino_val *s_val;
    mino_val *opts;
    hp_t h;
    int fragment = 0;
    (void)env;
    if (!mino_is_cons(args)
        || !mino_is_cons(args->as.cons.cdr)
        || mino_is_cons(args->as.cons.cdr->as.cons.cdr)) {
        return prim_throw_classified(S, "eval/arity", "MAR001",
            "html-parse requires two arguments");
    }
    s_val = args->as.cons.car;
    opts = args->as.cons.cdr->as.cons.car;
    if (s_val == NULL || mino_type_of(s_val) != MINO_STRING) {
        return prim_throw_classified(S, "eval/type", "MTY001",
            "html-parse: first argument must be a string");
    }
    if (opts != NULL && mino_type_of(opts) == MINO_MAP) {
        mino_val *frag = map_get_val(opts, mino_keyword(S, "fragment"));
        if (frag == mino_true(S)) {
            fragment = 1;
        }
    } else if (opts != NULL && mino_type_of(opts) != MINO_NIL) {
        return prim_throw_classified(S, "eval/type", "MTY001",
            "html-parse: second argument must be a map or nil");
    }
    memset(&h, 0, sizeof(h));
    h.S = S;
    h.env = env;
    h.p = (const unsigned char *)s_val->as.s.data;
    h.end = h.p + s_val->as.s.len;
    h.line_start = h.p;
    h.synced = h.p;
    h.line = 1;
    h.fragment = fragment;
    h.doc = hp_empty_transient(&h);
    if (h.doc == NULL) return NULL;
    if (hp_run(&h) != 0) {
        if (h.failed) {
            mino_val *items[5];
            const unsigned char *line_span_end;
            if (h.err_tok != NULL) {
                hp_sync_line(&h);
            }
            line_span_end = h.line_start;
            while (line_span_end < h.end && *line_span_end != '\n') {
                line_span_end++;
            }
            items[0] = mino_keyword(S, "html/error");
            items[1] = mino_string(S, h.err_code ? h.err_code
                                                 : "max-depth");
            items[2] = mino_int(S, (long long)h.err_line);
            items[3] = mino_int(S, (long long)h.err_col);
            items[4] = mino_string_n(S,
                                     (const char *)h.line_start,
                                     (size_t)(line_span_end
                                              - h.line_start));
            return mino_vector(S, items, 5);
        }
        return NULL;
    }
    {
        mino_val *content = mino_persistent(S, h.doc);
        if (content == NULL) return NULL;
        if (fragment) {
            return content;
        }
        {
            mino_val *keys[2];
            mino_val *vals[2];
            keys[0] = mino_keyword(S, "type");
            keys[1] = mino_keyword(S, "content");
            vals[0] = mino_keyword(S, "document");
            vals[1] = content;
            return mino_map(S, keys, vals, 2);
        }
    }
}

static mino_val *prim_xml_parse(mino_state *S, mino_val *args,
                                mino_env *env)
{
    mino_val *s_val;
    mino_val *opts;
    hp_t h;
    (void)env;
    if (!mino_is_cons(args)
        || !mino_is_cons(args->as.cons.cdr)
        || mino_is_cons(args->as.cons.cdr->as.cons.cdr)) {
        return prim_throw_classified(S, "eval/arity", "MAR001",
            "xml-parse requires two arguments");
    }
    s_val = args->as.cons.car;
    opts = args->as.cons.cdr->as.cons.car;
    if (s_val == NULL || mino_type_of(s_val) != MINO_STRING) {
        return prim_throw_classified(S, "eval/type", "MTY001",
            "xml-parse: first argument must be a string");
    }
    if (opts != NULL && mino_type_of(opts) != MINO_MAP
        && mino_type_of(opts) != MINO_NIL) {
        return prim_throw_classified(S, "eval/type", "MTY001",
            "xml-parse: second argument must be a map or nil");
    }
    memset(&h, 0, sizeof(h));
    h.S = S;
    h.env = env;
    h.p = (const unsigned char *)s_val->as.s.data;
    h.end = h.p + s_val->as.s.len;
    h.line_start = h.p;
    h.synced = h.p;
    h.line = 1;
    h.xml = 1;
    h.doc = hp_empty_transient(&h);
    if (h.doc == NULL) return NULL;
    if (hp_xml_run(&h) != 0) {
        if (h.failed) {
            mino_val *items[5];
            const unsigned char *line_span_end;
            line_span_end = h.line_start;
            while (line_span_end < h.end && *line_span_end != '\n') {
                line_span_end++;
            }
            items[0] = mino_keyword(S, "xml/error");
            items[1] = mino_string(S, h.err_code);
            items[2] = mino_int(S, (long long)h.err_line);
            items[3] = mino_int(S, (long long)h.err_col);
            items[4] = mino_string_n(S,
                                     (const char *)h.line_start,
                                     (size_t)(line_span_end
                                              - h.line_start));
            return mino_vector(S, items, 5);
        }
        return NULL;
    }
    return h.xml_root;
}

const mino_prim_def k_prims_html[] = {
    {"html-parse", prim_html_parse,
     "Parses HTML text into a hickory-shaped node tree (ADR 28): "
     "elements are {:type :element :tag keyword :attrs {keyword "
     "string} :content [node|string]} with comments and the first "
     "DOCTYPE preserved and text as bare strings. Tolerant tier per "
     "the campaign design (implied closes, stray-end drop, pop-until, "
     "solidus honored, RAWTEXT script/style, RCDATA title/textarea, "
     "entity decode through the oracle table, simplified "
     "html/head/body synthesis). Second argument is an opts map; "
     "{:fragment true} returns the top-level nodes as a vector with "
     "no synthesized wrappers (the mino.html parse-fragment path). "
     "Returns the tree, or an error descriptor vector "
     "[:html/error code line col text] the facade converts to "
     "ex-info."},
};

const size_t k_prims_html_count =
    sizeof(k_prims_html) / sizeof(k_prims_html[0]);

/* xml-parse gates under its own MINO_CAP_XML (clojure.xml), split from
 * the HTML table so a host can take the tolerant HTML reader without the
 * strict XML one or the reverse. */
const mino_prim_def k_prims_xml[] = {
    {"xml-parse", prim_xml_parse,
     "Parses a well-formed XML 1.0 document into the JVM "
     "clojure.xml element shape (ADR 28, strict mode): elements "
     "are {:tag keyword :attrs {keyword string} :content "
     "[string|node]} with case-sensitive names (a QName prefix "
     "keywordizes at its first colon), :attrs {} and :content [] "
     "always present, the root element only (comments, processing "
     "instructions, and the DOCTYPE drop; character data merges "
     "across them into one string per position). Strictness per "
     "the campaign design: only the five predefined entities plus "
     "numeric references resolve (any other named reference, a "
     "bare ampersand, or an out-of-range code point is "
     ":undefined-entity); a DOCTYPE is accepted and dropped but "
     "any internal-subset ENTITY declaration throws "
     ":unsupported-doctype (XXE is impossible by construction); "
     "attribute values must be quoted and get XML 1.0 3.3.3 "
     "whitespace normalization; exactly one root element; the XML "
     "declaration only as the first bytes; the 256-deep nesting "
     "cap. Line endings normalize (CR LF and lone CR to LF) and a "
     "leading UTF-8 BOM drops. Second argument is an opts map, "
     "reserved. Returns the root element map, or an error "
     "descriptor vector [:xml/error code line col text] the "
     "facade converts to positioned ex-info."},
};

const size_t k_prims_xml_count =
    sizeof(k_prims_xml) / sizeof(k_prims_xml[0]);
