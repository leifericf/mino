/*
 * mino.c — runtime implementation.
 *
 * Single-file amalgamation: this translation unit, paired with mino.h,
 * is the entire runtime. ANSI C, no external dependencies.
 */

#include "mino.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------------------- */
/* Allocation                                                                */
/* ------------------------------------------------------------------------- */

/*
 * v0.1 uses a simple per-allocation malloc with no reclamation. A real
 * collector arrives later in the roadmap; the API treats values as opaque
 * handles so the representation can change without breaking embedders.
 */
static mino_val_t *alloc_val(mino_type_t type)
{
    mino_val_t *v = (mino_val_t *)calloc(1, sizeof(*v));
    if (v == NULL) {
        /* Fatal: out of memory during construction. v0.1 has no recovery. */
        abort();
    }
    v->type = type;
    return v;
}

static char *dup_n(const char *s, size_t len)
{
    char *out = (char *)malloc(len + 1);
    if (out == NULL) {
        abort();
    }
    if (len > 0) {
        memcpy(out, s, len);
    }
    out[len] = '\0';
    return out;
}

/* ------------------------------------------------------------------------- */
/* Singletons                                                                */
/* ------------------------------------------------------------------------- */

static mino_val_t nil_singleton  = { MINO_NIL,  { 0 } };
static mino_val_t true_singleton  = { MINO_BOOL, { 1 } };
static mino_val_t false_singleton = { MINO_BOOL, { 0 } };

mino_val_t *mino_nil(void)   { return &nil_singleton; }
mino_val_t *mino_true(void)  { return &true_singleton; }
mino_val_t *mino_false(void) { return &false_singleton; }

/* ------------------------------------------------------------------------- */
/* Constructors                                                              */
/* ------------------------------------------------------------------------- */

mino_val_t *mino_int(long long n)
{
    mino_val_t *v = alloc_val(MINO_INT);
    v->as.i = n;
    return v;
}

mino_val_t *mino_float(double f)
{
    mino_val_t *v = alloc_val(MINO_FLOAT);
    v->as.f = f;
    return v;
}

mino_val_t *mino_string_n(const char *s, size_t len)
{
    mino_val_t *v = alloc_val(MINO_STRING);
    v->as.s.data = dup_n(s, len);
    v->as.s.len  = len;
    return v;
}

mino_val_t *mino_string(const char *s)
{
    return mino_string_n(s, strlen(s));
}

mino_val_t *mino_symbol_n(const char *s, size_t len)
{
    mino_val_t *v = alloc_val(MINO_SYMBOL);
    v->as.s.data = dup_n(s, len);
    v->as.s.len  = len;
    return v;
}

mino_val_t *mino_symbol(const char *s)
{
    return mino_symbol_n(s, strlen(s));
}

mino_val_t *mino_cons(mino_val_t *car, mino_val_t *cdr)
{
    mino_val_t *v = alloc_val(MINO_CONS);
    v->as.cons.car = car;
    v->as.cons.cdr = cdr;
    return v;
}

mino_val_t *mino_prim(const char *name, mino_prim_fn fn)
{
    mino_val_t *v = alloc_val(MINO_PRIM);
    v->as.prim.name = name;
    v->as.prim.fn   = fn;
    return v;
}

/* ------------------------------------------------------------------------- */
/* Predicates and accessors                                                  */
/* ------------------------------------------------------------------------- */

int mino_is_nil(const mino_val_t *v)
{
    return v == NULL || v->type == MINO_NIL;
}

int mino_is_truthy(const mino_val_t *v)
{
    if (v == NULL) {
        return 0;
    }
    if (v->type == MINO_NIL) {
        return 0;
    }
    if (v->type == MINO_BOOL) {
        return v->as.b != 0;
    }
    return 1;
}

int mino_is_cons(const mino_val_t *v)
{
    return v != NULL && v->type == MINO_CONS;
}

mino_val_t *mino_car(const mino_val_t *v)
{
    if (!mino_is_cons(v)) {
        return mino_nil();
    }
    return v->as.cons.car;
}

mino_val_t *mino_cdr(const mino_val_t *v)
{
    if (!mino_is_cons(v)) {
        return mino_nil();
    }
    return v->as.cons.cdr;
}

size_t mino_length(const mino_val_t *list)
{
    size_t n = 0;
    while (mino_is_cons(list)) {
        n++;
        list = list->as.cons.cdr;
    }
    return n;
}

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

void mino_print_to(FILE *out, const mino_val_t *v)
{
    if (v == NULL || v->type == MINO_NIL) {
        fputs("nil", out);
        return;
    }
    switch (v->type) {
    case MINO_NIL:
        fputs("nil", out);
        return;
    case MINO_BOOL:
        fputs(v->as.b ? "true" : "false", out);
        return;
    case MINO_INT:
        fprintf(out, "%lld", v->as.i);
        return;
    case MINO_FLOAT: {
        /*
         * Always include a decimal point so the printed form re-reads as a
         * float, not an int. %g may drop the dot for whole numbers.
         */
        char buf[64];
        int n = snprintf(buf, sizeof(buf), "%g", v->as.f);
        int needs_dot = 1;
        int i;
        if (n < 0) {
            fputs("nan", out);
            return;
        }
        for (i = 0; i < n; i++) {
            if (buf[i] == '.' || buf[i] == 'e' || buf[i] == 'E'
                || buf[i] == 'n' || buf[i] == 'i') {
                needs_dot = 0;
                break;
            }
        }
        fputs(buf, out);
        if (needs_dot) {
            fputs(".0", out);
        }
        return;
    }
    case MINO_STRING:
        print_string_escaped(out, v->as.s.data, v->as.s.len);
        return;
    case MINO_SYMBOL:
        fwrite(v->as.s.data, 1, v->as.s.len, out);
        return;
    case MINO_CONS: {
        const mino_val_t *p = v;
        fputc('(', out);
        while (p != NULL && p->type == MINO_CONS) {
            mino_print_to(out, p->as.cons.car);
            p = p->as.cons.cdr;
            if (p != NULL && p->type == MINO_CONS) {
                fputc(' ', out);
            } else if (p != NULL && p->type != MINO_NIL) {
                fputs(" . ", out);
                mino_print_to(out, p);
                break;
            }
        }
        fputc(')', out);
        return;
    }
    case MINO_PRIM:
        fputs("#<prim ", out);
        if (v->as.prim.name != NULL) {
            fputs(v->as.prim.name, out);
        }
        fputc('>', out);
        return;
    }
}

void mino_print(const mino_val_t *v)
{
    mino_print_to(stdout, v);
}

void mino_println(const mino_val_t *v)
{
    mino_print_to(stdout, v);
    fputc('\n', stdout);
}

/* ------------------------------------------------------------------------- */
/* Error reporting                                                           */
/* ------------------------------------------------------------------------- */

static char error_buf[256] = { 0 };

const char *mino_last_error(void)
{
    return error_buf[0] ? error_buf : NULL;
}

static void set_error(const char *msg)
{
    size_t n = strlen(msg);
    if (n >= sizeof(error_buf)) {
        n = sizeof(error_buf) - 1;
    }
    memcpy(error_buf, msg, n);
    error_buf[n] = '\0';
}

static void clear_error(void)
{
    error_buf[0] = '\0';
}

/* ------------------------------------------------------------------------- */
/* Reader                                                                    */
/* ------------------------------------------------------------------------- */

static int is_ws(char c)
{
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == ',';
}

static int is_terminator(char c)
{
    return c == '\0' || c == '(' || c == ')' || c == '\'' || c == '"'
        || c == ';' || is_ws(c);
}

static void skip_ws(const char **p)
{
    while (**p) {
        char c = **p;
        if (is_ws(c)) {
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
        mino_val_t *v = mino_string_n(buf, len);
        free(buf);
        return v;
    }
}

static mino_val_t *read_list_form(const char **p)
{
    /* Caller has positioned *p on the opening '('. */
    mino_val_t *head = mino_nil();
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
            mino_val_t *elem = read_form(p);
            if (elem == NULL && mino_last_error() != NULL) {
                return NULL;
            }
            if (elem == NULL) {
                /* EOF mid-list */
                set_error("unterminated list");
                return NULL;
            }
            {
                mino_val_t *cell = mino_cons(elem, mino_nil());
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

static mino_val_t *read_atom(const char **p)
{
    const char *start = *p;
    size_t len = 0;
    while (!is_terminator((*p)[len])) {
        len++;
    }
    *p += len;

    if (len == 3 && memcmp(start, "nil", 3) == 0) {
        return mino_nil();
    }
    if (len == 4 && memcmp(start, "true", 4) == 0) {
        return mino_true();
    }
    if (len == 5 && memcmp(start, "false", 5) == 0) {
        return mino_false();
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
                        return mino_float(d);
                    }
                } else {
                    long long n = strtoll(buf, &endp, 10);
                    if (endp == buf + len) {
                        return mino_int(n);
                    }
                }
            }
        }
    }

    return mino_symbol_n(start, len);
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
    if (**p == '"') {
        return read_string_form(p);
    }
    if (**p == '\'') {
        (*p)++;
        {
            mino_val_t *quoted = read_form(p);
            if (quoted == NULL) {
                if (mino_last_error() == NULL) {
                    set_error("expected form after quote");
                }
                return NULL;
            }
            return mino_cons(mino_symbol("quote"),
                             mino_cons(quoted, mino_nil()));
        }
    }
    return read_atom(p);
}

mino_val_t *mino_read(const char *src, const char **end)
{
    const char *p = src;
    mino_val_t *v;
    clear_error();
    v = read_form(&p);
    if (end != NULL) {
        *end = p;
    }
    return v;
}

/* ------------------------------------------------------------------------- */
/* Equality                                                                  */
/* ------------------------------------------------------------------------- */

int mino_eq(const mino_val_t *a, const mino_val_t *b)
{
    if (a == b) {
        return 1;
    }
    if (a == NULL || b == NULL) {
        return 0;
    }
    if (a->type != b->type) {
        /*
         * Cross-type numeric equality: int and float compare by value.
         */
        if (a->type == MINO_INT && b->type == MINO_FLOAT) {
            return (double)a->as.i == b->as.f;
        }
        if (a->type == MINO_FLOAT && b->type == MINO_INT) {
            return a->as.f == (double)b->as.i;
        }
        return 0;
    }
    switch (a->type) {
    case MINO_NIL:
        return 1;
    case MINO_BOOL:
        return a->as.b == b->as.b;
    case MINO_INT:
        return a->as.i == b->as.i;
    case MINO_FLOAT:
        return a->as.f == b->as.f;
    case MINO_STRING:
    case MINO_SYMBOL:
        return a->as.s.len == b->as.s.len
            && memcmp(a->as.s.data, b->as.s.data, a->as.s.len) == 0;
    case MINO_CONS:
        return mino_eq(a->as.cons.car, b->as.cons.car)
            && mino_eq(a->as.cons.cdr, b->as.cons.cdr);
    case MINO_PRIM:
        return a->as.prim.fn == b->as.prim.fn;
    }
    return 0;
}

/* ------------------------------------------------------------------------- */
/* Environment                                                               */
/* ------------------------------------------------------------------------- */

/*
 * Environment: a chain of frames. Each frame is a flat (name, value) array
 * with linear search. The root frame has parent == NULL and holds globals;
 * child frames are created by let, fn application, and loop. Lookup walks
 * parents; binding always writes to the current frame so that let and fn
 * parameters shadow rather than mutate outer bindings.
 *
 * Frames created for locals are currently never freed — v0.7 introduces a
 * tracing collector that owns environment lifetimes. Only the root frame
 * passed to mino_env_free is reclaimed.
 */

typedef struct {
    char       *name;
    mino_val_t *val;
} env_binding_t;

struct mino_env {
    env_binding_t *bindings;
    size_t         len;
    size_t         cap;
    mino_env_t    *parent;
};

static mino_env_t *env_alloc(mino_env_t *parent)
{
    mino_env_t *env = (mino_env_t *)calloc(1, sizeof(*env));
    if (env == NULL) {
        abort();
    }
    env->parent = parent;
    return env;
}

mino_env_t *mino_env_new(void)
{
    return env_alloc(NULL);
}

void mino_env_free(mino_env_t *env)
{
    size_t i;
    if (env == NULL) {
        return;
    }
    for (i = 0; i < env->len; i++) {
        free(env->bindings[i].name);
    }
    free(env->bindings);
    free(env);
}

static env_binding_t *env_find_here(mino_env_t *env, const char *name)
{
    size_t i;
    for (i = 0; i < env->len; i++) {
        if (strcmp(env->bindings[i].name, name) == 0) {
            return &env->bindings[i];
        }
    }
    return NULL;
}

static void env_bind(mino_env_t *env, const char *name, mino_val_t *val)
{
    env_binding_t *b = env_find_here(env, name);
    if (b != NULL) {
        b->val = val;
        return;
    }
    if (env->len == env->cap) {
        size_t new_cap = env->cap == 0 ? 16 : env->cap * 2;
        env_binding_t *nb = (env_binding_t *)realloc(
            env->bindings, new_cap * sizeof(*nb));
        if (nb == NULL) {
            abort();
        }
        env->bindings = nb;
        env->cap = new_cap;
    }
    env->bindings[env->len].name = dup_n(name, strlen(name));
    env->bindings[env->len].val  = val;
    env->len++;
}

static mino_env_t *env_root(mino_env_t *env)
{
    while (env->parent != NULL) {
        env = env->parent;
    }
    return env;
}

void mino_env_set(mino_env_t *env, const char *name, mino_val_t *val)
{
    env_bind(env, name, val);
}

mino_val_t *mino_env_get(mino_env_t *env, const char *name)
{
    while (env != NULL) {
        env_binding_t *b = env_find_here(env, name);
        if (b != NULL) {
            return b->val;
        }
        env = env->parent;
    }
    return NULL;
}

/* ------------------------------------------------------------------------- */
/* Evaluator                                                                 */
/* ------------------------------------------------------------------------- */

static int sym_eq(const mino_val_t *v, const char *s)
{
    size_t n;
    if (v == NULL || v->type != MINO_SYMBOL) {
        return 0;
    }
    n = strlen(s);
    return v->as.s.len == n && memcmp(v->as.s.data, s, n) == 0;
}

static mino_val_t *eval_args(mino_val_t *args, mino_env_t *env)
{
    mino_val_t *head = mino_nil();
    mino_val_t *tail = NULL;
    while (mino_is_cons(args)) {
        mino_val_t *v = mino_eval(args->as.cons.car, env);
        mino_val_t *cell;
        if (v == NULL) {
            return NULL;
        }
        cell = mino_cons(v, mino_nil());
        if (tail == NULL) {
            head = cell;
        } else {
            tail->as.cons.cdr = cell;
        }
        tail = cell;
        args = args->as.cons.cdr;
    }
    return head;
}

mino_val_t *mino_eval(mino_val_t *form, mino_env_t *env)
{
    if (form == NULL) {
        return mino_nil();
    }
    switch (form->type) {
    case MINO_NIL:
    case MINO_BOOL:
    case MINO_INT:
    case MINO_FLOAT:
    case MINO_STRING:
    case MINO_PRIM:
        return form;
    case MINO_SYMBOL: {
        char buf[256];
        size_t n = form->as.s.len;
        mino_val_t *v;
        if (n >= sizeof(buf)) {
            set_error("symbol name too long");
            return NULL;
        }
        memcpy(buf, form->as.s.data, n);
        buf[n] = '\0';
        v = mino_env_get(env, buf);
        if (v == NULL) {
            char msg[300];
            snprintf(msg, sizeof(msg), "unbound symbol: %s", buf);
            set_error(msg);
            return NULL;
        }
        return v;
    }
    case MINO_CONS: {
        mino_val_t *head = form->as.cons.car;
        mino_val_t *args = form->as.cons.cdr;

        /* Special forms. */
        if (sym_eq(head, "quote")) {
            if (!mino_is_cons(args)) {
                set_error("quote requires one argument");
                return NULL;
            }
            return args->as.cons.car;
        }
        if (sym_eq(head, "def")) {
            mino_val_t *name_form;
            mino_val_t *value_form;
            mino_val_t *value;
            char buf[256];
            size_t n;
            if (!mino_is_cons(args) || !mino_is_cons(args->as.cons.cdr)) {
                set_error("def requires a name and a value");
                return NULL;
            }
            name_form  = args->as.cons.car;
            value_form = args->as.cons.cdr->as.cons.car;
            if (name_form == NULL || name_form->type != MINO_SYMBOL) {
                set_error("def name must be a symbol");
                return NULL;
            }
            n = name_form->as.s.len;
            if (n >= sizeof(buf)) {
                set_error("def name too long");
                return NULL;
            }
            memcpy(buf, name_form->as.s.data, n);
            buf[n] = '\0';
            value = mino_eval(value_form, env);
            if (value == NULL) {
                return NULL;
            }
            env_bind(env_root(env), buf, value);
            return value;
        }

        /* Function application. */
        {
            mino_val_t *fn = mino_eval(head, env);
            mino_val_t *evaled;
            if (fn == NULL) {
                return NULL;
            }
            if (fn->type != MINO_PRIM) {
                set_error("not a function");
                return NULL;
            }
            evaled = eval_args(args, env);
            if (evaled == NULL && mino_last_error() != NULL) {
                return NULL;
            }
            return fn->as.prim.fn(evaled, env);
        }
    }
    }
    set_error("eval: unknown value type");
    return NULL;
}

/* ------------------------------------------------------------------------- */
/* Core primitives                                                           */
/* ------------------------------------------------------------------------- */

/*
 * Numeric coercion: if any argument is a float, the result is a float.
 * Otherwise integer arithmetic is used end-to-end.
 */

static int args_have_float(mino_val_t *args)
{
    while (mino_is_cons(args)) {
        mino_val_t *a = args->as.cons.car;
        if (a != NULL && a->type == MINO_FLOAT) {
            return 1;
        }
        args = args->as.cons.cdr;
    }
    return 0;
}

static int as_double(const mino_val_t *v, double *out)
{
    if (v == NULL) {
        return 0;
    }
    if (v->type == MINO_INT) {
        *out = (double)v->as.i;
        return 1;
    }
    if (v->type == MINO_FLOAT) {
        *out = v->as.f;
        return 1;
    }
    return 0;
}

static int as_long(const mino_val_t *v, long long *out)
{
    if (v == NULL || v->type != MINO_INT) {
        return 0;
    }
    *out = v->as.i;
    return 1;
}

static mino_val_t *prim_add(mino_val_t *args, mino_env_t *env)
{
    (void)env;
    if (args_have_float(args)) {
        double acc = 0.0;
        while (mino_is_cons(args)) {
            double x;
            if (!as_double(args->as.cons.car, &x)) {
                set_error("+ expects numbers");
                return NULL;
            }
            acc += x;
            args = args->as.cons.cdr;
        }
        return mino_float(acc);
    } else {
        long long acc = 0;
        while (mino_is_cons(args)) {
            long long x;
            if (!as_long(args->as.cons.car, &x)) {
                set_error("+ expects numbers");
                return NULL;
            }
            acc += x;
            args = args->as.cons.cdr;
        }
        return mino_int(acc);
    }
}

static mino_val_t *prim_sub(mino_val_t *args, mino_env_t *env)
{
    (void)env;
    if (!mino_is_cons(args)) {
        set_error("- requires at least one argument");
        return NULL;
    }
    if (args_have_float(args)) {
        double acc;
        if (!as_double(args->as.cons.car, &acc)) {
            set_error("- expects numbers");
            return NULL;
        }
        args = args->as.cons.cdr;
        if (!mino_is_cons(args)) {
            return mino_float(-acc);
        }
        while (mino_is_cons(args)) {
            double x;
            if (!as_double(args->as.cons.car, &x)) {
                set_error("- expects numbers");
                return NULL;
            }
            acc -= x;
            args = args->as.cons.cdr;
        }
        return mino_float(acc);
    } else {
        long long acc;
        if (!as_long(args->as.cons.car, &acc)) {
            set_error("- expects numbers");
            return NULL;
        }
        args = args->as.cons.cdr;
        if (!mino_is_cons(args)) {
            return mino_int(-acc);
        }
        while (mino_is_cons(args)) {
            long long x;
            if (!as_long(args->as.cons.car, &x)) {
                set_error("- expects numbers");
                return NULL;
            }
            acc -= x;
            args = args->as.cons.cdr;
        }
        return mino_int(acc);
    }
}

static mino_val_t *prim_mul(mino_val_t *args, mino_env_t *env)
{
    (void)env;
    if (args_have_float(args)) {
        double acc = 1.0;
        while (mino_is_cons(args)) {
            double x;
            if (!as_double(args->as.cons.car, &x)) {
                set_error("* expects numbers");
                return NULL;
            }
            acc *= x;
            args = args->as.cons.cdr;
        }
        return mino_float(acc);
    } else {
        long long acc = 1;
        while (mino_is_cons(args)) {
            long long x;
            if (!as_long(args->as.cons.car, &x)) {
                set_error("* expects numbers");
                return NULL;
            }
            acc *= x;
            args = args->as.cons.cdr;
        }
        return mino_int(acc);
    }
}

static mino_val_t *prim_div(mino_val_t *args, mino_env_t *env)
{
    /* Division always yields a float result for now. */
    double acc;
    (void)env;
    if (!mino_is_cons(args)) {
        set_error("/ requires at least one argument");
        return NULL;
    }
    if (!as_double(args->as.cons.car, &acc)) {
        set_error("/ expects numbers");
        return NULL;
    }
    args = args->as.cons.cdr;
    if (!mino_is_cons(args)) {
        if (acc == 0.0) {
            set_error("division by zero");
            return NULL;
        }
        return mino_float(1.0 / acc);
    }
    while (mino_is_cons(args)) {
        double x;
        if (!as_double(args->as.cons.car, &x)) {
            set_error("/ expects numbers");
            return NULL;
        }
        if (x == 0.0) {
            set_error("division by zero");
            return NULL;
        }
        acc /= x;
        args = args->as.cons.cdr;
    }
    return mino_float(acc);
}

static mino_val_t *prim_eq(mino_val_t *args, mino_env_t *env)
{
    (void)env;
    if (!mino_is_cons(args)) {
        return mino_true();
    }
    {
        mino_val_t *first = args->as.cons.car;
        args = args->as.cons.cdr;
        while (mino_is_cons(args)) {
            if (!mino_eq(first, args->as.cons.car)) {
                return mino_false();
            }
            args = args->as.cons.cdr;
        }
    }
    return mino_true();
}

static mino_val_t *prim_lt(mino_val_t *args, mino_env_t *env)
{
    double prev;
    (void)env;
    if (!mino_is_cons(args)) {
        return mino_true();
    }
    if (!as_double(args->as.cons.car, &prev)) {
        set_error("< expects numbers");
        return NULL;
    }
    args = args->as.cons.cdr;
    while (mino_is_cons(args)) {
        double x;
        if (!as_double(args->as.cons.car, &x)) {
            set_error("< expects numbers");
            return NULL;
        }
        if (!(prev < x)) {
            return mino_false();
        }
        prev = x;
        args = args->as.cons.cdr;
    }
    return mino_true();
}

static mino_val_t *prim_car(mino_val_t *args, mino_env_t *env)
{
    (void)env;
    if (!mino_is_cons(args)) {
        set_error("car requires one argument");
        return NULL;
    }
    return mino_car(args->as.cons.car);
}

static mino_val_t *prim_cdr(mino_val_t *args, mino_env_t *env)
{
    (void)env;
    if (!mino_is_cons(args)) {
        set_error("cdr requires one argument");
        return NULL;
    }
    return mino_cdr(args->as.cons.car);
}

static mino_val_t *prim_cons(mino_val_t *args, mino_env_t *env)
{
    (void)env;
    if (!mino_is_cons(args) || !mino_is_cons(args->as.cons.cdr)) {
        set_error("cons requires two arguments");
        return NULL;
    }
    return mino_cons(args->as.cons.car, args->as.cons.cdr->as.cons.car);
}

static mino_val_t *prim_list(mino_val_t *args, mino_env_t *env)
{
    (void)env;
    /* Args are already a list of evaluated values. */
    return args == NULL ? mino_nil() : args;
}

void mino_install_core(mino_env_t *env)
{
    mino_env_set(env, "+",    mino_prim("+",    prim_add));
    mino_env_set(env, "-",    mino_prim("-",    prim_sub));
    mino_env_set(env, "*",    mino_prim("*",    prim_mul));
    mino_env_set(env, "/",    mino_prim("/",    prim_div));
    mino_env_set(env, "=",    mino_prim("=",    prim_eq));
    mino_env_set(env, "<",    mino_prim("<",    prim_lt));
    mino_env_set(env, "car",  mino_prim("car",  prim_car));
    mino_env_set(env, "cdr",  mino_prim("cdr",  prim_cdr));
    mino_env_set(env, "cons", mino_prim("cons", prim_cons));
    mino_env_set(env, "list", mino_prim("list", prim_list));
}
