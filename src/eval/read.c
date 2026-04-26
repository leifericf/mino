/*
 * read.c -- tokenizer and reader.
 */

#include "runtime/internal.h"

/* ------------------------------------------------------------------------- */
/* Reader                                                                    */
/* ------------------------------------------------------------------------- */

const char *intern_filename(mino_state_t *S, const char *name)
{
    size_t i;
    char  *dup;
    size_t len;
    if (name == NULL) {
        return NULL;
    }
    for (i = 0; i < S->interned_files_len; i++) {
        if (strcmp(S->interned_files[i], name) == 0) {
            return S->interned_files[i];
        }
    }
    len = strlen(name);
    dup = (char *)malloc(len + 1);
    if (dup == NULL) {
        return name;
    }
    memcpy(dup, name, len + 1);
    if (S->interned_files_len == S->interned_files_cap) {
        size_t new_cap = S->interned_files_cap == 0 ? 64
                                                    : S->interned_files_cap * 2;
        const char **nb = (const char **)realloc(
            S->interned_files, new_cap * sizeof(*nb));
        if (nb == NULL) {
            free(dup);
            return name;
        }
        S->interned_files     = nb;
        S->interned_files_cap = new_cap;
    }
    S->interned_files[S->interned_files_len++] = dup;
    return dup;
}

/* Reader error codes. */
#define MRE001 "MRE001" /* unterminated string literal */
#define MRE002 "MRE002" /* unexpected closing delimiter */
#define MRE003 "MRE003" /* unterminated list/vector/map/set */
#define MRE004 "MRE004" /* out of memory during read */
#define MRE005 "MRE005" /* unterminated reader conditional */
#define MRE006 "MRE006" /* invalid reader conditional form */
#define MRE007 "MRE007" /* invalid reader macro usage */
#define MRE008 "MRE008" /* malformed literal */
#define MRE009 "MRE009" /* expected form after reader macro */
#define MRE010 "MRE010" /* invalid metadata */

static void set_reader_diag(mino_state_t *S, const char *code,
                            const char *msg, int line, int col)
{
    mino_diag_t *d = diag_new("reader", code, "read", msg);
    if (d != NULL) {
        mino_span_t span;
        memset(&span, 0, sizeof(span));
        span.file   = S->reader_file;
        span.line   = line;
        span.column = col;
        diag_set_span(d, span);
    }
    set_diag(S, d);
}

/* Advance the cursor by one character and track column position. */
static inline void ADVANCE(mino_state_t *S, const char **p)
{
    (*p)++;
    S->reader_col++;
}

/* Advance by n characters (no embedded newlines expected). */
static inline void ADVANCE_N(mino_state_t *S, const char **p, size_t n)
{
    *p += n;
    S->reader_col += (int)n;
}

static int is_ws(char c)
{
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == ',';
}

static int is_terminator(char c)
{
    return c == '\0' || c == '(' || c == ')' || c == '[' || c == ']'
        || c == '{' || c == '}' || c == '"' || c == ';'
        || c == '`'  || c == '~' || c == '@' || c == '^'
        || is_ws(c);
}

static void skip_ws(mino_state_t *S, const char **p)
{
    while (**p) {
        char c = **p;
        if (c == '\n') {
            S->reader_line++;
            S->reader_col = 1;
            (*p)++;
        } else if (is_ws(c)) {
            ADVANCE(S, p);
        } else if (c == ';') {
            while (**p && **p != '\n') {
                ADVANCE(S, p);
            }
        } else {
            return;
        }
    }
}

static mino_val_t *read_form(mino_state_t *S, const char **p);

static mino_val_t *read_string_form(mino_state_t *S, const char **p)
{
    /* Caller has positioned *p on the opening '"'. */
    int str_line = S->reader_line;
    int str_col  = S->reader_col;
    char *buf;
    size_t cap = 16;
    size_t len = 0;
    ADVANCE(S, p); /* skip opening quote */
    buf = (char *)malloc(cap);
    if (buf == NULL) {
        set_reader_diag(S, MRE004, "out of memory reading string",
                        str_line, str_col);
        return NULL;
    }
    while (**p && **p != '"') {
        char c = **p;
        if (c == '\n') {
            S->reader_line++;
            S->reader_col = 1;
        }
        if (c == '\\') {
            ADVANCE(S, p);
            switch (**p) {
            case 'n':  c = '\n'; break;
            case 't':  c = '\t'; break;
            case 'r':  c = '\r'; break;
            case '\\': c = '\\'; break;
            case '"':  c = '"';  break;
            case '0':  c = '\0'; break;
            case '\0':
                free(buf);
                set_reader_diag(S, MRE001, "unterminated string literal",
                                str_line, str_col);
                return NULL;
            default:
                /* Unknown escape: keep the character literally. */
                c = **p;
                break;
            }
        }
        if (len + 1 >= cap) {
            cap *= 2;
            {
                char *nb = (char *)realloc(buf, cap);
                if (nb == NULL) {
                    free(buf);
                    set_reader_diag(S, MRE004,
                                    "out of memory reading string",
                                    str_line, str_col);
                    return NULL;
                }
                buf = nb;
            }
        }
        buf[len++] = c;
        ADVANCE(S, p);
    }
    if (**p != '"') {
        free(buf);
        set_reader_diag(S, MRE001, "unterminated string literal",
                        str_line, str_col);
        return NULL;
    }
    ADVANCE(S, p); /* skip closing quote */
    {
        mino_val_t *v = mino_string_n(S, buf, len);
        free(buf);
        return v;
    }
}

/* Read a reader-conditional body: (keyword form keyword form ...).
 * Matches S->reader_dialect first, then "default".
 * Returns the matched form in *found, or NULL if no match.
 * Caller must have consumed the opening '('. */
static mino_val_t *read_cond_body(mino_state_t *S, const char **p,
                                  mino_val_t **found)
{
    mino_val_t *result = NULL;
    int         matched = 0;
    *found = NULL;
    for (;;) {
        mino_val_t *key;
        mino_val_t *val;
        skip_ws(S, p);
        if (**p == '\0') {
            set_reader_diag(S, MRE005, "unterminated reader conditional",
                            S->reader_line, S->reader_col);
            return NULL;
        }
        if (**p == ')') {
            ADVANCE(S, p);
            *found = result;
            return result; /* may be NULL if no branch matched */
        }
        key = read_form(S, p);
        if (key == NULL) {
            if (mino_last_error(S) == NULL) {
                set_reader_diag(S, MRE005, "unterminated reader conditional",
                            S->reader_line, S->reader_col);
            }
            return NULL;
        }
        if (key->type != MINO_KEYWORD) {
            set_reader_diag(S, MRE006,
                            "reader conditional key must be a keyword",
                            S->reader_line, S->reader_col);
            return NULL;
        }
        skip_ws(S, p);
        val = read_form(S, p);
        if (val == NULL) {
            if (mino_last_error(S) == NULL) {
                set_reader_diag(S, MRE006,
                            "reader conditional: missing value for key",
                            S->reader_line, S->reader_col);
            }
            return NULL;
        }
        if (!matched) {
            const char *kname = key->as.s.data;
            size_t      klen  = key->as.s.len;
            if ((klen == strlen(S->reader_dialect)
                 && memcmp(kname, S->reader_dialect, klen) == 0)
                || (klen == 7 && memcmp(kname, "default", 7) == 0)) {
                result  = val;
                matched = (klen != 7
                           || memcmp(kname, "default", 7) != 0);
                gc_pin(result);
            }
        }
    }
}

/* Check whether the cursor is at a #?@ splice sequence. If so, consume
 * the prefix (leaving the cursor on the opening '(' of the body) and
 * return 1. Otherwise return 0 without advancing. */
static int peek_reader_cond_splice(mino_state_t *S, const char **p)
{
    if ((*p)[0] == '#' && (*p)[1] == '?' && (*p)[2] == '@') {
        ADVANCE_N(S, p, 3);
        return 1;
    }
    return 0;
}

static mino_val_t *read_list_form(mino_state_t *S, const char **p)
{
    /* Caller has positioned *p on the opening '('. */
    int         list_line = S->reader_line;
    int         list_col  = S->reader_col;
    mino_val_t *head = mino_nil(S);
    mino_val_t *tail = NULL;
    ADVANCE(S, p); /* skip '(' */
    for (;;) {
        skip_ws(S, p);
        if (**p == '\0') {
            set_reader_diag(S, MRE003, "unterminated list",
                            list_line, list_col);
            return NULL;
        }
        if (**p == ')') {
            ADVANCE(S, p);
            return head;
        }
        if (peek_reader_cond_splice(S, p)) {
            /* #?@ splice: read conditional body, splice matching list */
            mino_val_t *found = NULL;
            int         splice_line = S->reader_line;
            skip_ws(S, p);
            if (**p != '(') {
                set_reader_diag(S, MRE007,
                                "#?@ must be followed by a list",
                                S->reader_line, S->reader_col);
                return NULL;
            }
            ADVANCE(S, p);
            read_cond_body(S, p, &found);
            if (mino_last_error(S) != NULL) return NULL;
            if (found != NULL) {
                /* Splice: iterate found and append elements */
                if (found->type == MINO_VECTOR) {
                    size_t i;
                    for (i = 0; i < found->as.vec.len; i++) {
                        mino_val_t *cell = mino_cons(S,
                            vec_nth(found, i), mino_nil(S));
                        cell->as.cons.file   = S->reader_file;
                        cell->as.cons.line   = (tail == NULL)
                                              ? list_line : splice_line;
                        cell->as.cons.column = (tail == NULL)
                                              ? list_col : 0;
                        if (tail == NULL) {
                            head = cell;
                        } else {
                            mino_cons_cdr_set(S, tail, cell);
                        }
                        tail = cell;
                    }
                } else {
                    mino_val_t *cur = found;
                    while (mino_is_cons(cur)) {
                        mino_val_t *cell = mino_cons(S,
                            cur->as.cons.car, mino_nil(S));
                        cell->as.cons.file   = S->reader_file;
                        cell->as.cons.line   = (tail == NULL)
                                              ? list_line : splice_line;
                        cell->as.cons.column = (tail == NULL)
                                              ? list_col : 0;
                        if (tail == NULL) {
                            head = cell;
                        } else {
                            mino_cons_cdr_set(S, tail, cell);
                        }
                        tail = cell;
                        cur = cur->as.cons.cdr;
                    }
                }
                gc_unpin(1);
            }
            continue;
        }
        {
            int         elem_line = S->reader_line;
            int         elem_col  = S->reader_col;
            mino_val_t *elem = read_form(S, p);
            if (elem == NULL && mino_last_error(S) != NULL) {
                return NULL;
            }
            if (elem == NULL) {
                /* No form produced (e.g. unmatched #?). Check for
                 * closing delimiter before declaring unterminated. */
                skip_ws(S, p);
                if (**p == ')') {
                    ADVANCE(S, p);
                    return head;
                }
                if (**p == '\0') {
                    set_reader_diag(S, MRE003, "unterminated list",
                            list_line, list_col);
                    return NULL;
                }
                continue;
            }
            {
                mino_val_t *cell = mino_cons(S, elem, mino_nil(S));
                cell->as.cons.file   = S->reader_file;
                cell->as.cons.line   = (tail == NULL) ? list_line : elem_line;
                cell->as.cons.column = (tail == NULL) ? list_col  : elem_col;
                if (tail == NULL) {
                    head = cell;
                } else {
                    mino_cons_cdr_set(S, tail, cell);
                }
                tail = cell;
            }
        }
    }
}

static mino_val_t *read_vector_form(mino_state_t *S, const char **p)
{
    /* Caller has positioned *p on the opening '['. `buf` accumulates the
     * partially-built element list and is tracked by the GC so intermediate
     * allocations inside nested read_form calls don't collect the entries
     * that have already been parsed. */
    mino_val_t **buf = NULL;
    size_t       cap = 0;
    size_t       len = 0;
    ADVANCE(S, p); /* skip '[' */
    for (;;) {
        skip_ws(S, p);
        if (**p == '\0') {
            set_reader_diag(S, MRE003, "unterminated vector",
                            S->reader_line, S->reader_col);
            return NULL;
        }
        if (**p == ']') {
            ADVANCE(S, p);
            break;
        }
        if (peek_reader_cond_splice(S, p)) {
            /* #?@ splice in vector */
            mino_val_t *found = NULL;
            skip_ws(S, p);
            if (**p != '(') {
                set_reader_diag(S, MRE007,
                                "#?@ must be followed by a list",
                                S->reader_line, S->reader_col);
                return NULL;
            }
            ADVANCE(S, p);
            read_cond_body(S, p, &found);
            if (mino_last_error(S) != NULL) return NULL;
            if (found != NULL) {
                /* Splice elements from matched vector/list into buf */
                if (found->type == MINO_VECTOR) {
                    size_t i;
                    size_t flen = found->as.vec.len;
                    for (i = 0; i < flen; i++) {
                        if (len == cap) {
                            size_t new_cap = cap == 0 ? 8 : cap * 2;
                            mino_val_t **nb = (mino_val_t **)gc_alloc_typed(
                                S, GC_T_VALARR, new_cap * sizeof(*nb));
                            if (buf != NULL && len > 0)
                                memcpy(nb, buf, len * sizeof(*nb));
                            buf = nb; cap = new_cap;
                        }
                        buf[len++] = vec_nth(found, i);
                    }
                } else {
                    mino_val_t *cur = found;
                    while (mino_is_cons(cur)) {
                        if (len == cap) {
                            size_t new_cap = cap == 0 ? 8 : cap * 2;
                            mino_val_t **nb = (mino_val_t **)gc_alloc_typed(
                                S, GC_T_VALARR, new_cap * sizeof(*nb));
                            if (buf != NULL && len > 0)
                                memcpy(nb, buf, len * sizeof(*nb));
                            buf = nb; cap = new_cap;
                        }
                        buf[len++] = cur->as.cons.car;
                        cur = cur->as.cons.cdr;
                    }
                }
                gc_unpin(1);
            }
            continue;
        }
        {
            mino_val_t *elem = read_form(S, p);
            if (elem == NULL) {
                if (mino_last_error(S) != NULL) return NULL;
                /* No form produced (e.g. unmatched #?). Check for
                 * closing delimiter before declaring unterminated. */
                skip_ws(S, p);
                if (**p == ']') { ADVANCE(S, p); break; }
                if (**p == '\0') {
                    set_reader_diag(S, MRE003, "unterminated vector",
                            S->reader_line, S->reader_col);
                    return NULL;
                }
                continue;
            }
            if (len == cap) {
                size_t       new_cap = cap == 0 ? 8 : cap * 2;
                mino_val_t **nb      = (mino_val_t **)gc_alloc_typed(S,
                    GC_T_VALARR, new_cap * sizeof(*nb));
                if (buf != NULL && len > 0) {
                    memcpy(nb, buf, len * sizeof(*nb));
                }
                buf = nb;
                cap = new_cap;
            }
            buf[len++] = elem;
        }
    }
    return mino_vector(S, buf, len);
}

static mino_val_t *read_map_form(mino_state_t *S, const char **p)
{
    /* Caller has positioned *p on the opening '{'. Elements alternate as
     * key, value, key, value. An odd count is a parse error. The key and
     * value buffers are GC-tracked so parsed entries survive allocations
     * performed by later nested read_form calls. */
    mino_val_t **kbuf = NULL;
    mino_val_t **vbuf = NULL;
    size_t       cap  = 0;
    size_t       len  = 0;
    ADVANCE(S, p); /* skip '{' */
    for (;;) {
        mino_val_t *key;
        mino_val_t *val;
        skip_ws(S, p);
        if (**p == '\0') {
            set_reader_diag(S, MRE003, "unterminated map",
                            S->reader_line, S->reader_col);
            return NULL;
        }
        if (**p == '}') {
            ADVANCE(S, p);
            break;
        }
        if (peek_reader_cond_splice(S, p)) {
            mino_val_t *found = NULL;
            skip_ws(S, p);
            if (**p != '(') {
                set_reader_diag(S, MRE007,
                                "#?@ must be followed by a list",
                                S->reader_line, S->reader_col);
                return NULL;
            }
            ADVANCE(S, p);
            read_cond_body(S, p, &found);
            if (mino_last_error(S) != NULL) return NULL;
            if (found != NULL) {
                /* Splice alternating key-value pairs. */
                if (found->type == MINO_VECTOR) {
                    size_t i;
                    if (found->as.vec.len % 2 != 0) {
                        set_reader_diag(S, MRE008,
                            "#?@ splice into map requires even number of forms",
                            S->reader_line, S->reader_col);
                        gc_unpin(1);
                        return NULL;
                    }
                    for (i = 0; i < found->as.vec.len; i += 2) {
                        if (len == cap) {
                            size_t new_cap = cap == 0 ? 8 : cap * 2;
                            mino_val_t **nk = (mino_val_t **)gc_alloc_typed(S,
                                GC_T_VALARR, new_cap * sizeof(*nk));
                            mino_val_t **nv = (mino_val_t **)gc_alloc_typed(S,
                                GC_T_VALARR, new_cap * sizeof(*nv));
                            if (kbuf != NULL && len > 0) {
                                memcpy(nk, kbuf, len * sizeof(*nk));
                                memcpy(nv, vbuf, len * sizeof(*nv));
                            }
                            kbuf = nk; vbuf = nv; cap = new_cap;
                        }
                        kbuf[len] = vec_nth(found, i);
                        vbuf[len] = vec_nth(found, i + 1);
                        len++;
                    }
                } else {
                    mino_val_t *cur = found;
                    while (mino_is_cons(cur) && mino_is_cons(cur->as.cons.cdr)) {
                        if (len == cap) {
                            size_t new_cap = cap == 0 ? 8 : cap * 2;
                            mino_val_t **nk = (mino_val_t **)gc_alloc_typed(S,
                                GC_T_VALARR, new_cap * sizeof(*nk));
                            mino_val_t **nv = (mino_val_t **)gc_alloc_typed(S,
                                GC_T_VALARR, new_cap * sizeof(*nv));
                            if (kbuf != NULL && len > 0) {
                                memcpy(nk, kbuf, len * sizeof(*nk));
                                memcpy(nv, vbuf, len * sizeof(*nv));
                            }
                            kbuf = nk; vbuf = nv; cap = new_cap;
                        }
                        kbuf[len] = cur->as.cons.car;
                        vbuf[len] = cur->as.cons.cdr->as.cons.car;
                        len++;
                        cur = cur->as.cons.cdr->as.cons.cdr;
                    }
                    if (mino_is_cons(cur)) {
                        set_reader_diag(S, MRE008,
                            "#?@ splice into map requires even number of forms",
                            S->reader_line, S->reader_col);
                        gc_unpin(1);
                        return NULL;
                    }
                }
                gc_unpin(1);
            }
            continue;
        }
        key = read_form(S, p);
        if (key == NULL) {
            if (mino_last_error(S) != NULL) return NULL;
            /* Key eliminated by reader conditional — consume and discard
             * the paired value before continuing. */
            skip_ws(S, p);
            if (**p == '}') { ADVANCE(S, p); break; }
            {
                mino_val_t *discard = read_form(S, p);
                if (discard == NULL && mino_last_error(S) != NULL)
                    return NULL;
            }
            skip_ws(S, p);
            if (**p == '}') { ADVANCE(S, p); break; }
            continue;
        }
        skip_ws(S, p);
        if (**p == '}' || **p == '\0') {
            set_reader_diag(S, MRE008,
                            "map literal has odd number of forms",
                            S->reader_line, S->reader_col);
            return NULL;
        }
        val = read_form(S, p);
        if (val == NULL) {
            if (mino_last_error(S) != NULL) return NULL;
            /* Value form produced nothing — skip the key too. */
            skip_ws(S, p);
            if (**p == '}') { ADVANCE(S, p); break; }
            continue;
        }
        if (len == cap) {
            size_t       new_cap = cap == 0 ? 8 : cap * 2;
            mino_val_t **nk      = (mino_val_t **)gc_alloc_typed(S,
                GC_T_VALARR, new_cap * sizeof(*nk));
            mino_val_t **nv      = (mino_val_t **)gc_alloc_typed(S,
                GC_T_VALARR, new_cap * sizeof(*nv));
            if (kbuf != NULL && len > 0) {
                memcpy(nk, kbuf, len * sizeof(*nk));
                memcpy(nv, vbuf, len * sizeof(*nv));
            }
            kbuf = nk;
            vbuf = nv;
            cap  = new_cap;
        }
        kbuf[len] = key;
        vbuf[len] = val;
        len++;
    }
    return mino_map(S, kbuf, vbuf, len);
}

static mino_val_t *read_set_form(mino_state_t *S, const char **p)
{
    /* Caller has positioned *p on the opening '{' after '#'. */
    mino_val_t **buf = NULL;
    size_t       cap = 0;
    size_t       len = 0;
    ADVANCE(S, p); /* skip '{' */
    for (;;) {
        mino_val_t *elem;
        skip_ws(S, p);
        if (**p == '\0') {
            set_reader_diag(S, MRE003, "unterminated set",
                            S->reader_line, S->reader_col);
            return NULL;
        }
        if (**p == '}') {
            ADVANCE(S, p);
            break;
        }
        elem = read_form(S, p);
        if (elem == NULL) {
            if (mino_last_error(S) == NULL) {
                set_reader_diag(S, MRE003, "unterminated set",
                            S->reader_line, S->reader_col);
            }
            return NULL;
        }
        if (len == cap) {
            size_t       new_cap = cap == 0 ? 8 : cap * 2;
            mino_val_t **nb      = (mino_val_t **)gc_alloc_typed(S,
                GC_T_VALARR, new_cap * sizeof(*nb));
            if (buf != NULL && len > 0) {
                memcpy(nb, buf, len * sizeof(*nb));
            }
            buf = nb;
            cap = new_cap;
        }
        buf[len++] = elem;
    }
    return mino_set(S, buf, len);
}

static mino_val_t *read_atom(mino_state_t *S, const char **p)
{
    const char *start = *p;
    size_t len = 0;
    while (!is_terminator((*p)[len])) {
        len++;
    }
    ADVANCE_N(S, p, len);

    if (len >= 2 && start[0] == ':') {
        /* ::foo / ::alias/foo: auto-resolve at read time. */
        if (len >= 3 && start[1] == ':') {
            const char *body = start + 2;
            size_t      body_len = len - 2;
            const char *slash = memchr(body, '/', body_len);
            const char *resolved_ns = NULL;
            size_t      resolved_ns_len = 0;
            const char *kw_name;
            size_t      kw_name_len;
            char        full[512];
            if (body_len == 0) {
                set_reader_diag(S, MRE008,
                                "auto-resolved keyword missing name",
                                S->reader_line, S->reader_col);
                return NULL;
            }
            if (slash != NULL) {
                size_t alias_len = (size_t)(slash - body);
                size_t i;
                const char *cur = S->current_ns != NULL ? S->current_ns : "user";
                if (alias_len == 0 || alias_len + 1 == body_len) {
                    set_reader_diag(S, MRE008,
                                    "malformed auto-resolved keyword",
                                    S->reader_line, S->reader_col);
                    return NULL;
                }
                for (i = 0; i < S->ns_alias_len; i++) {
                    const char *a = S->ns_aliases[i].alias;
                    if (S->ns_aliases[i].owning_ns == NULL
                        || strcmp(S->ns_aliases[i].owning_ns, cur) != 0) continue;
                    if (strlen(a) == alias_len
                        && memcmp(a, body, alias_len) == 0) {
                        resolved_ns     = S->ns_aliases[i].full_name;
                        resolved_ns_len = strlen(resolved_ns);
                        break;
                    }
                }
                if (resolved_ns == NULL) {
                    char msg[256];
                    snprintf(msg, sizeof(msg),
                             "no such alias: %.*s",
                             (int)alias_len, body);
                    set_reader_diag(S, MRE008, msg,
                                    S->reader_line, S->reader_col);
                    return NULL;
                }
                kw_name     = slash + 1;
                kw_name_len = body_len - alias_len - 1;
            } else {
                resolved_ns     = (S->current_ns != NULL)
                                  ? S->current_ns : "user";
                resolved_ns_len = strlen(resolved_ns);
                kw_name         = body;
                kw_name_len     = body_len;
            }
            if (resolved_ns_len + 1 + kw_name_len >= sizeof(full)) {
                set_reader_diag(S, MRE008,
                                "auto-resolved keyword too long",
                                S->reader_line, S->reader_col);
                return NULL;
            }
            memcpy(full, resolved_ns, resolved_ns_len);
            full[resolved_ns_len] = '/';
            memcpy(full + resolved_ns_len + 1, kw_name, kw_name_len);
            return mino_keyword_n(S, full, resolved_ns_len + 1 + kw_name_len);
        }
        /* Reject trailing slash like :bar/ */
        {
            const char *slash = memchr(start + 1, '/', len - 1);
            if (slash != NULL && slash == start + len - 1) {
                set_reader_diag(S, MRE008, "malformed keyword",
                                S->reader_line, S->reader_col);
                return NULL;
            }
        }
        return mino_keyword_n(S, start + 1, len - 1);
    }
    if (len == 1 && start[0] == ':') {
        set_reader_diag(S, MRE008, "keyword missing name",
                        S->reader_line, S->reader_col);
        return NULL;
    }

    if (len == 3 && memcmp(start, "nil", 3) == 0) {
        return mino_nil(S);
    }
    if (len == 4 && memcmp(start, "true", 4) == 0) {
        return mino_true(S);
    }
    if (len == 5 && memcmp(start, "false", 5) == 0) {
        return mino_false(S);
    }

    /* Try numeric. */
    if (len < sizeof(((struct { char b[64]; } *)0)->b)) {
        char buf[64];
        char *endp = NULL;
        size_t scan_start = 0;
        size_t num_len = len;
        int has_dot_or_exp = 0;
        int looks_numeric = 1;
        size_t i;
        memcpy(buf, start, len);
        buf[len] = '\0';

        /* Sign prefix. */
        if (buf[0] == '+' || buf[0] == '-') scan_start = 1;
        if (scan_start == len) looks_numeric = 0;

        /* Hex: 0x... or [+-]0x... */
        if (looks_numeric && scan_start + 2 < len
            && buf[scan_start] == '0'
            && (buf[scan_start + 1] == 'x' || buf[scan_start + 1] == 'X')) {
            long long n = strtoll(buf, &endp, 16);
            if (endp == buf + len)
                return mino_int(S, n);
            looks_numeric = 0;
        }

        /* Radix: [+-]?NrDIGITS where N is base 2-36 (e.g. 2r1010, 16rFF) */
        if (looks_numeric) {
            const char *r_pos = NULL;
            for (i = scan_start; i < len; i++) {
                if (buf[i] == 'r' || buf[i] == 'R') { r_pos = buf + i; break; }
            }
            if (r_pos != NULL && r_pos > buf + scan_start && r_pos < buf + len - 1) {
                int all_base_digits = 1;
                for (i = scan_start; i < (size_t)(r_pos - buf); i++) {
                    if (!isdigit((unsigned char)buf[i])) { all_base_digits = 0; break; }
                }
                if (all_base_digits) {
                    long base = strtol(buf + scan_start, NULL, 10);
                    if (base >= 2 && base <= 36) {
                        /* Parse the digits after 'r' in the given base. */
                        char radix_buf[64];
                        size_t radix_len = len - (size_t)(r_pos - buf) - 1;
                        int sign = 1;
                        if (scan_start > 0 && buf[0] == '-') sign = -1;
                        if (radix_len > 0 && radix_len < sizeof(radix_buf)) {
                            memcpy(radix_buf, r_pos + 1, radix_len);
                            radix_buf[radix_len] = '\0';
                            {
                                long long n = strtoll(radix_buf, &endp, (int)base);
                                if (endp == radix_buf + radix_len)
                                    return mino_int(S, sign * n);
                            }
                        }
                    }
                }
            }
        }

        /* Ratio: digits/digits with optional sign prefix.
         * Must not match namespace-qualified symbols (alpha/alpha). */
        if (looks_numeric) {
            const char *slash = NULL;
            for (i = scan_start; i < len; i++) {
                if (buf[i] == '/') { slash = buf + i; break; }
            }
            if (slash != NULL && slash > buf + scan_start && slash < buf + len - 1) {
                int all_digits = 1;
                for (i = scan_start; i < (size_t)(slash - buf); i++) {
                    if (!isdigit((unsigned char)buf[i])) { all_digits = 0; break; }
                }
                if (all_digits) {
                    for (i = (size_t)(slash - buf) + 1; i < len; i++) {
                        if (!isdigit((unsigned char)buf[i])) { all_digits = 0; break; }
                    }
                }
                if (all_digits) {
                    /* Parse numerator and denominator as bigints so
                     * arbitrary magnitudes are supported, then build the
                     * canonical ratio. mino_ratio_make handles gcd-
                     * reduction and integer narrowing (e.g. `4/2` reads
                     * back as `2`). */
                    size_t      num_str_len = (size_t)(slash - buf);
                    size_t      den_str_len = len - num_str_len - 1;
                    mino_val_t *num_bi = mino_bigint_from_string_n(
                        S, buf, num_str_len);
                    mino_val_t *den_bi;
                    if (num_bi == NULL) {
                        set_reader_diag(S, MRE008,
                                        "invalid ratio literal",
                                        S->reader_line, S->reader_col);
                        return NULL;
                    }
                    den_bi = mino_bigint_from_string_n(
                        S, slash + 1, den_str_len);
                    if (den_bi == NULL) {
                        set_reader_diag(S, MRE008,
                                        "invalid ratio literal",
                                        S->reader_line, S->reader_col);
                        return NULL;
                    }
                    return mino_ratio_make(S, num_bi, den_bi);
                }
            }
        }

        /* Bigint N suffix: 42N -> (bigint 42). Always produces MINO_BIGINT,
         * even for values that would fit in a long long. Clojure parity:
         * `(type 1N)` returns clojure.lang.BigInt regardless of magnitude. */
        if (looks_numeric && len > 1 && buf[len - 1] == 'N') {
            size_t       digit_start = scan_start;
            int          all_digits  = 1;
            num_len = len - 1;
            /* Require digits before N (possibly after a sign prefix). */
            if (num_len <= digit_start) {
                all_digits = 0;
            } else {
                for (i = digit_start; i < num_len; i++) {
                    if (!isdigit((unsigned char)buf[i])) { all_digits = 0; break; }
                }
            }
            if (all_digits) {
                mino_val_t *bi;
                buf[num_len] = '\0';
                bi = mino_bigint_from_string_n(S, buf, num_len);
                buf[num_len] = 'N';
                if (bi != NULL) return bi;
            }
            num_len = len;
        }

        /* Bigdec M suffix: 1.5M -> arbitrary-precision decimal. */
        if (looks_numeric && len > 1 && buf[len - 1] == 'M') {
            mino_val_t *bd;
            num_len = len - 1;
            buf[num_len] = '\0';
            bd = mino_bigdec_from_string(S, buf);
            buf[num_len] = 'M';
            num_len = len;
            if (bd != NULL) return bd;
        }

        /* Standard decimal. */
        for (i = scan_start; i < len; i++) {
            char c = buf[i];
            if (c == '.' || c == 'e' || c == 'E') {
                has_dot_or_exp = 1;
                if ((c == 'e' || c == 'E') && i + 1 < len &&
                    (buf[i + 1] == '+' || buf[i + 1] == '-')) {
                    i++;
                }
            } else if (!isdigit((unsigned char)c)) {
                looks_numeric = 0;
                break;
            }
        }
        if (looks_numeric) {
            if (has_dot_or_exp) {
                double d = strtod(buf, &endp);
                if (endp == buf + len)
                    return mino_float(S, d);
            } else {
                long long n = strtoll(buf, &endp, 10);
                if (endp == buf + len)
                    return mino_int(S, n);
            }
        }
    }

    /* Symbols must not end with a colon (foo: is rejected by upstream
     * readers; ::-prefix and :name keywords are handled earlier). */
    if (len > 0 && start[len - 1] == ':') {
        set_reader_diag(S, MRE008, "invalid symbol",
                        S->reader_line, S->reader_col);
        return NULL;
    }
    return mino_symbol_n(S, start, len);
}

/* ---- #() anonymous function reader macro helpers ---- */

/*
 * Walk a parsed form tree, recording which % arg slots are used.
 * used[0] = true means bare % or %1, used[1] = %2, ..., used[8] = %9.
 * *has_rest = true means %& was found.  *max_arg = highest numbered slot.
 */
static void scan_percent_args(mino_val_t *form, int used[9],
                              int *max_arg, int *has_rest)
{
    if (form == NULL) return;
    if (form->type == MINO_SYMBOL) {
        const char *s = form->as.s.data;
        size_t      n = form->as.s.len;
        if (n == 1 && s[0] == '%') {
            used[0] = 1;
            if (*max_arg < 1) *max_arg = 1;
        } else if (n == 2 && s[0] == '%' && s[1] >= '1' && s[1] <= '9') {
            int idx = s[1] - '1';
            used[idx] = 1;
            if (*max_arg < idx + 1) *max_arg = idx + 1;
        } else if (n == 2 && s[0] == '%' && s[1] == '&') {
            *has_rest = 1;
        }
        return;
    }
    if (mino_is_cons(form)) {
        scan_percent_args(form->as.cons.car, used, max_arg, has_rest);
        scan_percent_args(form->as.cons.cdr, used, max_arg, has_rest);
        return;
    }
    if (form->type == MINO_VECTOR) {
        size_t i;
        for (i = 0; i < form->as.vec.len; i++)
            scan_percent_args(vec_nth(form, i), used, max_arg, has_rest);
        return;
    }
    if (form->type == MINO_MAP) {
        size_t i;
        for (i = 0; i < form->as.map.len; i++) {
            scan_percent_args(vec_nth(form->as.map.key_order, i),
                              used, max_arg, has_rest);
            scan_percent_args(map_get_val(form,
                              vec_nth(form->as.map.key_order, i)),
                              used, max_arg, has_rest);
        }
        return;
    }
    if (form->type == MINO_SET) {
        size_t i;
        for (i = 0; i < form->as.set.len; i++)
            scan_percent_args(vec_nth(form->as.set.key_order, i),
                              used, max_arg, has_rest);
        return;
    }
}

/*
 * Replace bare % with %1 in a parsed form tree so the evaluator
 * sees a single canonical name.
 */
static mino_val_t *normalize_percent(mino_state_t *S, mino_val_t *form)
{
    if (form == NULL) return form;
    if (form->type == MINO_SYMBOL && form->as.s.len == 1
        && form->as.s.data[0] == '%') {
        return mino_symbol(S, "%1");
    }
    if (mino_is_cons(form)) {
        mino_val_t *car = normalize_percent(S, form->as.cons.car);
        mino_val_t *cdr = normalize_percent(S, form->as.cons.cdr);
        if (car == form->as.cons.car && cdr == form->as.cons.cdr)
            return form;
        {
            mino_val_t *c = mino_cons(S, car, cdr);
            c->as.cons.file   = form->as.cons.file;
            c->as.cons.line   = form->as.cons.line;
            c->as.cons.column = form->as.cons.column;
            return c;
        }
    }
    if (form->type == MINO_VECTOR) {
        size_t      i;
        int         changed = 0;
        mino_val_t *stack_items[64];
        mino_val_t **items;
        size_t      len = form->as.vec.len;
        if (len <= 64) {
            items = stack_items;
        } else {
            items = (mino_val_t **)malloc(len * sizeof(*items));
            if (items == NULL) {
                set_reader_diag(S, MRE004,
                                "out of memory in anonymous fn expansion",
                                S->reader_line, S->reader_col);
                return NULL;
            }
        }
        for (i = 0; i < len; i++) {
            items[i] = normalize_percent(S, vec_nth(form, i));
            if (items[i] != vec_nth(form, i)) changed = 1;
        }
        {
            mino_val_t *result = changed ? mino_vector(S, items, len) : form;
            if (items != stack_items) free(items);
            return result;
        }
    }
    return form;
}

/*
 * read_anon_fn_form: #(inc %) => (fn [%1] (inc %1))
 * Called after '#' has been verified; caller already checked *(*p+1)=='('.
 */
static mino_val_t *read_anon_fn_form(mino_state_t *S, const char **p)
{
    int fn_line = S->reader_line;
    int fn_col  = S->reader_col;
    int used[9] = {0};
    int max_arg = 0, has_rest = 0;
    mino_val_t *body;
    mino_val_t *params_vec;
    mino_val_t *fn_form;
    ADVANCE(S, p); /* skip '#', read_list_form will handle '(' */
    body = read_list_form(S, p);
    if (body == NULL) return NULL;
    scan_percent_args(body, used, &max_arg, &has_rest);
    body = normalize_percent(S, body);
    {
        mino_val_t *items[12]; /* max 9 + & + %& */
        size_t      nparams = 0;
        int         i;
        char        name[4];
        for (i = 0; i < max_arg; i++) {
            name[0] = '%';
            name[1] = (char)('1' + i);
            name[2] = '\0';
            items[nparams++] = mino_symbol(S, name);
        }
        if (has_rest) {
            items[nparams++] = mino_symbol(S, "&");
            items[nparams++] = mino_symbol(S, "%&");
        }
        params_vec = mino_vector(S, items, nparams);
    }
    fn_form = mino_cons(S, mino_symbol(S, "fn"),
                  mino_cons(S, params_vec,
                      mino_cons(S, body, mino_nil(S))));
    fn_form->as.cons.file   = S->reader_file;
    fn_form->as.cons.line   = fn_line;
    fn_form->as.cons.column = fn_col;
    return fn_form;
}

/*
 * read_metadata_form: ^{:k v} form, ^:k form, ^Symbol form, ^"S" form.
 * Called after '^' has been verified at **p.
 */
static mino_val_t *read_metadata_form(mino_state_t *S, const char **p)
{
    mino_val_t *meta_val, *target;
    ADVANCE(S, p);
    meta_val = read_form(S, p);
    if (meta_val == NULL) {
        if (mino_last_error(S) == NULL) {
            set_reader_diag(S, MRE010, "expected metadata after ^",
                            S->reader_line, S->reader_col);
        }
        return NULL;
    }
    /* ^:key shorthand: expand to {:key true}. */
    if (meta_val->type == MINO_KEYWORD) {
        mino_val_t *kv[1], *vv[1];
        kv[0] = meta_val;
        vv[0] = mino_true(S);
        meta_val = mino_map(S, kv, vv, 1);
    }
    /* ^Symbol shorthand: expand to {:tag Symbol}. */
    if (meta_val->type == MINO_SYMBOL) {
        mino_val_t *kv[1], *vv[1];
        kv[0] = mino_keyword(S, "tag");
        vv[0] = meta_val;
        meta_val = mino_map(S, kv, vv, 1);
    }
    /* ^"String" shorthand: expand to {:tag "String"}. */
    if (meta_val->type == MINO_STRING) {
        mino_val_t *kv[1], *vv[1];
        kv[0] = mino_keyword(S, "tag");
        vv[0] = meta_val;
        meta_val = mino_map(S, kv, vv, 1);
    }
    if (meta_val->type != MINO_MAP) {
        set_reader_diag(S, MRE010,
                        "metadata must be a map, keyword, symbol, or string",
                        S->reader_line, S->reader_col);
        return NULL;
    }
    target = read_form(S, p);
    if (target == NULL) {
        if (mino_last_error(S) == NULL) {
            set_reader_diag(S, MRE009, "expected form after metadata",
                            S->reader_line, S->reader_col);
        }
        return NULL;
    }
    /* Attach metadata directly to the value instead of desugaring. */
    if (target->type == MINO_SYMBOL || target->type == MINO_VECTOR
        || target->type == MINO_MAP || target->type == MINO_CONS
        || target->type == MINO_SET) {
        /* Symbols are interned (shared). Make a fresh copy so we
         * do not mutate the interned instance. */
        if (target->type == MINO_SYMBOL) {
            mino_val_t *fresh = alloc_val(S, MINO_SYMBOL);
            fresh->as.s.data = target->as.s.data;
            fresh->as.s.len  = target->as.s.len;
            target = fresh;
        }
        /* Merge with any existing metadata from chained ^ syntax. */
        if (target->meta != NULL && target->meta->type == MINO_MAP) {
            size_t i;
            size_t ko_len;
            mino_val_t *ko = meta_val->as.map.key_order;
            ko_len = (ko != NULL) ? ko->as.vec.len : 0;
            for (i = 0; i < ko_len; i++) {
                mino_val_t *k = vec_nth(ko, i);
                mino_val_t *v = map_get_val(meta_val, k);
                int replaced = 0;
                hamt_entry_t *e = hamt_entry_new(S, k, v);
                uint32_t h = hash_val(k);
                mino_hamt_node_t *nr = hamt_assoc(S,
                    target->meta->as.map.root, e, h, 0, &replaced);
                if (!replaced) {
                    target->meta->as.map.key_order =
                        vec_conj1(S, target->meta->as.map.key_order, k);
                    target->meta->as.map.len++;
                }
                target->meta->as.map.root = nr;
            }
        } else {
            target->meta = meta_val;
        }
        return target;
    }
    /* Fallback for types that do not support metadata: desugar to
     * (with-meta target meta-map) so it fails at eval time. */
    {
        mino_val_t *outer;
        outer = mino_cons(S, mino_symbol(S, "with-meta"),
                    mino_cons(S, target,
                        mino_cons(S, meta_val, mino_nil(S))));
        return outer;
    }
}

/*
 * Character literal reader: \space, \newline, \A, \uNNNN, \oNNN,
 * UTF-8 codepoints (\é, \☃), and single-character tokens like \{.
 * Produces a first-class MINO_CHAR holding the decoded codepoint.
 * On entry, **p == '\\'.
 */
static mino_val_t *read_char_literal(mino_state_t *S, const char **p)
{
    const char *start = *p + 1;
    size_t      tlen  = 0;
    int         cp;
    ADVANCE(S, p);
    if (**p == '\0') {
        set_reader_diag(S, MRE001,
                        "unexpected end of input after \\",
                        S->reader_line, S->reader_col);
        return NULL;
    }
    while (!is_terminator(start[tlen])) tlen++;
    ADVANCE_N(S, p, tlen);
    if (tlen == 5 && memcmp(start, "space", 5) == 0) {
        cp = ' ';
    } else if (tlen == 7 && memcmp(start, "newline", 7) == 0) {
        cp = '\n';
    } else if (tlen == 3 && memcmp(start, "tab", 3) == 0) {
        cp = '\t';
    } else if (tlen == 6 && memcmp(start, "return", 6) == 0) {
        cp = '\r';
    } else if (tlen == 9 && memcmp(start, "backspace", 9) == 0) {
        cp = '\b';
    } else if (tlen == 8 && memcmp(start, "formfeed", 8) == 0) {
        cp = '\f';
    } else if (tlen == 1) {
        cp = (unsigned char)start[0];
    } else if (tlen >= 2 && tlen <= 4 && ((unsigned char)start[0] & 0x80)) {
        /* Multi-byte UTF-8 literal: \é, \☃, etc. Decode the leading
         * byte's bit pattern to determine expected length, then
         * accumulate continuation bytes. */
        unsigned char lead = (unsigned char)start[0];
        size_t        expect;
        unsigned      u;
        size_t        j;
        if      ((lead & 0xE0) == 0xC0) { expect = 2; u = lead & 0x1Fu; }
        else if ((lead & 0xF0) == 0xE0) { expect = 3; u = lead & 0x0Fu; }
        else if ((lead & 0xF8) == 0xF0) { expect = 4; u = lead & 0x07u; }
        else {
            set_reader_diag(S, MRE008,
                            "invalid UTF-8 character literal",
                            S->reader_line, S->reader_col);
            return NULL;
        }
        if (tlen != expect) {
            set_reader_diag(S, MRE008,
                            "malformed UTF-8 character literal",
                            S->reader_line, S->reader_col);
            return NULL;
        }
        for (j = 1; j < expect; j++) {
            unsigned char c = (unsigned char)start[j];
            if ((c & 0xC0) != 0x80) {
                set_reader_diag(S, MRE008,
                                "invalid UTF-8 continuation byte",
                                S->reader_line, S->reader_col);
                return NULL;
            }
            u = (u << 6) | (c & 0x3Fu);
        }
        cp = (int)u;
    } else if (tlen == 0 && start[0] != '\0') {
        /* Single terminator/whitespace character as literal: \{ \; \, etc. */
        cp = (unsigned char)start[0];
        ADVANCE(S, p);
    } else if (tlen >= 2 && start[0] == 'o') {
        /* Octal escape: \oNNN */
        unsigned oval = 0;
        size_t   j;
        for (j = 1; j < tlen; j++) {
            if (start[j] < '0' || start[j] > '7') {
                set_reader_diag(S, MRE008,
                                "invalid octal character literal",
                                S->reader_line, S->reader_col);
                return NULL;
            }
            oval = (oval << 3) | (unsigned)(start[j] - '0');
        }
        if (oval > 0377) {
            set_reader_diag(S, MRE008,
                            "octal character literal out of range",
                            S->reader_line, S->reader_col);
            return NULL;
        }
        cp = (int)oval;
    } else if (tlen >= 5 && start[0] == 'u') {
        /* Unicode escape: \uXXXX */
        unsigned u = 0;
        size_t   j;
        if (tlen != 5) {
            set_reader_diag(S, MRE008, "invalid unicode character literal",
                            S->reader_line, S->reader_col);
            return NULL;
        }
        for (j = 1; j < 5; j++) {
            unsigned d;
            char     c = start[j];
            if      (c >= '0' && c <= '9') d = (unsigned)(c - '0');
            else if (c >= 'a' && c <= 'f') d = (unsigned)(c - 'a' + 10);
            else if (c >= 'A' && c <= 'F') d = (unsigned)(c - 'A' + 10);
            else {
                set_reader_diag(S, MRE008,
                                "invalid unicode character literal",
                                S->reader_line, S->reader_col);
                return NULL;
            }
            u = (u << 4) | d;
        }
        cp = (int)u;
    } else {
        set_reader_diag(S, MRE008, "unknown character literal",
                        S->reader_line, S->reader_col);
        return NULL;
    }
    return mino_char(S, cp);
}

/*
 * Wrap the next form in (sym form), preserving the source position
 * of the originating reader macro. Used by the prefix-quote family
 * (`'`, `\``, `@`, `~`, `~@`, `#'`) where each macro reads exactly
 * one following form and tags it with a known head symbol.
 */
static mino_val_t *read_wrap_one(mino_state_t *S, const char **p,
                                 const char *sym_name, const char *after_msg,
                                 int q_line, int q_col)
{
    mino_val_t *inner = read_form(S, p);
    mino_val_t *outer;
    if (inner == NULL) {
        if (mino_last_error(S) == NULL) {
            set_reader_diag(S, MRE009, after_msg,
                            S->reader_line, S->reader_col);
        }
        return NULL;
    }
    outer = mino_cons(S, mino_symbol(S, sym_name),
                      mino_cons(S, inner, mino_nil(S)));
    outer->as.cons.file   = S->reader_file;
    outer->as.cons.line   = q_line;
    outer->as.cons.column = q_col;
    return outer;
}

/*
 * Dispatch the `#`-prefix family. On entry, **p == '#'. Handles the
 * full set of dispatch macros: #{ set, #_ discard, #( anon-fn, #'
 * var-quote, ## special-float, #" regex, #?@ splice (top-level error),
 * #? reader-conditional, and #tag tagged-literal. Returns the read
 * form, NULL with a reader diag on error, or NULL without an error
 * to signal "no form produced" (for #_ at end-of-list and #? with no
 * matching branch).
 */
/* Build a key keyword/symbol qualified against `prefix` per the
 * namespaced-map literal rules:
 *   bare :name      -> :<prefix>/name
 *   :foo/name       -> unchanged (already qualified)
 *   :_/name         -> :name (the underscore namespace strips off)
 *   non keyword/sym -> unchanged
 */
static mino_val_t *namespaced_map_qualify_key(mino_state_t *S, mino_val_t *k,
                                              const char *prefix,
                                              size_t prefix_len)
{
    int is_kw;
    const char *name;
    size_t      namelen;
    const char *slash;
    char        full[512];
    size_t      flen;
    if (k == NULL) return k;
    if (k->type != MINO_KEYWORD && k->type != MINO_SYMBOL) return k;
    is_kw   = (k->type == MINO_KEYWORD);
    name    = k->as.s.data;
    namelen = k->as.s.len;
    slash   = memchr(name, '/', namelen);
    if (slash != NULL) {
        size_t ns_len = (size_t)(slash - name);
        if (ns_len == 1 && name[0] == '_') {
            /* :_/x -> :x */
            const char *bare = slash + 1;
            size_t      bare_len = namelen - 2;
            if (is_kw) return mino_keyword_n(S, bare, bare_len);
            return mino_symbol_n(S, bare, bare_len);
        }
        return k; /* already qualified */
    }
    if (prefix_len + 1 + namelen >= sizeof(full)) return k;
    memcpy(full, prefix, prefix_len);
    full[prefix_len] = '/';
    memcpy(full + prefix_len + 1, name, namelen);
    flen = prefix_len + 1 + namelen;
    if (is_kw) return mino_keyword_n(S, full, flen);
    return mino_symbol_n(S, full, flen);
}

static mino_val_t *read_namespaced_map(mino_state_t *S, const char **p)
{
    /* On entry, **p == '#'. Caller already saw "#:" prefix. */
    char        prefix[256];
    size_t      prefix_len = 0;
    int         saw_double_colon = 0;
    mino_val_t *m;
    mino_val_t *out;
    int         line = S->reader_line;
    int         col  = S->reader_col;
    ADVANCE_N(S, p, 2); /* skip "#:" */
    if (**p == ':') {
        /* "#::" -- auto-resolve form */
        saw_double_colon = 1;
        ADVANCE(S, p);
    } else if (**p == ' ' || **p == '\t' || **p == '\n' || **p == '\r') {
        /* "#: name" -- whitespace between #: and name is illegal. */
        set_reader_diag(S, MRE008,
            "namespaced map: no whitespace allowed after #:",
            line, col);
        return NULL;
    }
    /* Read prefix name (optional after ::, required after :). */
    {
        const char *start = *p;
        size_t      tlen  = 0;
        while ((*p)[tlen] != '\0' && (*p)[tlen] != '{'
               && (*p)[tlen] != ' ' && (*p)[tlen] != '\t'
               && (*p)[tlen] != '\n' && (*p)[tlen] != '\r'
               && (*p)[tlen] != ',') {
            tlen++;
        }
        if (tlen >= sizeof(prefix)) {
            set_reader_diag(S, MRE008,
                "namespaced map prefix too long", line, col);
            return NULL;
        }
        if (memchr(start, '/', tlen) != NULL) {
            set_reader_diag(S, MRE008,
                "namespaced map prefix must not contain /", line, col);
            return NULL;
        }
        if (tlen > 0) {
            if (saw_double_colon) {
                /* #::alias -- look up alias to a real namespace. */
                size_t i;
                const char *resolved = NULL;
                const char *cur = S->current_ns != NULL ? S->current_ns : "user";
                for (i = 0; i < S->ns_alias_len; i++) {
                    const char *a = S->ns_aliases[i].alias;
                    if (S->ns_aliases[i].owning_ns == NULL
                        || strcmp(S->ns_aliases[i].owning_ns, cur) != 0) continue;
                    if (strlen(a) == tlen && memcmp(a, start, tlen) == 0) {
                        resolved = S->ns_aliases[i].full_name;
                        break;
                    }
                }
                if (resolved == NULL) {
                    char msg[256];
                    snprintf(msg, sizeof(msg),
                        "namespaced map: no such alias: %.*s",
                        (int)tlen, start);
                    set_reader_diag(S, MRE008, msg, line, col);
                    return NULL;
                }
                prefix_len = strlen(resolved);
                if (prefix_len >= sizeof(prefix)) {
                    set_reader_diag(S, MRE008,
                        "namespaced map alias target too long", line, col);
                    return NULL;
                }
                memcpy(prefix, resolved, prefix_len);
                prefix[prefix_len] = '\0';
            } else {
                memcpy(prefix, start, tlen);
                prefix[tlen] = '\0';
                prefix_len = tlen;
            }
            ADVANCE_N(S, p, tlen);
        } else if (saw_double_colon) {
            /* #::{...} -- current namespace */
            const char *cur = (S->current_ns != NULL) ? S->current_ns : "user";
            prefix_len = strlen(cur);
            if (prefix_len >= sizeof(prefix)) {
                set_reader_diag(S, MRE008,
                    "namespaced map current ns too long", line, col);
                return NULL;
            }
            memcpy(prefix, cur, prefix_len);
            prefix[prefix_len] = '\0';
        } else {
            /* "#:" with no prefix and no `::` -- malformed. */
            set_reader_diag(S, MRE008,
                "malformed namespaced map prefix", line, col);
            return NULL;
        }
    }
    skip_ws(S, p);
    if (**p != '{') {
        set_reader_diag(S, MRE008,
            "namespaced map prefix must be followed by {",
            S->reader_line, S->reader_col);
        return NULL;
    }
    {
        /* Read the inner map as a flat key/value sequence to catch
         * duplicate keys; an upstream-visible duplicate (whether bare
         * or after prefix qualification) is a reader error. */
        mino_val_t **rk    = NULL;
        mino_val_t **rv    = NULL;
        size_t       cap   = 0;
        size_t       len   = 0;
        size_t       i, j;
        mino_val_t **ks;
        mino_val_t **vs;
        ADVANCE(S, p); /* skip '{' */
        for (;;) {
            mino_val_t *k;
            mino_val_t *v;
            skip_ws(S, p);
            if (**p == '\0') {
                set_reader_diag(S, MRE003, "unterminated map",
                                S->reader_line, S->reader_col);
                return NULL;
            }
            if (**p == '}') { ADVANCE(S, p); break; }
            k = read_form(S, p);
            if (k == NULL) return NULL;
            skip_ws(S, p);
            if (**p == '}' || **p == '\0') {
                set_reader_diag(S, MRE008,
                                "map literal has odd number of forms",
                                S->reader_line, S->reader_col);
                return NULL;
            }
            v = read_form(S, p);
            if (v == NULL) return NULL;
            if (len == cap) {
                size_t       nc  = cap == 0 ? 8 : cap * 2;
                mino_val_t **nk  = (mino_val_t **)gc_alloc_typed(S,
                    GC_T_VALARR, nc * sizeof(*nk));
                mino_val_t **nv  = (mino_val_t **)gc_alloc_typed(S,
                    GC_T_VALARR, nc * sizeof(*nv));
                if (rk != NULL && len > 0) {
                    for (i = 0; i < len; i++) {
                        gc_valarr_set(S, nk, i, rk[i]);
                        gc_valarr_set(S, nv, i, rv[i]);
                    }
                }
                rk = nk; rv = nv; cap = nc;
            }
            gc_valarr_set(S, rk, len, k);
            gc_valarr_set(S, rv, len, v);
            len++;
        }
        ks = (mino_val_t **)gc_alloc_typed(S, GC_T_VALARR,
            len > 0 ? len * sizeof(*ks) : sizeof(*ks));
        vs = (mino_val_t **)gc_alloc_typed(S, GC_T_VALARR,
            len > 0 ? len * sizeof(*vs) : sizeof(*vs));
        for (i = 0; i < len; i++) {
            mino_val_t *qk = namespaced_map_qualify_key(S, rk[i], prefix,
                                                       prefix_len);
            if (qk == NULL) return NULL;
            for (j = 0; j < i; j++) {
                if (mino_eq(ks[j], qk)) {
                    set_reader_diag(S, MRE008,
                        "namespaced map literal contains duplicate key",
                        S->reader_line, S->reader_col);
                    return NULL;
                }
            }
            gc_valarr_set(S, ks, i, qk);
            gc_valarr_set(S, vs, i, rv[i]);
        }
        out = mino_map(S, ks, vs, len);
    }
    return out;
}

static mino_val_t *read_dispatch(mino_state_t *S, const char **p)
{
    char next = *(*p + 1);
    if (next == ':') {
        return read_namespaced_map(S, p);
    }
    if (next == '{') {
        ADVANCE(S, p); /* skip '#', read_set_form will skip '{' */
        return read_set_form(S, p);
    }
    if (next == '_') {
        /* Discard reader macro: #_ discards the next form. */
        mino_val_t *discarded;
        ADVANCE_N(S, p, 2);
        discarded = read_form(S, p);
        (void)discarded;
        if (mino_last_error(S) != NULL)
            return NULL;
        skip_ws(S, p);
        if (**p == ')' || **p == ']' || **p == '}' || **p == '\0')
            return NULL; /* let parent handle closing delimiter */
        return read_form(S, p);
    }
    if (next == '(') {
        return read_anon_fn_form(S, p);
    }
    if (next == '\'') {
        int vq_line = S->reader_line;
        int vq_col  = S->reader_col;
        ADVANCE_N(S, p, 2);
        return read_wrap_one(S, p, "var", "expected form after #'",
                             vq_line, vq_col);
    }
    if (next == '#') {
        /* Special float tokens: ##Inf, ##-Inf, ##NaN */
        const char *start;
        size_t      tlen;
        ADVANCE_N(S, p, 2);
        start = *p;
        tlen = 0;
        while (!is_terminator((*p)[tlen])) tlen++;
        ADVANCE_N(S, p, tlen);
        if (tlen == 3 && memcmp(start, "Inf", 3) == 0)
            return mino_float(S, INFINITY);
        if (tlen == 4 && memcmp(start, "-Inf", 4) == 0)
            return mino_float(S, -INFINITY);
        if (tlen == 3 && memcmp(start, "NaN", 3) == 0)
            return mino_float(S, NAN);
        set_reader_diag(S, MRE008, "unknown tagged literal",
                        S->reader_line, S->reader_col);
        return NULL;
    }
    if (next == '"') {
        /* Regex literal: #"pattern" -- wrap as (re-pattern "pattern"). */
        int         rx_line = S->reader_line;
        int         rx_col  = S->reader_col;
        mino_val_t *str;
        mino_val_t *outer;
        ADVANCE(S, p); /* skip '#', now *p points at '"' */
        str = read_string_form(S, p);
        if (str == NULL) return NULL;
        outer = mino_cons(S, mino_symbol(S, "re-pattern"),
                          mino_cons(S, str, mino_nil(S)));
        outer->as.cons.file   = S->reader_file;
        outer->as.cons.line   = rx_line;
        outer->as.cons.column = rx_col;
        return outer;
    }
    if (next == '?' && *(*p + 2) == '@') {
        set_reader_diag(S, MRE007, "#?@ splice not allowed at top level",
                        S->reader_line, S->reader_col);
        return NULL;
    }
    if (next == '?') {
        mino_val_t *found = NULL;
        ADVANCE_N(S, p, 2);
        skip_ws(S, p);
        if (**p != '(') {
            set_reader_diag(S, MRE007, "#? must be followed by a list",
                            S->reader_line, S->reader_col);
            return NULL;
        }
        ADVANCE(S, p);
        read_cond_body(S, p, &found);
        if (mino_last_error(S) != NULL) return NULL;
        if (found != NULL) {
            gc_unpin(1);
            return found;
        }
        /* No branch matched: return NULL without error to signal
         * "no form produced" to the enclosing reader (list, vector,
         * or map). The enclosing reader will continue to the next
         * form. For maps this is critical -- the map reader must
         * consume and discard the paired value. */
        return NULL;
    }
    if (isalpha((unsigned char)next)) {
        /* Tagged literals: #tag form -- wrap as (tagged-literal :tag form).
         * Handles unknown tags like #js, #inst, #uuid gracefully. */
        const char *tag_start;
        size_t      tag_len;
        mino_val_t *tag_val;
        mino_val_t *body;
        ADVANCE(S, p);
        tag_start = *p;
        tag_len = 0;
        while (!is_terminator((*p)[tag_len])) tag_len++;
        ADVANCE_N(S, p, tag_len);
        skip_ws(S, p);
        body = read_form(S, p);
        if (body == NULL && mino_last_error(S) != NULL) return NULL;
        tag_val = mino_keyword_n(S, tag_start, tag_len);
        return mino_cons(S, mino_symbol(S, "tagged-literal"),
                         mino_cons(S, tag_val,
                                   mino_cons(S, body, mino_nil(S))));
    }
    set_reader_diag(S, MRE008, "unknown reader dispatch macro",
                    S->reader_line, S->reader_col);
    return NULL;
}

static mino_val_t *read_form(mino_state_t *S, const char **p)
{
    skip_ws(S, p);
    if (**p == '\0') {
        return NULL;
    }
    if (**p == '(') {
        return read_list_form(S, p);
    }
    if (**p == ')') {
        set_reader_diag(S, MRE002, "unexpected ')'",
                        S->reader_line, S->reader_col);
        return NULL;
    }
    if (**p == '[') {
        return read_vector_form(S, p);
    }
    if (**p == ']') {
        set_reader_diag(S, MRE002, "unexpected ']'",
                        S->reader_line, S->reader_col);
        return NULL;
    }
    if (**p == '{') {
        return read_map_form(S, p);
    }
    if (**p == '}') {
        set_reader_diag(S, MRE002, "unexpected '}'",
                        S->reader_line, S->reader_col);
        return NULL;
    }
    if (**p == '#') {
        return read_dispatch(S, p);
    }
    if (**p == '"') {
        return read_string_form(S, p);
    }
    if (**p == '\'') {
        int q_line = S->reader_line;
        int q_col  = S->reader_col;
        ADVANCE(S, p);
        return read_wrap_one(S, p, "quote", "expected form after quote",
                             q_line, q_col);
    }
    if (**p == '`') {
        int q_line = S->reader_line;
        int q_col  = S->reader_col;
        ADVANCE(S, p);
        return read_wrap_one(S, p, "quasiquote", "expected form after `",
                             q_line, q_col);
    }
    if (**p == '@') {
        int q_line = S->reader_line;
        int q_col  = S->reader_col;
        ADVANCE(S, p);
        return read_wrap_one(S, p, "deref", "expected form after @",
                             q_line, q_col);
    }
    if (**p == '^') {
        return read_metadata_form(S, p);
    }
    if (**p == '~') {
        int         q_line = S->reader_line;
        int         q_col  = S->reader_col;
        const char *name   = "unquote";
        ADVANCE(S, p);
        if (**p == '@') {
            name = "unquote-splicing";
            ADVANCE(S, p);
        }
        return read_wrap_one(S, p, name, "expected form after ~",
                             q_line, q_col);
    }
    if (**p == '\\') {
        return read_char_literal(S, p);
    }
    return read_atom(S, p);
}

mino_val_t *mino_read(mino_state_t *S, const char *src, const char **end)
{
    volatile char probe = 0;
    const char   *p = src;
    mino_val_t   *v;
    /* Record this frame as a host-level stack bottom so the collector's
     * conservative scan covers the reader's call chain in full. */
    gc_note_host_frame(S, (void *)&probe);
    (void)probe;
    if (S->reader_file == NULL) {
        S->reader_file = intern_filename(S, "<input>");
    }
    clear_error(S);
    v = read_form(S, &p);
    if (end != NULL) {
        *end = p;
    }
    return v;
}

