/*
 * read.c -- tokenizer and reader.
 */

#include "mino_internal.h"

/* ------------------------------------------------------------------------- */
/* Reader                                                                    */
/* ------------------------------------------------------------------------- */

#define MAX_INTERNED_FILES 64

const char *intern_filename(const char *name)
{
    static const char *files[MAX_INTERNED_FILES];
    static size_t      file_count = 0;
    size_t i;
    if (name == NULL) {
        return NULL;
    }
    for (i = 0; i < file_count; i++) {
        if (strcmp(files[i], name) == 0) {
            return files[i];
        }
    }
    if (file_count < MAX_INTERNED_FILES) {
        size_t len = strlen(name);
        char  *dup = (char *)malloc(len + 1);
        if (dup == NULL) {
            return name;
        }
        memcpy(dup, name, len + 1);
        files[file_count++] = dup;
        return dup;
    }
    return name;
}

static int is_ws(char c)
{
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == ',';
}

static int is_terminator(char c)
{
    return c == '\0' || c == '(' || c == ')' || c == '[' || c == ']'
        || c == '{' || c == '}' || c == '\'' || c == '"' || c == ';'
        || c == '`'  || c == '~' || c == '@'
        || is_ws(c);
}

static void skip_ws(const char **p)
{
    while (**p) {
        char c = **p;
        if (c == '\n') {
            reader_line++;
            (*p)++;
        } else if (is_ws(c)) {
            (*p)++;
        } else if (c == ';') {
            while (**p && **p != '\n') {
                (*p)++;
            }
        } else {
            return;
        }
    }
}

static mino_val_t *read_form(const char **p);

static mino_val_t *read_string_form(const char **p)
{
    /* Caller has positioned *p on the opening '"'. */
    char *buf;
    size_t cap = 16;
    size_t len = 0;
    (*p)++; /* skip opening quote */
    buf = (char *)malloc(cap);
    if (buf == NULL) {
        abort();
    }
    while (**p && **p != '"') {
        char c = **p;
        if (c == '\n') {
            reader_line++;
        }
        if (c == '\\') {
            (*p)++;
            switch (**p) {
            case 'n':  c = '\n'; break;
            case 't':  c = '\t'; break;
            case 'r':  c = '\r'; break;
            case '\\': c = '\\'; break;
            case '"':  c = '"';  break;
            case '0':  c = '\0'; break;
            case '\0':
                free(buf);
                set_error("unterminated string literal");
                return NULL;
            default:
                /* Unknown escape: keep the character literally. */
                c = **p;
                break;
            }
        }
        if (len + 1 >= cap) {
            cap *= 2;
            buf = (char *)realloc(buf, cap);
            if (buf == NULL) {
                abort();
            }
        }
        buf[len++] = c;
        (*p)++;
    }
    if (**p != '"') {
        free(buf);
        set_error("unterminated string literal");
        return NULL;
    }
    (*p)++; /* skip closing quote */
    {
        mino_val_t *v = mino_string_n(S_, buf, len);
        free(buf);
        return v;
    }
}

static mino_val_t *read_list_form(const char **p)
{
    /* Caller has positioned *p on the opening '('. */
    int         list_line = reader_line;
    mino_val_t *head = mino_nil(S_);
    mino_val_t *tail = NULL;
    (*p)++; /* skip '(' */
    for (;;) {
        skip_ws(p);
        if (**p == '\0') {
            set_error("unterminated list");
            return NULL;
        }
        if (**p == ')') {
            (*p)++;
            return head;
        }
        {
            int         elem_line = reader_line;
            mino_val_t *elem = read_form(p);
            if (elem == NULL && mino_last_error(S_) != NULL) {
                return NULL;
            }
            if (elem == NULL) {
                /* EOF mid-list */
                set_error("unterminated list");
                return NULL;
            }
            {
                mino_val_t *cell = mino_cons(S_, elem, mino_nil(S_));
                cell->as.cons.file = reader_file;
                cell->as.cons.line = (tail == NULL) ? list_line : elem_line;
                if (tail == NULL) {
                    head = cell;
                } else {
                    tail->as.cons.cdr = cell;
                }
                tail = cell;
            }
        }
    }
}

static mino_val_t *read_vector_form(const char **p)
{
    /* Caller has positioned *p on the opening '['. `buf` accumulates the
     * partially-built element list and is tracked by the GC so intermediate
     * allocations inside nested read_form calls don't collect the entries
     * that have already been parsed. */
    mino_val_t **buf = NULL;
    size_t       cap = 0;
    size_t       len = 0;
    (*p)++; /* skip '[' */
    for (;;) {
        skip_ws(p);
        if (**p == '\0') {
            set_error("unterminated vector");
            return NULL;
        }
        if (**p == ']') {
            (*p)++;
            break;
        }
        {
            mino_val_t *elem = read_form(p);
            if (elem == NULL) {
                if (mino_last_error(S_) == NULL) {
                    set_error("unterminated vector");
                }
                return NULL;
            }
            if (len == cap) {
                size_t       new_cap = cap == 0 ? 8 : cap * 2;
                mino_val_t **nb      = (mino_val_t **)gc_alloc_typed(
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
    return mino_vector(S_, buf, len);
}

static mino_val_t *read_map_form(const char **p)
{
    /* Caller has positioned *p on the opening '{'. Elements alternate as
     * key, value, key, value. An odd count is a parse error. The key and
     * value buffers are GC-tracked so parsed entries survive allocations
     * performed by later nested read_form calls. */
    mino_val_t **kbuf = NULL;
    mino_val_t **vbuf = NULL;
    size_t       cap  = 0;
    size_t       len  = 0;
    (*p)++; /* skip '{' */
    for (;;) {
        mino_val_t *key;
        mino_val_t *val;
        skip_ws(p);
        if (**p == '\0') {
            set_error("unterminated map");
            return NULL;
        }
        if (**p == '}') {
            (*p)++;
            break;
        }
        key = read_form(p);
        if (key == NULL) {
            if (mino_last_error(S_) == NULL) {
                set_error("unterminated map");
            }
            return NULL;
        }
        skip_ws(p);
        if (**p == '}' || **p == '\0') {
            set_error("map literal has odd number of forms");
            return NULL;
        }
        val = read_form(p);
        if (val == NULL) {
            if (mino_last_error(S_) == NULL) {
                set_error("unterminated map");
            }
            return NULL;
        }
        if (len == cap) {
            size_t       new_cap = cap == 0 ? 8 : cap * 2;
            mino_val_t **nk      = (mino_val_t **)gc_alloc_typed(
                GC_T_VALARR, new_cap * sizeof(*nk));
            mino_val_t **nv      = (mino_val_t **)gc_alloc_typed(
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
    return mino_map(S_, kbuf, vbuf, len);
}

static mino_val_t *read_set_form(const char **p)
{
    /* Caller has positioned *p on the opening '{' after '#'. */
    mino_val_t **buf = NULL;
    size_t       cap = 0;
    size_t       len = 0;
    (*p)++; /* skip '{' */
    for (;;) {
        mino_val_t *elem;
        skip_ws(p);
        if (**p == '\0') {
            set_error("unterminated set");
            return NULL;
        }
        if (**p == '}') {
            (*p)++;
            break;
        }
        elem = read_form(p);
        if (elem == NULL) {
            if (mino_last_error(S_) == NULL) {
                set_error("unterminated set");
            }
            return NULL;
        }
        if (len == cap) {
            size_t       new_cap = cap == 0 ? 8 : cap * 2;
            mino_val_t **nb      = (mino_val_t **)gc_alloc_typed(
                GC_T_VALARR, new_cap * sizeof(*nb));
            if (buf != NULL && len > 0) {
                memcpy(nb, buf, len * sizeof(*nb));
            }
            buf = nb;
            cap = new_cap;
        }
        buf[len++] = elem;
    }
    return mino_set(S_, buf, len);
}

static mino_val_t *read_atom(const char **p)
{
    const char *start = *p;
    size_t len = 0;
    while (!is_terminator((*p)[len])) {
        len++;
    }
    *p += len;

    if (len >= 2 && start[0] == ':') {
        return mino_keyword_n(S_, start + 1, len - 1);
    }
    if (len == 1 && start[0] == ':') {
        set_error("keyword missing name");
        return NULL;
    }

    if (len == 3 && memcmp(start, "nil", 3) == 0) {
        return mino_nil(S_);
    }
    if (len == 4 && memcmp(start, "true", 4) == 0) {
        return mino_true(S_);
    }
    if (len == 5 && memcmp(start, "false", 5) == 0) {
        return mino_false(S_);
    }

    /* Try numeric. */
    {
        char buf[64];
        char *endp = NULL;
        if (len < sizeof(buf)) {
            int has_dot_or_exp = 0;
            int looks_numeric = 1;
            size_t i = 0;
            size_t scan_start = 0;
            memcpy(buf, start, len);
            buf[len] = '\0';
            if (buf[0] == '+' || buf[0] == '-') {
                scan_start = 1;
            }
            if (scan_start == len) {
                looks_numeric = 0;
            }
            for (i = scan_start; i < len; i++) {
                char c = buf[i];
                if (c == '.' || c == 'e' || c == 'E') {
                    has_dot_or_exp = 1;
                } else if (!isdigit((unsigned char)c)) {
                    looks_numeric = 0;
                    break;
                }
            }
            if (looks_numeric) {
                if (has_dot_or_exp) {
                    double d = strtod(buf, &endp);
                    if (endp == buf + len) {
                        return mino_float(S_, d);
                    }
                } else {
                    long long n = strtoll(buf, &endp, 10);
                    if (endp == buf + len) {
                        return mino_int(S_, n);
                    }
                }
            }
        }
    }

    return mino_symbol_n(S_, start, len);
}

static mino_val_t *read_form(const char **p)
{
    skip_ws(p);
    if (**p == '\0') {
        return NULL;
    }
    if (**p == '(') {
        return read_list_form(p);
    }
    if (**p == ')') {
        set_error("unexpected ')'");
        return NULL;
    }
    if (**p == '[') {
        return read_vector_form(p);
    }
    if (**p == ']') {
        set_error("unexpected ']'");
        return NULL;
    }
    if (**p == '{') {
        return read_map_form(p);
    }
    if (**p == '}') {
        set_error("unexpected '}'");
        return NULL;
    }
    if (**p == '#' && *(*p + 1) == '{') {
        (*p)++; /* skip '#', read_set_form will skip '{' */
        return read_set_form(p);
    }
    if (**p == '"') {
        return read_string_form(p);
    }
    if (**p == '\'') {
        int q_line = reader_line;
        (*p)++;
        {
            mino_val_t *quoted = read_form(p);
            mino_val_t *outer;
            if (quoted == NULL) {
                if (mino_last_error(S_) == NULL) {
                    set_error("expected form after quote");
                }
                return NULL;
            }
            outer = mino_cons(S_, mino_symbol(S_, "quote"),
                              mino_cons(S_, quoted, mino_nil(S_)));
            outer->as.cons.file = reader_file;
            outer->as.cons.line = q_line;
            return outer;
        }
    }
    if (**p == '`') {
        int q_line = reader_line;
        (*p)++;
        {
            mino_val_t *qq = read_form(p);
            mino_val_t *outer;
            if (qq == NULL) {
                if (mino_last_error(S_) == NULL) {
                    set_error("expected form after `");
                }
                return NULL;
            }
            outer = mino_cons(S_, mino_symbol(S_, "quasiquote"),
                              mino_cons(S_, qq, mino_nil(S_)));
            outer->as.cons.file = reader_file;
            outer->as.cons.line = q_line;
            return outer;
        }
    }
    if (**p == '@') {
        int q_line = reader_line;
        (*p)++;
        {
            mino_val_t *target = read_form(p);
            mino_val_t *outer;
            if (target == NULL) {
                if (mino_last_error(S_) == NULL) {
                    set_error("expected form after @");
                }
                return NULL;
            }
            outer = mino_cons(S_, mino_symbol(S_, "deref"),
                              mino_cons(S_, target, mino_nil(S_)));
            outer->as.cons.file = reader_file;
            outer->as.cons.line = q_line;
            return outer;
        }
    }
    if (**p == '~') {
        int         q_line = reader_line;
        const char *name = "unquote";
        (*p)++;
        if (**p == '@') {
            name = "unquote-splicing";
            (*p)++;
        }
        {
            mino_val_t *uq = read_form(p);
            mino_val_t *outer;
            if (uq == NULL) {
                if (mino_last_error(S_) == NULL) {
                    set_error("expected form after ~");
                }
                return NULL;
            }
            outer = mino_cons(S_, mino_symbol(S_, name),
                              mino_cons(S_, uq, mino_nil(S_)));
            outer->as.cons.file = reader_file;
            outer->as.cons.line = q_line;
            return outer;
        }
    }
    return read_atom(p);
}

mino_val_t *mino_read(mino_state_t *S, const char *src, const char **end)
{
    S_ = S;
    volatile char probe = 0;
    const char   *p = src;
    mino_val_t   *v;
    /* Record this frame as a host-level stack bottom so the collector's
     * conservative scan covers the reader's call chain in full. */
    gc_note_host_frame((void *)&probe);
    (void)probe;
    if (reader_file == NULL) {
        reader_file = intern_filename("<input>");
    }
    clear_error();
    v = read_form(&p);
    if (end != NULL) {
        *end = p;
    }
    return v;
}

