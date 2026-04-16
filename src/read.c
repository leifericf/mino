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

static void skip_ws(mino_state_t *S, const char **p)
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

static mino_val_t *read_form(mino_state_t *S, const char **p);

static mino_val_t *read_string_form(mino_state_t *S, const char **p)
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
                set_error(S, "unterminated string literal");
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
        set_error(S, "unterminated string literal");
        return NULL;
    }
    (*p)++; /* skip closing quote */
    {
        mino_val_t *v = mino_string_n(S, buf, len);
        free(buf);
        return v;
    }
}

static mino_val_t *read_list_form(mino_state_t *S, const char **p)
{
    /* Caller has positioned *p on the opening '('. */
    int         list_line = reader_line;
    mino_val_t *head = mino_nil(S);
    mino_val_t *tail = NULL;
    (*p)++; /* skip '(' */
    for (;;) {
        skip_ws(S, p);
        if (**p == '\0') {
            set_error(S, "unterminated list");
            return NULL;
        }
        if (**p == ')') {
            (*p)++;
            return head;
        }
        {
            int         elem_line = reader_line;
            mino_val_t *elem = read_form(S, p);
            if (elem == NULL && mino_last_error(S) != NULL) {
                return NULL;
            }
            if (elem == NULL) {
                /* EOF mid-list */
                set_error(S, "unterminated list");
                return NULL;
            }
            {
                mino_val_t *cell = mino_cons(S, elem, mino_nil(S));
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

static mino_val_t *read_vector_form(mino_state_t *S, const char **p)
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
        skip_ws(S, p);
        if (**p == '\0') {
            set_error(S, "unterminated vector");
            return NULL;
        }
        if (**p == ']') {
            (*p)++;
            break;
        }
        {
            mino_val_t *elem = read_form(S, p);
            if (elem == NULL) {
                if (mino_last_error(S) == NULL) {
                    set_error(S, "unterminated vector");
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
    (*p)++; /* skip '{' */
    for (;;) {
        mino_val_t *key;
        mino_val_t *val;
        skip_ws(S, p);
        if (**p == '\0') {
            set_error(S, "unterminated map");
            return NULL;
        }
        if (**p == '}') {
            (*p)++;
            break;
        }
        key = read_form(S, p);
        if (key == NULL) {
            if (mino_last_error(S) == NULL) {
                set_error(S, "unterminated map");
            }
            return NULL;
        }
        skip_ws(S, p);
        if (**p == '}' || **p == '\0') {
            set_error(S, "map literal has odd number of forms");
            return NULL;
        }
        val = read_form(S, p);
        if (val == NULL) {
            if (mino_last_error(S) == NULL) {
                set_error(S, "unterminated map");
            }
            return NULL;
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
    (*p)++; /* skip '{' */
    for (;;) {
        mino_val_t *elem;
        skip_ws(S, p);
        if (**p == '\0') {
            set_error(S, "unterminated set");
            return NULL;
        }
        if (**p == '}') {
            (*p)++;
            break;
        }
        elem = read_form(S, p);
        if (elem == NULL) {
            if (mino_last_error(S) == NULL) {
                set_error(S, "unterminated set");
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
    *p += len;

    if (len >= 2 && start[0] == ':') {
        return mino_keyword_n(S, start + 1, len - 1);
    }
    if (len == 1 && start[0] == ':') {
        set_error(S, "keyword missing name");
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
                        return mino_float(S, d);
                    }
                } else {
                    long long n = strtoll(buf, &endp, 10);
                    if (endp == buf + len) {
                        return mino_int(S, n);
                    }
                }
            }
        }
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
            c->as.cons.file = form->as.cons.file;
            c->as.cons.line = form->as.cons.line;
            return c;
        }
    }
    if (form->type == MINO_VECTOR) {
        size_t      i;
        int         changed = 0;
        mino_val_t *items[64]; /* plenty for fn bodies */
        size_t      len = form->as.vec.len;
        if (len > 64) return form;
        for (i = 0; i < len; i++) {
            items[i] = normalize_percent(S, vec_nth(form, i));
            if (items[i] != vec_nth(form, i)) changed = 1;
        }
        return changed ? mino_vector(S, items, len) : form;
    }
    return form;
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
        set_error(S, "unexpected ')'");
        return NULL;
    }
    if (**p == '[') {
        return read_vector_form(S, p);
    }
    if (**p == ']') {
        set_error(S, "unexpected ']'");
        return NULL;
    }
    if (**p == '{') {
        return read_map_form(S, p);
    }
    if (**p == '}') {
        set_error(S, "unexpected '}'");
        return NULL;
    }
    if (**p == '#' && *(*p + 1) == '{') {
        (*p)++; /* skip '#', read_set_form will skip '{' */
        return read_set_form(S, p);
    }
    if (**p == '#' && *(*p + 1) == '_') {
        /* Discard reader macro: #_ discards the next form. */
        (*p) += 2;
        {
            mino_val_t *discarded = read_form(S, p);
            if (discarded == NULL && mino_last_error(S) != NULL)
                return NULL;
            return read_form(S, p);
        }
    }
    if (**p == '#' && *(*p + 1) == '(') {
        /* Anonymous function shorthand: #(inc %) => (fn [%1] (inc %1)) */
        int fn_line = reader_line;
        int used[9] = {0};
        int max_arg = 0, has_rest = 0;
        mino_val_t *body;
        mino_val_t *params_vec;
        mino_val_t *fn_form;
        (*p)++; /* skip '#', read_list_form will handle '(' */
        body = read_list_form(S, p);
        if (body == NULL) return NULL;
        /* Scan for % arg slots. */
        scan_percent_args(body, used, &max_arg, &has_rest);
        /* Normalize bare % to %1. */
        body = normalize_percent(S, body);
        /* Build params vector: [%1 %2 ... & %&] */
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
        /* Build (fn [params] (body...)) — body is already a list form. */
        fn_form = mino_cons(S, mino_symbol(S, "fn"),
                      mino_cons(S, params_vec,
                          mino_cons(S, body, mino_nil(S))));
        fn_form->as.cons.file = reader_file;
        fn_form->as.cons.line = fn_line;
        return fn_form;
    }
    if (**p == '"') {
        return read_string_form(S, p);
    }
    if (**p == '\'') {
        int q_line = reader_line;
        (*p)++;
        {
            mino_val_t *quoted = read_form(S, p);
            mino_val_t *outer;
            if (quoted == NULL) {
                if (mino_last_error(S) == NULL) {
                    set_error(S, "expected form after quote");
                }
                return NULL;
            }
            outer = mino_cons(S, mino_symbol(S, "quote"),
                              mino_cons(S, quoted, mino_nil(S)));
            outer->as.cons.file = reader_file;
            outer->as.cons.line = q_line;
            return outer;
        }
    }
    if (**p == '`') {
        int q_line = reader_line;
        (*p)++;
        {
            mino_val_t *qq = read_form(S, p);
            mino_val_t *outer;
            if (qq == NULL) {
                if (mino_last_error(S) == NULL) {
                    set_error(S, "expected form after `");
                }
                return NULL;
            }
            outer = mino_cons(S, mino_symbol(S, "quasiquote"),
                              mino_cons(S, qq, mino_nil(S)));
            outer->as.cons.file = reader_file;
            outer->as.cons.line = q_line;
            return outer;
        }
    }
    if (**p == '@') {
        int q_line = reader_line;
        (*p)++;
        {
            mino_val_t *target = read_form(S, p);
            mino_val_t *outer;
            if (target == NULL) {
                if (mino_last_error(S) == NULL) {
                    set_error(S, "expected form after @");
                }
                return NULL;
            }
            outer = mino_cons(S, mino_symbol(S, "deref"),
                              mino_cons(S, target, mino_nil(S)));
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
            mino_val_t *uq = read_form(S, p);
            mino_val_t *outer;
            if (uq == NULL) {
                if (mino_last_error(S) == NULL) {
                    set_error(S, "expected form after ~");
                }
                return NULL;
            }
            outer = mino_cons(S, mino_symbol(S, name),
                              mino_cons(S, uq, mino_nil(S)));
            outer->as.cons.file = reader_file;
            outer->as.cons.line = q_line;
            return outer;
        }
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
    if (reader_file == NULL) {
        reader_file = intern_filename("<input>");
    }
    clear_error(S);
    v = read_form(S, &p);
    if (end != NULL) {
        *end = p;
    }
    return v;
}

