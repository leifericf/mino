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
static mino_val_t *prim_throw_error(mino_state_t *S, const char *msg)
{
    mino_val_t *ex = mino_string(S, msg);
    if (try_depth > 0) {
        try_stack[try_depth - 1].exception = ex;
        longjmp(try_stack[try_depth - 1].buf, 1);
    }
    set_error(S, msg);
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

static mino_val_t *prim_add(mino_state_t *S, mino_val_t *args, mino_env_t *env)
{
    (void)env;
    if (args_have_float(args)) {
        double acc = 0.0;
        while (mino_is_cons(args)) {
            double x;
            if (!as_double(args->as.cons.car, &x)) {
                set_error(S, "+ expects numbers");
                return NULL;
            }
            acc += x;
            args = args->as.cons.cdr;
        }
        return mino_float(S, acc);
    } else {
        long long acc = 0;
        while (mino_is_cons(args)) {
            long long x;
            if (!as_long(args->as.cons.car, &x)) {
                set_error(S, "+ expects numbers");
                return NULL;
            }
            acc += x;
            args = args->as.cons.cdr;
        }
        return mino_int(S, acc);
    }
}

static mino_val_t *prim_sub(mino_state_t *S, mino_val_t *args, mino_env_t *env)
{
    (void)env;
    if (!mino_is_cons(args)) {
        set_error(S, "- requires at least one argument");
        return NULL;
    }
    if (args_have_float(args)) {
        double acc;
        if (!as_double(args->as.cons.car, &acc)) {
            set_error(S, "- expects numbers");
            return NULL;
        }
        args = args->as.cons.cdr;
        if (!mino_is_cons(args)) {
            return mino_float(S, -acc);
        }
        while (mino_is_cons(args)) {
            double x;
            if (!as_double(args->as.cons.car, &x)) {
                set_error(S, "- expects numbers");
                return NULL;
            }
            acc -= x;
            args = args->as.cons.cdr;
        }
        return mino_float(S, acc);
    } else {
        long long acc;
        if (!as_long(args->as.cons.car, &acc)) {
            set_error(S, "- expects numbers");
            return NULL;
        }
        args = args->as.cons.cdr;
        if (!mino_is_cons(args)) {
            return mino_int(S, -acc);
        }
        while (mino_is_cons(args)) {
            long long x;
            if (!as_long(args->as.cons.car, &x)) {
                set_error(S, "- expects numbers");
                return NULL;
            }
            acc -= x;
            args = args->as.cons.cdr;
        }
        return mino_int(S, acc);
    }
}

static mino_val_t *prim_mul(mino_state_t *S, mino_val_t *args, mino_env_t *env)
{
    (void)env;
    if (args_have_float(args)) {
        double acc = 1.0;
        while (mino_is_cons(args)) {
            double x;
            if (!as_double(args->as.cons.car, &x)) {
                set_error(S, "* expects numbers");
                return NULL;
            }
            acc *= x;
            args = args->as.cons.cdr;
        }
        return mino_float(S, acc);
    } else {
        long long acc = 1;
        while (mino_is_cons(args)) {
            long long x;
            if (!as_long(args->as.cons.car, &x)) {
                set_error(S, "* expects numbers");
                return NULL;
            }
            acc *= x;
            args = args->as.cons.cdr;
        }
        return mino_int(S, acc);
    }
}

static mino_val_t *prim_div(mino_state_t *S, mino_val_t *args, mino_env_t *env)
{
    /* Division returns an integer when all operands are integers and the
     * result is exact, a float otherwise. */
    double acc;
    int all_int = 1;
    (void)env;
    if (!mino_is_cons(args)) {
        set_error(S, "/ requires at least one argument");
        return NULL;
    }
    if (args->as.cons.car == NULL
        || (args->as.cons.car->type != MINO_INT
            && args->as.cons.car->type != MINO_FLOAT)) {
        set_error(S, "/ expects numbers");
        return NULL;
    }
    if (args->as.cons.car->type == MINO_FLOAT) all_int = 0;
    if (!as_double(args->as.cons.car, &acc)) {
        set_error(S, "/ expects numbers");
        return NULL;
    }
    args = args->as.cons.cdr;
    if (!mino_is_cons(args)) {
        if (acc == 0.0) {
            return prim_throw_error(S, "division by zero");
        }
        return mino_float(S, 1.0 / acc);
    }
    while (mino_is_cons(args)) {
        double x;
        if (args->as.cons.car == NULL
            || (args->as.cons.car->type != MINO_INT
                && args->as.cons.car->type != MINO_FLOAT)) {
            set_error(S, "/ expects numbers");
            return NULL;
        }
        if (args->as.cons.car->type == MINO_FLOAT) all_int = 0;
        if (!as_double(args->as.cons.car, &x)) {
            set_error(S, "/ expects numbers");
            return NULL;
        }
        if (x == 0.0) {
            return prim_throw_error(S, "division by zero");
        }
        acc /= x;
        args = args->as.cons.cdr;
    }
    /* Return integer when all operands were ints and result is exact. */
    if (all_int && acc == (double)(long long)acc) {
        return mino_int(S, (long long)acc);
    }
    return mino_float(S, acc);
}

static mino_val_t *prim_mod(mino_state_t *S, mino_val_t *args, mino_env_t *env)
{
    double a, b, r;
    (void)env;
    if (!mino_is_cons(args) || !mino_is_cons(args->as.cons.cdr) ||
        mino_is_cons(args->as.cons.cdr->as.cons.cdr)) {
        set_error(S, "mod requires two arguments");
        return NULL;
    }
    if (!as_double(args->as.cons.car, &a) ||
        !as_double(args->as.cons.cdr->as.cons.car, &b)) {
        set_error(S, "mod expects numbers");
        return NULL;
    }
    if (b == 0.0) {
        return prim_throw_error(S, "mod: division by zero");
    }
    r = fmod(a, b);
    /* Floored modulo: result has same sign as divisor. */
    if (r != 0.0 && ((r < 0.0) != (b < 0.0))) r += b;
    /* Return int if both args are ints. */
    if (args->as.cons.car->type == MINO_INT &&
        args->as.cons.cdr->as.cons.car->type == MINO_INT) {
        return mino_int(S, (long long)r);
    }
    return mino_float(S, r);
}

static mino_val_t *prim_rem(mino_state_t *S, mino_val_t *args, mino_env_t *env)
{
    double a, b, r;
    (void)env;
    if (!mino_is_cons(args) || !mino_is_cons(args->as.cons.cdr) ||
        mino_is_cons(args->as.cons.cdr->as.cons.cdr)) {
        set_error(S, "rem requires two arguments");
        return NULL;
    }
    if (!as_double(args->as.cons.car, &a) ||
        !as_double(args->as.cons.cdr->as.cons.car, &b)) {
        set_error(S, "rem expects numbers");
        return NULL;
    }
    if (b == 0.0) {
        return prim_throw_error(S, "rem: division by zero");
    }
    r = fmod(a, b);
    if (args->as.cons.car->type == MINO_INT &&
        args->as.cons.cdr->as.cons.car->type == MINO_INT) {
        return mino_int(S, (long long)r);
    }
    return mino_float(S, r);
}

static mino_val_t *prim_quot(mino_state_t *S, mino_val_t *args, mino_env_t *env)
{
    double a, b, q;
    (void)env;
    if (!mino_is_cons(args) || !mino_is_cons(args->as.cons.cdr) ||
        mino_is_cons(args->as.cons.cdr->as.cons.cdr)) {
        set_error(S, "quot requires two arguments");
        return NULL;
    }
    if (!as_double(args->as.cons.car, &a) ||
        !as_double(args->as.cons.cdr->as.cons.car, &b)) {
        set_error(S, "quot expects numbers");
        return NULL;
    }
    if (b == 0.0) {
        return prim_throw_error(S, "quot: division by zero");
    }
    q = a / b;
    q = q >= 0 ? floor(q) : ceil(q);
    if (args->as.cons.car->type == MINO_INT &&
        args->as.cons.cdr->as.cons.car->type == MINO_INT) {
        return mino_int(S, (long long)q);
    }
    return mino_float(S, q);
}

/* --- Math functions (thin wrappers around math.h) --- */

#define MATH_UNARY(cname, cfn, label)                                  \
    static mino_val_t *cname(mino_state_t *S, mino_val_t *args, mino_env_t *env) \
    {                                                                   \
        double x;                                                       \
        (void)env;                                                      \
        if (!mino_is_cons(args) ||                                      \
            mino_is_cons(args->as.cons.cdr)) {                          \
            set_error(S, label " requires one argument");                  \
            return NULL;                                                \
        }                                                               \
        if (!as_double(args->as.cons.car, &x)) {                       \
            set_error(S, label " expects a number");                       \
            return NULL;                                                \
        }                                                               \
        return mino_float(S, cfn(x));                                      \
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

static mino_val_t *prim_math_pow(mino_state_t *S, mino_val_t *args, mino_env_t *env)
{
    double base, exponent;
    (void)env;
    if (!mino_is_cons(args) || !mino_is_cons(args->as.cons.cdr) ||
        mino_is_cons(args->as.cons.cdr->as.cons.cdr)) {
        set_error(S, "math-pow requires two arguments");
        return NULL;
    }
    if (!as_double(args->as.cons.car, &base) ||
        !as_double(args->as.cons.cdr->as.cons.car, &exponent)) {
        set_error(S, "math-pow expects numbers");
        return NULL;
    }
    return mino_float(S, pow(base, exponent));
}

static mino_val_t *prim_math_atan2(mino_state_t *S, mino_val_t *args, mino_env_t *env)
{
    double y, x;
    (void)env;
    if (!mino_is_cons(args) || !mino_is_cons(args->as.cons.cdr) ||
        mino_is_cons(args->as.cons.cdr->as.cons.cdr)) {
        set_error(S, "math-atan2 requires two arguments");
        return NULL;
    }
    if (!as_double(args->as.cons.car, &y) ||
        !as_double(args->as.cons.cdr->as.cons.car, &x)) {
        set_error(S, "math-atan2 expects numbers");
        return NULL;
    }
    return mino_float(S, atan2(y, x));
}

static mino_val_t *prim_bit_and(mino_state_t *S, mino_val_t *args, mino_env_t *env)
{
    long long a, b;
    (void)env;
    if (!mino_is_cons(args) || !mino_is_cons(args->as.cons.cdr) ||
        mino_is_cons(args->as.cons.cdr->as.cons.cdr)) {
        set_error(S, "bit-and requires two arguments");
        return NULL;
    }
    if (!as_long(args->as.cons.car, &a) ||
        !as_long(args->as.cons.cdr->as.cons.car, &b)) {
        set_error(S, "bit-and expects integers");
        return NULL;
    }
    return mino_int(S, a & b);
}

static mino_val_t *prim_bit_or(mino_state_t *S, mino_val_t *args, mino_env_t *env)
{
    long long a, b;
    (void)env;
    if (!mino_is_cons(args) || !mino_is_cons(args->as.cons.cdr) ||
        mino_is_cons(args->as.cons.cdr->as.cons.cdr)) {
        set_error(S, "bit-or requires two arguments");
        return NULL;
    }
    if (!as_long(args->as.cons.car, &a) ||
        !as_long(args->as.cons.cdr->as.cons.car, &b)) {
        set_error(S, "bit-or expects integers");
        return NULL;
    }
    return mino_int(S, a | b);
}

static mino_val_t *prim_bit_xor(mino_state_t *S, mino_val_t *args, mino_env_t *env)
{
    long long a, b;
    (void)env;
    if (!mino_is_cons(args) || !mino_is_cons(args->as.cons.cdr) ||
        mino_is_cons(args->as.cons.cdr->as.cons.cdr)) {
        set_error(S, "bit-xor requires two arguments");
        return NULL;
    }
    if (!as_long(args->as.cons.car, &a) ||
        !as_long(args->as.cons.cdr->as.cons.car, &b)) {
        set_error(S, "bit-xor expects integers");
        return NULL;
    }
    return mino_int(S, a ^ b);
}

static mino_val_t *prim_bit_not(mino_state_t *S, mino_val_t *args, mino_env_t *env)
{
    long long a;
    (void)env;
    if (!mino_is_cons(args) || mino_is_cons(args->as.cons.cdr)) {
        set_error(S, "bit-not requires one argument");
        return NULL;
    }
    if (!as_long(args->as.cons.car, &a)) {
        set_error(S, "bit-not expects an integer");
        return NULL;
    }
    return mino_int(S, ~a);
}

static mino_val_t *prim_bit_shift_left(mino_state_t *S, mino_val_t *args, mino_env_t *env)
{
    long long a, b;
    (void)env;
    if (!mino_is_cons(args) || !mino_is_cons(args->as.cons.cdr) ||
        mino_is_cons(args->as.cons.cdr->as.cons.cdr)) {
        set_error(S, "bit-shift-left requires two arguments");
        return NULL;
    }
    if (!as_long(args->as.cons.car, &a) ||
        !as_long(args->as.cons.cdr->as.cons.car, &b)) {
        set_error(S, "bit-shift-left expects integers");
        return NULL;
    }
    return mino_int(S, a << b);
}

static mino_val_t *prim_bit_shift_right(mino_state_t *S, mino_val_t *args, mino_env_t *env)
{
    long long a, b;
    (void)env;
    if (!mino_is_cons(args) || !mino_is_cons(args->as.cons.cdr) ||
        mino_is_cons(args->as.cons.cdr->as.cons.cdr)) {
        set_error(S, "bit-shift-right requires two arguments");
        return NULL;
    }
    if (!as_long(args->as.cons.car, &a) ||
        !as_long(args->as.cons.cdr->as.cons.car, &b)) {
        set_error(S, "bit-shift-right expects integers");
        return NULL;
    }
    return mino_int(S, a >> b);
}

static mino_val_t *prim_int(mino_state_t *S, mino_val_t *args, mino_env_t *env)
{
    mino_val_t *v;
    (void)env;
    if (!mino_is_cons(args) || mino_is_cons(args->as.cons.cdr)) {
        set_error(S, "int requires one argument");
        return NULL;
    }
    v = args->as.cons.car;
    if (v != NULL && v->type == MINO_INT) return v;
    if (v != NULL && v->type == MINO_FLOAT) return mino_int(S, (long long)v->as.f);
    set_error(S, "int: expected a number");
    return NULL;
}

static mino_val_t *prim_float(mino_state_t *S, mino_val_t *args, mino_env_t *env)
{
    mino_val_t *v;
    (void)env;
    if (!mino_is_cons(args) || mino_is_cons(args->as.cons.cdr)) {
        set_error(S, "float requires one argument");
        return NULL;
    }
    v = args->as.cons.car;
    if (v != NULL && v->type == MINO_FLOAT) return v;
    if (v != NULL && v->type == MINO_INT) return mino_float(S, (double)v->as.i);
    set_error(S, "float: expected a number");
    return NULL;
}

/*
 * Helper: print a value to a string buffer using the standard printer.
 * Returns a mino string. Uses tmpfile() for ANSI C portability.
 */
static mino_val_t *print_to_string(mino_state_t *S, const mino_val_t *v)
{
    FILE  *f = tmpfile();
    long   n;
    char  *buf;
    mino_val_t *result;
    if (f == NULL) {
        set_error(S, "pr-str: tmpfile failed");
        return NULL;
    }
    mino_print_to(S, f, v);
    n = ftell(f);
    if (n < 0) n = 0;
    rewind(f);
    buf = (char *)malloc((size_t)n + 1);
    if (buf == NULL) {
        fclose(f);
        set_error(S, "out of memory");
        return NULL;
    }
    if (n > 0) {
        size_t rd = fread(buf, 1, (size_t)n, f);
        (void)rd;
    }
    buf[n] = '\0';
    fclose(f);
    result = mino_string_n(S, buf, (size_t)n);
    free(buf);
    return result;
}

/*
 * (format fmt & args) — simple string formatting.
 * Directives: %s (str of arg), %d (integer), %f (float), %% (literal %).
 */
static mino_val_t *prim_format(mino_state_t *S, mino_val_t *args, mino_env_t *env)
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
        set_error(S, "format requires at least a format string");
        return NULL;
    }
    fmt_val = args->as.cons.car;
    if (fmt_val == NULL || fmt_val->type != MINO_STRING) {
        set_error(S, "format: first argument must be a string");
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
            if (buf == NULL) { set_error(S, "out of memory"); return NULL; } \
        } \
    } while (0)

    for (i = 0; i < fmt_len; i++) {
        if (fmt[i] == '%' && i + 1 < fmt_len) {
            /* Collect the full format directive: %[flags][width][.prec]spec
             * Flags: '-', '+', ' ', '0', '#'
             * Width/precision: digits and '.'
             * Spec: d, f, e, g, s, x, o, % */
            char   directive[32];
            size_t di = 0;
            char   spec;
            directive[di++] = '%';
            i++;
            /* Flags. */
            while (i < fmt_len && di < sizeof(directive) - 4 &&
                   (fmt[i] == '-' || fmt[i] == '+' || fmt[i] == ' ' ||
                    fmt[i] == '0' || fmt[i] == '#')) {
                directive[di++] = fmt[i++];
            }
            /* Width. */
            while (i < fmt_len && di < sizeof(directive) - 4 &&
                   fmt[i] >= '0' && fmt[i] <= '9') {
                directive[di++] = fmt[i++];
            }
            /* Precision. */
            if (i < fmt_len && fmt[i] == '.') {
                directive[di++] = fmt[i++];
                while (i < fmt_len && di < sizeof(directive) - 4 &&
                       fmt[i] >= '0' && fmt[i] <= '9') {
                    directive[di++] = fmt[i++];
                }
            }
            if (i >= fmt_len) {
                /* Incomplete directive at end of string: emit literal. */
                FMT_ENSURE(di);
                memcpy(buf + len, directive, di);
                len += di;
                break;
            }
            spec = fmt[i];
            if (spec == '%') {
                FMT_ENSURE(1);
                buf[len++] = '%';
            } else if (spec == 's') {
                mino_val_t *a;
                if (!mino_is_cons(arg_list)) {
                    free(buf);
                    return prim_throw_error(S, "format: not enough arguments for format string");
                }
                a = arg_list->as.cons.car;
                arg_list = arg_list->as.cons.cdr;
                if (a != NULL && a->type == MINO_STRING) {
                    FMT_ENSURE(a->as.s.len);
                    memcpy(buf + len, a->as.s.data, a->as.s.len);
                    len += a->as.s.len;
                } else {
                    mino_val_t *s = print_to_string(S, a);
                    if (s == NULL) { free(buf); return NULL; }
                    FMT_ENSURE(s->as.s.len);
                    memcpy(buf + len, s->as.s.data, s->as.s.len);
                    len += s->as.s.len;
                }
            } else if (spec == 'd' || spec == 'x' || spec == 'o') {
                long long n;
                char tmp[64];
                int  tn;
                if (!mino_is_cons(arg_list)) {
                    free(buf);
                    return prim_throw_error(S, "format: not enough arguments for format string");
                }
                if (!as_long(arg_list->as.cons.car, &n)) {
                    double d;
                    if (as_double(arg_list->as.cons.car, &d)) {
                        n = (long long)d;
                    } else {
                        free(buf);
                        return prim_throw_error(S, "format: integer directive expects a number");
                    }
                }
                arg_list = arg_list->as.cons.cdr;
                /* Build snprintf format: replace spec with lld/llx/llo. */
                directive[di++] = 'l';
                directive[di++] = 'l';
                directive[di++] = spec;
                directive[di]   = '\0';
                tn = snprintf(tmp, sizeof(tmp), directive, n);
                FMT_ENSURE((size_t)tn);
                memcpy(buf + len, tmp, (size_t)tn);
                len += (size_t)tn;
            } else if (spec == 'f' || spec == 'e' || spec == 'g') {
                double d;
                char tmp[128];
                int  tn;
                if (!mino_is_cons(arg_list)) {
                    free(buf);
                    return prim_throw_error(S, "format: not enough arguments for format string");
                }
                if (!as_double(arg_list->as.cons.car, &d)) {
                    free(buf);
                    return prim_throw_error(S, "format: float directive expects a number");
                }
                arg_list = arg_list->as.cons.cdr;
                directive[di++] = spec;
                directive[di]   = '\0';
                tn = snprintf(tmp, sizeof(tmp), directive, d);
                FMT_ENSURE((size_t)tn);
                memcpy(buf + len, tmp, (size_t)tn);
                len += (size_t)tn;
            } else {
                /* Unknown directive: emit literal. */
                FMT_ENSURE(di + 1);
                memcpy(buf + len, directive, di);
                len += di;
                buf[len++] = spec;
            }
        } else {
            FMT_ENSURE(1);
            buf[len++] = fmt[i];
        }
    }
#undef FMT_ENSURE
    {
        mino_val_t *result = mino_string_n(S, buf != NULL ? buf : "", len);
        free(buf);
        return result;
    }
}

static mino_val_t *prim_read_string(mino_state_t *S, mino_val_t *args, mino_env_t *env)
{
    mino_val_t *s;
    mino_val_t *result;
    (void)env;
    if (!mino_is_cons(args) || mino_is_cons(args->as.cons.cdr)) {
        set_error(S, "read-string requires one string argument");
        return NULL;
    }
    s = args->as.cons.car;
    if (s == NULL || s->type != MINO_STRING) {
        set_error(S, "read-string: argument must be a string");
        return NULL;
    }
    clear_error(S);
    result = mino_read(S, s->as.s.data, NULL);
    if (result == NULL && mino_last_error(S) != NULL) {
        /* Throw parse errors as catchable exceptions so user code can
         * handle them via try/catch. */
        mino_val_t *ex = mino_string(S, mino_last_error(S));
        if (try_depth > 0) {
            try_stack[try_depth - 1].exception = ex;
            longjmp(try_stack[try_depth - 1].buf, 1);
        }
        /* No enclosing try — propagate as fatal error. */
        {
            char msg[512];
            snprintf(msg, sizeof(msg), "unhandled exception: %.*s",
                     (int)ex->as.s.len, ex->as.s.data);
            set_error(S, msg);
        }
        return NULL;
    }
    return result != NULL ? result : mino_nil(S);
}

static mino_val_t *prim_pr_str(mino_state_t *S, mino_val_t *args, mino_env_t *env)
{
    char  *buf = NULL;
    size_t len = 0;
    size_t cap = 0;
    int    first = 1;
    (void)env;
    while (mino_is_cons(args)) {
        mino_val_t *printed = print_to_string(S, args->as.cons.car);
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
            if (buf == NULL) { set_error(S, "out of memory"); return NULL; }
        }
        if (!first) buf[len++] = ' ';
        memcpy(buf + len, printed->as.s.data, printed->as.s.len);
        len += printed->as.s.len;
        first = 0;
        args = args->as.cons.cdr;
    }
    {
        mino_val_t *result = mino_string_n(S, buf != NULL ? buf : "", len);
        free(buf);
        return result;
    }
}

static mino_val_t *prim_char_at(mino_state_t *S, mino_val_t *args, mino_env_t *env)
{
    mino_val_t *s;
    long long   idx;
    (void)env;
    if (!mino_is_cons(args) || !mino_is_cons(args->as.cons.cdr) ||
        mino_is_cons(args->as.cons.cdr->as.cons.cdr)) {
        set_error(S, "char-at requires two arguments");
        return NULL;
    }
    s = args->as.cons.car;
    if (s == NULL || s->type != MINO_STRING) {
        set_error(S, "char-at: first argument must be a string");
        return NULL;
    }
    if (!as_long(args->as.cons.cdr->as.cons.car, &idx)) {
        set_error(S, "char-at: second argument must be an integer");
        return NULL;
    }
    if (idx < 0 || (size_t)idx >= s->as.s.len) {
        return prim_throw_error(S, "char-at: index out of range");
    }
    return mino_string_n(S, s->as.s.data + idx, 1);
}

static mino_val_t *prim_name(mino_state_t *S, mino_val_t *args, mino_env_t *env)
{
    mino_val_t *v;
    (void)env;
    if (!mino_is_cons(args) || mino_is_cons(args->as.cons.cdr)) {
        set_error(S, "name requires one argument");
        return NULL;
    }
    v = args->as.cons.car;
    if (v == NULL || v->type == MINO_NIL) return mino_nil(S);
    if (v->type == MINO_STRING)  return v;
    if (v->type == MINO_KEYWORD) return mino_string_n(S, v->as.s.data, v->as.s.len);
    if (v->type == MINO_SYMBOL)  return mino_string_n(S, v->as.s.data, v->as.s.len);
    set_error(S, "name: expected a keyword, symbol, or string");
    return NULL;
}

/* (rand) — return a random float in [0.0, 1.0). */
static mino_val_t *prim_rand(mino_state_t *S, mino_val_t *args, mino_env_t *env)
{
    (void)env;
    if (mino_is_cons(args)) {
        set_error(S, "rand takes no arguments");
        return NULL;
    }
    if (!rand_seeded) {
        srand((unsigned int)time(NULL));
        rand_seeded = 1;
    }
    return mino_float(S, (double)rand() / ((double)RAND_MAX + 1.0));
}

/* (eval form) — evaluate a form at runtime. */
static mino_val_t *prim_eval(mino_state_t *S, mino_val_t *args, mino_env_t *env)
{
    if (!mino_is_cons(args) || mino_is_cons(args->as.cons.cdr)) {
        set_error(S, "eval requires one argument");
        return NULL;
    }
    return eval(S, args->as.cons.car, env);
}

/* (symbol str) — create a symbol from a string. */
static mino_val_t *prim_symbol(mino_state_t *S, mino_val_t *args, mino_env_t *env)
{
    mino_val_t *v;
    (void)env;
    if (!mino_is_cons(args) || mino_is_cons(args->as.cons.cdr)) {
        set_error(S, "symbol requires one string argument");
        return NULL;
    }
    v = args->as.cons.car;
    if (v == NULL || v->type != MINO_STRING) {
        set_error(S, "symbol: argument must be a string");
        return NULL;
    }
    return mino_symbol_n(S, v->as.s.data, v->as.s.len);
}

/* (keyword str) — create a keyword from a string. */
static mino_val_t *prim_keyword(mino_state_t *S, mino_val_t *args, mino_env_t *env)
{
    mino_val_t *v;
    (void)env;
    if (!mino_is_cons(args) || mino_is_cons(args->as.cons.cdr)) {
        set_error(S, "keyword requires one string argument");
        return NULL;
    }
    v = args->as.cons.car;
    if (v == NULL || v->type != MINO_STRING) {
        set_error(S, "keyword: argument must be a string");
        return NULL;
    }
    return mino_keyword_n(S, v->as.s.data, v->as.s.len);
}

/* (hash val) — return the integer hash code of any value. */
static mino_val_t *prim_hash(mino_state_t *S, mino_val_t *args, mino_env_t *env)
{
    (void)env;
    if (!mino_is_cons(args) || mino_is_cons(args->as.cons.cdr)) {
        set_error(S, "hash requires one argument");
        return NULL;
    }
    return mino_int(S, (long long)hash_val(args->as.cons.car));
}

/* (compare a b) — general comparison returning -1, 0, or 1.
 * Compares numbers, strings, keywords, symbols, and nil. */
static mino_val_t *prim_compare(mino_state_t *S, mino_val_t *args, mino_env_t *env)
{
    mino_val_t *a, *b;
    (void)env;
    if (!mino_is_cons(args) || !mino_is_cons(args->as.cons.cdr) ||
        mino_is_cons(args->as.cons.cdr->as.cons.cdr)) {
        set_error(S, "compare requires two arguments");
        return NULL;
    }
    a = args->as.cons.car;
    b = args->as.cons.cdr->as.cons.car;
    /* nil sorts before everything */
    if ((a == NULL || (a->type == MINO_NIL)) &&
        (b == NULL || (b->type == MINO_NIL))) return mino_int(S, 0);
    if (a == NULL || a->type == MINO_NIL) return mino_int(S, -1);
    if (b == NULL || b->type == MINO_NIL) return mino_int(S, 1);
    /* numbers */
    {
        double da, db;
        if (as_double(a, &da) && as_double(b, &db)) {
            return mino_int(S, da < db ? -1 : da > db ? 1 : 0);
        }
    }
    /* strings, keywords, symbols — lexicographic */
    if ((a->type == MINO_STRING || a->type == MINO_KEYWORD ||
         a->type == MINO_SYMBOL) && a->type == b->type) {
        int cmp = strcmp(a->as.s.data, b->as.s.data);
        return mino_int(S, cmp < 0 ? -1 : cmp > 0 ? 1 : 0);
    }
    set_error(S, "compare: cannot compare values of different types");
    return NULL;
}

static mino_val_t *prim_eq(mino_state_t *S, mino_val_t *args, mino_env_t *env)
{
    (void)env;
    if (!mino_is_cons(args)) {
        return mino_true(S);
    }
    {
        mino_val_t *first = args->as.cons.car;
        args = args->as.cons.cdr;
        while (mino_is_cons(args)) {
            if (!mino_eq_force(S, first, args->as.cons.car)) {
                return mino_false(S);
            }
            args = args->as.cons.cdr;
        }
    }
    return mino_true(S);
}

static mino_val_t *prim_identical(mino_state_t *S, mino_val_t *args,
                                  mino_env_t *env)
{
    mino_val_t *a, *b;
    (void)env;
    if (!mino_is_cons(args) || !mino_is_cons(args->as.cons.cdr)) {
        set_error(S, "identical? requires 2 arguments");
        return NULL;
    }
    a = args->as.cons.car;
    b = args->as.cons.cdr->as.cons.car;
    return (a == b) ? mino_true(S) : mino_false(S);
}

/* --- Metadata primitives ------------------------------------------------- */

/*
 * Return 1 if the type supports value metadata, 0 otherwise.
 * Supported: symbols, cons, vectors, maps, sets, fns/macros.
 */
static int supports_meta(mino_type_t t)
{
    return t == MINO_SYMBOL || t == MINO_CONS || t == MINO_VECTOR
        || t == MINO_MAP    || t == MINO_SET  || t == MINO_FN
        || t == MINO_MACRO;
}

/* (meta obj) — return the metadata map, or nil if none. */
static mino_val_t *prim_meta(mino_state_t *S, mino_val_t *args,
                             mino_env_t *env)
{
    mino_val_t *obj;
    (void)env;
    if (!mino_is_cons(args)) {
        set_error(S, "meta requires 1 argument");
        return NULL;
    }
    obj = args->as.cons.car;
    if (obj == NULL || obj->meta == NULL) {
        return mino_nil(S);
    }
    return obj->meta;
}

/*
 * (with-meta obj m) — return a shallow copy of obj with metadata m.
 * m must be a map or nil.
 */
static mino_val_t *prim_with_meta(mino_state_t *S, mino_val_t *args,
                                  mino_env_t *env)
{
    mino_val_t *obj, *m, *copy;
    (void)env;
    if (!mino_is_cons(args) || !mino_is_cons(args->as.cons.cdr)) {
        set_error(S, "with-meta requires 2 arguments");
        return NULL;
    }
    obj = args->as.cons.car;
    m   = args->as.cons.cdr->as.cons.car;
    if (obj == NULL || !supports_meta(obj->type)) {
        set_error(S, "with-meta: type does not support metadata");
        return NULL;
    }
    if (m != NULL && m->type != MINO_NIL && m->type != MINO_MAP) {
        set_error(S, "with-meta: metadata must be a map or nil");
        return NULL;
    }
    /* Shallow-copy the value and attach the new metadata. */
    copy = alloc_val(S, obj->type);
    copy->as = obj->as;
    copy->meta = (m != NULL && m->type == MINO_NIL) ? NULL : m;
    return copy;
}

/*
 * (vary-meta obj f & args) — return (with-meta obj (apply f (meta obj) args)).
 */
static mino_val_t *prim_vary_meta(mino_state_t *S, mino_val_t *args,
                                  mino_env_t *env)
{
    mino_val_t *obj, *f, *old_meta, *extra, *call_args, *new_meta, *copy;
    if (!mino_is_cons(args) || !mino_is_cons(args->as.cons.cdr)) {
        set_error(S, "vary-meta requires at least 2 arguments");
        return NULL;
    }
    obj = args->as.cons.car;
    f   = args->as.cons.cdr->as.cons.car;
    extra = args->as.cons.cdr->as.cons.cdr; /* remaining args (cons list or nil) */
    if (obj == NULL || !supports_meta(obj->type)) {
        set_error(S, "vary-meta: type does not support metadata");
        return NULL;
    }
    old_meta = (obj->meta != NULL) ? obj->meta : mino_nil(S);
    /* Build (old-meta extra...) argument list for f. */
    call_args = mino_cons(S, old_meta, extra);
    new_meta = mino_call(S, f, call_args, env);
    if (new_meta == NULL) {
        return NULL;
    }
    if (new_meta->type != MINO_NIL && new_meta->type != MINO_MAP) {
        set_error(S, "vary-meta: f must return a map or nil");
        return NULL;
    }
    copy = alloc_val(S, obj->type);
    copy->as = obj->as;
    copy->meta = (new_meta->type == MINO_NIL) ? NULL : new_meta;
    return copy;
}

/*
 * (alter-meta! ref f & args) — mutate metadata in place.
 * Applies f to the current metadata of ref (plus any extra args)
 * and sets the result as the new metadata.
 */
static mino_val_t *prim_alter_meta(mino_state_t *S, mino_val_t *args,
                                   mino_env_t *env)
{
    mino_val_t *obj, *f, *old_meta, *extra, *call_args, *new_meta;
    if (!mino_is_cons(args) || !mino_is_cons(args->as.cons.cdr)) {
        set_error(S, "alter-meta! requires at least 2 arguments");
        return NULL;
    }
    obj   = args->as.cons.car;
    f     = args->as.cons.cdr->as.cons.car;
    extra = args->as.cons.cdr->as.cons.cdr;
    if (obj == NULL || !supports_meta(obj->type)) {
        set_error(S, "alter-meta!: type does not support metadata");
        return NULL;
    }
    old_meta = (obj->meta != NULL) ? obj->meta : mino_nil(S);
    call_args = mino_cons(S, old_meta, extra);
    new_meta = mino_call(S, f, call_args, env);
    if (new_meta == NULL) {
        return NULL;
    }
    if (new_meta->type != MINO_NIL && new_meta->type != MINO_MAP) {
        set_error(S, "alter-meta!: f must return a map or nil");
        return NULL;
    }
    obj->meta = (new_meta->type == MINO_NIL) ? NULL : new_meta;
    return obj->meta != NULL ? obj->meta : mino_nil(S);
}

/*
 * Chained numeric comparison. `op` selects the relation:
 *   0: <    1: <=    2: >    3: >=
 * Returns true if each successive pair satisfies the relation (and
 * trivially true on zero or one argument).
 */
static mino_val_t *compare_chain(mino_state_t *S, mino_val_t *args, const char *name, int op)
{
    double prev;
    if (!mino_is_cons(args)) {
        return mino_true(S);
    }
    if (!as_double(args->as.cons.car, &prev)) {
        char msg[64];
        snprintf(msg, sizeof(msg), "%s expects numbers", name);
        set_error(S, msg);
        return NULL;
    }
    args = args->as.cons.cdr;
    while (mino_is_cons(args)) {
        double x;
        int    ok;
        if (!as_double(args->as.cons.car, &x)) {
            char msg[64];
            snprintf(msg, sizeof(msg), "%s expects numbers", name);
            set_error(S, msg);
            return NULL;
        }
        switch (op) {
        case 0:  ok = prev <  x; break;
        case 1:  ok = prev <= x; break;
        case 2:  ok = prev >  x; break;
        default: ok = prev >= x; break;
        }
        if (!ok) {
            return mino_false(S);
        }
        prev = x;
        args = args->as.cons.cdr;
    }
    return mino_true(S);
}

static mino_val_t *prim_lt(mino_state_t *S, mino_val_t *args, mino_env_t *env)
{
    (void)env;
    return compare_chain(S, args, "<", 0);
}

static mino_val_t *prim_car(mino_state_t *S, mino_val_t *args, mino_env_t *env)
{
    (void)env;
    if (!mino_is_cons(args)) {
        set_error(S, "car requires one argument");
        return NULL;
    }
    return mino_car(args->as.cons.car);
}

static mino_val_t *prim_cdr(mino_state_t *S, mino_val_t *args, mino_env_t *env)
{
    (void)env;
    if (!mino_is_cons(args)) {
        set_error(S, "cdr requires one argument");
        return NULL;
    }
    return mino_cdr(args->as.cons.car);
}

static mino_val_t *prim_cons(mino_state_t *S, mino_val_t *args, mino_env_t *env)
{
    (void)env;
    if (!mino_is_cons(args) || !mino_is_cons(args->as.cons.cdr)) {
        set_error(S, "cons requires two arguments");
        return NULL;
    }
    return mino_cons(S, args->as.cons.car, args->as.cons.cdr->as.cons.car);
}

/* ------------------------------------------------------------------------- */
/* Collection primitives                                                     */
/*                                                                           */
/* All collection ops treat values as immutable: every operation that        */
/* "modifies" a collection returns a freshly allocated value. v0.3 uses      */
/* naïve array-backed representations; persistent tries arrive in v0.4/v0.5 */
/* without changing the public primitive contracts.                          */
/* ------------------------------------------------------------------------- */

static mino_val_t *set_conj1(mino_state_t *S, const mino_val_t *s, mino_val_t *elem);
static mino_val_t *prim_str(mino_state_t *S, mino_val_t *args, mino_env_t *env);

static size_t list_length(mino_state_t *S, mino_val_t *list)
{
    size_t n = 0;
    while (list != NULL && list->type == MINO_LAZY) {
        list = lazy_force(S, list);
    }
    while (mino_is_cons(list)) {
        n++;
        list = list->as.cons.cdr;
        /* Force lazy tails. */
        while (list != NULL && list->type == MINO_LAZY) {
            list = lazy_force(S, list);
        }
    }
    return n;
}

static int arg_count(mino_state_t *S, mino_val_t *args, size_t *out)
{
    *out = list_length(S, args);
    return 1;
}

static mino_val_t *prim_count(mino_state_t *S, mino_val_t *args, mino_env_t *env)
{
    mino_val_t *coll;
    (void)env;
    if (!mino_is_cons(args) || mino_is_cons(args->as.cons.cdr)) {
        set_error(S, "count requires one argument");
        return NULL;
    }
    coll = args->as.cons.car;
    if (coll == NULL || coll->type == MINO_NIL) {
        return mino_int(S, 0);
    }
    switch (coll->type) {
    case MINO_CONS:   return mino_int(S, (long long)list_length(S, coll));
    case MINO_VECTOR: return mino_int(S, (long long)coll->as.vec.len);
    case MINO_MAP:    return mino_int(S, (long long)coll->as.map.len);
    case MINO_SET:    return mino_int(S, (long long)coll->as.set.len);
    case MINO_STRING: return mino_int(S, (long long)coll->as.s.len);
    case MINO_LAZY: {
        /* Force the entire lazy seq and count it. */
        mino_val_t *forced = lazy_force(S, coll);
        if (forced == NULL) return NULL;
        if (forced->type == MINO_NIL) return mino_int(S, 0);
        return mino_int(S, (long long)list_length(S, forced));
    }
    default:
        {
            char msg[96];
            snprintf(msg, sizeof(msg), "count: expected a collection, got %s",
                     type_tag_str(coll));
            set_error(S, msg);
        }
        return NULL;
    }
}

static mino_val_t *prim_vector(mino_state_t *S, mino_val_t *args, mino_env_t *env)
{
    size_t n;
    size_t i;
    mino_val_t **tmp;
    mino_val_t *p;
    (void)env;
    arg_count(S, args, &n);
    if (n == 0) {
        return mino_vector(S, NULL, 0);
    }
    /* GC_T_VALARR keeps partially-gathered pointers visible to the collector;
     * without this, the optimizer may drop the `args` parameter and the cons
     * cells holding the element values become unreachable mid-construction. */
    tmp = (mino_val_t **)gc_alloc_typed(S, GC_T_VALARR, n * sizeof(*tmp));
    p = args;
    for (i = 0; i < n; i++) {
        tmp[i] = p->as.cons.car;
        p = p->as.cons.cdr;
    }
    return mino_vector(S, tmp, n);
}

static mino_val_t *prim_hash_map(mino_state_t *S, mino_val_t *args, mino_env_t *env)
{
    size_t n;
    size_t pairs;
    size_t i;
    mino_val_t **ks;
    mino_val_t **vs;
    mino_val_t *p;
    (void)env;
    arg_count(S, args, &n);
    if (n % 2 != 0) {
        set_error(S, "hash-map requires an even number of arguments");
        return NULL;
    }
    if (n == 0) {
        return mino_map(S, NULL, NULL, 0);
    }
    pairs = n / 2;
    ks = (mino_val_t **)gc_alloc_typed(S, GC_T_VALARR, pairs * sizeof(*ks));
    vs = (mino_val_t **)gc_alloc_typed(S, GC_T_VALARR, pairs * sizeof(*vs));
    p = args;
    for (i = 0; i < pairs; i++) {
        ks[i] = p->as.cons.car;
        p = p->as.cons.cdr;
        vs[i] = p->as.cons.car;
        p = p->as.cons.cdr;
    }
    return mino_map(S, ks, vs, pairs);
}

static mino_val_t *prim_nth(mino_state_t *S, mino_val_t *args, mino_env_t *env)
{
    mino_val_t *coll;
    mino_val_t *idx_val;
    mino_val_t *def_val = NULL;
    size_t      n;
    long long   idx;
    (void)env;
    arg_count(S, args, &n);
    if (n != 2 && n != 3) {
        set_error(S, "nth requires 2 or 3 arguments");
        return NULL;
    }
    coll    = args->as.cons.car;
    idx_val = args->as.cons.cdr->as.cons.car;
    if (n == 3) {
        def_val = args->as.cons.cdr->as.cons.cdr->as.cons.car;
    }
    if (idx_val == NULL || idx_val->type != MINO_INT) {
        set_error(S, "nth index must be an integer");
        return NULL;
    }
    idx = idx_val->as.i;
    if (idx < 0) {
        if (def_val != NULL) return def_val;
        return prim_throw_error(S, "nth index out of range");
    }
    if (coll == NULL || coll->type == MINO_NIL) {
        if (def_val != NULL) return def_val;
        return prim_throw_error(S, "nth index out of range");
    }
    if (coll->type == MINO_LAZY) {
        coll = lazy_force(S, coll);
        if (coll == NULL) return NULL;
        if (coll->type == MINO_NIL) {
            if (def_val != NULL) return def_val;
            return prim_throw_error(S, "nth index out of range");
        }
    }
    if (coll->type == MINO_VECTOR) {
        if ((size_t)idx >= coll->as.vec.len) {
            if (def_val != NULL) return def_val;
            return prim_throw_error(S, "nth index out of range");
        }
        return vec_nth(coll, (size_t)idx);
    }
    if (coll->type == MINO_CONS) {
        mino_val_t *p = coll;
        long long   i;
        for (i = 0; i < idx; i++) {
            if (!mino_is_cons(p)) {
                if (def_val != NULL) return def_val;
                return prim_throw_error(S, "nth index out of range");
            }
            p = p->as.cons.cdr;
            if (p != NULL && p->type == MINO_LAZY) {
                p = lazy_force(S, p);
                if (p == NULL) return NULL;
            }
        }
        if (!mino_is_cons(p)) {
            if (def_val != NULL) return def_val;
            return prim_throw_error(S, "nth index out of range");
        }
        return p->as.cons.car;
    }
    if (coll->type == MINO_STRING) {
        if ((size_t)idx >= coll->as.s.len) {
            if (def_val != NULL) return def_val;
            return prim_throw_error(S, "nth index out of range");
        }
        return mino_string_n(S, coll->as.s.data + idx, 1);
    }
    {
        char msg[96];
        snprintf(msg, sizeof(msg), "nth: expected a list, vector, or string, got %s",
                 type_tag_str(coll));
        set_error(S, msg);
    }
    return NULL;
}

static mino_val_t *prim_first(mino_state_t *S, mino_val_t *args, mino_env_t *env)
{
    mino_val_t *coll;
    (void)env;
    if (!mino_is_cons(args) || mino_is_cons(args->as.cons.cdr)) {
        set_error(S, "first requires one argument");
        return NULL;
    }
    coll = args->as.cons.car;
    if (coll == NULL || coll->type == MINO_NIL) {
        return mino_nil(S);
    }
    if (coll->type == MINO_CONS) {
        return coll->as.cons.car;
    }
    if (coll->type == MINO_VECTOR) {
        if (coll->as.vec.len == 0) {
            return mino_nil(S);
        }
        return vec_nth(coll, 0);
    }
    if (coll->type == MINO_LAZY) {
        mino_val_t *s = lazy_force(S, coll);
        if (s == NULL) return NULL;
        if (s->type == MINO_NIL || s == NULL) return mino_nil(S);
        if (s->type == MINO_CONS) return s->as.cons.car;
        return mino_nil(S);
    }
    if (coll->type == MINO_STRING) {
        if (coll->as.s.len == 0) return mino_nil(S);
        return mino_string_n(S, coll->as.s.data, 1);
    }
    if (coll->type == MINO_MAP) {
        if (coll->as.map.len == 0) return mino_nil(S);
        {
            mino_val_t *key = vec_nth(coll->as.map.key_order, 0);
            mino_val_t *val = map_get_val(coll, key);
            mino_val_t *kv[2];
            kv[0] = key;
            kv[1] = val;
            return mino_vector(S, kv, 2);
        }
    }
    if (coll->type == MINO_SET) {
        if (coll->as.set.len == 0) return mino_nil(S);
        return vec_nth(coll->as.set.key_order, 0);
    }
    {
        char msg[96];
        snprintf(msg, sizeof(msg), "first: expected a list or vector, got %s",
                 type_tag_str(coll));
        set_error(S, msg);
    }
    return NULL;
}

static mino_val_t *prim_rest(mino_state_t *S, mino_val_t *args, mino_env_t *env)
{
    mino_val_t *coll;
    (void)env;
    if (!mino_is_cons(args) || mino_is_cons(args->as.cons.cdr)) {
        set_error(S, "rest requires one argument");
        return NULL;
    }
    coll = args->as.cons.car;
    if (coll == NULL || coll->type == MINO_NIL) {
        return mino_nil(S);
    }
    if (coll->type == MINO_CONS) {
        return coll->as.cons.cdr;
    }
    if (coll->type == MINO_VECTOR) {
        /* Rest of a vector is a list of the trailing elements. v0.11 will
         * promote this to a seq abstraction. */
        mino_val_t *head = mino_nil(S);
        mino_val_t *tail = NULL;
        size_t i;
        for (i = 1; i < coll->as.vec.len; i++) {
            mino_val_t *cell = mino_cons(S, vec_nth(coll, i), mino_nil(S));
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
        mino_val_t *s = lazy_force(S, coll);
        if (s == NULL) return NULL;
        if (s->type == MINO_NIL || s == NULL) return mino_nil(S);
        if (s->type == MINO_CONS) return s->as.cons.cdr;
        return mino_nil(S);
    }
    if (coll->type == MINO_STRING) {
        if (coll->as.s.len <= 1) return mino_nil(S);
        /* Build a cons list of single-character strings. */
        mino_val_t *head = mino_nil(S);
        mino_val_t *tail = NULL;
        size_t i;
        for (i = 1; i < coll->as.s.len; i++) {
            mino_val_t *cell = mino_cons(S, mino_string_n(S, coll->as.s.data + i, 1), mino_nil(S));
            if (tail == NULL) {
                head = cell;
            } else {
                tail->as.cons.cdr = cell;
            }
            tail = cell;
        }
        return head;
    }
    if (coll->type == MINO_MAP) {
        mino_val_t *head = mino_nil(S);
        mino_val_t *tail = NULL;
        size_t i;
        for (i = 1; i < coll->as.map.len; i++) {
            mino_val_t *key = vec_nth(coll->as.map.key_order, i);
            mino_val_t *val = map_get_val(coll, key);
            mino_val_t *kv[2];
            mino_val_t *cell;
            kv[0] = key;
            kv[1] = val;
            cell = mino_cons(S, mino_vector(S, kv, 2), mino_nil(S));
            if (tail == NULL) head = cell;
            else tail->as.cons.cdr = cell;
            tail = cell;
        }
        return head;
    }
    if (coll->type == MINO_SET) {
        mino_val_t *head = mino_nil(S);
        mino_val_t *tail = NULL;
        size_t i;
        for (i = 1; i < coll->as.set.len; i++) {
            mino_val_t *cell = mino_cons(S, vec_nth(coll->as.set.key_order, i),
                                         mino_nil(S));
            if (tail == NULL) head = cell;
            else tail->as.cons.cdr = cell;
            tail = cell;
        }
        return head;
    }
    {
        char msg[96];
        snprintf(msg, sizeof(msg), "rest: expected a list or vector, got %s",
                 type_tag_str(coll));
        set_error(S, msg);
    }
    return NULL;
}

/* Layer n k/v pairs onto an existing map, returning a new map value that
 * shares structure with `coll`. Nil is treated as an empty map. */
static mino_val_t *map_assoc_pairs(mino_state_t *S, mino_val_t *coll,
                                    mino_val_t *p, size_t extra_pairs)
{
    mino_hamt_node_t *root;
    mino_val_t       *order;
    size_t            len_out;
    size_t            i;
    if (coll == NULL || coll->type == MINO_NIL) {
        root    = NULL;
        order   = mino_vector(S, NULL, 0);
        len_out = 0;
    } else {
        root    = coll->as.map.root;
        order   = coll->as.map.key_order;
        len_out = coll->as.map.len;
    }
    for (i = 0; i < extra_pairs; i++) {
        mino_val_t   *k = p->as.cons.car;
        mino_val_t   *v = p->as.cons.cdr->as.cons.car;
        hamt_entry_t *e = hamt_entry_new(S, k, v);
        uint32_t      h = hash_val(k);
        int           replaced = 0;
        root = hamt_assoc(S, root, e, h, 0u, &replaced);
        if (!replaced) {
            order = vec_conj1(S, order, k);
            len_out++;
        }
        p = p->as.cons.cdr->as.cons.cdr;
    }
    {
        mino_val_t *out = alloc_val(S, MINO_MAP);
        out->as.map.root      = root;
        out->as.map.key_order = order;
        out->as.map.len       = len_out;
        return out;
    }
}

static mino_val_t *prim_assoc(mino_state_t *S, mino_val_t *args, mino_env_t *env)
{
    mino_val_t *coll;
    size_t      n;
    size_t      extra_pairs;
    size_t      i;
    mino_val_t *p;
    (void)env;
    arg_count(S, args, &n);
    if (n < 3 || (n - 1) % 2 != 0) {
        set_error(S, "assoc requires a collection and an even number of k/v pairs");
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
                set_error(S, "assoc on vector requires integer indices");
                return NULL;
            }
            idx = k->as.i;
            if (idx < 0 || (size_t)idx > acc->as.vec.len) {
                return prim_throw_error(S, "assoc on vector: index out of range");
            }
            acc = vec_assoc1(S, acc, (size_t)idx, v);
            p = p->as.cons.cdr->as.cons.cdr;
        }
        return acc;
    }
    if (coll == NULL || coll->type == MINO_NIL || coll->type == MINO_MAP) {
        return map_assoc_pairs(S, coll, args->as.cons.cdr, extra_pairs);
    }
    {
        char msg[96];
        snprintf(msg, sizeof(msg), "assoc: expected a map or vector, got %s",
                 type_tag_str(coll));
        set_error(S, msg);
    }
    return NULL;
}

static mino_val_t *prim_get(mino_state_t *S, mino_val_t *args, mino_env_t *env)
{
    mino_val_t *coll;
    mino_val_t *key;
    mino_val_t *def_val = mino_nil(S);
    size_t      n;
    (void)env;
    arg_count(S, args, &n);
    if (n != 2 && n != 3) {
        set_error(S, "get requires 2 or 3 arguments");
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

static mino_val_t *prim_conj(mino_state_t *S, mino_val_t *args, mino_env_t *env)
{
    mino_val_t *coll;
    size_t      n;
    mino_val_t *p;
    (void)env;
    arg_count(S, args, &n);
    if (n < 2) {
        set_error(S, "conj requires a collection and at least one item");
        return NULL;
    }
    coll = args->as.cons.car;
    p    = args->as.cons.cdr;
    if (coll == NULL || coll->type == MINO_NIL || coll->type == MINO_CONS) {
        /* List/nil: prepend each item so (conj '(1 2) 3 4) => (4 3 1 2). */
        mino_val_t *out = (coll == NULL || coll->type == MINO_NIL)
            ? mino_nil(S) : coll;
        while (mino_is_cons(p)) {
            out = mino_cons(S, p->as.cons.car, out);
            p = p->as.cons.cdr;
        }
        return out;
    }
    if (coll->type == MINO_VECTOR) {
        size_t extra = n - 1;
        mino_val_t *acc = coll;
        size_t i;
        for (i = 0; i < extra; i++) {
            acc = vec_conj1(S, acc, p->as.cons.car);
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
                set_error(S, "conj on map requires 2-element vectors");
                return NULL;
            }
            pair_args = mino_cons(S, vec_nth(item, 0),
                                   mino_cons(S, vec_nth(item, 1), mino_nil(S)));
            acc = map_assoc_pairs(S, acc, pair_args, 1);
            p = p->as.cons.cdr;
        }
        return acc;
    }
    if (coll->type == MINO_SET) {
        mino_val_t *acc = coll;
        while (mino_is_cons(p)) {
            acc = set_conj1(S, acc, p->as.cons.car);
            p = p->as.cons.cdr;
        }
        return acc;
    }
    {
        char msg[96];
        snprintf(msg, sizeof(msg),
                 "conj: expected a list, vector, map, or set, got %s",
                 type_tag_str(coll));
        set_error(S, msg);
    }
    return NULL;
}

static mino_val_t *prim_keys(mino_state_t *S, mino_val_t *args, mino_env_t *env)
{
    mino_val_t *coll;
    mino_val_t *head = mino_nil(S);
    mino_val_t *tail = NULL;
    size_t i;
    (void)env;
    if (!mino_is_cons(args) || mino_is_cons(args->as.cons.cdr)) {
        set_error(S, "keys requires one argument");
        return NULL;
    }
    coll = args->as.cons.car;
    if (coll == NULL || coll->type == MINO_NIL) {
        return mino_nil(S);
    }
    if (coll->type != MINO_MAP) {
        set_error(S, "keys: argument must be a map");
        return NULL;
    }
    for (i = 0; i < coll->as.map.len; i++) {
        mino_val_t *cell = mino_cons(S, vec_nth(coll->as.map.key_order, i),
                                      mino_nil(S));
        if (tail == NULL) {
            head = cell;
        } else {
            tail->as.cons.cdr = cell;
        }
        tail = cell;
    }
    return head;
}

static mino_val_t *prim_vals(mino_state_t *S, mino_val_t *args, mino_env_t *env)
{
    mino_val_t *coll;
    mino_val_t *head = mino_nil(S);
    mino_val_t *tail = NULL;
    size_t i;
    (void)env;
    if (!mino_is_cons(args) || mino_is_cons(args->as.cons.cdr)) {
        set_error(S, "vals requires one argument");
        return NULL;
    }
    coll = args->as.cons.car;
    if (coll == NULL || coll->type == MINO_NIL) {
        return mino_nil(S);
    }
    if (coll->type != MINO_MAP) {
        set_error(S, "vals: argument must be a map");
        return NULL;
    }
    for (i = 0; i < coll->as.map.len; i++) {
        mino_val_t *key  = vec_nth(coll->as.map.key_order, i);
        mino_val_t *cell = mino_cons(S, map_get_val(coll, key), mino_nil(S));
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
static mino_val_t *set_conj1(mino_state_t *S, const mino_val_t *s, mino_val_t *elem)
{
    mino_val_t       *v        = alloc_val(S, MINO_SET);
    mino_val_t       *sentinel = mino_true(S);
    hamt_entry_t     *e        = hamt_entry_new(S, elem, sentinel);
    uint32_t          h        = hash_val(elem);
    int               replaced = 0;
    mino_hamt_node_t *root     = hamt_assoc(S, s->as.set.root, e, h, 0u, &replaced);
    v->as.set.root      = root;
    if (replaced) {
        v->as.set.key_order = s->as.set.key_order;
        v->as.set.len       = s->as.set.len;
    } else {
        v->as.set.key_order = vec_conj1(S, s->as.set.key_order, elem);
        v->as.set.len       = s->as.set.len + 1;
    }
    return v;
}

static mino_val_t *prim_hash_set(mino_state_t *S, mino_val_t *args, mino_env_t *env)
{
    size_t      n;
    size_t      i;
    mino_val_t **tmp;
    mino_val_t *p;
    (void)env;
    arg_count(S, args, &n);
    tmp = (mino_val_t **)gc_alloc_typed(S, GC_T_VALARR, (n > 0 ? n : 1) * sizeof(*tmp));
    p = args;
    for (i = 0; i < n; i++) {
        tmp[i] = p->as.cons.car;
        p = p->as.cons.cdr;
    }
    return mino_set(S, tmp, n);
}

static mino_val_t *prim_contains_p(mino_state_t *S, mino_val_t *args, mino_env_t *env)
{
    mino_val_t *coll;
    mino_val_t *key;
    (void)env;
    if (!mino_is_cons(args) || !mino_is_cons(args->as.cons.cdr)
        || mino_is_cons(args->as.cons.cdr->as.cons.cdr)) {
        set_error(S, "contains? requires two arguments");
        return NULL;
    }
    coll = args->as.cons.car;
    key  = args->as.cons.cdr->as.cons.car;
    if (coll == NULL || coll->type == MINO_NIL) {
        return mino_false(S);
    }
    if (coll->type == MINO_MAP) {
        return map_get_val(coll, key) != NULL ? mino_true(S) : mino_false(S);
    }
    if (coll->type == MINO_SET) {
        uint32_t h = hash_val(key);
        return hamt_get(coll->as.set.root, key, h, 0u) != NULL
            ? mino_true(S) : mino_false(S);
    }
    if (coll->type == MINO_VECTOR) {
        /* For vectors, key is an index. */
        if (key != NULL && key->type == MINO_INT) {
            long long idx = key->as.i;
            return (idx >= 0 && (size_t)idx < coll->as.vec.len)
                ? mino_true(S) : mino_false(S);
        }
        return mino_false(S);
    }
    {
        char msg[96];
        snprintf(msg, sizeof(msg),
                 "contains?: expected a map, set, or vector, got %s",
                 type_tag_str(coll));
        set_error(S, msg);
    }
    return NULL;
}

static mino_val_t *prim_disj(mino_state_t *S, mino_val_t *args, mino_env_t *env)
{
    mino_val_t *coll;
    mino_val_t *p;
    size_t      n;
    (void)env;
    arg_count(S, args, &n);
    if (n < 2) {
        set_error(S, "disj requires a set and at least one key");
        return NULL;
    }
    coll = args->as.cons.car;
    if (coll == NULL || coll->type == MINO_NIL) {
        return mino_nil(S);
    }
    if (coll->type != MINO_SET) {
        set_error(S, "disj: first argument must be a set");
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
            mino_val_t *new_set = alloc_val(S, MINO_SET);
            mino_val_t *order   = mino_vector(S, NULL, 0);
            mino_hamt_node_t *root = NULL;
            size_t i;
            size_t new_len = 0;
            for (i = 0; i < coll->as.set.len; i++) {
                mino_val_t *elem = vec_nth(coll->as.set.key_order, i);
                if (!mino_eq(elem, key)) {
                    hamt_entry_t *e2 = hamt_entry_new(S, elem, mino_true(S));
                    uint32_t h2 = hash_val(elem);
                    int rep = 0;
                    root = hamt_assoc(S, root, e2, h2, 0u, &rep);
                    order = vec_conj1(S, order, elem);
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

static mino_val_t *prim_dissoc(mino_state_t *S, mino_val_t *args, mino_env_t *env)
{
    mino_val_t *coll;
    mino_val_t *p;
    size_t      n;
    (void)env;
    arg_count(S, args, &n);
    if (n < 2) {
        set_error(S, "dissoc requires a map and at least one key");
        return NULL;
    }
    coll = args->as.cons.car;
    if (coll == NULL || coll->type == MINO_NIL) {
        return mino_nil(S);
    }
    if (coll->type != MINO_MAP) {
        set_error(S, "dissoc: first argument must be a map");
        return NULL;
    }
    p = args->as.cons.cdr;
    while (mino_is_cons(p)) {
        mino_val_t *key = p->as.cons.car;
        uint32_t    h   = hash_val(key);
        if (hamt_get(coll->as.map.root, key, h, 0u) != NULL) {
            mino_val_t *new_map = alloc_val(S, MINO_MAP);
            mino_val_t *order   = mino_vector(S, NULL, 0);
            mino_hamt_node_t *root = NULL;
            size_t i;
            size_t new_len = 0;
            for (i = 0; i < coll->as.map.len; i++) {
                mino_val_t *k = vec_nth(coll->as.map.key_order, i);
                if (!mino_eq(k, key)) {
                    mino_val_t   *v  = map_get_val(coll, k);
                    hamt_entry_t *e2 = hamt_entry_new(S, k, v);
                    uint32_t      h2 = hash_val(k);
                    int rep = 0;
                    root = hamt_assoc(S, root, e2, h2, 0u, &rep);
                    order = vec_conj1(S, order, k);
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

static void seq_iter_init(mino_state_t *S, seq_iter_t *it, const mino_val_t *coll)
{
    /* Force lazy seqs so they behave as cons lists. */
    if (coll != NULL && coll->type == MINO_LAZY) {
        coll = lazy_force(S, (mino_val_t *)coll);
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

static mino_val_t *seq_iter_val(mino_state_t *S, const seq_iter_t *it)
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
        return mino_vector(S, kv, 2);
    }
    case MINO_SET:    return vec_nth(c->as.set.key_order, it->idx);
    case MINO_STRING: return mino_string_n(S, c->as.s.data + it->idx, 1);
    default:          return mino_nil(S);
    }
}

static void seq_iter_next(mino_state_t *S, seq_iter_t *it)
{
    if (it->coll != NULL && it->coll->type == MINO_CONS) {
        if (it->cons_p != NULL && it->cons_p->type == MINO_CONS) {
            const mino_val_t *next = it->cons_p->as.cons.cdr;
            /* Force lazy tail if present. */
            if (next != NULL && next->type == MINO_LAZY) {
                next = lazy_force(S, (mino_val_t *)next);
            }
            it->cons_p = next;
        }
    } else {
        it->idx++;
    }
}

/* (map, filter are now lazy in core.mino) */

/* (reduced val) — wrap val to signal early termination in reduce. */
static mino_val_t *prim_reduced(mino_state_t *S, mino_val_t *args, mino_env_t *env)
{
    mino_val_t *v;
    (void)env;
    if (!mino_is_cons(args) || mino_is_cons(args->as.cons.cdr)) {
        set_error(S, "reduced requires exactly 1 argument");
        return NULL;
    }
    v = alloc_val(S, MINO_REDUCED);
    v->as.reduced.val = args->as.cons.car;
    return v;
}

/* (reduced? x) — true if x is a reduced wrapper. */
static mino_val_t *prim_reduced_p(mino_state_t *S, mino_val_t *args, mino_env_t *env)
{
    (void)env;
    if (!mino_is_cons(args) || mino_is_cons(args->as.cons.cdr)) {
        set_error(S, "reduced? requires exactly 1 argument");
        return NULL;
    }
    return (args->as.cons.car != NULL && args->as.cons.car->type == MINO_REDUCED)
        ? mino_true(S) : mino_false(S);
}

static mino_val_t *prim_reduce(mino_state_t *S, mino_val_t *args, mino_env_t *env)
{
    mino_val_t *fn;
    mino_val_t *acc;
    mino_val_t *coll;
    seq_iter_t  it;
    size_t      n;
    arg_count(S, args, &n);
    if (n == 2) {
        /* (reduce f coll) — first element is the initial accumulator. */
        fn   = args->as.cons.car;
        coll = args->as.cons.cdr->as.cons.car;
        if (coll == NULL || coll->type == MINO_NIL) {
            /* (reduce f nil) → (f) */
            return apply_callable(S, fn, mino_nil(S), env);
        }
        seq_iter_init(S, &it, coll);
        if (seq_iter_done(&it)) {
            return apply_callable(S, fn, mino_nil(S), env);
        }
        acc = seq_iter_val(S, &it);
        seq_iter_next(S, &it);
    } else if (n == 3) {
        /* (reduce f init coll) */
        fn   = args->as.cons.car;
        acc  = args->as.cons.cdr->as.cons.car;
        coll = args->as.cons.cdr->as.cons.cdr->as.cons.car;
        if (coll == NULL || coll->type == MINO_NIL) {
            return acc;
        }
        seq_iter_init(S, &it, coll);
    } else {
        set_error(S, "reduce requires 2 or 3 arguments");
        return NULL;
    }
    while (!seq_iter_done(&it)) {
        mino_val_t *elem   = seq_iter_val(S, &it);
        mino_val_t *call_a = mino_cons(S, acc, mino_cons(S, elem, mino_nil(S)));
        acc = apply_callable(S, fn, call_a, env);
        if (acc == NULL) return NULL;
        if (acc->type == MINO_REDUCED) return acc->as.reduced.val;
        seq_iter_next(S, &it);
    }
    return acc;
}

/* (set coll) — create a set from a collection. */
static mino_val_t *prim_set(mino_state_t *S, mino_val_t *args, mino_env_t *env)
{
    mino_val_t *coll;
    mino_val_t *result;
    seq_iter_t  it;
    (void)env;
    if (!mino_is_cons(args) || mino_is_cons(args->as.cons.cdr)) {
        set_error(S, "set requires exactly 1 argument");
        return NULL;
    }
    coll = args->as.cons.car;
    if (coll == NULL || coll->type == MINO_NIL) {
        return mino_set(S, NULL, 0);
    }
    result = mino_set(S, NULL, 0);
    gc_pin(result);
    seq_iter_init(S, &it, coll);
    while (!seq_iter_done(&it)) {
        result = set_conj1(S, result, seq_iter_val(S, &it));
        seq_iter_next(S, &it);
    }
    gc_unpin(1);
    return result;
}

/* (take, drop, range, repeat, concat are now lazy in core.mino) */

/* Eager range returning a vector. Avoids lazy thunk overhead for tight loops.
 * (rangev end) or (rangev start end) or (rangev start end step). */
static mino_val_t *prim_rangev(mino_state_t *S, mino_val_t *args, mino_env_t *env)
{
    long long start = 0, end = 0, step = 1, i;
    size_t n, len;
    mino_val_t **items;
    mino_val_t *result;
    (void)env;
    arg_count(S, args, &n);
    if (n == 1) {
        if (!mino_to_int(args->as.cons.car, &end)) {
            set_error(S, "rangev argument must be an integer"); return NULL;
        }
    } else if (n == 2) {
        if (!mino_to_int(args->as.cons.car, &start) ||
            !mino_to_int(args->as.cons.cdr->as.cons.car, &end)) {
            set_error(S, "rangev arguments must be integers"); return NULL;
        }
    } else if (n == 3) {
        if (!mino_to_int(args->as.cons.car, &start) ||
            !mino_to_int(args->as.cons.cdr->as.cons.car, &end) ||
            !mino_to_int(args->as.cons.cdr->as.cons.cdr->as.cons.car, &step)) {
            set_error(S, "rangev arguments must be integers"); return NULL;
        }
        if (step == 0) {
            set_error(S, "rangev step must not be zero"); return NULL;
        }
    } else {
        set_error(S, "rangev requires 1, 2, or 3 arguments"); return NULL;
    }
    /* Compute length. */
    if (step > 0) {
        len = (end > start) ? (size_t)((end - start + step - 1) / step) : 0;
    } else {
        len = (start > end) ? (size_t)((start - end + (-step) - 1) / (-step)) : 0;
    }
    items = malloc(len * sizeof(mino_val_t *));
    if (!items && len > 0) { set_error(S, "rangev: out of memory"); return NULL; }
    for (i = start, n = 0; n < len; i += step, n++) {
        items[n] = mino_int(S, i);
    }
    result = mino_vector(S, items, len);
    free(items);
    return result;
}

/* Eager map returning a vector. (mapv f coll) */
static mino_val_t *prim_mapv(mino_state_t *S, mino_val_t *args, mino_env_t *env)
{
    mino_val_t *fn, *coll;
    seq_iter_t  it;
    size_t      cap = 64, len = 0;
    mino_val_t **items;
    mino_val_t *result;
    size_t n;
    arg_count(S, args, &n);
    if (n != 2) {
        set_error(S, "mapv requires 2 arguments: function and collection");
        return NULL;
    }
    fn   = args->as.cons.car;
    coll = args->as.cons.cdr->as.cons.car;
    if (coll == NULL || mino_is_nil(coll)) {
        return mino_vector(S, NULL, 0);
    }
    items = malloc(cap * sizeof(mino_val_t *));
    if (!items) { set_error(S, "mapv: out of memory"); return NULL; }
    seq_iter_init(S, &it, coll);
    while (!seq_iter_done(&it)) {
        mino_val_t *elem = seq_iter_val(S, &it);
        mino_val_t *call_args = mino_cons(S, elem, mino_nil(S));
        mino_val_t *val = apply_callable(S, fn, call_args, env);
        if (val == NULL) { free(items); return NULL; }
        if (len >= cap) {
            cap *= 2;
            items = realloc(items, cap * sizeof(mino_val_t *));
            if (!items) { set_error(S, "mapv: out of memory"); return NULL; }
        }
        items[len++] = val;
        seq_iter_next(S, &it);
    }
    result = mino_vector(S, items, len);
    free(items);
    return result;
}

/* Eager filter returning a vector. (filterv pred coll) */
static mino_val_t *prim_filterv(mino_state_t *S, mino_val_t *args, mino_env_t *env)
{
    mino_val_t *pred, *coll;
    seq_iter_t  it;
    size_t      cap = 64, len = 0;
    mino_val_t **items;
    mino_val_t *result;
    size_t n;
    arg_count(S, args, &n);
    if (n != 2) {
        set_error(S, "filterv requires 2 arguments: predicate and collection");
        return NULL;
    }
    pred = args->as.cons.car;
    coll = args->as.cons.cdr->as.cons.car;
    if (coll == NULL || mino_is_nil(coll)) {
        return mino_vector(S, NULL, 0);
    }
    items = malloc(cap * sizeof(mino_val_t *));
    if (!items) { set_error(S, "filterv: out of memory"); return NULL; }
    seq_iter_init(S, &it, coll);
    while (!seq_iter_done(&it)) {
        mino_val_t *elem = seq_iter_val(S, &it);
        mino_val_t *call_args = mino_cons(S, elem, mino_nil(S));
        mino_val_t *test = apply_callable(S, pred, call_args, env);
        if (test == NULL) { free(items); return NULL; }
        if (mino_is_truthy(test)) {
            if (len >= cap) {
                cap *= 2;
                items = realloc(items, cap * sizeof(mino_val_t *));
                if (!items) { set_error(S, "filterv: out of memory"); return NULL; }
            }
            items[len++] = elem;
        }
        seq_iter_next(S, &it);
    }
    result = mino_vector(S, items, len);
    free(items);
    return result;
}

static mino_val_t *prim_into(mino_state_t *S, mino_val_t *args, mino_env_t *env)
{
    mino_val_t *to;
    mino_val_t *from;
    seq_iter_t  it;
    (void)env;
    if (!mino_is_cons(args) || !mino_is_cons(args->as.cons.cdr)) {
        set_error(S, "into requires two arguments");
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
        mino_val_t *out = mino_nil(S);
        seq_iter_init(S, &it, from);
        while (!seq_iter_done(&it)) {
            out = mino_cons(S, seq_iter_val(S, &it), out);
            seq_iter_next(S, &it);
        }
        return out;
    }
    if (to->type == MINO_VECTOR) {
        mino_val_t *acc = to;
        seq_iter_init(S, &it, from);
        while (!seq_iter_done(&it)) {
            acc = vec_conj1(S, acc, seq_iter_val(S, &it));
            seq_iter_next(S, &it);
        }
        return acc;
    }
    if (to->type == MINO_MAP) {
        mino_val_t *acc = to;
        seq_iter_init(S, &it, from);
        while (!seq_iter_done(&it)) {
            mino_val_t *item = seq_iter_val(S, &it);
            mino_val_t *pair_args;
            if (item == NULL || item->type != MINO_VECTOR
                || item->as.vec.len != 2) {
                set_error(S, "into map: each element must be a 2-element vector");
                return NULL;
            }
            pair_args = mino_cons(S, vec_nth(item, 0),
                                   mino_cons(S, vec_nth(item, 1), mino_nil(S)));
            acc = map_assoc_pairs(S, acc, pair_args, 1);
            seq_iter_next(S, &it);
        }
        return acc;
    }
    if (to->type == MINO_SET) {
        mino_val_t *acc = to;
        seq_iter_init(S, &it, from);
        while (!seq_iter_done(&it)) {
            acc = set_conj1(S, acc, seq_iter_val(S, &it));
            seq_iter_next(S, &it);
        }
        return acc;
    }
    if (to->type == MINO_CONS) {
        mino_val_t *out = to;
        seq_iter_init(S, &it, from);
        while (!seq_iter_done(&it)) {
            out = mino_cons(S, seq_iter_val(S, &it), out);
            seq_iter_next(S, &it);
        }
        return out;
    }
    {
        char msg[96];
        snprintf(msg, sizeof(msg),
                 "into: expected a list, vector, map, or set as target, got %s",
                 type_tag_str(to));
        set_error(S, msg);
    }
    return NULL;
}

static mino_val_t *prim_apply(mino_state_t *S, mino_val_t *args, mino_env_t *env)
{
    mino_val_t *fn;
    mino_val_t *last;
    mino_val_t *call_args;
    mino_val_t *p;
    size_t      n;
    arg_count(S, args, &n);
    if (n < 2) {
        set_error(S, "apply requires a function and arguments");
        return NULL;
    }
    fn = args->as.cons.car;
    if (n == 2) {
        /* (apply f coll) — spread coll as args. */
        last = args->as.cons.cdr->as.cons.car;
    } else {
        /* (apply f a b ... coll) — prepend individual args, then spread coll. */
        mino_val_t *head = mino_nil(S);
        mino_val_t *tail2 = NULL;
        p = args->as.cons.cdr;
        /* Collect all but the last arg as individual args. */
        while (mino_is_cons(p) && mino_is_cons(p->as.cons.cdr)) {
            mino_val_t *cell = mino_cons(S, p->as.cons.car, mino_nil(S));
            if (tail2 == NULL) { head = cell; } else { tail2->as.cons.cdr = cell; }
            tail2 = cell;
            p = p->as.cons.cdr;
        }
        last = p->as.cons.car; /* the final collection argument */
        /* Append elements from `last` collection. */
        if (last != NULL && last->type != MINO_NIL) {
            seq_iter_t it;
            seq_iter_init(S, &it, last);
            while (!seq_iter_done(&it)) {
                mino_val_t *cell = mino_cons(S, seq_iter_val(S, &it), mino_nil(S));
                if (tail2 == NULL) { head = cell; } else { tail2->as.cons.cdr = cell; }
                tail2 = cell;
                seq_iter_next(S, &it);
            }
        }
        return apply_callable(S, fn, head, env);
    }
    /* (apply f coll) — convert coll to a cons arg list. */
    if (last == NULL || last->type == MINO_NIL) {
        return apply_callable(S, fn, mino_nil(S), env);
    }
    if (last->type == MINO_CONS) {
        return apply_callable(S, fn, last, env);
    }
    /* Convert non-list collection to cons list. */
    {
        mino_val_t *head = mino_nil(S);
        mino_val_t *tail2 = NULL;
        seq_iter_t it;
        seq_iter_init(S, &it, last);
        while (!seq_iter_done(&it)) {
            mino_val_t *cell = mino_cons(S, seq_iter_val(S, &it), mino_nil(S));
            if (tail2 == NULL) { head = cell; } else { tail2->as.cons.cdr = cell; }
            tail2 = cell;
            seq_iter_next(S, &it);
        }
        call_args = head;
    }
    return apply_callable(S, fn, call_args, env);
}

static mino_val_t *prim_reverse(mino_state_t *S, mino_val_t *args, mino_env_t *env)
{
    mino_val_t *coll;
    mino_val_t *out = mino_nil(S);
    seq_iter_t  it;
    (void)env;
    if (!mino_is_cons(args) || mino_is_cons(args->as.cons.cdr)) {
        set_error(S, "reverse requires one argument");
        return NULL;
    }
    coll = args->as.cons.car;
    if (coll == NULL || coll->type == MINO_NIL) {
        return mino_nil(S);
    }
    seq_iter_init(S, &it, coll);
    while (!seq_iter_done(&it)) {
        out = mino_cons(S, seq_iter_val(S, &it), out);
        seq_iter_next(S, &it);
    }
    return out;
}

static mino_val_t *prim_sort(mino_state_t *S, mino_val_t *args, mino_env_t *env);

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

static int sort_compare(mino_state_t *S, const mino_val_t *a, const mino_val_t *b)
{
    if (sort_comp_fn != NULL) {
        mino_val_t *call_args = mino_cons(S, (mino_val_t *)a,
                                  mino_cons(S, (mino_val_t *)b, mino_nil(S)));
        mino_val_t *result = mino_call(S, sort_comp_fn, call_args, sort_comp_env);
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
static void merge_sort_vals(mino_state_t *S, mino_val_t **arr, mino_val_t **tmp, size_t len)
{
    size_t mid, i, j, k;
    if (len <= 1) return;
    mid = len / 2;
    merge_sort_vals(S, arr, tmp, mid);
    merge_sort_vals(S, arr + mid, tmp, len - mid);
    memcpy(tmp, arr, mid * sizeof(*tmp));
    i = 0; j = mid; k = 0;
    while (i < mid && j < len) {
        if (sort_compare(S, tmp[i], arr[j]) <= 0) {
            arr[k++] = tmp[i++];
        } else {
            arr[k++] = arr[j++];
        }
    }
    while (i < mid) { arr[k++] = tmp[i++]; }
}

/* (sort coll) or (sort comp coll) */
static mino_val_t *prim_sort(mino_state_t *S, mino_val_t *args, mino_env_t *env)
{
    mino_val_t *coll;
    mino_val_t *comp = NULL;
    mino_val_t **arr;
    mino_val_t **tmp;
    mino_val_t *head = mino_nil(S);
    mino_val_t *tail = NULL;
    size_t      n_items, i;
    seq_iter_t  it;
    if (!mino_is_cons(args)) {
        set_error(S, "sort requires one or two arguments");
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
        set_error(S, "sort requires one or two arguments");
        return NULL;
    }
    if (coll == NULL || coll->type == MINO_NIL) {
        return mino_nil(S);
    }
    /* Collect elements into an array. */
    n_items = 0;
    seq_iter_init(S, &it, coll);
    while (!seq_iter_done(&it)) { n_items++; seq_iter_next(S, &it); }
    if (n_items == 0) return mino_nil(S);
    arr = (mino_val_t **)gc_alloc_typed(S, GC_T_VALARR, n_items * sizeof(*arr));
    tmp = (mino_val_t **)gc_alloc_typed(S, GC_T_VALARR, n_items * sizeof(*tmp));
    i = 0;
    seq_iter_init(S, &it, coll);
    while (!seq_iter_done(&it)) { arr[i++] = seq_iter_val(S, &it); seq_iter_next(S, &it); }
    sort_comp_fn  = comp;
    sort_comp_env = env;
    merge_sort_vals(S, arr, tmp, n_items);
    sort_comp_fn  = NULL;
    sort_comp_env = NULL;
    for (i = 0; i < n_items; i++) {
        mino_val_t *cell = mino_cons(S, arr[i], mino_nil(S));
        if (tail == NULL) { head = cell; } else { tail->as.cons.cdr = cell; }
        tail = cell;
    }
    return head;
}

/* ------------------------------------------------------------------------- */
/* String primitives                                                         */
/* ------------------------------------------------------------------------- */

static mino_val_t *prim_subs(mino_state_t *S, mino_val_t *args, mino_env_t *env)
{
    mino_val_t *s_val;
    long long   start, end_idx;
    size_t      n;
    (void)env;
    arg_count(S, args, &n);
    if (n != 2 && n != 3) {
        set_error(S, "subs requires 2 or 3 arguments");
        return NULL;
    }
    s_val = args->as.cons.car;
    if (s_val == NULL || s_val->type != MINO_STRING) {
        set_error(S, "subs: first argument must be a string");
        return NULL;
    }
    if (args->as.cons.cdr->as.cons.car == NULL
        || args->as.cons.cdr->as.cons.car->type != MINO_INT) {
        set_error(S, "subs: start index must be an integer");
        return NULL;
    }
    start = args->as.cons.cdr->as.cons.car->as.i;
    if (n == 3) {
        if (args->as.cons.cdr->as.cons.cdr->as.cons.car == NULL
            || args->as.cons.cdr->as.cons.cdr->as.cons.car->type != MINO_INT) {
            set_error(S, "subs: end index must be an integer");
            return NULL;
        }
        end_idx = args->as.cons.cdr->as.cons.cdr->as.cons.car->as.i;
    } else {
        end_idx = (long long)s_val->as.s.len;
    }
    if (start < 0 || end_idx < start || (size_t)end_idx > s_val->as.s.len) {
        return prim_throw_error(S, "subs: index out of range");
    }
    return mino_string_n(S, s_val->as.s.data + start, (size_t)(end_idx - start));
}

static mino_val_t *prim_split(mino_state_t *S, mino_val_t *args, mino_env_t *env)
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
        set_error(S, "split requires a string and a separator");
        return NULL;
    }
    s_val   = args->as.cons.car;
    sep_val = args->as.cons.cdr->as.cons.car;
    if (s_val == NULL || s_val->type != MINO_STRING
        || sep_val == NULL || sep_val->type != MINO_STRING) {
        set_error(S, "split: both arguments must be strings");
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
        buf = (mino_val_t **)gc_alloc_typed(S, GC_T_VALARR,
              (slen > 0 ? slen : 1) * sizeof(*buf));
        for (i = 0; i < slen; i++) {
            buf[i] = mino_string_n(S, s + i, 1);
        }
        return mino_vector(S, buf, slen);
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
            mino_val_t **nb = (mino_val_t **)gc_alloc_typed(S, 
                GC_T_VALARR, new_cap * sizeof(*nb));
            if (buf != NULL && len > 0) memcpy(nb, buf, len * sizeof(*nb));
            buf = nb;
            cap = new_cap;
        }
        if (found != NULL) {
            buf[len++] = mino_string_n(S, p, (size_t)(found - p));
            p = found + sep_len;
        } else {
            buf[len++] = mino_string_n(S, p, (size_t)(s + slen - p));
            break;
        }
    }
    return mino_vector(S, buf, len);
}

static mino_val_t *prim_join(mino_state_t *S, mino_val_t *args, mino_env_t *env)
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
    arg_count(S, args, &n);
    if (n == 1) {
        /* (join coll) — no separator. */
        coll = args->as.cons.car;
    } else if (n == 2) {
        /* (join sep coll) */
        sep_val = args->as.cons.car;
        coll    = args->as.cons.cdr->as.cons.car;
        if (sep_val == NULL || sep_val->type != MINO_STRING) {
            set_error(S, "join: separator must be a string");
            return NULL;
        }
        sep     = sep_val->as.s.data;
        sep_len = sep_val->as.s.len;
    } else {
        set_error(S, "join requires 1 or 2 arguments");
        return NULL;
    }
    if (coll == NULL || coll->type == MINO_NIL) {
        return mino_string(S, "");
    }
    seq_iter_init(S, &it, coll);
    while (!seq_iter_done(&it)) {
        mino_val_t *elem = seq_iter_val(S, &it);
        const char *part;
        size_t      part_len;
        size_t      need;
        if (elem == NULL || elem->type == MINO_NIL) {
            seq_iter_next(S, &it);
            continue;
        }
        if (elem->type == MINO_STRING) {
            part     = elem->as.s.data;
            part_len = elem->as.s.len;
        } else {
            /* Convert to string. */
            mino_val_t *str_a = mino_cons(S, elem, mino_nil(S));
            mino_val_t *str   = prim_str(S, str_a, env);
            if (str == NULL) return NULL;
            part     = str->as.s.data;
            part_len = str->as.s.len;
        }
        need = buf_len + (first ? 0 : sep_len) + part_len + 1;
        if (need > buf_cap) {
            buf_cap = buf_cap == 0 ? 128 : buf_cap;
            while (buf_cap < need) buf_cap *= 2;
            buf = (char *)realloc(buf, buf_cap);
            if (buf == NULL) { set_error(S, "out of memory"); return NULL; }
        }
        if (!first && sep_len > 0) {
            memcpy(buf + buf_len, sep, sep_len);
            buf_len += sep_len;
        }
        memcpy(buf + buf_len, part, part_len);
        buf_len += part_len;
        first = 0;
        seq_iter_next(S, &it);
    }
    {
        mino_val_t *result = mino_string_n(S, buf != NULL ? buf : "", buf_len);
        free(buf);
        return result;
    }
}

static mino_val_t *prim_starts_with_p(mino_state_t *S, mino_val_t *args, mino_env_t *env)
{
    mino_val_t *s, *prefix;
    (void)env;
    if (!mino_is_cons(args) || !mino_is_cons(args->as.cons.cdr)) {
        set_error(S, "starts-with? requires two string arguments");
        return NULL;
    }
    s      = args->as.cons.car;
    prefix = args->as.cons.cdr->as.cons.car;
    if (s == NULL || s->type != MINO_STRING
        || prefix == NULL || prefix->type != MINO_STRING) {
        set_error(S, "starts-with? requires two string arguments");
        return NULL;
    }
    if (prefix->as.s.len > s->as.s.len) return mino_false(S);
    return memcmp(s->as.s.data, prefix->as.s.data, prefix->as.s.len) == 0
        ? mino_true(S) : mino_false(S);
}

static mino_val_t *prim_ends_with_p(mino_state_t *S, mino_val_t *args, mino_env_t *env)
{
    mino_val_t *s, *suffix;
    (void)env;
    if (!mino_is_cons(args) || !mino_is_cons(args->as.cons.cdr)) {
        set_error(S, "ends-with? requires two string arguments");
        return NULL;
    }
    s      = args->as.cons.car;
    suffix = args->as.cons.cdr->as.cons.car;
    if (s == NULL || s->type != MINO_STRING
        || suffix == NULL || suffix->type != MINO_STRING) {
        set_error(S, "ends-with? requires two string arguments");
        return NULL;
    }
    if (suffix->as.s.len > s->as.s.len) return mino_false(S);
    return memcmp(s->as.s.data + s->as.s.len - suffix->as.s.len,
                  suffix->as.s.data, suffix->as.s.len) == 0
        ? mino_true(S) : mino_false(S);
}

static mino_val_t *prim_includes_p(mino_state_t *S, mino_val_t *args, mino_env_t *env)
{
    mino_val_t *s, *sub;
    const char *p;
    (void)env;
    if (!mino_is_cons(args) || !mino_is_cons(args->as.cons.cdr)) {
        set_error(S, "includes? requires two string arguments");
        return NULL;
    }
    s   = args->as.cons.car;
    sub = args->as.cons.cdr->as.cons.car;
    if (s == NULL || s->type != MINO_STRING
        || sub == NULL || sub->type != MINO_STRING) {
        set_error(S, "includes? requires two string arguments");
        return NULL;
    }
    if (sub->as.s.len == 0) return mino_true(S);
    if (sub->as.s.len > s->as.s.len) return mino_false(S);
    for (p = s->as.s.data; p + sub->as.s.len <= s->as.s.data + s->as.s.len; p++) {
        if (memcmp(p, sub->as.s.data, sub->as.s.len) == 0) {
            return mino_true(S);
        }
    }
    return mino_false(S);
}

static mino_val_t *prim_upper_case(mino_state_t *S, mino_val_t *args, mino_env_t *env)
{
    mino_val_t *s;
    char       *buf;
    size_t      i;
    (void)env;
    if (!mino_is_cons(args) || mino_is_cons(args->as.cons.cdr)) {
        set_error(S, "upper-case requires one string argument");
        return NULL;
    }
    s = args->as.cons.car;
    if (s == NULL || s->type != MINO_STRING) {
        set_error(S, "upper-case requires one string argument");
        return NULL;
    }
    buf = (char *)malloc(s->as.s.len);
    if (buf == NULL && s->as.s.len > 0) {
        set_error(S, "out of memory"); return NULL;
    }
    for (i = 0; i < s->as.s.len; i++) {
        buf[i] = (char)toupper((unsigned char)s->as.s.data[i]);
    }
    {
        mino_val_t *result = mino_string_n(S, buf, s->as.s.len);
        free(buf);
        return result;
    }
}

static mino_val_t *prim_lower_case(mino_state_t *S, mino_val_t *args, mino_env_t *env)
{
    mino_val_t *s;
    char       *buf;
    size_t      i;
    (void)env;
    if (!mino_is_cons(args) || mino_is_cons(args->as.cons.cdr)) {
        set_error(S, "lower-case requires one string argument");
        return NULL;
    }
    s = args->as.cons.car;
    if (s == NULL || s->type != MINO_STRING) {
        set_error(S, "lower-case requires one string argument");
        return NULL;
    }
    buf = (char *)malloc(s->as.s.len);
    if (buf == NULL && s->as.s.len > 0) {
        set_error(S, "out of memory"); return NULL;
    }
    for (i = 0; i < s->as.s.len; i++) {
        buf[i] = (char)tolower((unsigned char)s->as.s.data[i]);
    }
    {
        mino_val_t *result = mino_string_n(S, buf, s->as.s.len);
        free(buf);
        return result;
    }
}

static mino_val_t *prim_trim(mino_state_t *S, mino_val_t *args, mino_env_t *env)
{
    mino_val_t *s;
    const char *start, *end_ptr;
    (void)env;
    if (!mino_is_cons(args) || mino_is_cons(args->as.cons.cdr)) {
        set_error(S, "trim requires one string argument");
        return NULL;
    }
    s = args->as.cons.car;
    if (s == NULL || s->type != MINO_STRING) {
        set_error(S, "trim requires one string argument");
        return NULL;
    }
    start   = s->as.s.data;
    end_ptr = s->as.s.data + s->as.s.len;
    while (start < end_ptr && isspace((unsigned char)*start)) start++;
    while (end_ptr > start && isspace((unsigned char)*(end_ptr - 1))) end_ptr--;
    return mino_string_n(S, start, (size_t)(end_ptr - start));
}

/* ------------------------------------------------------------------------- */
/* Utility primitives                                                        */
/* ------------------------------------------------------------------------- */

static mino_val_t *prim_type(mino_state_t *S, mino_val_t *args, mino_env_t *env)
{
    mino_val_t *v;
    (void)env;
    if (!mino_is_cons(args) || mino_is_cons(args->as.cons.cdr)) {
        set_error(S, "type requires one argument");
        return NULL;
    }
    v = args->as.cons.car;
    if (v == NULL || v->type == MINO_NIL)  return mino_keyword(S, "nil");
    switch (v->type) {
    case MINO_NIL:     return mino_keyword(S, "nil");
    case MINO_BOOL:    return mino_keyword(S, "bool");
    case MINO_INT:     return mino_keyword(S, "int");
    case MINO_FLOAT:   return mino_keyword(S, "float");
    case MINO_STRING:  return mino_keyword(S, "string");
    case MINO_SYMBOL:  return mino_keyword(S, "symbol");
    case MINO_KEYWORD: return mino_keyword(S, "keyword");
    case MINO_CONS:    return mino_keyword(S, "list");
    case MINO_VECTOR:  return mino_keyword(S, "vector");
    case MINO_MAP:     return mino_keyword(S, "map");
    case MINO_SET:     return mino_keyword(S, "set");
    case MINO_PRIM:    return mino_keyword(S, "fn");
    case MINO_FN:      return mino_keyword(S, "fn");
    case MINO_MACRO:   return mino_keyword(S, "macro");
    case MINO_HANDLE:  return mino_keyword(S, "handle");
    case MINO_ATOM:    return mino_keyword(S, "atom");
    case MINO_LAZY:    return mino_keyword(S, "lazy-seq");
    case MINO_RECUR:     return mino_keyword(S, "recur");
    case MINO_TAIL_CALL: return mino_keyword(S, "tail-call");
    case MINO_REDUCED:   return mino_keyword(S, "reduced");
    }
    return mino_keyword(S, "unknown");
}

/*
 * (str & args) — concatenate printed representations. Strings contribute
 * their raw content (no quotes); everything else uses the printer form.
 */
static mino_val_t *prim_str(mino_state_t *S, mino_val_t *args, mino_env_t *env)
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
                if (buf == NULL) { set_error(S, "out of memory"); return NULL; }
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
                mino_val_t *printed = print_to_string(S, a);
                if (printed != NULL && printed->type == MINO_STRING) {
                    size_t plen = printed->as.s.len;
                    size_t need2 = len + plen + 1;
                    if (need2 > cap) {
                        cap = cap == 0 ? 128 : cap;
                        while (cap < need2) cap *= 2;
                        buf = (char *)realloc(buf, cap);
                        if (buf == NULL) { set_error(S, "out of memory"); return NULL; }
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
                    if (buf == NULL) { set_error(S, "out of memory"); return NULL; }
                }
                memcpy(buf + len, tmp, (size_t)n);
                len += (size_t)n;
            }
        }
        args = args->as.cons.cdr;
    }
    {
        mino_val_t *result = mino_string_n(S, buf != NULL ? buf : "", len);
        free(buf);
        return result;
    }
}

static mino_val_t *prim_println(mino_state_t *S, mino_val_t *args, mino_env_t *env)
{
    mino_val_t *result = prim_str(S, args, env);
    if (result == NULL) return NULL;
    fwrite(result->as.s.data, 1, result->as.s.len, stdout);
    fputc('\n', stdout);
    fflush(stdout);
    return mino_nil(S);
}

static mino_val_t *prim_prn(mino_state_t *S, mino_val_t *args, mino_env_t *env)
{
    int first = 1;
    (void)env;
    while (mino_is_cons(args)) {
        if (!first) fputc(' ', stdout);
        mino_print(S, args->as.cons.car);
        first = 0;
        args = args->as.cons.cdr;
    }
    fputc('\n', stdout);
    fflush(stdout);
    return mino_nil(S);
}

static mino_val_t *prim_macroexpand_1(mino_state_t *S, mino_val_t *args, mino_env_t *env)
{
    int expanded;
    if (!mino_is_cons(args) || mino_is_cons(args->as.cons.cdr)) {
        set_error(S, "macroexpand-1 requires one argument");
        return NULL;
    }
    return macroexpand1(S, args->as.cons.car, env, &expanded);
}

static mino_val_t *prim_macroexpand(mino_state_t *S, mino_val_t *args, mino_env_t *env)
{
    if (!mino_is_cons(args) || mino_is_cons(args->as.cons.cdr)) {
        set_error(S, "macroexpand requires one argument");
        return NULL;
    }
    return macroexpand_all(S, args->as.cons.car, env);
}

static mino_val_t *prim_gensym(mino_state_t *S, mino_val_t *args, mino_env_t *env)
{
    const char *prefix_src = "G__";
    size_t      prefix_len = 3;
    char        buf[256];
    size_t      nargs;
    (void)env;
    arg_count(S, args, &nargs);
    if (nargs > 1) {
        set_error(S, "gensym takes 0 or 1 arguments");
        return NULL;
    }
    if (nargs == 1) {
        mino_val_t *p = args->as.cons.car;
        if (p == NULL || p->type != MINO_STRING) {
            set_error(S, "gensym prefix must be a string");
            return NULL;
        }
        prefix_src = p->as.s.data;
        prefix_len = p->as.s.len;
        if (prefix_len >= sizeof(buf) - 32) {
            set_error(S, "gensym prefix too long");
            return NULL;
        }
    }
    {
        int used;
        memcpy(buf, prefix_src, prefix_len);
        used = snprintf(buf + prefix_len, sizeof(buf) - prefix_len,
                        "%ld", ++gensym_counter);
        if (used < 0) {
            set_error(S, "gensym formatting failed");
            return NULL;
        }
        return mino_symbol_n(S, buf, prefix_len + (size_t)used);
    }
}

/* (throw value) — raise a script exception. Caught by try/catch; if no
 * enclosing try, becomes a fatal runtime error. */
static mino_val_t *prim_throw(mino_state_t *S, mino_val_t *args, mino_env_t *env)
{
    mino_val_t *ex;
    (void)env;
    if (!mino_is_cons(args)) {
        set_error(S, "throw requires one argument");
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
        set_error(S, msg);
        return NULL;
    }
    try_stack[try_depth - 1].exception = ex;
    longjmp(try_stack[try_depth - 1].buf, 1);
    return NULL; /* unreachable */
}

/* (slurp path) — read a file's entire contents as a string. I/O capability;
 * only installed by mino_install_io, not mino_install_core. */
static mino_val_t *prim_slurp(mino_state_t *S, mino_val_t *args, mino_env_t *env)
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
        set_error(S, "slurp requires one argument");
        return NULL;
    }
    path_val = args->as.cons.car;
    if (path_val == NULL || path_val->type != MINO_STRING) {
        set_error(S, "slurp: argument must be a string");
        return NULL;
    }
    path = path_val->as.s.data;
    f = fopen(path, "rb");
    if (f == NULL) {
        char msg[300];
        snprintf(msg, sizeof(msg), "slurp: cannot open file: %s", path);
        set_error(S, msg);
        return NULL;
    }
    fseek(f, 0, SEEK_END);
    sz = ftell(f);
    if (sz < 0) {
        fclose(f);
        set_error(S, "slurp: cannot determine file size");
        return NULL;
    }
    fseek(f, 0, SEEK_SET);
    buf = (char *)malloc((size_t)sz + 1);
    if (buf == NULL) {
        fclose(f);
        set_error(S, "slurp: out of memory");
        return NULL;
    }
    rd = fread(buf, 1, (size_t)sz, f);
    fclose(f);
    buf[rd] = '\0';
    result = mino_string_n(S, buf, rd);
    free(buf);
    return result;
}

static mino_val_t *prim_spit(mino_state_t *S, mino_val_t *args, mino_env_t *env)
{
    mino_val_t *path_val;
    mino_val_t *content;
    const char *path;
    FILE       *f;
    (void)env;
    if (!mino_is_cons(args) || !mino_is_cons(args->as.cons.cdr)
        || mino_is_cons(args->as.cons.cdr->as.cons.cdr)) {
        set_error(S, "spit requires two arguments");
        return NULL;
    }
    path_val = args->as.cons.car;
    content  = args->as.cons.cdr->as.cons.car;
    if (path_val == NULL || path_val->type != MINO_STRING) {
        set_error(S, "spit: first argument must be a string path");
        return NULL;
    }
    path = path_val->as.s.data;
    f = fopen(path, "wb");
    if (f == NULL) {
        char msg[300];
        snprintf(msg, sizeof(msg), "spit: cannot open file: %s", path);
        set_error(S, msg);
        return NULL;
    }
    if (content != NULL && content->type == MINO_STRING) {
        fwrite(content->as.s.data, 1, content->as.s.len, f);
    } else {
        mino_print_to(S, f, content);
    }
    fclose(f);
    return mino_nil(S);
}

/* (exit code) — terminate the process with the given exit code.
 * Defaults to 0 if no argument is given. */
static mino_val_t *prim_exit(mino_state_t *S, mino_val_t *args, mino_env_t *env)
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
    return mino_nil(S); /* unreachable */
}

/* --- Regex primitives (using bundled tiny-regex-c) --- */

/* (re-find pattern text) — find first match of pattern in text.
 * Returns the matched substring, or nil if no match. */
static mino_val_t *prim_re_find(mino_state_t *S, mino_val_t *args, mino_env_t *env)
{
    mino_val_t *pat_val, *text_val;
    int match_len = 0;
    int match_idx;
    (void)env;
    if (!mino_is_cons(args) || !mino_is_cons(args->as.cons.cdr) ||
        mino_is_cons(args->as.cons.cdr->as.cons.cdr)) {
        set_error(S, "re-find requires two arguments");
        return NULL;
    }
    pat_val  = args->as.cons.car;
    text_val = args->as.cons.cdr->as.cons.car;
    if (pat_val == NULL || pat_val->type != MINO_STRING) {
        set_error(S, "re-find: first argument must be a pattern string");
        return NULL;
    }
    if (text_val == NULL || text_val->type != MINO_STRING) {
        set_error(S, "re-find: second argument must be a string");
        return NULL;
    }
    match_idx = re_match(pat_val->as.s.data, text_val->as.s.data, &match_len);
    if (match_idx == -1) {
        return mino_nil(S);
    }
    return mino_string_n(S, text_val->as.s.data + match_idx, (size_t)match_len);
}

/* (re-matches pattern text) — true if the entire text matches pattern. */
static mino_val_t *prim_re_matches(mino_state_t *S, mino_val_t *args, mino_env_t *env)
{
    mino_val_t *pat_val, *text_val;
    int match_len = 0;
    int match_idx;
    (void)env;
    if (!mino_is_cons(args) || !mino_is_cons(args->as.cons.cdr) ||
        mino_is_cons(args->as.cons.cdr->as.cons.cdr)) {
        set_error(S, "re-matches requires two arguments");
        return NULL;
    }
    pat_val  = args->as.cons.car;
    text_val = args->as.cons.cdr->as.cons.car;
    if (pat_val == NULL || pat_val->type != MINO_STRING) {
        set_error(S, "re-matches: first argument must be a pattern string");
        return NULL;
    }
    if (text_val == NULL || text_val->type != MINO_STRING) {
        set_error(S, "re-matches: second argument must be a string");
        return NULL;
    }
    match_idx = re_match(pat_val->as.s.data, text_val->as.s.data, &match_len);
    if (match_idx == 0 && (size_t)match_len == text_val->as.s.len) {
        return text_val;
    }
    return mino_nil(S);
}

/* (time-ms) — return process time in milliseconds as a float.
 * Uses ANSI C clock() for portability across all C99 platforms. */
static mino_val_t *prim_time_ms(mino_state_t *S, mino_val_t *args, mino_env_t *env)
{
    (void)args;
    (void)env;
    if (mino_is_cons(args)) {
        set_error(S, "time-ms takes no arguments");
        return NULL;
    }
    return mino_float(S, (double)clock() / (double)CLOCKS_PER_SEC * 1000.0);
}

/* (require name) — load a module by name using the host-supplied resolver.
 * Returns the cached value on subsequent calls with the same name. */
static mino_val_t *prim_require(mino_state_t *S, mino_val_t *args, mino_env_t *env)
{
    mino_val_t *name_val;
    const char *name;
    const char *path;
    size_t      i;
    mino_val_t *result;
    if (!mino_is_cons(args) || mino_is_cons(args->as.cons.cdr)) {
        set_error(S, "require requires one argument");
        return NULL;
    }
    name_val = args->as.cons.car;
    if (name_val == NULL || name_val->type != MINO_STRING) {
        set_error(S, "require: argument must be a string");
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
        set_error(S, "require: no module resolver configured");
        return NULL;
    }
    path = module_resolver(name, module_resolver_ctx);
    if (path == NULL) {
        char msg[300];
        snprintf(msg, sizeof(msg), "require: cannot resolve module: %s", name);
        set_error(S, msg);
        return NULL;
    }
    /* Load. */
    result = mino_load_file(S, path, env);
    if (result == NULL) {
        return NULL;
    }
    /* Cache. */
    if (module_cache_len == module_cache_cap) {
        size_t         new_cap = module_cache_cap == 0 ? 8 : module_cache_cap * 2;
        module_entry_t *nb     = (module_entry_t *)realloc(
            module_cache, new_cap * sizeof(*nb));
        if (nb == NULL) {
            set_error(S, "require: out of memory");
            return NULL;
        }
        module_cache     = nb;
        module_cache_cap = new_cap;
    }
    {
        size_t nlen = strlen(name);
        char *dup   = (char *)malloc(nlen + 1);
        if (dup == NULL) {
            set_error(S, "require: out of memory");
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
static mino_val_t *prim_doc(mino_state_t *S, mino_val_t *args, mino_env_t *env)
{
    mino_val_t   *name_val;
    char          buf[256];
    size_t        n;
    meta_entry_t *e;
    (void)env;
    if (!mino_is_cons(args) || mino_is_cons(args->as.cons.cdr)) {
        set_error(S, "doc requires one argument");
        return NULL;
    }
    name_val = args->as.cons.car;
    if (name_val == NULL || name_val->type != MINO_SYMBOL) {
        set_error(S, "doc: argument must be a symbol");
        return NULL;
    }
    n = name_val->as.s.len;
    if (n >= sizeof(buf)) {
        set_error(S, "doc: name too long");
        return NULL;
    }
    memcpy(buf, name_val->as.s.data, n);
    buf[n] = '\0';
    e = meta_find(S, buf);
    if (e != NULL && e->docstring != NULL) {
        return mino_string(S, e->docstring);
    }
    return mino_nil(S);
}

/* (source name) — return the source form of a def/defmacro binding. */
static mino_val_t *prim_source(mino_state_t *S, mino_val_t *args, mino_env_t *env)
{
    mino_val_t   *name_val;
    char          buf[256];
    size_t        n;
    meta_entry_t *e;
    (void)env;
    if (!mino_is_cons(args) || mino_is_cons(args->as.cons.cdr)) {
        set_error(S, "source requires one argument");
        return NULL;
    }
    name_val = args->as.cons.car;
    if (name_val == NULL || name_val->type != MINO_SYMBOL) {
        set_error(S, "source: argument must be a symbol");
        return NULL;
    }
    n = name_val->as.s.len;
    if (n >= sizeof(buf)) {
        set_error(S, "source: name too long");
        return NULL;
    }
    memcpy(buf, name_val->as.s.data, n);
    buf[n] = '\0';
    e = meta_find(S, buf);
    if (e != NULL && e->source != NULL) {
        return e->source;
    }
    return mino_nil(S);
}

/* (apropos substring) — return a list of bound names containing substring. */
static mino_val_t *prim_apropos(mino_state_t *S, mino_val_t *args, mino_env_t *env)
{
    mino_val_t *pat_val;
    const char *pat;
    mino_val_t *head = mino_nil(S);
    mino_val_t *tail = NULL;
    mino_env_t *e;
    if (!mino_is_cons(args) || mino_is_cons(args->as.cons.cdr)) {
        set_error(S, "apropos requires one argument");
        return NULL;
    }
    pat_val = args->as.cons.car;
    if (pat_val == NULL || pat_val->type != MINO_STRING) {
        set_error(S, "apropos: argument must be a string");
        return NULL;
    }
    pat = pat_val->as.s.data;
    /* Walk every env frame from the given env up to root. */
    for (e = env; e != NULL; e = e->parent) {
        size_t i;
        for (i = 0; i < e->len; i++) {
            if (strstr(e->bindings[i].name, pat) != NULL) {
                mino_val_t *sym  = mino_symbol(S, e->bindings[i].name);
                mino_val_t *cell = mino_cons(S, sym, mino_nil(S));
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

static void install_core_mino(mino_state_t *S, mino_env_t *env)
{
    size_t i;

    /* On first call for this state, parse and eval each form, caching
     * the parsed ASTs for subsequent mino_install_core calls.  We set
     * S->core_forms immediately and update core_forms_len as we go so
     * that the GC root walker can pin the forms during collection. */
    if (S->core_forms == NULL) {
        const char  *src        = core_mino_src;
        const char  *saved_file = reader_file;
        int          saved_line = reader_line;
        size_t       cap        = 256;

        S->core_forms     = malloc(cap * sizeof(mino_val_t *));
        S->core_forms_len = 0;
        if (!S->core_forms) {
            fprintf(stderr, "core.mino: out of memory\n"); abort();
        }

        reader_file = intern_filename("<core>");
        reader_line = 1;
        while (*src != '\0') {
            const char *end  = NULL;
            mino_val_t *form = mino_read(S, src, &end);
            if (form == NULL) {
                if (mino_last_error(S) != NULL) {
                    fprintf(stderr, "core.mino parse error: %s\n",
                            mino_last_error(S));
                    abort();
                }
                break;
            }
            if (S->core_forms_len >= cap) {
                cap *= 2;
                S->core_forms = realloc(S->core_forms,
                                        cap * sizeof(mino_val_t *));
                if (!S->core_forms) {
                    fprintf(stderr, "core.mino: out of memory\n");
                    abort();
                }
            }
            S->core_forms[S->core_forms_len++] = form;
            if (mino_eval(S, form, env) == NULL) {
                fprintf(stderr, "core.mino eval error: %s\n",
                        mino_last_error(S));
                abort();
            }
            src = end;
        }
        reader_file = saved_file;
        reader_line = saved_line;
        return;
    }

    /* Subsequent calls: evaluate cached forms into the target env. */
    for (i = 0; i < S->core_forms_len; i++) {
        if (mino_eval(S, S->core_forms[i], env) == NULL) {
            fprintf(stderr, "core.mino eval error: %s\n", mino_last_error(S));
            abort();
        }
    }
}

/* --- Atom primitives --------------------------------------------------- */

/*
 * (seq coll) — coerce a collection to a sequence (cons chain).
 * Returns nil for empty collections. Forces lazy sequences.
 */
static mino_val_t *prim_seq(mino_state_t *S, mino_val_t *args, mino_env_t *env)
{
    mino_val_t *coll;
    (void)env;
    if (!mino_is_cons(args) || mino_is_cons(args->as.cons.cdr)) {
        set_error(S, "seq requires one argument");
        return NULL;
    }
    coll = args->as.cons.car;
    if (coll == NULL || coll->type == MINO_NIL) return mino_nil(S);
    if (coll->type == MINO_LAZY) {
        mino_val_t *forced = lazy_force(S, coll);
        if (forced == NULL) return NULL;
        if (forced->type == MINO_NIL) return mino_nil(S);
        return forced;
    }
    if (coll->type == MINO_CONS) return coll;
    if (coll->type == MINO_VECTOR) {
        mino_val_t *head = mino_nil(S);
        mino_val_t *tail = NULL;
        size_t i;
        if (coll->as.vec.len == 0) return mino_nil(S);
        for (i = 0; i < coll->as.vec.len; i++) {
            mino_val_t *cell = mino_cons(S, vec_nth(coll, i), mino_nil(S));
            if (tail == NULL) head = cell;
            else tail->as.cons.cdr = cell;
            tail = cell;
        }
        return head;
    }
    if (coll->type == MINO_MAP) {
        mino_val_t *head = mino_nil(S);
        mino_val_t *tail = NULL;
        size_t i;
        if (coll->as.map.len == 0) return mino_nil(S);
        for (i = 0; i < coll->as.map.len; i++) {
            mino_val_t *key = vec_nth(coll->as.map.key_order, i);
            mino_val_t *val = map_get_val(coll, key);
            mino_val_t *kv[2];
            mino_val_t *cell;
            kv[0] = key; kv[1] = val;
            cell = mino_cons(S, mino_vector(S, kv, 2), mino_nil(S));
            if (tail == NULL) head = cell;
            else tail->as.cons.cdr = cell;
            tail = cell;
        }
        return head;
    }
    if (coll->type == MINO_SET) {
        mino_val_t *head = mino_nil(S);
        mino_val_t *tail = NULL;
        size_t i;
        if (coll->as.set.len == 0) return mino_nil(S);
        for (i = 0; i < coll->as.set.len; i++) {
            mino_val_t *elem = vec_nth(coll->as.set.key_order, i);
            mino_val_t *cell = mino_cons(S, elem, mino_nil(S));
            if (tail == NULL) head = cell;
            else tail->as.cons.cdr = cell;
            tail = cell;
        }
        return head;
    }
    if (coll->type == MINO_STRING) {
        mino_val_t *head = mino_nil(S);
        mino_val_t *tail = NULL;
        size_t i;
        if (coll->as.s.len == 0) return mino_nil(S);
        for (i = 0; i < coll->as.s.len; i++) {
            mino_val_t *ch = mino_string_n(S, coll->as.s.data + i, 1);
            mino_val_t *cell = mino_cons(S, ch, mino_nil(S));
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
        set_error(S, msg);
    }
    return NULL;
}

static mino_val_t *prim_realized_p(mino_state_t *S, mino_val_t *args, mino_env_t *env)
{
    mino_val_t *v;
    (void)env;
    if (!mino_is_cons(args) || mino_is_cons(args->as.cons.cdr)) {
        set_error(S, "realized? requires one argument");
        return NULL;
    }
    v = args->as.cons.car;
    if (v != NULL && v->type == MINO_LAZY) {
        return v->as.lazy.realized ? mino_true(S) : mino_false(S);
    }
    /* Non-lazy values are always realized. */
    return mino_true(S);
}

static mino_val_t *prim_atom(mino_state_t *S, mino_val_t *args, mino_env_t *env)
{
    (void)env;
    if (!mino_is_cons(args) || mino_is_cons(args->as.cons.cdr)) {
        set_error(S, "atom requires one argument");
        return NULL;
    }
    return mino_atom(S, args->as.cons.car);
}

static mino_val_t *prim_deref(mino_state_t *S, mino_val_t *args, mino_env_t *env)
{
    mino_val_t *a;
    (void)env;
    if (!mino_is_cons(args) || mino_is_cons(args->as.cons.cdr)) {
        set_error(S, "deref requires one argument");
        return NULL;
    }
    a = args->as.cons.car;
    if (a != NULL && a->type == MINO_ATOM) {
        return a->as.atom.val;
    }
    if (a != NULL && a->type == MINO_REDUCED) {
        return a->as.reduced.val;
    }
    set_error(S, "deref: expected an atom or reduced");
    return NULL;
}

static mino_val_t *prim_reset_bang(mino_state_t *S, mino_val_t *args, mino_env_t *env)
{
    mino_val_t *a, *val;
    (void)env;
    if (!mino_is_cons(args) || !mino_is_cons(args->as.cons.cdr)
        || mino_is_cons(args->as.cons.cdr->as.cons.cdr)) {
        set_error(S, "reset! requires two arguments");
        return NULL;
    }
    a   = args->as.cons.car;
    val = args->as.cons.cdr->as.cons.car;
    if (a == NULL || a->type != MINO_ATOM) {
        set_error(S, "reset!: first argument must be an atom");
        return NULL;
    }
    a->as.atom.val = val;
    return val;
}

/* (swap! atom f & args) — applies (f current-val args...) and sets result. */
static mino_val_t *prim_swap_bang(mino_state_t *S, mino_val_t *args, mino_env_t *env)
{
    mino_val_t *a, *fn, *cur, *call_args, *extra, *tail, *result;
    if (!mino_is_cons(args) || !mino_is_cons(args->as.cons.cdr)) {
        set_error(S, "swap! requires at least 2 arguments: atom and function");
        return NULL;
    }
    a  = args->as.cons.car;
    fn = args->as.cons.cdr->as.cons.car;
    if (a == NULL || a->type != MINO_ATOM) {
        set_error(S, "swap!: first argument must be an atom");
        return NULL;
    }
    cur = a->as.atom.val;
    /* Build arg list: (cur extra1 extra2 ...) */
    call_args = mino_nil(S);
    /* Append extra args in reverse then prepend cur. */
    extra = args->as.cons.cdr->as.cons.cdr; /* rest after fn */
    if (extra != NULL && extra->type == MINO_CONS) {
        /* Collect extras into a list. */
        tail = mino_nil(S);
        while (extra != NULL && extra->type == MINO_CONS) {
            tail = mino_cons(S, extra->as.cons.car, tail);
            extra = extra->as.cons.cdr;
        }
        /* Reverse to get correct order. */
        call_args = mino_nil(S);
        while (tail != NULL && tail->type == MINO_CONS) {
            call_args = mino_cons(S, tail->as.cons.car, call_args);
            tail = tail->as.cons.cdr;
        }
    }
    call_args = mino_cons(S, cur, call_args);
    result = mino_call(S, fn, call_args, env);
    if (result == NULL) return NULL;
    a->as.atom.val = result;
    return result;
}

static mino_val_t *prim_atom_p(mino_state_t *S, mino_val_t *args, mino_env_t *env)
{
    (void)env;
    if (!mino_is_cons(args) || mino_is_cons(args->as.cons.cdr)) {
        set_error(S, "atom? requires one argument");
        return NULL;
    }
    return mino_is_atom(args->as.cons.car) ? mino_true(S) : mino_false(S);
}

void mino_install_core(mino_state_t *S, mino_env_t *env)
{
    volatile char probe = 0;
    gc_note_host_frame(S, (void *)&probe);
    (void)probe;
    mino_env_set(S, env, "+",        mino_prim(S, "+",        prim_add));
    mino_env_set(S, env, "-",        mino_prim(S, "-",        prim_sub));
    mino_env_set(S, env, "*",        mino_prim(S, "*",        prim_mul));
    mino_env_set(S, env, "/",        mino_prim(S, "/",        prim_div));
    mino_env_set(S, env, "=",        mino_prim(S, "=",        prim_eq));
    mino_env_set(S, env, "identical?", mino_prim(S, "identical?", prim_identical));
    mino_env_set(S, env, "meta",      mino_prim(S, "meta",      prim_meta));
    mino_env_set(S, env, "with-meta", mino_prim(S, "with-meta", prim_with_meta));
    mino_env_set(S, env, "vary-meta", mino_prim(S, "vary-meta", prim_vary_meta));
    mino_env_set(S, env, "alter-meta!", mino_prim(S, "alter-meta!", prim_alter_meta));
    mino_env_set(S, env, "<",        mino_prim(S, "<",        prim_lt));
    mino_env_set(S, env, "mod",      mino_prim(S, "mod",      prim_mod));
    mino_env_set(S, env, "rem",      mino_prim(S, "rem",      prim_rem));
    mino_env_set(S, env, "quot",     mino_prim(S, "quot",     prim_quot));
    /* math */
    mino_env_set(S, env, "math-floor", mino_prim(S, "math-floor", prim_math_floor));
    mino_env_set(S, env, "math-ceil",  mino_prim(S, "math-ceil",  prim_math_ceil));
    mino_env_set(S, env, "math-round", mino_prim(S, "math-round", prim_math_round));
    mino_env_set(S, env, "math-sqrt",  mino_prim(S, "math-sqrt",  prim_math_sqrt));
    mino_env_set(S, env, "math-pow",   mino_prim(S, "math-pow",   prim_math_pow));
    mino_env_set(S, env, "math-log",   mino_prim(S, "math-log",   prim_math_log));
    mino_env_set(S, env, "math-exp",   mino_prim(S, "math-exp",   prim_math_exp));
    mino_env_set(S, env, "math-sin",   mino_prim(S, "math-sin",   prim_math_sin));
    mino_env_set(S, env, "math-cos",   mino_prim(S, "math-cos",   prim_math_cos));
    mino_env_set(S, env, "math-tan",   mino_prim(S, "math-tan",   prim_math_tan));
    mino_env_set(S, env, "math-atan2", mino_prim(S, "math-atan2", prim_math_atan2));
    mino_env_set(S, env, "math-pi",    mino_float(S, 3.14159265358979323846));
    mino_env_set(S, env, "bit-and", mino_prim(S, "bit-and", prim_bit_and));
    mino_env_set(S, env, "bit-or",  mino_prim(S, "bit-or",  prim_bit_or));
    mino_env_set(S, env, "bit-xor", mino_prim(S, "bit-xor", prim_bit_xor));
    mino_env_set(S, env, "bit-not", mino_prim(S, "bit-not", prim_bit_not));
    mino_env_set(S, env, "bit-shift-left",
                 mino_prim(S, "bit-shift-left", prim_bit_shift_left));
    mino_env_set(S, env, "bit-shift-right",
                 mino_prim(S, "bit-shift-right", prim_bit_shift_right));
    mino_env_set(S, env, "car",      mino_prim(S, "car",      prim_car));
    mino_env_set(S, env, "cdr",      mino_prim(S, "cdr",      prim_cdr));
    mino_env_set(S, env, "cons",     mino_prim(S, "cons",     prim_cons));
    mino_env_set(S, env, "count",    mino_prim(S, "count",    prim_count));
    mino_env_set(S, env, "nth",      mino_prim(S, "nth",      prim_nth));
    mino_env_set(S, env, "first",    mino_prim(S, "first",    prim_first));
    mino_env_set(S, env, "rest",     mino_prim(S, "rest",     prim_rest));
    mino_env_set(S, env, "vector",   mino_prim(S, "vector",   prim_vector));
    mino_env_set(S, env, "hash-map", mino_prim(S, "hash-map", prim_hash_map));
    mino_env_set(S, env, "assoc",    mino_prim(S, "assoc",    prim_assoc));
    mino_env_set(S, env, "get",      mino_prim(S, "get",      prim_get));
    mino_env_set(S, env, "conj",     mino_prim(S, "conj",     prim_conj));
    mino_env_set(S, env, "keys",     mino_prim(S, "keys",     prim_keys));
    mino_env_set(S, env, "vals",     mino_prim(S, "vals",     prim_vals));
    mino_env_set(S, env, "macroexpand-1",
                 mino_prim(S, "macroexpand-1", prim_macroexpand_1));
    mino_env_set(S, env, "macroexpand",
                 mino_prim(S, "macroexpand", prim_macroexpand));
    mino_env_set(S, env, "gensym",   mino_prim(S, "gensym",   prim_gensym));
    mino_env_set(S, env, "type",     mino_prim(S, "type",     prim_type));
    mino_env_set(S, env, "name",     mino_prim(S, "name",     prim_name));
    mino_env_set(S, env, "rand",     mino_prim(S, "rand",     prim_rand));
    /* regex */
    mino_env_set(S, env, "re-find",    mino_prim(S, "re-find",    prim_re_find));
    mino_env_set(S, env, "re-matches", mino_prim(S, "re-matches", prim_re_matches));
    mino_env_set(S, env, "eval",     mino_prim(S, "eval",     prim_eval));
    mino_env_set(S, env, "symbol",   mino_prim(S, "symbol",   prim_symbol));
    mino_env_set(S, env, "keyword",  mino_prim(S, "keyword",  prim_keyword));
    mino_env_set(S, env, "hash",     mino_prim(S, "hash",     prim_hash));
    mino_env_set(S, env, "compare",  mino_prim(S, "compare",  prim_compare));
    mino_env_set(S, env, "int",      mino_prim(S, "int",      prim_int));
    mino_env_set(S, env, "float",    mino_prim(S, "float",    prim_float));
    mino_env_set(S, env, "str",      mino_prim(S, "str",      prim_str));
    mino_env_set(S, env, "pr-str",   mino_prim(S, "pr-str",   prim_pr_str));
    mino_env_set(S, env, "read-string",
                 mino_prim(S, "read-string", prim_read_string));
    mino_env_set(S, env, "format",   mino_prim(S, "format",   prim_format));
    mino_env_set(S, env, "throw",    mino_prim(S, "throw",    prim_throw));
    mino_env_set(S, env, "require",  mino_prim(S, "require",  prim_require));
    mino_env_set(S, env, "doc",      mino_prim(S, "doc",      prim_doc));
    mino_env_set(S, env, "source",   mino_prim(S, "source",   prim_source));
    mino_env_set(S, env, "apropos",  mino_prim(S, "apropos",  prim_apropos));
    /* set operations */
    mino_env_set(S, env, "hash-set", mino_prim(S, "hash-set", prim_hash_set));
    mino_env_set(S, env, "set",      mino_prim(S, "set",      prim_set));
    mino_env_set(S, env, "contains?",mino_prim(S, "contains?",prim_contains_p));
    mino_env_set(S, env, "disj",     mino_prim(S, "disj",     prim_disj));
    mino_env_set(S, env, "dissoc",   mino_prim(S, "dissoc",   prim_dissoc));
    /* sequence operations (map, filter, take, drop, range, repeat,
       concat are now lazy in core.mino) */
    mino_env_set(S, env, "reduce",   mino_prim(S, "reduce",   prim_reduce));
    mino_env_set(S, env, "reduced",  mino_prim(S, "reduced",  prim_reduced));
    mino_env_set(S, env, "reduced?", mino_prim(S, "reduced?", prim_reduced_p));
    mino_env_set(S, env, "into",     mino_prim(S, "into",     prim_into));
    /* eager collection builders -- bypass lazy thunk overhead */
    mino_env_set(S, env, "rangev",   mino_prim(S, "rangev",   prim_rangev));
    mino_env_set(S, env, "mapv",     mino_prim(S, "mapv",     prim_mapv));
    mino_env_set(S, env, "filterv",  mino_prim(S, "filterv",  prim_filterv));
    mino_env_set(S, env, "apply",    mino_prim(S, "apply",    prim_apply));
    mino_env_set(S, env, "reverse",  mino_prim(S, "reverse",  prim_reverse));
    mino_env_set(S, env, "sort",     mino_prim(S, "sort",     prim_sort));
    /* string operations */
    mino_env_set(S, env, "subs",     mino_prim(S, "subs",     prim_subs));
    mino_env_set(S, env, "split",    mino_prim(S, "split",    prim_split));
    mino_env_set(S, env, "join",     mino_prim(S, "join",     prim_join));
    mino_env_set(S, env, "starts-with?",
                 mino_prim(S, "starts-with?", prim_starts_with_p));
    mino_env_set(S, env, "ends-with?",
                 mino_prim(S, "ends-with?", prim_ends_with_p));
    mino_env_set(S, env, "includes?",
                 mino_prim(S, "includes?", prim_includes_p));
    mino_env_set(S, env, "upper-case",
                 mino_prim(S, "upper-case", prim_upper_case));
    mino_env_set(S, env, "lower-case",
                 mino_prim(S, "lower-case", prim_lower_case));
    mino_env_set(S, env, "trim",     mino_prim(S, "trim",     prim_trim));
    mino_env_set(S, env, "char-at",  mino_prim(S, "char-at",  prim_char_at));
    /* (some and every? are now in core.mino) */
    /* sequences */
    mino_env_set(S, env, "seq",       mino_prim(S, "seq",       prim_seq));
    mino_env_set(S, env, "realized?", mino_prim(S, "realized?", prim_realized_p));
    /* atoms */
    mino_env_set(S, env, "atom",     mino_prim(S, "atom",     prim_atom));
    mino_env_set(S, env, "deref",    mino_prim(S, "deref",    prim_deref));
    mino_env_set(S, env, "reset!",   mino_prim(S, "reset!",   prim_reset_bang));
    mino_env_set(S, env, "swap!",    mino_prim(S, "swap!",    prim_swap_bang));
    mino_env_set(S, env, "atom?",    mino_prim(S, "atom?",    prim_atom_p));
    /* actors */
    mino_env_set(S, env, "spawn*",   mino_prim(S, "spawn*",   prim_spawn));
    mino_env_set(S, env, "send!",    mino_prim(S, "send!",    prim_send_bang));
    mino_env_set(S, env, "receive",  mino_prim(S, "receive",  prim_receive));
    install_core_mino(S, env);
}

void mino_install_io(mino_state_t *S, mino_env_t *env)
{
    mino_env_set(S, env, "println",  mino_prim(S, "println",  prim_println));
    mino_env_set(S, env, "prn",      mino_prim(S, "prn",      prim_prn));
    mino_env_set(S, env, "slurp",    mino_prim(S, "slurp",    prim_slurp));
    mino_env_set(S, env, "spit",     mino_prim(S, "spit",     prim_spit));
    mino_env_set(S, env, "exit",     mino_prim(S, "exit",     prim_exit));
    mino_env_set(S, env, "time-ms",  mino_prim(S, "time-ms",  prim_time_ms));
}
