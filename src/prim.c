/*
 * prim.c -- built-in primitive functions.
 */

#include "mino_internal.h"
#include "re.h"

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

/* Throw a catchable exception from a primitive.  If inside a try block,
 * this longjmps to the catch handler.  Otherwise it sets a fatal error
 * and the caller returns NULL to propagate to the host. */
static mino_val_t *prim_throw_error(const char *msg)
{
    mino_val_t *ex = mino_string(S_, msg);
    if (try_depth > 0) {
        try_stack[try_depth - 1].exception = ex;
        longjmp(try_stack[try_depth - 1].buf, 1);
    }
    set_error(msg);
    return NULL;
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
        return mino_float(S_, acc);
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
        return mino_int(S_, acc);
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
            return mino_float(S_, -acc);
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
        return mino_float(S_, acc);
    } else {
        long long acc;
        if (!as_long(args->as.cons.car, &acc)) {
            set_error("- expects numbers");
            return NULL;
        }
        args = args->as.cons.cdr;
        if (!mino_is_cons(args)) {
            return mino_int(S_, -acc);
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
        return mino_int(S_, acc);
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
        return mino_float(S_, acc);
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
        return mino_int(S_, acc);
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
            return prim_throw_error("division by zero");
        }
        return mino_float(S_, 1.0 / acc);
    }
    while (mino_is_cons(args)) {
        double x;
        if (!as_double(args->as.cons.car, &x)) {
            set_error("/ expects numbers");
            return NULL;
        }
        if (x == 0.0) {
            return prim_throw_error("division by zero");
        }
        acc /= x;
        args = args->as.cons.cdr;
    }
    return mino_float(S_, acc);
}

static mino_val_t *prim_mod(mino_val_t *args, mino_env_t *env)
{
    double a, b, r;
    (void)env;
    if (!mino_is_cons(args) || !mino_is_cons(args->as.cons.cdr) ||
        mino_is_cons(args->as.cons.cdr->as.cons.cdr)) {
        set_error("mod requires two arguments");
        return NULL;
    }
    if (!as_double(args->as.cons.car, &a) ||
        !as_double(args->as.cons.cdr->as.cons.car, &b)) {
        set_error("mod expects numbers");
        return NULL;
    }
    if (b == 0.0) {
        return prim_throw_error("mod: division by zero");
    }
    r = fmod(a, b);
    /* Floored modulo: result has same sign as divisor. */
    if (r != 0.0 && ((r < 0.0) != (b < 0.0))) r += b;
    /* Return int if both args are ints. */
    if (args->as.cons.car->type == MINO_INT &&
        args->as.cons.cdr->as.cons.car->type == MINO_INT) {
        return mino_int(S_, (long long)r);
    }
    return mino_float(S_, r);
}

static mino_val_t *prim_rem(mino_val_t *args, mino_env_t *env)
{
    double a, b, r;
    (void)env;
    if (!mino_is_cons(args) || !mino_is_cons(args->as.cons.cdr) ||
        mino_is_cons(args->as.cons.cdr->as.cons.cdr)) {
        set_error("rem requires two arguments");
        return NULL;
    }
    if (!as_double(args->as.cons.car, &a) ||
        !as_double(args->as.cons.cdr->as.cons.car, &b)) {
        set_error("rem expects numbers");
        return NULL;
    }
    if (b == 0.0) {
        return prim_throw_error("rem: division by zero");
    }
    r = fmod(a, b);
    if (args->as.cons.car->type == MINO_INT &&
        args->as.cons.cdr->as.cons.car->type == MINO_INT) {
        return mino_int(S_, (long long)r);
    }
    return mino_float(S_, r);
}

static mino_val_t *prim_quot(mino_val_t *args, mino_env_t *env)
{
    double a, b, q;
    (void)env;
    if (!mino_is_cons(args) || !mino_is_cons(args->as.cons.cdr) ||
        mino_is_cons(args->as.cons.cdr->as.cons.cdr)) {
        set_error("quot requires two arguments");
        return NULL;
    }
    if (!as_double(args->as.cons.car, &a) ||
        !as_double(args->as.cons.cdr->as.cons.car, &b)) {
        set_error("quot expects numbers");
        return NULL;
    }
    if (b == 0.0) {
        return prim_throw_error("quot: division by zero");
    }
    q = a / b;
    q = q >= 0 ? floor(q) : ceil(q);
    if (args->as.cons.car->type == MINO_INT &&
        args->as.cons.cdr->as.cons.car->type == MINO_INT) {
        return mino_int(S_, (long long)q);
    }
    return mino_float(S_, q);
}

/* --- Math functions (thin wrappers around math.h) --- */

#define MATH_UNARY(cname, cfn, label)                                  \
    static mino_val_t *cname(mino_val_t *args, mino_env_t *env)        \
    {                                                                   \
        double x;                                                       \
        (void)env;                                                      \
        if (!mino_is_cons(args) ||                                      \
            mino_is_cons(args->as.cons.cdr)) {                          \
            set_error(label " requires one argument");                  \
            return NULL;                                                \
        }                                                               \
        if (!as_double(args->as.cons.car, &x)) {                       \
            set_error(label " expects a number");                       \
            return NULL;                                                \
        }                                                               \
        return mino_float(S_, cfn(x));                                      \
    }

MATH_UNARY(prim_math_floor, floor, "math-floor")
MATH_UNARY(prim_math_ceil,  ceil,  "math-ceil")
MATH_UNARY(prim_math_round, round, "math-round")
MATH_UNARY(prim_math_sqrt,  sqrt,  "math-sqrt")
MATH_UNARY(prim_math_log,   log,   "math-log")
MATH_UNARY(prim_math_exp,   exp,   "math-exp")
MATH_UNARY(prim_math_sin,   sin,   "math-sin")
MATH_UNARY(prim_math_cos,   cos,   "math-cos")
MATH_UNARY(prim_math_tan,   tan,   "math-tan")

#undef MATH_UNARY

static mino_val_t *prim_math_pow(mino_val_t *args, mino_env_t *env)
{
    double base, exponent;
    (void)env;
    if (!mino_is_cons(args) || !mino_is_cons(args->as.cons.cdr) ||
        mino_is_cons(args->as.cons.cdr->as.cons.cdr)) {
        set_error("math-pow requires two arguments");
        return NULL;
    }
    if (!as_double(args->as.cons.car, &base) ||
        !as_double(args->as.cons.cdr->as.cons.car, &exponent)) {
        set_error("math-pow expects numbers");
        return NULL;
    }
    return mino_float(S_, pow(base, exponent));
}

static mino_val_t *prim_math_atan2(mino_val_t *args, mino_env_t *env)
{
    double y, x;
    (void)env;
    if (!mino_is_cons(args) || !mino_is_cons(args->as.cons.cdr) ||
        mino_is_cons(args->as.cons.cdr->as.cons.cdr)) {
        set_error("math-atan2 requires two arguments");
        return NULL;
    }
    if (!as_double(args->as.cons.car, &y) ||
        !as_double(args->as.cons.cdr->as.cons.car, &x)) {
        set_error("math-atan2 expects numbers");
        return NULL;
    }
    return mino_float(S_, atan2(y, x));
}

static mino_val_t *prim_bit_and(mino_val_t *args, mino_env_t *env)
{
    long long a, b;
    (void)env;
    if (!mino_is_cons(args) || !mino_is_cons(args->as.cons.cdr) ||
        mino_is_cons(args->as.cons.cdr->as.cons.cdr)) {
        set_error("bit-and requires two arguments");
        return NULL;
    }
    if (!as_long(args->as.cons.car, &a) ||
        !as_long(args->as.cons.cdr->as.cons.car, &b)) {
        set_error("bit-and expects integers");
        return NULL;
    }
    return mino_int(S_, a & b);
}

static mino_val_t *prim_bit_or(mino_val_t *args, mino_env_t *env)
{
    long long a, b;
    (void)env;
    if (!mino_is_cons(args) || !mino_is_cons(args->as.cons.cdr) ||
        mino_is_cons(args->as.cons.cdr->as.cons.cdr)) {
        set_error("bit-or requires two arguments");
        return NULL;
    }
    if (!as_long(args->as.cons.car, &a) ||
        !as_long(args->as.cons.cdr->as.cons.car, &b)) {
        set_error("bit-or expects integers");
        return NULL;
    }
    return mino_int(S_, a | b);
}

static mino_val_t *prim_bit_xor(mino_val_t *args, mino_env_t *env)
{
    long long a, b;
    (void)env;
    if (!mino_is_cons(args) || !mino_is_cons(args->as.cons.cdr) ||
        mino_is_cons(args->as.cons.cdr->as.cons.cdr)) {
        set_error("bit-xor requires two arguments");
        return NULL;
    }
    if (!as_long(args->as.cons.car, &a) ||
        !as_long(args->as.cons.cdr->as.cons.car, &b)) {
        set_error("bit-xor expects integers");
        return NULL;
    }
    return mino_int(S_, a ^ b);
}

static mino_val_t *prim_bit_not(mino_val_t *args, mino_env_t *env)
{
    long long a;
    (void)env;
    if (!mino_is_cons(args) || mino_is_cons(args->as.cons.cdr)) {
        set_error("bit-not requires one argument");
        return NULL;
    }
    if (!as_long(args->as.cons.car, &a)) {
        set_error("bit-not expects an integer");
        return NULL;
    }
    return mino_int(S_, ~a);
}

static mino_val_t *prim_bit_shift_left(mino_val_t *args, mino_env_t *env)
{
    long long a, b;
    (void)env;
    if (!mino_is_cons(args) || !mino_is_cons(args->as.cons.cdr) ||
        mino_is_cons(args->as.cons.cdr->as.cons.cdr)) {
        set_error("bit-shift-left requires two arguments");
        return NULL;
    }
    if (!as_long(args->as.cons.car, &a) ||
        !as_long(args->as.cons.cdr->as.cons.car, &b)) {
        set_error("bit-shift-left expects integers");
        return NULL;
    }
    return mino_int(S_, a << b);
}

static mino_val_t *prim_bit_shift_right(mino_val_t *args, mino_env_t *env)
{
    long long a, b;
    (void)env;
    if (!mino_is_cons(args) || !mino_is_cons(args->as.cons.cdr) ||
        mino_is_cons(args->as.cons.cdr->as.cons.cdr)) {
        set_error("bit-shift-right requires two arguments");
        return NULL;
    }
    if (!as_long(args->as.cons.car, &a) ||
        !as_long(args->as.cons.cdr->as.cons.car, &b)) {
        set_error("bit-shift-right expects integers");
        return NULL;
    }
    return mino_int(S_, a >> b);
}

static mino_val_t *prim_int(mino_val_t *args, mino_env_t *env)
{
    mino_val_t *v;
    (void)env;
    if (!mino_is_cons(args) || mino_is_cons(args->as.cons.cdr)) {
        set_error("int requires one argument");
        return NULL;
    }
    v = args->as.cons.car;
    if (v != NULL && v->type == MINO_INT) return v;
    if (v != NULL && v->type == MINO_FLOAT) return mino_int(S_, (long long)v->as.f);
    set_error("int: expected a number");
    return NULL;
}

static mino_val_t *prim_float(mino_val_t *args, mino_env_t *env)
{
    mino_val_t *v;
    (void)env;
    if (!mino_is_cons(args) || mino_is_cons(args->as.cons.cdr)) {
        set_error("float requires one argument");
        return NULL;
    }
    v = args->as.cons.car;
    if (v != NULL && v->type == MINO_FLOAT) return v;
    if (v != NULL && v->type == MINO_INT) return mino_float(S_, (double)v->as.i);
    set_error("float: expected a number");
    return NULL;
}

/*
 * Helper: print a value to a string buffer using the standard printer.
 * Returns a mino string. Uses tmpfile() for ANSI C portability.
 */
static mino_val_t *print_to_string(const mino_val_t *v)
{
    FILE  *f = tmpfile();
    long   n;
    char  *buf;
    mino_val_t *result;
    if (f == NULL) {
        set_error("pr-str: tmpfile failed");
        return NULL;
    }
    mino_print_to(S_, f, v);
    n = ftell(f);
    if (n < 0) n = 0;
    rewind(f);
    buf = (char *)malloc((size_t)n + 1);
    if (buf == NULL) {
        fclose(f);
        set_error("out of memory");
        return NULL;
    }
    if (n > 0) {
        size_t rd = fread(buf, 1, (size_t)n, f);
        (void)rd;
    }
    buf[n] = '\0';
    fclose(f);
    result = mino_string_n(S_, buf, (size_t)n);
    free(buf);
    return result;
}

/*
 * (format fmt & args) — simple string formatting.
 * Directives: %s (str of arg), %d (integer), %f (float), %% (literal %).
 */
static mino_val_t *prim_format(mino_val_t *args, mino_env_t *env)
{
    mino_val_t *fmt_val;
    const char *fmt;
    size_t      fmt_len;
    mino_val_t *arg_list;
    char  *buf = NULL;
    size_t len = 0;
    size_t cap = 0;
    size_t i;
    (void)env;
    if (!mino_is_cons(args)) {
        set_error("format requires at least a format string");
        return NULL;
    }
    fmt_val = args->as.cons.car;
    if (fmt_val == NULL || fmt_val->type != MINO_STRING) {
        set_error("format: first argument must be a string");
        return NULL;
    }
    fmt     = fmt_val->as.s.data;
    fmt_len = fmt_val->as.s.len;
    arg_list = args->as.cons.cdr;

#define FMT_ENSURE(extra) do { \
        size_t _need = len + (extra) + 1; \
        if (_need > cap) { \
            cap = cap == 0 ? 128 : cap; \
            while (cap < _need) cap *= 2; \
            buf = (char *)realloc(buf, cap); \
            if (buf == NULL) { set_error("out of memory"); return NULL; } \
        } \
    } while (0)

    for (i = 0; i < fmt_len; i++) {
        if (fmt[i] == '%' && i + 1 < fmt_len) {
            char spec = fmt[i + 1];
            i++;
            if (spec == '%') {
                FMT_ENSURE(1);
                buf[len++] = '%';
            } else if (spec == 's') {
                mino_val_t *a;
                if (!mino_is_cons(arg_list)) {
                    free(buf);
                    set_error("format: not enough arguments for format string");
                    return NULL;
                }
                a = arg_list->as.cons.car;
                arg_list = arg_list->as.cons.cdr;
                if (a != NULL && a->type == MINO_STRING) {
                    FMT_ENSURE(a->as.s.len);
                    memcpy(buf + len, a->as.s.data, a->as.s.len);
                    len += a->as.s.len;
                } else {
                    mino_val_t *s = print_to_string(a);
                    if (s == NULL) { free(buf); return NULL; }
                    FMT_ENSURE(s->as.s.len);
                    memcpy(buf + len, s->as.s.data, s->as.s.len);
                    len += s->as.s.len;
                }
            } else if (spec == 'd') {
                long long n;
                char tmp[32];
                int  tn;
                if (!mino_is_cons(arg_list)) {
                    free(buf);
                    set_error("format: not enough arguments for format string");
                    return NULL;
                }
                if (!as_long(arg_list->as.cons.car, &n)) {
                    double d;
                    if (as_double(arg_list->as.cons.car, &d)) {
                        n = (long long)d;
                    } else {
                        free(buf);
                        set_error("format: %d expects a number");
                        return NULL;
                    }
                }
                arg_list = arg_list->as.cons.cdr;
                tn = snprintf(tmp, sizeof(tmp), "%lld", n);
                FMT_ENSURE((size_t)tn);
                memcpy(buf + len, tmp, (size_t)tn);
                len += (size_t)tn;
            } else if (spec == 'f') {
                double d;
                char tmp[64];
                int  tn;
                if (!mino_is_cons(arg_list)) {
                    free(buf);
                    set_error("format: not enough arguments for format string");
                    return NULL;
                }
                if (!as_double(arg_list->as.cons.car, &d)) {
                    free(buf);
                    set_error("format: %f expects a number");
                    return NULL;
                }
                arg_list = arg_list->as.cons.cdr;
                tn = snprintf(tmp, sizeof(tmp), "%f", d);
                FMT_ENSURE((size_t)tn);
                memcpy(buf + len, tmp, (size_t)tn);
                len += (size_t)tn;
            } else {
                /* Unknown directive: emit literal. */
                FMT_ENSURE(2);
                buf[len++] = '%';
                buf[len++] = spec;
            }
        } else {
            FMT_ENSURE(1);
            buf[len++] = fmt[i];
        }
    }
#undef FMT_ENSURE
    {
        mino_val_t *result = mino_string_n(S_, buf != NULL ? buf : "", len);
        free(buf);
        return result;
    }
}

static mino_val_t *prim_read_string(mino_val_t *args, mino_env_t *env)
{
    mino_val_t *s;
    mino_val_t *result;
    (void)env;
    if (!mino_is_cons(args) || mino_is_cons(args->as.cons.cdr)) {
        set_error("read-string requires one string argument");
        return NULL;
    }
    s = args->as.cons.car;
    if (s == NULL || s->type != MINO_STRING) {
        set_error("read-string: argument must be a string");
        return NULL;
    }
    clear_error();
    result = mino_read(S_, s->as.s.data, NULL);
    if (result == NULL && mino_last_error(S_) != NULL) {
        /* Throw parse errors as catchable exceptions so user code can
         * handle them via try/catch. */
        mino_val_t *ex = mino_string(S_, mino_last_error(S_));
        if (try_depth > 0) {
            try_stack[try_depth - 1].exception = ex;
            longjmp(try_stack[try_depth - 1].buf, 1);
        }
        /* No enclosing try — propagate as fatal error. */
        {
            char msg[512];
            snprintf(msg, sizeof(msg), "unhandled exception: %.*s",
                     (int)ex->as.s.len, ex->as.s.data);
            set_error(msg);
        }
        return NULL;
    }
    return result != NULL ? result : mino_nil(S_);
}

static mino_val_t *prim_pr_str(mino_val_t *args, mino_env_t *env)
{
    char  *buf = NULL;
    size_t len = 0;
    size_t cap = 0;
    int    first = 1;
    (void)env;
    while (mino_is_cons(args)) {
        mino_val_t *printed = print_to_string(args->as.cons.car);
        size_t      need;
        if (printed == NULL) {
            free(buf);
            return NULL;
        }
        need = len + (!first ? 1 : 0) + printed->as.s.len + 1;
        if (need > cap) {
            cap = cap == 0 ? 128 : cap;
            while (cap < need) cap *= 2;
            buf = (char *)realloc(buf, cap);
            if (buf == NULL) { set_error("out of memory"); return NULL; }
        }
        if (!first) buf[len++] = ' ';
        memcpy(buf + len, printed->as.s.data, printed->as.s.len);
        len += printed->as.s.len;
        first = 0;
        args = args->as.cons.cdr;
    }
    {
        mino_val_t *result = mino_string_n(S_, buf != NULL ? buf : "", len);
        free(buf);
        return result;
    }
}

static mino_val_t *prim_char_at(mino_val_t *args, mino_env_t *env)
{
    mino_val_t *s;
    long long   idx;
    (void)env;
    if (!mino_is_cons(args) || !mino_is_cons(args->as.cons.cdr) ||
        mino_is_cons(args->as.cons.cdr->as.cons.cdr)) {
        set_error("char-at requires two arguments");
        return NULL;
    }
    s = args->as.cons.car;
    if (s == NULL || s->type != MINO_STRING) {
        set_error("char-at: first argument must be a string");
        return NULL;
    }
    if (!as_long(args->as.cons.cdr->as.cons.car, &idx)) {
        set_error("char-at: second argument must be an integer");
        return NULL;
    }
    if (idx < 0 || (size_t)idx >= s->as.s.len) {
        return prim_throw_error("char-at: index out of range");
    }
    return mino_string_n(S_, s->as.s.data + idx, 1);
}

static mino_val_t *prim_name(mino_val_t *args, mino_env_t *env)
{
    mino_val_t *v;
    (void)env;
    if (!mino_is_cons(args) || mino_is_cons(args->as.cons.cdr)) {
        set_error("name requires one argument");
        return NULL;
    }
    v = args->as.cons.car;
    if (v == NULL || v->type == MINO_NIL) return mino_nil(S_);
    if (v->type == MINO_STRING)  return v;
    if (v->type == MINO_KEYWORD) return mino_string_n(S_, v->as.s.data, v->as.s.len);
    if (v->type == MINO_SYMBOL)  return mino_string_n(S_, v->as.s.data, v->as.s.len);
    set_error("name: expected a keyword, symbol, or string");
    return NULL;
}

/* (rand) — return a random float in [0.0, 1.0). */
static mino_val_t *prim_rand(mino_val_t *args, mino_env_t *env)
{
    (void)env;
    if (mino_is_cons(args)) {
        set_error("rand takes no arguments");
        return NULL;
    }
    if (!rand_seeded) {
        srand((unsigned int)time(NULL));
        rand_seeded = 1;
    }
    return mino_float(S_, (double)rand() / ((double)RAND_MAX + 1.0));
}

/* (eval form) — evaluate a form at runtime. */
static mino_val_t *prim_eval(mino_val_t *args, mino_env_t *env)
{
    if (!mino_is_cons(args) || mino_is_cons(args->as.cons.cdr)) {
        set_error("eval requires one argument");
        return NULL;
    }
    return eval(args->as.cons.car, env);
}

/* (symbol str) — create a symbol from a string. */
static mino_val_t *prim_symbol(mino_val_t *args, mino_env_t *env)
{
    mino_val_t *v;
    (void)env;
    if (!mino_is_cons(args) || mino_is_cons(args->as.cons.cdr)) {
        set_error("symbol requires one string argument");
        return NULL;
    }
    v = args->as.cons.car;
    if (v == NULL || v->type != MINO_STRING) {
        set_error("symbol: argument must be a string");
        return NULL;
    }
    return mino_symbol_n(S_, v->as.s.data, v->as.s.len);
}

/* (keyword str) — create a keyword from a string. */
static mino_val_t *prim_keyword(mino_val_t *args, mino_env_t *env)
{
    mino_val_t *v;
    (void)env;
    if (!mino_is_cons(args) || mino_is_cons(args->as.cons.cdr)) {
        set_error("keyword requires one string argument");
        return NULL;
    }
    v = args->as.cons.car;
    if (v == NULL || v->type != MINO_STRING) {
        set_error("keyword: argument must be a string");
        return NULL;
    }
    return mino_keyword_n(S_, v->as.s.data, v->as.s.len);
}

/* (hash val) — return the integer hash code of any value. */
static mino_val_t *prim_hash(mino_val_t *args, mino_env_t *env)
{
    (void)env;
    if (!mino_is_cons(args) || mino_is_cons(args->as.cons.cdr)) {
        set_error("hash requires one argument");
        return NULL;
    }
    return mino_int(S_, (long long)hash_val(args->as.cons.car));
}

/* (compare a b) — general comparison returning -1, 0, or 1.
 * Compares numbers, strings, keywords, symbols, and nil. */
static mino_val_t *prim_compare(mino_val_t *args, mino_env_t *env)
{
    mino_val_t *a, *b;
    (void)env;
    if (!mino_is_cons(args) || !mino_is_cons(args->as.cons.cdr) ||
        mino_is_cons(args->as.cons.cdr->as.cons.cdr)) {
        set_error("compare requires two arguments");
        return NULL;
    }
    a = args->as.cons.car;
    b = args->as.cons.cdr->as.cons.car;
    /* nil sorts before everything */
    if ((a == NULL || (a->type == MINO_NIL)) &&
        (b == NULL || (b->type == MINO_NIL))) return mino_int(S_, 0);
    if (a == NULL || a->type == MINO_NIL) return mino_int(S_, -1);
    if (b == NULL || b->type == MINO_NIL) return mino_int(S_, 1);
    /* numbers */
    {
        double da, db;
        if (as_double(a, &da) && as_double(b, &db)) {
            return mino_int(S_, da < db ? -1 : da > db ? 1 : 0);
        }
    }
    /* strings, keywords, symbols — lexicographic */
    if ((a->type == MINO_STRING || a->type == MINO_KEYWORD ||
         a->type == MINO_SYMBOL) && a->type == b->type) {
        int cmp = strcmp(a->as.s.data, b->as.s.data);
        return mino_int(S_, cmp < 0 ? -1 : cmp > 0 ? 1 : 0);
    }
    set_error("compare: cannot compare values of different types");
    return NULL;
}

static mino_val_t *prim_eq(mino_val_t *args, mino_env_t *env)
{
    (void)env;
    if (!mino_is_cons(args)) {
        return mino_true(S_);
    }
    {
        mino_val_t *first = args->as.cons.car;
        args = args->as.cons.cdr;
        while (mino_is_cons(args)) {
            if (!mino_eq(first, args->as.cons.car)) {
                return mino_false(S_);
            }
            args = args->as.cons.cdr;
        }
    }
    return mino_true(S_);
}

/*
 * Chained numeric comparison. `op` selects the relation:
 *   0: <    1: <=    2: >    3: >=
 * Returns true if each successive pair satisfies the relation (and
 * trivially true on zero or one argument).
 */
static mino_val_t *compare_chain(mino_val_t *args, const char *name, int op)
{
    double prev;
    if (!mino_is_cons(args)) {
        return mino_true(S_);
    }
    if (!as_double(args->as.cons.car, &prev)) {
        char msg[64];
        snprintf(msg, sizeof(msg), "%s expects numbers", name);
        set_error(msg);
        return NULL;
    }
    args = args->as.cons.cdr;
    while (mino_is_cons(args)) {
        double x;
        int    ok;
        if (!as_double(args->as.cons.car, &x)) {
            char msg[64];
            snprintf(msg, sizeof(msg), "%s expects numbers", name);
            set_error(msg);
            return NULL;
        }
        switch (op) {
        case 0:  ok = prev <  x; break;
        case 1:  ok = prev <= x; break;
        case 2:  ok = prev >  x; break;
        default: ok = prev >= x; break;
        }
        if (!ok) {
            return mino_false(S_);
        }
        prev = x;
        args = args->as.cons.cdr;
    }
    return mino_true(S_);
}

static mino_val_t *prim_lt(mino_val_t *args, mino_env_t *env)
{
    (void)env;
    return compare_chain(args, "<", 0);
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
    return mino_cons(S_, args->as.cons.car, args->as.cons.cdr->as.cons.car);
}

/* ------------------------------------------------------------------------- */
/* Collection primitives                                                     */
/*                                                                           */
/* All collection ops treat values as immutable: every operation that        */
/* "modifies" a collection returns a freshly allocated value. v0.3 uses      */
/* naïve array-backed representations; persistent tries arrive in v0.4/v0.5 */
/* without changing the public primitive contracts.                          */
/* ------------------------------------------------------------------------- */

static mino_val_t *set_conj1(const mino_val_t *s, mino_val_t *elem);
static mino_val_t *prim_str(mino_val_t *args, mino_env_t *env);

static size_t list_length(mino_val_t *list)
{
    size_t n = 0;
    while (list != NULL && list->type == MINO_LAZY) {
        list = lazy_force(list);
    }
    while (mino_is_cons(list)) {
        n++;
        list = list->as.cons.cdr;
        /* Force lazy tails. */
        while (list != NULL && list->type == MINO_LAZY) {
            list = lazy_force(list);
        }
    }
    return n;
}

static int arg_count(mino_val_t *args, size_t *out)
{
    *out = list_length(args);
    return 1;
}

static mino_val_t *prim_count(mino_val_t *args, mino_env_t *env)
{
    mino_val_t *coll;
    (void)env;
    if (!mino_is_cons(args) || mino_is_cons(args->as.cons.cdr)) {
        set_error("count requires one argument");
        return NULL;
    }
    coll = args->as.cons.car;
    if (coll == NULL || coll->type == MINO_NIL) {
        return mino_int(S_, 0);
    }
    switch (coll->type) {
    case MINO_CONS:   return mino_int(S_, (long long)list_length(coll));
    case MINO_VECTOR: return mino_int(S_, (long long)coll->as.vec.len);
    case MINO_MAP:    return mino_int(S_, (long long)coll->as.map.len);
    case MINO_SET:    return mino_int(S_, (long long)coll->as.set.len);
    case MINO_STRING: return mino_int(S_, (long long)coll->as.s.len);
    case MINO_LAZY: {
        /* Force the entire lazy seq and count it. */
        mino_val_t *forced = lazy_force(coll);
        if (forced == NULL) return NULL;
        if (forced->type == MINO_NIL) return mino_int(S_, 0);
        return mino_int(S_, (long long)list_length(forced));
    }
    default:
        {
            char msg[96];
            snprintf(msg, sizeof(msg), "count: expected a collection, got %s",
                     type_tag_str(coll));
            set_error(msg);
        }
        return NULL;
    }
}

static mino_val_t *prim_vector(mino_val_t *args, mino_env_t *env)
{
    size_t n;
    size_t i;
    mino_val_t **tmp;
    mino_val_t *p;
    (void)env;
    arg_count(args, &n);
    if (n == 0) {
        return mino_vector(S_, NULL, 0);
    }
    /* GC_T_VALARR keeps partially-gathered pointers visible to the collector;
     * without this, the optimizer may drop the `args` parameter and the cons
     * cells holding the element values become unreachable mid-construction. */
    tmp = (mino_val_t **)gc_alloc_typed(GC_T_VALARR, n * sizeof(*tmp));
    p = args;
    for (i = 0; i < n; i++) {
        tmp[i] = p->as.cons.car;
        p = p->as.cons.cdr;
    }
    return mino_vector(S_, tmp, n);
}

static mino_val_t *prim_hash_map(mino_val_t *args, mino_env_t *env)
{
    size_t n;
    size_t pairs;
    size_t i;
    mino_val_t **ks;
    mino_val_t **vs;
    mino_val_t *p;
    (void)env;
    arg_count(args, &n);
    if (n % 2 != 0) {
        set_error("hash-map requires an even number of arguments");
        return NULL;
    }
    if (n == 0) {
        return mino_map(S_, NULL, NULL, 0);
    }
    pairs = n / 2;
    ks = (mino_val_t **)gc_alloc_typed(GC_T_VALARR, pairs * sizeof(*ks));
    vs = (mino_val_t **)gc_alloc_typed(GC_T_VALARR, pairs * sizeof(*vs));
    p = args;
    for (i = 0; i < pairs; i++) {
        ks[i] = p->as.cons.car;
        p = p->as.cons.cdr;
        vs[i] = p->as.cons.car;
        p = p->as.cons.cdr;
    }
    return mino_map(S_, ks, vs, pairs);
}

static mino_val_t *prim_nth(mino_val_t *args, mino_env_t *env)
{
    mino_val_t *coll;
    mino_val_t *idx_val;
    mino_val_t *def_val = NULL;
    size_t      n;
    long long   idx;
    (void)env;
    arg_count(args, &n);
    if (n != 2 && n != 3) {
        set_error("nth requires 2 or 3 arguments");
        return NULL;
    }
    coll    = args->as.cons.car;
    idx_val = args->as.cons.cdr->as.cons.car;
    if (n == 3) {
        def_val = args->as.cons.cdr->as.cons.cdr->as.cons.car;
    }
    if (idx_val == NULL || idx_val->type != MINO_INT) {
        set_error("nth index must be an integer");
        return NULL;
    }
    idx = idx_val->as.i;
    if (idx < 0) {
        if (def_val != NULL) return def_val;
        return prim_throw_error("nth index out of range");
    }
    if (coll == NULL || coll->type == MINO_NIL) {
        if (def_val != NULL) return def_val;
        return prim_throw_error("nth index out of range");
    }
    if (coll->type == MINO_VECTOR) {
        if ((size_t)idx >= coll->as.vec.len) {
            if (def_val != NULL) return def_val;
            set_error("nth index out of range");
            return NULL;
        }
        return vec_nth(coll, (size_t)idx);
    }
    if (coll->type == MINO_CONS) {
        mino_val_t *p = coll;
        long long   i;
        for (i = 0; i < idx; i++) {
            if (!mino_is_cons(p)) {
                if (def_val != NULL) return def_val;
                set_error("nth index out of range");
                return NULL;
            }
            p = p->as.cons.cdr;
        }
        if (!mino_is_cons(p)) {
            if (def_val != NULL) return def_val;
            set_error("nth index out of range");
            return NULL;
        }
        return p->as.cons.car;
    }
    {
        char msg[96];
        snprintf(msg, sizeof(msg), "nth: expected a list, vector, or string, got %s",
                 type_tag_str(coll));
        set_error(msg);
    }
    return NULL;
}

static mino_val_t *prim_first(mino_val_t *args, mino_env_t *env)
{
    mino_val_t *coll;
    (void)env;
    if (!mino_is_cons(args) || mino_is_cons(args->as.cons.cdr)) {
        set_error("first requires one argument");
        return NULL;
    }
    coll = args->as.cons.car;
    if (coll == NULL || coll->type == MINO_NIL) {
        return mino_nil(S_);
    }
    if (coll->type == MINO_CONS) {
        return coll->as.cons.car;
    }
    if (coll->type == MINO_VECTOR) {
        if (coll->as.vec.len == 0) {
            return mino_nil(S_);
        }
        return vec_nth(coll, 0);
    }
    if (coll->type == MINO_LAZY) {
        mino_val_t *s = lazy_force(coll);
        if (s == NULL) return NULL;
        if (s->type == MINO_NIL || s == NULL) return mino_nil(S_);
        if (s->type == MINO_CONS) return s->as.cons.car;
        return mino_nil(S_);
    }
    {
        char msg[96];
        snprintf(msg, sizeof(msg), "first: expected a list or vector, got %s",
                 type_tag_str(coll));
        set_error(msg);
    }
    return NULL;
}

static mino_val_t *prim_rest(mino_val_t *args, mino_env_t *env)
{
    mino_val_t *coll;
    (void)env;
    if (!mino_is_cons(args) || mino_is_cons(args->as.cons.cdr)) {
        set_error("rest requires one argument");
        return NULL;
    }
    coll = args->as.cons.car;
    if (coll == NULL || coll->type == MINO_NIL) {
        return mino_nil(S_);
    }
    if (coll->type == MINO_CONS) {
        return coll->as.cons.cdr;
    }
    if (coll->type == MINO_VECTOR) {
        /* Rest of a vector is a list of the trailing elements. v0.11 will
         * promote this to a seq abstraction. */
        mino_val_t *head = mino_nil(S_);
        mino_val_t *tail = NULL;
        size_t i;
        for (i = 1; i < coll->as.vec.len; i++) {
            mino_val_t *cell = mino_cons(S_, vec_nth(coll, i), mino_nil(S_));
            if (tail == NULL) {
                head = cell;
            } else {
                tail->as.cons.cdr = cell;
            }
            tail = cell;
        }
        return head;
    }
    if (coll->type == MINO_LAZY) {
        mino_val_t *s = lazy_force(coll);
        if (s == NULL) return NULL;
        if (s->type == MINO_NIL || s == NULL) return mino_nil(S_);
        if (s->type == MINO_CONS) return s->as.cons.cdr;
        return mino_nil(S_);
    }
    {
        char msg[96];
        snprintf(msg, sizeof(msg), "rest: expected a list or vector, got %s",
                 type_tag_str(coll));
        set_error(msg);
    }
    return NULL;
}

/* Layer n k/v pairs onto an existing map, returning a new map value that
 * shares structure with `coll`. Nil is treated as an empty map. */
static mino_val_t *map_assoc_pairs(mino_val_t *coll, mino_val_t *p,
                                    size_t extra_pairs)
{
    mino_hamt_node_t *root;
    mino_val_t       *order;
    size_t            len_out;
    size_t            i;
    if (coll == NULL || coll->type == MINO_NIL) {
        root    = NULL;
        order   = mino_vector(S_, NULL, 0);
        len_out = 0;
    } else {
        root    = coll->as.map.root;
        order   = coll->as.map.key_order;
        len_out = coll->as.map.len;
    }
    for (i = 0; i < extra_pairs; i++) {
        mino_val_t   *k = p->as.cons.car;
        mino_val_t   *v = p->as.cons.cdr->as.cons.car;
        hamt_entry_t *e = hamt_entry_new(k, v);
        uint32_t      h = hash_val(k);
        int           replaced = 0;
        root = hamt_assoc(root, e, h, 0u, &replaced);
        if (!replaced) {
            order = vec_conj1(order, k);
            len_out++;
        }
        p = p->as.cons.cdr->as.cons.cdr;
    }
    {
        mino_val_t *out = alloc_val(MINO_MAP);
        out->as.map.root      = root;
        out->as.map.key_order = order;
        out->as.map.len       = len_out;
        return out;
    }
}

static mino_val_t *prim_assoc(mino_val_t *args, mino_env_t *env)
{
    mino_val_t *coll;
    size_t      n;
    size_t      extra_pairs;
    size_t      i;
    mino_val_t *p;
    (void)env;
    arg_count(args, &n);
    if (n < 3 || (n - 1) % 2 != 0) {
        set_error("assoc requires a collection and an even number of k/v pairs");
        return NULL;
    }
    coll = args->as.cons.car;
    extra_pairs = (n - 1) / 2;
    if (coll != NULL && coll->type == MINO_VECTOR) {
        /* Vector assoc: each key must be an integer index in [0, len]; an
         * index == len is a one-past-end append. Apply pairs in order on
         * successively-derived vectors so each update shares structure with
         * its predecessor. */
        mino_val_t *acc = coll;
        p = args->as.cons.cdr;
        for (i = 0; i < extra_pairs; i++) {
            mino_val_t *k = p->as.cons.car;
            mino_val_t *v = p->as.cons.cdr->as.cons.car;
            long long   idx;
            if (k == NULL || k->type != MINO_INT) {
                set_error("assoc on vector requires integer indices");
                return NULL;
            }
            idx = k->as.i;
            if (idx < 0 || (size_t)idx > acc->as.vec.len) {
                return prim_throw_error("assoc on vector: index out of range");
            }
            acc = vec_assoc1(acc, (size_t)idx, v);
            p = p->as.cons.cdr->as.cons.cdr;
        }
        return acc;
    }
    if (coll == NULL || coll->type == MINO_NIL || coll->type == MINO_MAP) {
        return map_assoc_pairs(coll, args->as.cons.cdr, extra_pairs);
    }
    {
        char msg[96];
        snprintf(msg, sizeof(msg), "assoc: expected a map or vector, got %s",
                 type_tag_str(coll));
        set_error(msg);
    }
    return NULL;
}

static mino_val_t *prim_get(mino_val_t *args, mino_env_t *env)
{
    mino_val_t *coll;
    mino_val_t *key;
    mino_val_t *def_val = mino_nil(S_);
    size_t      n;
    (void)env;
    arg_count(args, &n);
    if (n != 2 && n != 3) {
        set_error("get requires 2 or 3 arguments");
        return NULL;
    }
    coll = args->as.cons.car;
    key  = args->as.cons.cdr->as.cons.car;
    if (n == 3) {
        def_val = args->as.cons.cdr->as.cons.cdr->as.cons.car;
    }
    if (coll == NULL || coll->type == MINO_NIL) {
        return def_val;
    }
    if (coll->type == MINO_MAP) {
        mino_val_t *v = map_get_val(coll, key);
        return v == NULL ? def_val : v;
    }
    if (coll->type == MINO_VECTOR) {
        long long idx;
        if (key == NULL || key->type != MINO_INT) {
            return def_val;
        }
        idx = key->as.i;
        if (idx < 0 || (size_t)idx >= coll->as.vec.len) {
            return def_val;
        }
        return vec_nth(coll, (size_t)idx);
    }
    if (coll->type == MINO_SET) {
        uint32_t h = hash_val(key);
        mino_val_t *found = hamt_get(coll->as.set.root, key, h, 0u);
        return found != NULL ? key : def_val;
    }
    return def_val;
}

static mino_val_t *prim_conj(mino_val_t *args, mino_env_t *env)
{
    mino_val_t *coll;
    size_t      n;
    mino_val_t *p;
    (void)env;
    arg_count(args, &n);
    if (n < 2) {
        set_error("conj requires a collection and at least one item");
        return NULL;
    }
    coll = args->as.cons.car;
    p    = args->as.cons.cdr;
    if (coll == NULL || coll->type == MINO_NIL || coll->type == MINO_CONS) {
        /* List/nil: prepend each item so (conj '(1 2) 3 4) => (4 3 1 2). */
        mino_val_t *out = (coll == NULL || coll->type == MINO_NIL)
            ? mino_nil(S_) : coll;
        while (mino_is_cons(p)) {
            out = mino_cons(S_, p->as.cons.car, out);
            p = p->as.cons.cdr;
        }
        return out;
    }
    if (coll->type == MINO_VECTOR) {
        size_t extra = n - 1;
        mino_val_t *acc = coll;
        size_t i;
        for (i = 0; i < extra; i++) {
            acc = vec_conj1(acc, p->as.cons.car);
            p = p->as.cons.cdr;
        }
        return acc;
    }
    if (coll->type == MINO_MAP) {
        /* Each added item must be a 2-element vector [k v]. Assoc each onto
         * the accumulator so successor maps share structure with the source. */
        size_t      extra = n - 1;
        mino_val_t *acc   = coll;
        size_t      i;
        for (i = 0; i < extra; i++) {
            mino_val_t *item = p->as.cons.car;
            mino_val_t *pair_args;
            if (item == NULL || item->type != MINO_VECTOR
                || item->as.vec.len != 2) {
                set_error("conj on map requires 2-element vectors");
                return NULL;
            }
            pair_args = mino_cons(S_, vec_nth(item, 0),
                                   mino_cons(S_, vec_nth(item, 1), mino_nil(S_)));
            acc = map_assoc_pairs(acc, pair_args, 1);
            p = p->as.cons.cdr;
        }
        return acc;
    }
    if (coll->type == MINO_SET) {
        mino_val_t *acc = coll;
        while (mino_is_cons(p)) {
            acc = set_conj1(acc, p->as.cons.car);
            p = p->as.cons.cdr;
        }
        return acc;
    }
    {
        char msg[96];
        snprintf(msg, sizeof(msg),
                 "conj: expected a list, vector, map, or set, got %s",
                 type_tag_str(coll));
        set_error(msg);
    }
    return NULL;
}

static mino_val_t *prim_keys(mino_val_t *args, mino_env_t *env)
{
    mino_val_t *coll;
    mino_val_t *head = mino_nil(S_);
    mino_val_t *tail = NULL;
    size_t i;
    (void)env;
    if (!mino_is_cons(args) || mino_is_cons(args->as.cons.cdr)) {
        set_error("keys requires one argument");
        return NULL;
    }
    coll = args->as.cons.car;
    if (coll == NULL || coll->type == MINO_NIL) {
        return mino_nil(S_);
    }
    if (coll->type != MINO_MAP) {
        set_error("keys: argument must be a map");
        return NULL;
    }
    for (i = 0; i < coll->as.map.len; i++) {
        mino_val_t *cell = mino_cons(S_, vec_nth(coll->as.map.key_order, i),
                                      mino_nil(S_));
        if (tail == NULL) {
            head = cell;
        } else {
            tail->as.cons.cdr = cell;
        }
        tail = cell;
    }
    return head;
}

static mino_val_t *prim_vals(mino_val_t *args, mino_env_t *env)
{
    mino_val_t *coll;
    mino_val_t *head = mino_nil(S_);
    mino_val_t *tail = NULL;
    size_t i;
    (void)env;
    if (!mino_is_cons(args) || mino_is_cons(args->as.cons.cdr)) {
        set_error("vals requires one argument");
        return NULL;
    }
    coll = args->as.cons.car;
    if (coll == NULL || coll->type == MINO_NIL) {
        return mino_nil(S_);
    }
    if (coll->type != MINO_MAP) {
        set_error("vals: argument must be a map");
        return NULL;
    }
    for (i = 0; i < coll->as.map.len; i++) {
        mino_val_t *key  = vec_nth(coll->as.map.key_order, i);
        mino_val_t *cell = mino_cons(S_, map_get_val(coll, key), mino_nil(S_));
        if (tail == NULL) {
            head = cell;
        } else {
            tail->as.cons.cdr = cell;
        }
        tail = cell;
    }
    return head;
}

/* Set helper: add one element to a set, returning a new set. */
static mino_val_t *set_conj1(const mino_val_t *s, mino_val_t *elem)
{
    mino_val_t       *v        = alloc_val(MINO_SET);
    mino_val_t       *sentinel = mino_true(S_);
    hamt_entry_t     *e        = hamt_entry_new(elem, sentinel);
    uint32_t          h        = hash_val(elem);
    int               replaced = 0;
    mino_hamt_node_t *root     = hamt_assoc(s->as.set.root, e, h, 0u, &replaced);
    v->as.set.root      = root;
    if (replaced) {
        v->as.set.key_order = s->as.set.key_order;
        v->as.set.len       = s->as.set.len;
    } else {
        v->as.set.key_order = vec_conj1(s->as.set.key_order, elem);
        v->as.set.len       = s->as.set.len + 1;
    }
    return v;
}

static mino_val_t *prim_hash_set(mino_val_t *args, mino_env_t *env)
{
    size_t      n;
    size_t      i;
    mino_val_t **tmp;
    mino_val_t *p;
    (void)env;
    arg_count(args, &n);
    tmp = (mino_val_t **)gc_alloc_typed(GC_T_VALARR, (n > 0 ? n : 1) * sizeof(*tmp));
    p = args;
    for (i = 0; i < n; i++) {
        tmp[i] = p->as.cons.car;
        p = p->as.cons.cdr;
    }
    return mino_set(S_, tmp, n);
}

static mino_val_t *prim_contains_p(mino_val_t *args, mino_env_t *env)
{
    mino_val_t *coll;
    mino_val_t *key;
    (void)env;
    if (!mino_is_cons(args) || !mino_is_cons(args->as.cons.cdr)
        || mino_is_cons(args->as.cons.cdr->as.cons.cdr)) {
        set_error("contains? requires two arguments");
        return NULL;
    }
    coll = args->as.cons.car;
    key  = args->as.cons.cdr->as.cons.car;
    if (coll == NULL || coll->type == MINO_NIL) {
        return mino_false(S_);
    }
    if (coll->type == MINO_MAP) {
        return map_get_val(coll, key) != NULL ? mino_true(S_) : mino_false(S_);
    }
    if (coll->type == MINO_SET) {
        uint32_t h = hash_val(key);
        return hamt_get(coll->as.set.root, key, h, 0u) != NULL
            ? mino_true(S_) : mino_false(S_);
    }
    if (coll->type == MINO_VECTOR) {
        /* For vectors, key is an index. */
        if (key != NULL && key->type == MINO_INT) {
            long long idx = key->as.i;
            return (idx >= 0 && (size_t)idx < coll->as.vec.len)
                ? mino_true(S_) : mino_false(S_);
        }
        return mino_false(S_);
    }
    {
        char msg[96];
        snprintf(msg, sizeof(msg),
                 "contains?: expected a map, set, or vector, got %s",
                 type_tag_str(coll));
        set_error(msg);
    }
    return NULL;
}

static mino_val_t *prim_disj(mino_val_t *args, mino_env_t *env)
{
    mino_val_t *coll;
    mino_val_t *p;
    size_t      n;
    (void)env;
    arg_count(args, &n);
    if (n < 2) {
        set_error("disj requires a set and at least one key");
        return NULL;
    }
    coll = args->as.cons.car;
    if (coll == NULL || coll->type == MINO_NIL) {
        return mino_nil(S_);
    }
    if (coll->type != MINO_SET) {
        set_error("disj: first argument must be a set");
        return NULL;
    }
    /* Rebuild set excluding the specified elements. Not the most efficient
     * approach, but keeps the code simple and correct. */
    p = args->as.cons.cdr;
    while (mino_is_cons(p)) {
        mino_val_t *key = p->as.cons.car;
        uint32_t    h   = hash_val(key);
        if (hamt_get(coll->as.set.root, key, h, 0u) != NULL) {
            /* Element exists; rebuild without it. */
            mino_val_t *new_set = alloc_val(MINO_SET);
            mino_val_t *order   = mino_vector(S_, NULL, 0);
            mino_hamt_node_t *root = NULL;
            size_t i;
            size_t new_len = 0;
            for (i = 0; i < coll->as.set.len; i++) {
                mino_val_t *elem = vec_nth(coll->as.set.key_order, i);
                if (!mino_eq(elem, key)) {
                    hamt_entry_t *e2 = hamt_entry_new(elem, mino_true(S_));
                    uint32_t h2 = hash_val(elem);
                    int rep = 0;
                    root = hamt_assoc(root, e2, h2, 0u, &rep);
                    order = vec_conj1(order, elem);
                    new_len++;
                }
            }
            new_set->as.set.root      = root;
            new_set->as.set.key_order = order;
            new_set->as.set.len       = new_len;
            coll = new_set;
        }
        p = p->as.cons.cdr;
    }
    return coll;
}

static mino_val_t *prim_dissoc(mino_val_t *args, mino_env_t *env)
{
    mino_val_t *coll;
    mino_val_t *p;
    size_t      n;
    (void)env;
    arg_count(args, &n);
    if (n < 2) {
        set_error("dissoc requires a map and at least one key");
        return NULL;
    }
    coll = args->as.cons.car;
    if (coll == NULL || coll->type == MINO_NIL) {
        return mino_nil(S_);
    }
    if (coll->type != MINO_MAP) {
        set_error("dissoc: first argument must be a map");
        return NULL;
    }
    p = args->as.cons.cdr;
    while (mino_is_cons(p)) {
        mino_val_t *key = p->as.cons.car;
        uint32_t    h   = hash_val(key);
        if (hamt_get(coll->as.map.root, key, h, 0u) != NULL) {
            mino_val_t *new_map = alloc_val(MINO_MAP);
            mino_val_t *order   = mino_vector(S_, NULL, 0);
            mino_hamt_node_t *root = NULL;
            size_t i;
            size_t new_len = 0;
            for (i = 0; i < coll->as.map.len; i++) {
                mino_val_t *k = vec_nth(coll->as.map.key_order, i);
                if (!mino_eq(k, key)) {
                    mino_val_t   *v  = map_get_val(coll, k);
                    hamt_entry_t *e2 = hamt_entry_new(k, v);
                    uint32_t      h2 = hash_val(k);
                    int rep = 0;
                    root = hamt_assoc(root, e2, h2, 0u, &rep);
                    order = vec_conj1(order, k);
                    new_len++;
                }
            }
            new_map->as.map.root      = root;
            new_map->as.map.key_order = order;
            new_map->as.map.len       = new_len;
            coll = new_map;
        }
        p = p->as.cons.cdr;
    }
    return coll;
}

/* ------------------------------------------------------------------------- */
/* Sequence primitives (strict — no lazy seqs)                               */
/* ------------------------------------------------------------------------- */

/*
 * Helper: build a freshly consed list from a collection. Works on lists,
 * vectors, maps (key-value vectors), and sets. Returns a (head, tail) pair
 * through pointers so the caller can append efficiently.
 */

/* Iterator abstraction over any sequential collection. */
typedef struct {
    const mino_val_t *coll;
    size_t            idx;       /* for vectors, maps, sets */
    const mino_val_t *cons_p;   /* for cons lists */
} seq_iter_t;

static void seq_iter_init(seq_iter_t *it, const mino_val_t *coll)
{
    /* Force lazy seqs so they behave as cons lists. */
    if (coll != NULL && coll->type == MINO_LAZY) {
        coll = lazy_force((mino_val_t *)coll);
    }
    it->coll  = coll;
    it->idx   = 0;
    it->cons_p = (coll != NULL && coll->type == MINO_CONS) ? coll : NULL;
}

static int seq_iter_done(const seq_iter_t *it)
{
    const mino_val_t *c = it->coll;
    if (c == NULL || c->type == MINO_NIL) return 1;
    switch (c->type) {
    case MINO_CONS:   return it->cons_p == NULL || it->cons_p->type != MINO_CONS;
    case MINO_VECTOR: return it->idx >= c->as.vec.len;
    case MINO_MAP:    return it->idx >= c->as.map.len;
    case MINO_SET:    return it->idx >= c->as.set.len;
    case MINO_STRING: return it->idx >= c->as.s.len;
    default:          return 1;
    }
}

static mino_val_t *seq_iter_val(const seq_iter_t *it)
{
    const mino_val_t *c = it->coll;
    switch (c->type) {
    case MINO_CONS:   return it->cons_p->as.cons.car;
    case MINO_VECTOR: return vec_nth(c, it->idx);
    case MINO_MAP: {
        /* Yield [key value] vectors for maps. */
        mino_val_t *key = vec_nth(c->as.map.key_order, it->idx);
        mino_val_t *val = map_get_val(c, key);
        mino_val_t *kv[2];
        kv[0] = key;
        kv[1] = val;
        return mino_vector(S_, kv, 2);
    }
    case MINO_SET:    return vec_nth(c->as.set.key_order, it->idx);
    case MINO_STRING: return mino_string_n(S_, c->as.s.data + it->idx, 1);
    default:          return mino_nil(S_);
    }
}

static void seq_iter_next(seq_iter_t *it)
{
    if (it->coll != NULL && it->coll->type == MINO_CONS) {
        if (it->cons_p != NULL && it->cons_p->type == MINO_CONS) {
            const mino_val_t *next = it->cons_p->as.cons.cdr;
            /* Force lazy tail if present. */
            if (next != NULL && next->type == MINO_LAZY) {
                next = lazy_force((mino_val_t *)next);
            }
            it->cons_p = next;
        }
    } else {
        it->idx++;
    }
}

/* (map, filter are now lazy in core.mino) */

static mino_val_t *prim_reduce(mino_val_t *args, mino_env_t *env)
{
    mino_val_t *fn;
    mino_val_t *acc;
    mino_val_t *coll;
    seq_iter_t  it;
    size_t      n;
    arg_count(args, &n);
    if (n == 2) {
        /* (reduce f coll) — first element is the initial accumulator. */
        fn   = args->as.cons.car;
        coll = args->as.cons.cdr->as.cons.car;
        if (coll == NULL || coll->type == MINO_NIL) {
            /* (reduce f nil) → (f) */
            return apply_callable(fn, mino_nil(S_), env);
        }
        seq_iter_init(&it, coll);
        if (seq_iter_done(&it)) {
            return apply_callable(fn, mino_nil(S_), env);
        }
        acc = seq_iter_val(&it);
        seq_iter_next(&it);
    } else if (n == 3) {
        /* (reduce f init coll) */
        fn   = args->as.cons.car;
        acc  = args->as.cons.cdr->as.cons.car;
        coll = args->as.cons.cdr->as.cons.cdr->as.cons.car;
        if (coll == NULL || coll->type == MINO_NIL) {
            return acc;
        }
        seq_iter_init(&it, coll);
    } else {
        set_error("reduce requires 2 or 3 arguments");
        return NULL;
    }
    while (!seq_iter_done(&it)) {
        mino_val_t *elem   = seq_iter_val(&it);
        mino_val_t *call_a = mino_cons(S_, acc, mino_cons(S_, elem, mino_nil(S_)));
        acc = apply_callable(fn, call_a, env);
        if (acc == NULL) return NULL;
        seq_iter_next(&it);
    }
    return acc;
}

/* (take, drop, range, repeat, concat are now lazy in core.mino) */

static mino_val_t *prim_into(mino_val_t *args, mino_env_t *env)
{
    mino_val_t *to;
    mino_val_t *from;
    seq_iter_t  it;
    (void)env;
    if (!mino_is_cons(args) || !mino_is_cons(args->as.cons.cdr)) {
        set_error("into requires two arguments");
        return NULL;
    }
    to   = args->as.cons.car;
    from = args->as.cons.cdr->as.cons.car;
    if (from == NULL || from->type == MINO_NIL) {
        return to;
    }
    /* Conj each element of `from` into `to`. The type of `to` determines
     * the conj semantics (vector appends, list prepends, map/set merges). */
    if (to == NULL || to->type == MINO_NIL) {
        /* Into nil: build a list. */
        mino_val_t *out = mino_nil(S_);
        seq_iter_init(&it, from);
        while (!seq_iter_done(&it)) {
            out = mino_cons(S_, seq_iter_val(&it), out);
            seq_iter_next(&it);
        }
        return out;
    }
    if (to->type == MINO_VECTOR) {
        mino_val_t *acc = to;
        seq_iter_init(&it, from);
        while (!seq_iter_done(&it)) {
            acc = vec_conj1(acc, seq_iter_val(&it));
            seq_iter_next(&it);
        }
        return acc;
    }
    if (to->type == MINO_MAP) {
        mino_val_t *acc = to;
        seq_iter_init(&it, from);
        while (!seq_iter_done(&it)) {
            mino_val_t *item = seq_iter_val(&it);
            mino_val_t *pair_args;
            if (item == NULL || item->type != MINO_VECTOR
                || item->as.vec.len != 2) {
                set_error("into map: each element must be a 2-element vector");
                return NULL;
            }
            pair_args = mino_cons(S_, vec_nth(item, 0),
                                   mino_cons(S_, vec_nth(item, 1), mino_nil(S_)));
            acc = map_assoc_pairs(acc, pair_args, 1);
            seq_iter_next(&it);
        }
        return acc;
    }
    if (to->type == MINO_SET) {
        mino_val_t *acc = to;
        seq_iter_init(&it, from);
        while (!seq_iter_done(&it)) {
            acc = set_conj1(acc, seq_iter_val(&it));
            seq_iter_next(&it);
        }
        return acc;
    }
    if (to->type == MINO_CONS) {
        mino_val_t *out = to;
        seq_iter_init(&it, from);
        while (!seq_iter_done(&it)) {
            out = mino_cons(S_, seq_iter_val(&it), out);
            seq_iter_next(&it);
        }
        return out;
    }
    {
        char msg[96];
        snprintf(msg, sizeof(msg),
                 "into: expected a list, vector, map, or set as target, got %s",
                 type_tag_str(to));
        set_error(msg);
    }
    return NULL;
}

static mino_val_t *prim_apply(mino_val_t *args, mino_env_t *env)
{
    mino_val_t *fn;
    mino_val_t *last;
    mino_val_t *call_args;
    mino_val_t *p;
    size_t      n;
    arg_count(args, &n);
    if (n < 2) {
        set_error("apply requires a function and arguments");
        return NULL;
    }
    fn = args->as.cons.car;
    if (n == 2) {
        /* (apply f coll) — spread coll as args. */
        last = args->as.cons.cdr->as.cons.car;
    } else {
        /* (apply f a b ... coll) — prepend individual args, then spread coll. */
        mino_val_t *head = mino_nil(S_);
        mino_val_t *tail2 = NULL;
        p = args->as.cons.cdr;
        /* Collect all but the last arg as individual args. */
        while (mino_is_cons(p) && mino_is_cons(p->as.cons.cdr)) {
            mino_val_t *cell = mino_cons(S_, p->as.cons.car, mino_nil(S_));
            if (tail2 == NULL) { head = cell; } else { tail2->as.cons.cdr = cell; }
            tail2 = cell;
            p = p->as.cons.cdr;
        }
        last = p->as.cons.car; /* the final collection argument */
        /* Append elements from `last` collection. */
        if (last != NULL && last->type != MINO_NIL) {
            seq_iter_t it;
            seq_iter_init(&it, last);
            while (!seq_iter_done(&it)) {
                mino_val_t *cell = mino_cons(S_, seq_iter_val(&it), mino_nil(S_));
                if (tail2 == NULL) { head = cell; } else { tail2->as.cons.cdr = cell; }
                tail2 = cell;
                seq_iter_next(&it);
            }
        }
        return apply_callable(fn, head, env);
    }
    /* (apply f coll) — convert coll to a cons arg list. */
    if (last == NULL || last->type == MINO_NIL) {
        return apply_callable(fn, mino_nil(S_), env);
    }
    if (last->type == MINO_CONS) {
        return apply_callable(fn, last, env);
    }
    /* Convert non-list collection to cons list. */
    {
        mino_val_t *head = mino_nil(S_);
        mino_val_t *tail2 = NULL;
        seq_iter_t it;
        seq_iter_init(&it, last);
        while (!seq_iter_done(&it)) {
            mino_val_t *cell = mino_cons(S_, seq_iter_val(&it), mino_nil(S_));
            if (tail2 == NULL) { head = cell; } else { tail2->as.cons.cdr = cell; }
            tail2 = cell;
            seq_iter_next(&it);
        }
        call_args = head;
    }
    return apply_callable(fn, call_args, env);
}

static mino_val_t *prim_reverse(mino_val_t *args, mino_env_t *env)
{
    mino_val_t *coll;
    mino_val_t *out = mino_nil(S_);
    seq_iter_t  it;
    (void)env;
    if (!mino_is_cons(args) || mino_is_cons(args->as.cons.cdr)) {
        set_error("reverse requires one argument");
        return NULL;
    }
    coll = args->as.cons.car;
    if (coll == NULL || coll->type == MINO_NIL) {
        return mino_nil(S_);
    }
    seq_iter_init(&it, coll);
    while (!seq_iter_done(&it)) {
        out = mino_cons(S_, seq_iter_val(&it), out);
        seq_iter_next(&it);
    }
    return out;
}

static mino_val_t *prim_sort(mino_val_t *args, mino_env_t *env);

/* Simple comparison function for sorting: numbers by value, strings
 * lexicographically, other types by type tag then identity. */
static int val_compare(const mino_val_t *a, const mino_val_t *b)
{
    if (a == b) return 0;
    if (a == NULL || a->type == MINO_NIL) return -1;
    if (b == NULL || b->type == MINO_NIL) return 1;
    if (a->type == MINO_INT && b->type == MINO_INT) {
        return a->as.i < b->as.i ? -1 : a->as.i > b->as.i ? 1 : 0;
    }
    if (a->type == MINO_FLOAT && b->type == MINO_FLOAT) {
        return a->as.f < b->as.f ? -1 : a->as.f > b->as.f ? 1 : 0;
    }
    if (a->type == MINO_INT && b->type == MINO_FLOAT) {
        double da = (double)a->as.i;
        return da < b->as.f ? -1 : da > b->as.f ? 1 : 0;
    }
    if (a->type == MINO_FLOAT && b->type == MINO_INT) {
        double db = (double)b->as.i;
        return a->as.f < db ? -1 : a->as.f > db ? 1 : 0;
    }
    if ((a->type == MINO_STRING || a->type == MINO_SYMBOL || a->type == MINO_KEYWORD)
        && a->type == b->type) {
        size_t min_len = a->as.s.len < b->as.s.len ? a->as.s.len : b->as.s.len;
        int c = memcmp(a->as.s.data, b->as.s.data, min_len);
        if (c != 0) return c;
        return a->as.s.len < b->as.s.len ? -1 : a->as.s.len > b->as.s.len ? 1 : 0;
    }
    /* Fall back to type tag ordering. */
    return a->type < b->type ? -1 : a->type > b->type ? 1 : 0;
}

/* Sort comparator state: when sort_comp_fn is non-NULL, the merge sort
 * calls the user-supplied comparison function instead of val_compare. */

static int sort_compare(const mino_val_t *a, const mino_val_t *b)
{
    if (sort_comp_fn != NULL) {
        mino_val_t *call_args = mino_cons(S_, (mino_val_t *)a,
                                  mino_cons(S_, (mino_val_t *)b, mino_nil(S_)));
        mino_val_t *result = mino_call(S_, sort_comp_fn, call_args, sort_comp_env);
        if (result == NULL) return 0;
        /* Numeric result: use sign directly (compare-style) */
        if (result->type == MINO_INT) {
            return result->as.i < 0 ? -1 : result->as.i > 0 ? 1 : 0;
        }
        if (result->type == MINO_FLOAT) {
            return result->as.f < 0 ? -1 : result->as.f > 0 ? 1 : 0;
        }
        /* Boolean result: true means a < b, false means a >= b */
        return mino_is_truthy(result) ? -1 : 1;
    }
    return val_compare(a, b);
}

/* Merge sort for mino_val_t* arrays. */
static void merge_sort_vals(mino_val_t **arr, mino_val_t **tmp, size_t len)
{
    size_t mid, i, j, k;
    if (len <= 1) return;
    mid = len / 2;
    merge_sort_vals(arr, tmp, mid);
    merge_sort_vals(arr + mid, tmp, len - mid);
    memcpy(tmp, arr, mid * sizeof(*tmp));
    i = 0; j = mid; k = 0;
    while (i < mid && j < len) {
        if (sort_compare(tmp[i], arr[j]) <= 0) {
            arr[k++] = tmp[i++];
        } else {
            arr[k++] = arr[j++];
        }
    }
    while (i < mid) { arr[k++] = tmp[i++]; }
}

/* (sort coll) or (sort comp coll) */
static mino_val_t *prim_sort(mino_val_t *args, mino_env_t *env)
{
    mino_val_t *coll;
    mino_val_t *comp = NULL;
    mino_val_t **arr;
    mino_val_t **tmp;
    mino_val_t *head = mino_nil(S_);
    mino_val_t *tail = NULL;
    size_t      n_items, i;
    seq_iter_t  it;
    if (!mino_is_cons(args)) {
        set_error("sort requires one or two arguments");
        return NULL;
    }
    if (mino_is_cons(args->as.cons.cdr) &&
        !mino_is_cons(args->as.cons.cdr->as.cons.cdr)) {
        /* Two args: (sort comp coll) */
        comp = args->as.cons.car;
        coll = args->as.cons.cdr->as.cons.car;
    } else if (!mino_is_cons(args->as.cons.cdr)) {
        /* One arg: (sort coll) */
        coll = args->as.cons.car;
    } else {
        set_error("sort requires one or two arguments");
        return NULL;
    }
    if (coll == NULL || coll->type == MINO_NIL) {
        return mino_nil(S_);
    }
    /* Collect elements into an array. */
    n_items = 0;
    seq_iter_init(&it, coll);
    while (!seq_iter_done(&it)) { n_items++; seq_iter_next(&it); }
    if (n_items == 0) return mino_nil(S_);
    arr = (mino_val_t **)gc_alloc_typed(GC_T_VALARR, n_items * sizeof(*arr));
    tmp = (mino_val_t **)gc_alloc_typed(GC_T_VALARR, n_items * sizeof(*tmp));
    i = 0;
    seq_iter_init(&it, coll);
    while (!seq_iter_done(&it)) { arr[i++] = seq_iter_val(&it); seq_iter_next(&it); }
    sort_comp_fn  = comp;
    sort_comp_env = env;
    merge_sort_vals(arr, tmp, n_items);
    sort_comp_fn  = NULL;
    sort_comp_env = NULL;
    for (i = 0; i < n_items; i++) {
        mino_val_t *cell = mino_cons(S_, arr[i], mino_nil(S_));
        if (tail == NULL) { head = cell; } else { tail->as.cons.cdr = cell; }
        tail = cell;
    }
    return head;
}

/* ------------------------------------------------------------------------- */
/* String primitives                                                         */
/* ------------------------------------------------------------------------- */

static mino_val_t *prim_subs(mino_val_t *args, mino_env_t *env)
{
    mino_val_t *s_val;
    long long   start, end_idx;
    size_t      n;
    (void)env;
    arg_count(args, &n);
    if (n != 2 && n != 3) {
        set_error("subs requires 2 or 3 arguments");
        return NULL;
    }
    s_val = args->as.cons.car;
    if (s_val == NULL || s_val->type != MINO_STRING) {
        set_error("subs: first argument must be a string");
        return NULL;
    }
    if (args->as.cons.cdr->as.cons.car == NULL
        || args->as.cons.cdr->as.cons.car->type != MINO_INT) {
        set_error("subs: start index must be an integer");
        return NULL;
    }
    start = args->as.cons.cdr->as.cons.car->as.i;
    if (n == 3) {
        if (args->as.cons.cdr->as.cons.cdr->as.cons.car == NULL
            || args->as.cons.cdr->as.cons.cdr->as.cons.car->type != MINO_INT) {
            set_error("subs: end index must be an integer");
            return NULL;
        }
        end_idx = args->as.cons.cdr->as.cons.cdr->as.cons.car->as.i;
    } else {
        end_idx = (long long)s_val->as.s.len;
    }
    if (start < 0 || end_idx < start || (size_t)end_idx > s_val->as.s.len) {
        return prim_throw_error("subs: index out of range");
    }
    return mino_string_n(S_, s_val->as.s.data + start, (size_t)(end_idx - start));
}

static mino_val_t *prim_split(mino_val_t *args, mino_env_t *env)
{
    mino_val_t  *s_val;
    mino_val_t  *sep_val;
    const char  *s;
    size_t       slen;
    const char  *sep;
    size_t       sep_len;
    mino_val_t **buf = NULL;
    size_t       cap = 0, len = 0;
    const char  *p;
    (void)env;
    if (!mino_is_cons(args) || !mino_is_cons(args->as.cons.cdr)) {
        set_error("split requires a string and a separator");
        return NULL;
    }
    s_val   = args->as.cons.car;
    sep_val = args->as.cons.cdr->as.cons.car;
    if (s_val == NULL || s_val->type != MINO_STRING
        || sep_val == NULL || sep_val->type != MINO_STRING) {
        set_error("split: both arguments must be strings");
        return NULL;
    }
    s       = s_val->as.s.data;
    slen    = s_val->as.s.len;
    sep     = sep_val->as.s.data;
    sep_len = sep_val->as.s.len;
    p       = s;
    if (sep_len == 0) {
        /* Split into individual characters. */
        size_t i;
        buf = (mino_val_t **)gc_alloc_typed(GC_T_VALARR,
              (slen > 0 ? slen : 1) * sizeof(*buf));
        for (i = 0; i < slen; i++) {
            buf[i] = mino_string_n(S_, s + i, 1);
        }
        return mino_vector(S_, buf, slen);
    }
    while (p <= s + slen) {
        const char *found = NULL;
        const char *q;
        for (q = p; q + sep_len <= s + slen; q++) {
            if (memcmp(q, sep, sep_len) == 0) {
                found = q;
                break;
            }
        }
        if (len == cap) {
            size_t new_cap = cap == 0 ? 8 : cap * 2;
            mino_val_t **nb = (mino_val_t **)gc_alloc_typed(
                GC_T_VALARR, new_cap * sizeof(*nb));
            if (buf != NULL && len > 0) memcpy(nb, buf, len * sizeof(*nb));
            buf = nb;
            cap = new_cap;
        }
        if (found != NULL) {
            buf[len++] = mino_string_n(S_, p, (size_t)(found - p));
            p = found + sep_len;
        } else {
            buf[len++] = mino_string_n(S_, p, (size_t)(s + slen - p));
            break;
        }
    }
    return mino_vector(S_, buf, len);
}

static mino_val_t *prim_join(mino_val_t *args, mino_env_t *env)
{
    mino_val_t  *sep_val;
    mino_val_t  *coll;
    const char  *sep = "";
    size_t       sep_len = 0;
    char        *buf = NULL;
    size_t       buf_len = 0, buf_cap = 0;
    int          first = 1;
    seq_iter_t   it;
    size_t       n;
    (void)env;
    arg_count(args, &n);
    if (n == 1) {
        /* (join coll) — no separator. */
        coll = args->as.cons.car;
    } else if (n == 2) {
        /* (join sep coll) */
        sep_val = args->as.cons.car;
        coll    = args->as.cons.cdr->as.cons.car;
        if (sep_val == NULL || sep_val->type != MINO_STRING) {
            set_error("join: separator must be a string");
            return NULL;
        }
        sep     = sep_val->as.s.data;
        sep_len = sep_val->as.s.len;
    } else {
        set_error("join requires 1 or 2 arguments");
        return NULL;
    }
    if (coll == NULL || coll->type == MINO_NIL) {
        return mino_string(S_, "");
    }
    seq_iter_init(&it, coll);
    while (!seq_iter_done(&it)) {
        mino_val_t *elem = seq_iter_val(&it);
        const char *part;
        size_t      part_len;
        size_t      need;
        if (elem == NULL || elem->type == MINO_NIL) {
            seq_iter_next(&it);
            continue;
        }
        if (elem->type == MINO_STRING) {
            part     = elem->as.s.data;
            part_len = elem->as.s.len;
        } else {
            /* Convert to string. */
            mino_val_t *str_a = mino_cons(S_, elem, mino_nil(S_));
            mino_val_t *str   = prim_str(str_a, env);
            if (str == NULL) return NULL;
            part     = str->as.s.data;
            part_len = str->as.s.len;
        }
        need = buf_len + (first ? 0 : sep_len) + part_len + 1;
        if (need > buf_cap) {
            buf_cap = buf_cap == 0 ? 128 : buf_cap;
            while (buf_cap < need) buf_cap *= 2;
            buf = (char *)realloc(buf, buf_cap);
            if (buf == NULL) { set_error("out of memory"); return NULL; }
        }
        if (!first && sep_len > 0) {
            memcpy(buf + buf_len, sep, sep_len);
            buf_len += sep_len;
        }
        memcpy(buf + buf_len, part, part_len);
        buf_len += part_len;
        first = 0;
        seq_iter_next(&it);
    }
    {
        mino_val_t *result = mino_string_n(S_, buf != NULL ? buf : "", buf_len);
        free(buf);
        return result;
    }
}

static mino_val_t *prim_starts_with_p(mino_val_t *args, mino_env_t *env)
{
    mino_val_t *s, *prefix;
    (void)env;
    if (!mino_is_cons(args) || !mino_is_cons(args->as.cons.cdr)) {
        set_error("starts-with? requires two string arguments");
        return NULL;
    }
    s      = args->as.cons.car;
    prefix = args->as.cons.cdr->as.cons.car;
    if (s == NULL || s->type != MINO_STRING
        || prefix == NULL || prefix->type != MINO_STRING) {
        set_error("starts-with? requires two string arguments");
        return NULL;
    }
    if (prefix->as.s.len > s->as.s.len) return mino_false(S_);
    return memcmp(s->as.s.data, prefix->as.s.data, prefix->as.s.len) == 0
        ? mino_true(S_) : mino_false(S_);
}

static mino_val_t *prim_ends_with_p(mino_val_t *args, mino_env_t *env)
{
    mino_val_t *s, *suffix;
    (void)env;
    if (!mino_is_cons(args) || !mino_is_cons(args->as.cons.cdr)) {
        set_error("ends-with? requires two string arguments");
        return NULL;
    }
    s      = args->as.cons.car;
    suffix = args->as.cons.cdr->as.cons.car;
    if (s == NULL || s->type != MINO_STRING
        || suffix == NULL || suffix->type != MINO_STRING) {
        set_error("ends-with? requires two string arguments");
        return NULL;
    }
    if (suffix->as.s.len > s->as.s.len) return mino_false(S_);
    return memcmp(s->as.s.data + s->as.s.len - suffix->as.s.len,
                  suffix->as.s.data, suffix->as.s.len) == 0
        ? mino_true(S_) : mino_false(S_);
}

static mino_val_t *prim_includes_p(mino_val_t *args, mino_env_t *env)
{
    mino_val_t *s, *sub;
    const char *p;
    (void)env;
    if (!mino_is_cons(args) || !mino_is_cons(args->as.cons.cdr)) {
        set_error("includes? requires two string arguments");
        return NULL;
    }
    s   = args->as.cons.car;
    sub = args->as.cons.cdr->as.cons.car;
    if (s == NULL || s->type != MINO_STRING
        || sub == NULL || sub->type != MINO_STRING) {
        set_error("includes? requires two string arguments");
        return NULL;
    }
    if (sub->as.s.len == 0) return mino_true(S_);
    if (sub->as.s.len > s->as.s.len) return mino_false(S_);
    for (p = s->as.s.data; p + sub->as.s.len <= s->as.s.data + s->as.s.len; p++) {
        if (memcmp(p, sub->as.s.data, sub->as.s.len) == 0) {
            return mino_true(S_);
        }
    }
    return mino_false(S_);
}

static mino_val_t *prim_upper_case(mino_val_t *args, mino_env_t *env)
{
    mino_val_t *s;
    char       *buf;
    size_t      i;
    (void)env;
    if (!mino_is_cons(args) || mino_is_cons(args->as.cons.cdr)) {
        set_error("upper-case requires one string argument");
        return NULL;
    }
    s = args->as.cons.car;
    if (s == NULL || s->type != MINO_STRING) {
        set_error("upper-case requires one string argument");
        return NULL;
    }
    buf = (char *)malloc(s->as.s.len);
    if (buf == NULL && s->as.s.len > 0) {
        set_error("out of memory"); return NULL;
    }
    for (i = 0; i < s->as.s.len; i++) {
        buf[i] = (char)toupper((unsigned char)s->as.s.data[i]);
    }
    {
        mino_val_t *result = mino_string_n(S_, buf, s->as.s.len);
        free(buf);
        return result;
    }
}

static mino_val_t *prim_lower_case(mino_val_t *args, mino_env_t *env)
{
    mino_val_t *s;
    char       *buf;
    size_t      i;
    (void)env;
    if (!mino_is_cons(args) || mino_is_cons(args->as.cons.cdr)) {
        set_error("lower-case requires one string argument");
        return NULL;
    }
    s = args->as.cons.car;
    if (s == NULL || s->type != MINO_STRING) {
        set_error("lower-case requires one string argument");
        return NULL;
    }
    buf = (char *)malloc(s->as.s.len);
    if (buf == NULL && s->as.s.len > 0) {
        set_error("out of memory"); return NULL;
    }
    for (i = 0; i < s->as.s.len; i++) {
        buf[i] = (char)tolower((unsigned char)s->as.s.data[i]);
    }
    {
        mino_val_t *result = mino_string_n(S_, buf, s->as.s.len);
        free(buf);
        return result;
    }
}

static mino_val_t *prim_trim(mino_val_t *args, mino_env_t *env)
{
    mino_val_t *s;
    const char *start, *end_ptr;
    (void)env;
    if (!mino_is_cons(args) || mino_is_cons(args->as.cons.cdr)) {
        set_error("trim requires one string argument");
        return NULL;
    }
    s = args->as.cons.car;
    if (s == NULL || s->type != MINO_STRING) {
        set_error("trim requires one string argument");
        return NULL;
    }
    start   = s->as.s.data;
    end_ptr = s->as.s.data + s->as.s.len;
    while (start < end_ptr && isspace((unsigned char)*start)) start++;
    while (end_ptr > start && isspace((unsigned char)*(end_ptr - 1))) end_ptr--;
    return mino_string_n(S_, start, (size_t)(end_ptr - start));
}

/* ------------------------------------------------------------------------- */
/* Utility primitives                                                        */
/* ------------------------------------------------------------------------- */

static mino_val_t *prim_type(mino_val_t *args, mino_env_t *env)
{
    mino_val_t *v;
    (void)env;
    if (!mino_is_cons(args) || mino_is_cons(args->as.cons.cdr)) {
        set_error("type requires one argument");
        return NULL;
    }
    v = args->as.cons.car;
    if (v == NULL || v->type == MINO_NIL)  return mino_keyword(S_, "nil");
    switch (v->type) {
    case MINO_NIL:     return mino_keyword(S_, "nil");
    case MINO_BOOL:    return mino_keyword(S_, "bool");
    case MINO_INT:     return mino_keyword(S_, "int");
    case MINO_FLOAT:   return mino_keyword(S_, "float");
    case MINO_STRING:  return mino_keyword(S_, "string");
    case MINO_SYMBOL:  return mino_keyword(S_, "symbol");
    case MINO_KEYWORD: return mino_keyword(S_, "keyword");
    case MINO_CONS:    return mino_keyword(S_, "list");
    case MINO_VECTOR:  return mino_keyword(S_, "vector");
    case MINO_MAP:     return mino_keyword(S_, "map");
    case MINO_SET:     return mino_keyword(S_, "set");
    case MINO_PRIM:    return mino_keyword(S_, "fn");
    case MINO_FN:      return mino_keyword(S_, "fn");
    case MINO_MACRO:   return mino_keyword(S_, "macro");
    case MINO_HANDLE:  return mino_keyword(S_, "handle");
    case MINO_ATOM:    return mino_keyword(S_, "atom");
    case MINO_LAZY:    return mino_keyword(S_, "lazy-seq");
    case MINO_RECUR:     return mino_keyword(S_, "recur");
    case MINO_TAIL_CALL: return mino_keyword(S_, "tail-call");
    }
    return mino_keyword(S_, "unknown");
}

/*
 * (str & args) — concatenate printed representations. Strings contribute
 * their raw content (no quotes); everything else uses the printer form.
 */
static mino_val_t *prim_str(mino_val_t *args, mino_env_t *env)
{
    char  *buf = NULL;
    size_t len = 0;
    size_t cap = 0;
    (void)env;
    while (mino_is_cons(args)) {
        mino_val_t *a = args->as.cons.car;
        if (a != NULL && a->type == MINO_STRING) {
            /* Append raw string content without quotes. */
            size_t need = len + a->as.s.len + 1;
            if (need > cap) {
                cap = cap == 0 ? 128 : cap;
                while (cap < need) cap *= 2;
                buf = (char *)realloc(buf, cap);
                if (buf == NULL) { set_error("out of memory"); return NULL; }
            }
            memcpy(buf + len, a->as.s.data, a->as.s.len);
            len += a->as.s.len;
        } else if (a != NULL && a->type == MINO_NIL) {
            /* nil contributes nothing. */
        } else if (a == NULL) {
            /* NULL treated as nil. */
        } else {
            /* Print to a temp buffer using the standard printer. */
            char tmp[256];
            int  n;
            switch (a->type) {
            case MINO_BOOL:
                n = snprintf(tmp, sizeof(tmp), "%s", a->as.b ? "true" : "false");
                break;
            case MINO_INT:
                n = snprintf(tmp, sizeof(tmp), "%lld", a->as.i);
                break;
            case MINO_FLOAT: {
                char fb[64];
                int fn2 = snprintf(fb, sizeof(fb), "%g", a->as.f);
                int needs_dot = 1, k;
                for (k = 0; k < fn2; k++) {
                    if (fb[k] == '.' || fb[k] == 'e' || fb[k] == 'E'
                        || fb[k] == 'n' || fb[k] == 'i') {
                        needs_dot = 0; break;
                    }
                }
                if (needs_dot) {
                    fb[fn2] = '.'; fb[fn2+1] = '0'; fb[fn2+2] = '\0';
                    fn2 += 2;
                }
                n = fn2;
                memcpy(tmp, fb, (size_t)n + 1);
                break;
            }
            case MINO_SYMBOL:
            case MINO_KEYWORD: {
                size_t slen = a->as.s.len;
                int    off  = a->type == MINO_KEYWORD ? 1 : 0;
                if (off + slen + 1 > sizeof(tmp)) slen = sizeof(tmp) - off - 1;
                if (off) tmp[0] = ':';
                memcpy(tmp + off, a->as.s.data, slen);
                n = (int)(off + slen);
                tmp[n] = '\0';
                break;
            }
            default: {
                /* Collections (vector, map, set, cons, lazy, atom) and
                 * opaque types: print via the standard printer so str
                 * produces readable output, not #<?>. */
                mino_val_t *printed = print_to_string(a);
                if (printed != NULL && printed->type == MINO_STRING) {
                    size_t plen = printed->as.s.len;
                    size_t need2 = len + plen + 1;
                    if (need2 > cap) {
                        cap = cap == 0 ? 128 : cap;
                        while (cap < need2) cap *= 2;
                        buf = (char *)realloc(buf, cap);
                        if (buf == NULL) { set_error("out of memory"); return NULL; }
                    }
                    memcpy(buf + len, printed->as.s.data, plen);
                    len += plen;
                    n = 0; /* already appended */
                } else {
                    n = snprintf(tmp, sizeof(tmp), "#<%s>",
                                 a->type == MINO_PRIM ? "prim" :
                                 a->type == MINO_FN   ? "fn" :
                                 a->type == MINO_MACRO ? "macro" :
                                 a->type == MINO_HANDLE ? "handle" : "?");
                }
                break;
            }
            }
            if (n > 0) {
                size_t need = len + (size_t)n + 1;
                if (need > cap) {
                    cap = cap == 0 ? 128 : cap;
                    while (cap < need) cap *= 2;
                    buf = (char *)realloc(buf, cap);
                    if (buf == NULL) { set_error("out of memory"); return NULL; }
                }
                memcpy(buf + len, tmp, (size_t)n);
                len += (size_t)n;
            }
        }
        args = args->as.cons.cdr;
    }
    {
        mino_val_t *result = mino_string_n(S_, buf != NULL ? buf : "", len);
        free(buf);
        return result;
    }
}

static mino_val_t *prim_println(mino_val_t *args, mino_env_t *env)
{
    mino_val_t *result = prim_str(args, env);
    if (result == NULL) return NULL;
    fwrite(result->as.s.data, 1, result->as.s.len, stdout);
    fputc('\n', stdout);
    fflush(stdout);
    return mino_nil(S_);
}

static mino_val_t *prim_prn(mino_val_t *args, mino_env_t *env)
{
    int first = 1;
    (void)env;
    while (mino_is_cons(args)) {
        if (!first) fputc(' ', stdout);
        mino_print(S_, args->as.cons.car);
        first = 0;
        args = args->as.cons.cdr;
    }
    fputc('\n', stdout);
    fflush(stdout);
    return mino_nil(S_);
}

static mino_val_t *prim_macroexpand_1(mino_val_t *args, mino_env_t *env)
{
    int expanded;
    if (!mino_is_cons(args) || mino_is_cons(args->as.cons.cdr)) {
        set_error("macroexpand-1 requires one argument");
        return NULL;
    }
    return macroexpand1(args->as.cons.car, env, &expanded);
}

static mino_val_t *prim_macroexpand(mino_val_t *args, mino_env_t *env)
{
    if (!mino_is_cons(args) || mino_is_cons(args->as.cons.cdr)) {
        set_error("macroexpand requires one argument");
        return NULL;
    }
    return macroexpand_all(args->as.cons.car, env);
}

static mino_val_t *prim_gensym(mino_val_t *args, mino_env_t *env)
{
    const char *prefix_src = "G__";
    size_t      prefix_len = 3;
    char        buf[256];
    size_t      nargs;
    (void)env;
    arg_count(args, &nargs);
    if (nargs > 1) {
        set_error("gensym takes 0 or 1 arguments");
        return NULL;
    }
    if (nargs == 1) {
        mino_val_t *p = args->as.cons.car;
        if (p == NULL || p->type != MINO_STRING) {
            set_error("gensym prefix must be a string");
            return NULL;
        }
        prefix_src = p->as.s.data;
        prefix_len = p->as.s.len;
        if (prefix_len >= sizeof(buf) - 32) {
            set_error("gensym prefix too long");
            return NULL;
        }
    }
    {
        int used;
        memcpy(buf, prefix_src, prefix_len);
        used = snprintf(buf + prefix_len, sizeof(buf) - prefix_len,
                        "%ld", ++gensym_counter);
        if (used < 0) {
            set_error("gensym formatting failed");
            return NULL;
        }
        return mino_symbol_n(S_, buf, prefix_len + (size_t)used);
    }
}

/* (throw value) — raise a script exception. Caught by try/catch; if no
 * enclosing try, becomes a fatal runtime error. */
static mino_val_t *prim_throw(mino_val_t *args, mino_env_t *env)
{
    mino_val_t *ex;
    (void)env;
    if (!mino_is_cons(args)) {
        set_error("throw requires one argument");
        return NULL;
    }
    ex = args->as.cons.car;
    if (try_depth <= 0) {
        /* No enclosing try — format as fatal error. */
        char msg[512];
        if (ex != NULL && ex->type == MINO_STRING) {
            snprintf(msg, sizeof(msg), "unhandled exception: %.*s",
                     (int)ex->as.s.len, ex->as.s.data);
        } else {
            snprintf(msg, sizeof(msg), "unhandled exception");
        }
        set_error(msg);
        return NULL;
    }
    try_stack[try_depth - 1].exception = ex;
    longjmp(try_stack[try_depth - 1].buf, 1);
    return NULL; /* unreachable */
}

/* (slurp path) — read a file's entire contents as a string. I/O capability;
 * only installed by mino_install_io, not mino_install_core. */
static mino_val_t *prim_slurp(mino_val_t *args, mino_env_t *env)
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
        set_error("slurp requires one argument");
        return NULL;
    }
    path_val = args->as.cons.car;
    if (path_val == NULL || path_val->type != MINO_STRING) {
        set_error("slurp: argument must be a string");
        return NULL;
    }
    path = path_val->as.s.data;
    f = fopen(path, "rb");
    if (f == NULL) {
        char msg[300];
        snprintf(msg, sizeof(msg), "slurp: cannot open file: %s", path);
        set_error(msg);
        return NULL;
    }
    fseek(f, 0, SEEK_END);
    sz = ftell(f);
    if (sz < 0) {
        fclose(f);
        set_error("slurp: cannot determine file size");
        return NULL;
    }
    fseek(f, 0, SEEK_SET);
    buf = (char *)malloc((size_t)sz + 1);
    if (buf == NULL) {
        fclose(f);
        set_error("slurp: out of memory");
        return NULL;
    }
    rd = fread(buf, 1, (size_t)sz, f);
    fclose(f);
    buf[rd] = '\0';
    result = mino_string_n(S_, buf, rd);
    free(buf);
    return result;
}

static mino_val_t *prim_spit(mino_val_t *args, mino_env_t *env)
{
    mino_val_t *path_val;
    mino_val_t *content;
    const char *path;
    FILE       *f;
    (void)env;
    if (!mino_is_cons(args) || !mino_is_cons(args->as.cons.cdr)
        || mino_is_cons(args->as.cons.cdr->as.cons.cdr)) {
        set_error("spit requires two arguments");
        return NULL;
    }
    path_val = args->as.cons.car;
    content  = args->as.cons.cdr->as.cons.car;
    if (path_val == NULL || path_val->type != MINO_STRING) {
        set_error("spit: first argument must be a string path");
        return NULL;
    }
    path = path_val->as.s.data;
    f = fopen(path, "wb");
    if (f == NULL) {
        char msg[300];
        snprintf(msg, sizeof(msg), "spit: cannot open file: %s", path);
        set_error(msg);
        return NULL;
    }
    if (content != NULL && content->type == MINO_STRING) {
        fwrite(content->as.s.data, 1, content->as.s.len, f);
    } else {
        mino_print_to(S_, f, content);
    }
    fclose(f);
    return mino_nil(S_);
}

/* (exit code) — terminate the process with the given exit code.
 * Defaults to 0 if no argument is given. */
static mino_val_t *prim_exit(mino_val_t *args, mino_env_t *env)
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
    return mino_nil(S_); /* unreachable */
}

/* --- Regex primitives (using bundled tiny-regex-c) --- */

/* (re-find pattern text) — find first match of pattern in text.
 * Returns the matched substring, or nil if no match. */
static mino_val_t *prim_re_find(mino_val_t *args, mino_env_t *env)
{
    mino_val_t *pat_val, *text_val;
    int match_len = 0;
    int match_idx;
    (void)env;
    if (!mino_is_cons(args) || !mino_is_cons(args->as.cons.cdr) ||
        mino_is_cons(args->as.cons.cdr->as.cons.cdr)) {
        set_error("re-find requires two arguments");
        return NULL;
    }
    pat_val  = args->as.cons.car;
    text_val = args->as.cons.cdr->as.cons.car;
    if (pat_val == NULL || pat_val->type != MINO_STRING) {
        set_error("re-find: first argument must be a pattern string");
        return NULL;
    }
    if (text_val == NULL || text_val->type != MINO_STRING) {
        set_error("re-find: second argument must be a string");
        return NULL;
    }
    match_idx = re_match(pat_val->as.s.data, text_val->as.s.data, &match_len);
    if (match_idx == -1) {
        return mino_nil(S_);
    }
    return mino_string_n(S_, text_val->as.s.data + match_idx, (size_t)match_len);
}

/* (re-matches pattern text) — true if the entire text matches pattern. */
static mino_val_t *prim_re_matches(mino_val_t *args, mino_env_t *env)
{
    mino_val_t *pat_val, *text_val;
    int match_len = 0;
    int match_idx;
    (void)env;
    if (!mino_is_cons(args) || !mino_is_cons(args->as.cons.cdr) ||
        mino_is_cons(args->as.cons.cdr->as.cons.cdr)) {
        set_error("re-matches requires two arguments");
        return NULL;
    }
    pat_val  = args->as.cons.car;
    text_val = args->as.cons.cdr->as.cons.car;
    if (pat_val == NULL || pat_val->type != MINO_STRING) {
        set_error("re-matches: first argument must be a pattern string");
        return NULL;
    }
    if (text_val == NULL || text_val->type != MINO_STRING) {
        set_error("re-matches: second argument must be a string");
        return NULL;
    }
    match_idx = re_match(pat_val->as.s.data, text_val->as.s.data, &match_len);
    if (match_idx == 0 && (size_t)match_len == text_val->as.s.len) {
        return text_val;
    }
    return mino_nil(S_);
}

/* (time-ms) — return process time in milliseconds as a float.
 * Uses ANSI C clock() for portability across all C99 platforms. */
static mino_val_t *prim_time_ms(mino_val_t *args, mino_env_t *env)
{
    (void)args;
    (void)env;
    if (mino_is_cons(args)) {
        set_error("time-ms takes no arguments");
        return NULL;
    }
    return mino_float(S_, (double)clock() / (double)CLOCKS_PER_SEC * 1000.0);
}

/* (require name) — load a module by name using the host-supplied resolver.
 * Returns the cached value on subsequent calls with the same name. */
static mino_val_t *prim_require(mino_val_t *args, mino_env_t *env)
{
    mino_val_t *name_val;
    const char *name;
    const char *path;
    size_t      i;
    mino_val_t *result;
    if (!mino_is_cons(args) || mino_is_cons(args->as.cons.cdr)) {
        set_error("require requires one argument");
        return NULL;
    }
    name_val = args->as.cons.car;
    if (name_val == NULL || name_val->type != MINO_STRING) {
        set_error("require: argument must be a string");
        return NULL;
    }
    name = name_val->as.s.data;
    /* Check cache. */
    for (i = 0; i < module_cache_len; i++) {
        if (strcmp(module_cache[i].name, name) == 0) {
            return module_cache[i].value;
        }
    }
    /* Resolve. */
    if (module_resolver == NULL) {
        set_error("require: no module resolver configured");
        return NULL;
    }
    path = module_resolver(name, module_resolver_ctx);
    if (path == NULL) {
        char msg[300];
        snprintf(msg, sizeof(msg), "require: cannot resolve module: %s", name);
        set_error(msg);
        return NULL;
    }
    /* Load. */
    result = mino_load_file(S_, path, env);
    if (result == NULL) {
        return NULL;
    }
    /* Cache. */
    if (module_cache_len == module_cache_cap) {
        size_t         new_cap = module_cache_cap == 0 ? 8 : module_cache_cap * 2;
        module_entry_t *nb     = (module_entry_t *)realloc(
            module_cache, new_cap * sizeof(*nb));
        if (nb == NULL) {
            set_error("require: out of memory");
            return NULL;
        }
        module_cache     = nb;
        module_cache_cap = new_cap;
    }
    {
        size_t nlen = strlen(name);
        char *dup   = (char *)malloc(nlen + 1);
        if (dup == NULL) {
            set_error("require: out of memory");
            return NULL;
        }
        memcpy(dup, name, nlen + 1);
        module_cache[module_cache_len].name  = dup;
        module_cache[module_cache_len].value = result;
        module_cache_len++;
    }
    return result;
}

/* (doc name) — print the docstring for a def/defmacro binding.
 * Returns the docstring as a string, or nil if no docstring. */
static mino_val_t *prim_doc(mino_val_t *args, mino_env_t *env)
{
    mino_val_t   *name_val;
    char          buf[256];
    size_t        n;
    meta_entry_t *e;
    (void)env;
    if (!mino_is_cons(args) || mino_is_cons(args->as.cons.cdr)) {
        set_error("doc requires one argument");
        return NULL;
    }
    name_val = args->as.cons.car;
    if (name_val == NULL || name_val->type != MINO_SYMBOL) {
        set_error("doc: argument must be a symbol");
        return NULL;
    }
    n = name_val->as.s.len;
    if (n >= sizeof(buf)) {
        set_error("doc: name too long");
        return NULL;
    }
    memcpy(buf, name_val->as.s.data, n);
    buf[n] = '\0';
    e = meta_find(buf);
    if (e != NULL && e->docstring != NULL) {
        return mino_string(S_, e->docstring);
    }
    return mino_nil(S_);
}

/* (source name) — return the source form of a def/defmacro binding. */
static mino_val_t *prim_source(mino_val_t *args, mino_env_t *env)
{
    mino_val_t   *name_val;
    char          buf[256];
    size_t        n;
    meta_entry_t *e;
    (void)env;
    if (!mino_is_cons(args) || mino_is_cons(args->as.cons.cdr)) {
        set_error("source requires one argument");
        return NULL;
    }
    name_val = args->as.cons.car;
    if (name_val == NULL || name_val->type != MINO_SYMBOL) {
        set_error("source: argument must be a symbol");
        return NULL;
    }
    n = name_val->as.s.len;
    if (n >= sizeof(buf)) {
        set_error("source: name too long");
        return NULL;
    }
    memcpy(buf, name_val->as.s.data, n);
    buf[n] = '\0';
    e = meta_find(buf);
    if (e != NULL && e->source != NULL) {
        return e->source;
    }
    return mino_nil(S_);
}

/* (apropos substring) — return a list of bound names containing substring. */
static mino_val_t *prim_apropos(mino_val_t *args, mino_env_t *env)
{
    mino_val_t *pat_val;
    const char *pat;
    mino_val_t *head = mino_nil(S_);
    mino_val_t *tail = NULL;
    mino_env_t *e;
    if (!mino_is_cons(args) || mino_is_cons(args->as.cons.cdr)) {
        set_error("apropos requires one argument");
        return NULL;
    }
    pat_val = args->as.cons.car;
    if (pat_val == NULL || pat_val->type != MINO_STRING) {
        set_error("apropos: argument must be a string");
        return NULL;
    }
    pat = pat_val->as.s.data;
    /* Walk every env frame from the given env up to root. */
    for (e = env; e != NULL; e = e->parent) {
        size_t i;
        for (i = 0; i < e->len; i++) {
            if (strstr(e->bindings[i].name, pat) != NULL) {
                mino_val_t *sym  = mino_symbol(S_, e->bindings[i].name);
                mino_val_t *cell = mino_cons(S_, sym, mino_nil(S_));
                if (tail == NULL) {
                    head = cell;
                } else {
                    tail->as.cons.cdr = cell;
                }
                tail = cell;
            }
        }
    }
    return head;
}

void mino_set_resolver(mino_state_t *S, mino_resolve_fn fn, void *ctx)
{
    S_ = S;
    module_resolver     = fn;
    module_resolver_ctx = ctx;
}

/*
 * Stdlib macros defined in mino itself. Each form is read + evaluated in
 * order against the installing env during mino_install_core, so downstream
 * code can depend on them as if they were primitives.
 *
 * Hygiene: macro writers introduce temporaries via (gensym) to avoid
 * capturing names from the caller's environment. 0.x makes no automatic
 * hygiene promise; gensym is the convention.
 */
#if defined(__clang__)
#  pragma clang diagnostic push
#  pragma clang diagnostic ignored "-Woverlength-strings"
#elif defined(__GNUC__)
#  pragma GCC diagnostic push
#  pragma GCC diagnostic ignored "-Woverlength-strings"
#endif
#include "core_mino.h"
#if defined(__clang__)
#  pragma clang diagnostic pop
#elif defined(__GNUC__)
#  pragma GCC diagnostic pop
#endif

static void install_core_mino(mino_env_t *env)
{
    const char *src        = core_mino_src;
    const char *saved_file = reader_file;
    int         saved_line = reader_line;
    reader_file = intern_filename("<core>");
    reader_line = 1;
    while (*src != '\0') {
        const char *end  = NULL;
        mino_val_t *form = mino_read(S_, src, &end);
        if (form == NULL) {
            if (mino_last_error(S_) != NULL) {
                /* Hardcoded source — a parse error here is a build-time bug. */
                fprintf(stderr, "core.mino parse error: %s\n", mino_last_error(S_));
                abort();
            }
            break;
        }
        if (mino_eval(S_, form, env) == NULL) {
            fprintf(stderr, "core.mino eval error: %s\n", mino_last_error(S_));
            abort();
        }
        src = end;
    }
    reader_file = saved_file;
    reader_line = saved_line;
}

/* --- Atom primitives --------------------------------------------------- */

/*
 * (seq coll) — coerce a collection to a sequence (cons chain).
 * Returns nil for empty collections. Forces lazy sequences.
 */
static mino_val_t *prim_seq(mino_val_t *args, mino_env_t *env)
{
    mino_val_t *coll;
    (void)env;
    if (!mino_is_cons(args) || mino_is_cons(args->as.cons.cdr)) {
        set_error("seq requires one argument");
        return NULL;
    }
    coll = args->as.cons.car;
    if (coll == NULL || coll->type == MINO_NIL) return mino_nil(S_);
    if (coll->type == MINO_LAZY) {
        mino_val_t *forced = lazy_force(coll);
        if (forced == NULL) return NULL;
        if (forced->type == MINO_NIL) return mino_nil(S_);
        return forced;
    }
    if (coll->type == MINO_CONS) return coll;
    if (coll->type == MINO_VECTOR) {
        mino_val_t *head = mino_nil(S_);
        mino_val_t *tail = NULL;
        size_t i;
        if (coll->as.vec.len == 0) return mino_nil(S_);
        for (i = 0; i < coll->as.vec.len; i++) {
            mino_val_t *cell = mino_cons(S_, vec_nth(coll, i), mino_nil(S_));
            if (tail == NULL) head = cell;
            else tail->as.cons.cdr = cell;
            tail = cell;
        }
        return head;
    }
    if (coll->type == MINO_MAP) {
        mino_val_t *head = mino_nil(S_);
        mino_val_t *tail = NULL;
        size_t i;
        if (coll->as.map.len == 0) return mino_nil(S_);
        for (i = 0; i < coll->as.map.len; i++) {
            mino_val_t *key = vec_nth(coll->as.map.key_order, i);
            mino_val_t *val = map_get_val(coll, key);
            mino_val_t *kv[2];
            mino_val_t *cell;
            kv[0] = key; kv[1] = val;
            cell = mino_cons(S_, mino_vector(S_, kv, 2), mino_nil(S_));
            if (tail == NULL) head = cell;
            else tail->as.cons.cdr = cell;
            tail = cell;
        }
        return head;
    }
    if (coll->type == MINO_SET) {
        mino_val_t *head = mino_nil(S_);
        mino_val_t *tail = NULL;
        size_t i;
        if (coll->as.set.len == 0) return mino_nil(S_);
        for (i = 0; i < coll->as.set.len; i++) {
            mino_val_t *elem = vec_nth(coll->as.set.key_order, i);
            mino_val_t *cell = mino_cons(S_, elem, mino_nil(S_));
            if (tail == NULL) head = cell;
            else tail->as.cons.cdr = cell;
            tail = cell;
        }
        return head;
    }
    if (coll->type == MINO_STRING) {
        mino_val_t *head = mino_nil(S_);
        mino_val_t *tail = NULL;
        size_t i;
        if (coll->as.s.len == 0) return mino_nil(S_);
        for (i = 0; i < coll->as.s.len; i++) {
            mino_val_t *ch = mino_string_n(S_, coll->as.s.data + i, 1);
            mino_val_t *cell = mino_cons(S_, ch, mino_nil(S_));
            if (tail == NULL) head = cell;
            else tail->as.cons.cdr = cell;
            tail = cell;
        }
        return head;
    }
    {
        char msg[96];
        snprintf(msg, sizeof(msg), "seq: cannot coerce %s to a sequence",
                 type_tag_str(coll));
        set_error(msg);
    }
    return NULL;
}

static mino_val_t *prim_realized_p(mino_val_t *args, mino_env_t *env)
{
    mino_val_t *v;
    (void)env;
    if (!mino_is_cons(args) || mino_is_cons(args->as.cons.cdr)) {
        set_error("realized? requires one argument");
        return NULL;
    }
    v = args->as.cons.car;
    if (v != NULL && v->type == MINO_LAZY) {
        return v->as.lazy.realized ? mino_true(S_) : mino_false(S_);
    }
    /* Non-lazy values are always realized. */
    return mino_true(S_);
}

static mino_val_t *prim_atom(mino_val_t *args, mino_env_t *env)
{
    (void)env;
    if (!mino_is_cons(args) || mino_is_cons(args->as.cons.cdr)) {
        set_error("atom requires one argument");
        return NULL;
    }
    return mino_atom(S_, args->as.cons.car);
}

static mino_val_t *prim_deref(mino_val_t *args, mino_env_t *env)
{
    mino_val_t *a;
    (void)env;
    if (!mino_is_cons(args) || mino_is_cons(args->as.cons.cdr)) {
        set_error("deref requires one argument");
        return NULL;
    }
    a = args->as.cons.car;
    if (a == NULL || a->type != MINO_ATOM) {
        set_error("deref: expected an atom");
        return NULL;
    }
    return a->as.atom.val;
}

static mino_val_t *prim_reset_bang(mino_val_t *args, mino_env_t *env)
{
    mino_val_t *a, *val;
    (void)env;
    if (!mino_is_cons(args) || !mino_is_cons(args->as.cons.cdr)
        || mino_is_cons(args->as.cons.cdr->as.cons.cdr)) {
        set_error("reset! requires two arguments");
        return NULL;
    }
    a   = args->as.cons.car;
    val = args->as.cons.cdr->as.cons.car;
    if (a == NULL || a->type != MINO_ATOM) {
        set_error("reset!: first argument must be an atom");
        return NULL;
    }
    a->as.atom.val = val;
    return val;
}

/* (swap! atom f & args) — applies (f current-val args...) and sets result. */
static mino_val_t *prim_swap_bang(mino_val_t *args, mino_env_t *env)
{
    mino_val_t *a, *fn, *cur, *call_args, *extra, *tail, *result;
    if (!mino_is_cons(args) || !mino_is_cons(args->as.cons.cdr)) {
        set_error("swap! requires at least 2 arguments: atom and function");
        return NULL;
    }
    a  = args->as.cons.car;
    fn = args->as.cons.cdr->as.cons.car;
    if (a == NULL || a->type != MINO_ATOM) {
        set_error("swap!: first argument must be an atom");
        return NULL;
    }
    cur = a->as.atom.val;
    /* Build arg list: (cur extra1 extra2 ...) */
    call_args = mino_nil(S_);
    /* Append extra args in reverse then prepend cur. */
    extra = args->as.cons.cdr->as.cons.cdr; /* rest after fn */
    if (extra != NULL && extra->type == MINO_CONS) {
        /* Collect extras into a list. */
        tail = mino_nil(S_);
        while (extra != NULL && extra->type == MINO_CONS) {
            tail = mino_cons(S_, extra->as.cons.car, tail);
            extra = extra->as.cons.cdr;
        }
        /* Reverse to get correct order. */
        call_args = mino_nil(S_);
        while (tail != NULL && tail->type == MINO_CONS) {
            call_args = mino_cons(S_, tail->as.cons.car, call_args);
            tail = tail->as.cons.cdr;
        }
    }
    call_args = mino_cons(S_, cur, call_args);
    result = mino_call(S_, fn, call_args, env);
    if (result == NULL) return NULL;
    a->as.atom.val = result;
    return result;
}

static mino_val_t *prim_atom_p(mino_val_t *args, mino_env_t *env)
{
    (void)env;
    if (!mino_is_cons(args) || mino_is_cons(args->as.cons.cdr)) {
        set_error("atom? requires one argument");
        return NULL;
    }
    return mino_is_atom(args->as.cons.car) ? mino_true(S_) : mino_false(S_);
}

void mino_install_core(mino_state_t *S, mino_env_t *env)
{
    S_ = S;
    volatile char probe = 0;
    gc_note_host_frame((void *)&probe);
    (void)probe;
    mino_env_set(S_, env, "+",        mino_prim(S_, "+",        prim_add));
    mino_env_set(S_, env, "-",        mino_prim(S_, "-",        prim_sub));
    mino_env_set(S_, env, "*",        mino_prim(S_, "*",        prim_mul));
    mino_env_set(S_, env, "/",        mino_prim(S_, "/",        prim_div));
    mino_env_set(S_, env, "=",        mino_prim(S_, "=",        prim_eq));
    mino_env_set(S_, env, "<",        mino_prim(S_, "<",        prim_lt));
    mino_env_set(S_, env, "mod",      mino_prim(S_, "mod",      prim_mod));
    mino_env_set(S_, env, "rem",      mino_prim(S_, "rem",      prim_rem));
    mino_env_set(S_, env, "quot",     mino_prim(S_, "quot",     prim_quot));
    /* math */
    mino_env_set(S_, env, "math-floor", mino_prim(S_, "math-floor", prim_math_floor));
    mino_env_set(S_, env, "math-ceil",  mino_prim(S_, "math-ceil",  prim_math_ceil));
    mino_env_set(S_, env, "math-round", mino_prim(S_, "math-round", prim_math_round));
    mino_env_set(S_, env, "math-sqrt",  mino_prim(S_, "math-sqrt",  prim_math_sqrt));
    mino_env_set(S_, env, "math-pow",   mino_prim(S_, "math-pow",   prim_math_pow));
    mino_env_set(S_, env, "math-log",   mino_prim(S_, "math-log",   prim_math_log));
    mino_env_set(S_, env, "math-exp",   mino_prim(S_, "math-exp",   prim_math_exp));
    mino_env_set(S_, env, "math-sin",   mino_prim(S_, "math-sin",   prim_math_sin));
    mino_env_set(S_, env, "math-cos",   mino_prim(S_, "math-cos",   prim_math_cos));
    mino_env_set(S_, env, "math-tan",   mino_prim(S_, "math-tan",   prim_math_tan));
    mino_env_set(S_, env, "math-atan2", mino_prim(S_, "math-atan2", prim_math_atan2));
    mino_env_set(S_, env, "math-pi",    mino_float(S_, 3.14159265358979323846));
    mino_env_set(S_, env, "bit-and", mino_prim(S_, "bit-and", prim_bit_and));
    mino_env_set(S_, env, "bit-or",  mino_prim(S_, "bit-or",  prim_bit_or));
    mino_env_set(S_, env, "bit-xor", mino_prim(S_, "bit-xor", prim_bit_xor));
    mino_env_set(S_, env, "bit-not", mino_prim(S_, "bit-not", prim_bit_not));
    mino_env_set(S_, env, "bit-shift-left",
                 mino_prim(S_, "bit-shift-left", prim_bit_shift_left));
    mino_env_set(S_, env, "bit-shift-right",
                 mino_prim(S_, "bit-shift-right", prim_bit_shift_right));
    mino_env_set(S_, env, "car",      mino_prim(S_, "car",      prim_car));
    mino_env_set(S_, env, "cdr",      mino_prim(S_, "cdr",      prim_cdr));
    mino_env_set(S_, env, "cons",     mino_prim(S_, "cons",     prim_cons));
    mino_env_set(S_, env, "count",    mino_prim(S_, "count",    prim_count));
    mino_env_set(S_, env, "nth",      mino_prim(S_, "nth",      prim_nth));
    mino_env_set(S_, env, "first",    mino_prim(S_, "first",    prim_first));
    mino_env_set(S_, env, "rest",     mino_prim(S_, "rest",     prim_rest));
    mino_env_set(S_, env, "vector",   mino_prim(S_, "vector",   prim_vector));
    mino_env_set(S_, env, "hash-map", mino_prim(S_, "hash-map", prim_hash_map));
    mino_env_set(S_, env, "assoc",    mino_prim(S_, "assoc",    prim_assoc));
    mino_env_set(S_, env, "get",      mino_prim(S_, "get",      prim_get));
    mino_env_set(S_, env, "conj",     mino_prim(S_, "conj",     prim_conj));
    mino_env_set(S_, env, "keys",     mino_prim(S_, "keys",     prim_keys));
    mino_env_set(S_, env, "vals",     mino_prim(S_, "vals",     prim_vals));
    mino_env_set(S_, env, "macroexpand-1",
                 mino_prim(S_, "macroexpand-1", prim_macroexpand_1));
    mino_env_set(S_, env, "macroexpand",
                 mino_prim(S_, "macroexpand", prim_macroexpand));
    mino_env_set(S_, env, "gensym",   mino_prim(S_, "gensym",   prim_gensym));
    mino_env_set(S_, env, "type",     mino_prim(S_, "type",     prim_type));
    mino_env_set(S_, env, "name",     mino_prim(S_, "name",     prim_name));
    mino_env_set(S_, env, "rand",     mino_prim(S_, "rand",     prim_rand));
    /* regex */
    mino_env_set(S_, env, "re-find",    mino_prim(S_, "re-find",    prim_re_find));
    mino_env_set(S_, env, "re-matches", mino_prim(S_, "re-matches", prim_re_matches));
    mino_env_set(S_, env, "eval",     mino_prim(S_, "eval",     prim_eval));
    mino_env_set(S_, env, "symbol",   mino_prim(S_, "symbol",   prim_symbol));
    mino_env_set(S_, env, "keyword",  mino_prim(S_, "keyword",  prim_keyword));
    mino_env_set(S_, env, "hash",     mino_prim(S_, "hash",     prim_hash));
    mino_env_set(S_, env, "compare",  mino_prim(S_, "compare",  prim_compare));
    mino_env_set(S_, env, "int",      mino_prim(S_, "int",      prim_int));
    mino_env_set(S_, env, "float",    mino_prim(S_, "float",    prim_float));
    mino_env_set(S_, env, "str",      mino_prim(S_, "str",      prim_str));
    mino_env_set(S_, env, "pr-str",   mino_prim(S_, "pr-str",   prim_pr_str));
    mino_env_set(S_, env, "read-string",
                 mino_prim(S_, "read-string", prim_read_string));
    mino_env_set(S_, env, "format",   mino_prim(S_, "format",   prim_format));
    mino_env_set(S_, env, "throw",    mino_prim(S_, "throw",    prim_throw));
    mino_env_set(S_, env, "require",  mino_prim(S_, "require",  prim_require));
    mino_env_set(S_, env, "doc",      mino_prim(S_, "doc",      prim_doc));
    mino_env_set(S_, env, "source",   mino_prim(S_, "source",   prim_source));
    mino_env_set(S_, env, "apropos",  mino_prim(S_, "apropos",  prim_apropos));
    /* set operations */
    mino_env_set(S_, env, "hash-set", mino_prim(S_, "hash-set", prim_hash_set));
    mino_env_set(S_, env, "contains?",mino_prim(S_, "contains?",prim_contains_p));
    mino_env_set(S_, env, "disj",     mino_prim(S_, "disj",     prim_disj));
    mino_env_set(S_, env, "dissoc",   mino_prim(S_, "dissoc",   prim_dissoc));
    /* sequence operations (map, filter, take, drop, range, repeat,
       concat are now lazy in core.mino) */
    mino_env_set(S_, env, "reduce",   mino_prim(S_, "reduce",   prim_reduce));
    mino_env_set(S_, env, "into",     mino_prim(S_, "into",     prim_into));
    mino_env_set(S_, env, "apply",    mino_prim(S_, "apply",    prim_apply));
    mino_env_set(S_, env, "reverse",  mino_prim(S_, "reverse",  prim_reverse));
    mino_env_set(S_, env, "sort",     mino_prim(S_, "sort",     prim_sort));
    /* string operations */
    mino_env_set(S_, env, "subs",     mino_prim(S_, "subs",     prim_subs));
    mino_env_set(S_, env, "split",    mino_prim(S_, "split",    prim_split));
    mino_env_set(S_, env, "join",     mino_prim(S_, "join",     prim_join));
    mino_env_set(S_, env, "starts-with?",
                 mino_prim(S_, "starts-with?", prim_starts_with_p));
    mino_env_set(S_, env, "ends-with?",
                 mino_prim(S_, "ends-with?", prim_ends_with_p));
    mino_env_set(S_, env, "includes?",
                 mino_prim(S_, "includes?", prim_includes_p));
    mino_env_set(S_, env, "upper-case",
                 mino_prim(S_, "upper-case", prim_upper_case));
    mino_env_set(S_, env, "lower-case",
                 mino_prim(S_, "lower-case", prim_lower_case));
    mino_env_set(S_, env, "trim",     mino_prim(S_, "trim",     prim_trim));
    mino_env_set(S_, env, "char-at",  mino_prim(S_, "char-at",  prim_char_at));
    /* (some and every? are now in core.mino) */
    /* sequences */
    mino_env_set(S_, env, "seq",       mino_prim(S_, "seq",       prim_seq));
    mino_env_set(S_, env, "realized?", mino_prim(S_, "realized?", prim_realized_p));
    /* atoms */
    mino_env_set(S_, env, "atom",     mino_prim(S_, "atom",     prim_atom));
    mino_env_set(S_, env, "deref",    mino_prim(S_, "deref",    prim_deref));
    mino_env_set(S_, env, "reset!",   mino_prim(S_, "reset!",   prim_reset_bang));
    mino_env_set(S_, env, "swap!",    mino_prim(S_, "swap!",    prim_swap_bang));
    mino_env_set(S_, env, "atom?",    mino_prim(S_, "atom?",    prim_atom_p));
    /* actors */
    mino_env_set(S_, env, "spawn",    mino_prim(S_, "spawn",    prim_spawn));
    mino_env_set(S_, env, "send!",    mino_prim(S_, "send!",    prim_send_bang));
    mino_env_set(S_, env, "receive",  mino_prim(S_, "receive",  prim_receive));
    install_core_mino(env);
}

void mino_install_io(mino_state_t *S, mino_env_t *env)
{
    S_ = S;
    mino_env_set(S_, env, "println",  mino_prim(S_, "println",  prim_println));
    mino_env_set(S_, env, "prn",      mino_prim(S_, "prn",      prim_prn));
    mino_env_set(S_, env, "slurp",    mino_prim(S_, "slurp",    prim_slurp));
    mino_env_set(S_, env, "spit",     mino_prim(S_, "spit",     prim_spit));
    mino_env_set(S_, env, "exit",     mino_prim(S_, "exit",     prim_exit));
    mino_env_set(S_, env, "time-ms",  mino_prim(S_, "time-ms",  prim_time_ms));
}
