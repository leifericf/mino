/*
 * string.c -- string primitives: str, pr-str, format, read-string,
 *                  char-at, subs, split, join, starts-with?, ends-with?,
 *                  includes?, upper-case, lower-case, trim.
 */

#include "prim/internal.h"
#include "regex/re.h"

#include <math.h>

/* Grow `buf` so that `len + extra + 1` bytes fit. Returns the (possibly
 * realloc'd) buffer, or NULL if allocation failed (in which case an
 * MIN001 diagnostic has already been recorded on S). The caller updates
 * its own `cap` via cap_ptr; `buf` is replaced by the return value. */
static inline char *fmt_ensure(mino_state *S, char *buf,
                               size_t len, size_t *cap_ptr, size_t extra)
{
    size_t need;
    /* len + extra + 1 must not wrap. */
    if (extra > SIZE_MAX - len - 1) {
        free(buf);
        set_eval_diag(S, mino_current_ctx(S)->eval_current_form,
                      "internal", "MIN001",
                      "format: result size overflow");
        return NULL;
    }
    need = len + extra + 1;
    if (need > *cap_ptr) {
        size_t newcap = *cap_ptr == 0 ? 128 : *cap_ptr;
        char  *newbuf;
        while (newcap < need) {
            if (newcap > SIZE_MAX / 2) {
                free(buf);
                set_eval_diag(S, mino_current_ctx(S)->eval_current_form,
                              "internal", "MIN001",
                              "format: result size overflow");
                return NULL;
            }
            newcap *= 2;
        }
        newbuf = (char *)realloc(buf, newcap);
        if (newbuf == NULL) {
            /* realloc failure leaves `buf` valid; free it before the
             * caller overwrites its variable with our NULL return.
             * The size-overflow branch above already does the same. */
            free(buf);
            set_eval_diag(S, mino_current_ctx(S)->eval_current_form, "internal", "MIN001",
                          "out of memory");
            return NULL;
        }
        *cap_ptr = newcap;
        return newbuf;
    }
    return buf;
}

/*
 * (format fmt & args) -- printf-style string formatting matching the
 * canonical Formatter surface for the directives mino supports.
 *
 * Directives: %s %S %b %B %c %C %d %x %X %o %f %e %E %g %G %a %A %n %%
 * Flags: '-' '+' ' ' '0' '#' ',' '('  plus width and .precision.
 * Positional arguments: %N$<spec> (1-based; does not advance the
 * sequential argument cursor, matching the Formatter).
 *
 * Deviations, all accommodations of mino's numeric model and all
 * looser than canon (they format where canon throws):
 *   - %c accepts an integer codepoint (canon rejects a long).
 *   - %d/%x/%o accept a bigint that fits 64 bits (canon rejects big
 *     integers outright). A float is an illegal conversion and
 *     throws, matching canon.
 * %g follows the Formatter's semantics (fixed significant digits,
 * trailing zeros kept), not C's shortest-form %g. %a strips the
 * exponent's '+' like Double/toHexString. %n is always "\n": mino
 * output is byte-identical across hosts. %,d grouping is a fixed ','
 * (no locale). An unknown or incomplete directive throws; nothing
 * passes through as literal text. %t/%T (date/time) and %h/%H (JVM
 * hashCode) stay absent by design and throw like any unknown
 * directive.
 */

static size_t utf8_encode(char *p, uint32_t cp);        /* defined further down */
static uint32_t utf8_decode(const char *p, size_t len); /* defined further down */
static uint32_t mino_unicode_to_upper(uint32_t cp);     /* generated tables    */

static char *fmt_append(mino_state *S, char *buf, size_t *len,
                         size_t *cap, const char *src, size_t n)
{
    buf = fmt_ensure(S, buf, *len, cap, n);
    if (buf == NULL) return NULL;
    memcpy(buf + *len, src, n);
    *len += n;
    return buf;
}

/* Append `s` (slen bytes) honoring a minimum width and the '-'
 * (left-justify) flag; pads with spaces. Width measures the rendered
 * argument in codepoints, matching count, not UTF-8 bytes. */
static char *fmt_append_padded(mino_state *S, char *buf, size_t *len,
                                size_t *cap, const char *s, size_t slen,
                                long width, int left)
{
    size_t disp = (size_t)utf8_codepoint_count(s, slen);
    size_t pad = (width > 0 && (size_t)width > disp)
                     ? (size_t)width - disp : 0;
    size_t k;
    if (!left) {
        for (k = 0; k < pad; k++) {
            buf = fmt_append(S, buf, len, cap, " ", 1);
            if (buf == NULL) return NULL;
        }
    }
    buf = fmt_append(S, buf, len, cap, s, slen);
    if (buf == NULL) return NULL;
    if (left) {
        for (k = 0; k < pad; k++) {
            buf = fmt_append(S, buf, len, cap, " ", 1);
            if (buf == NULL) return NULL;
        }
    }
    return buf;
}

/* Numeric pad path: like fmt_append_padded, but with the '0' flag a
 * right-justified numeric string zero-fills after its sign, opening
 * paren, and hex prefix ("-000003.14", "(0000012345)",
 * "0x000000001.8p0"). A non-numeric rendering ("NaN", "Infinity")
 * falls back to space padding, and '-' wins over '0'. */
static char *fmt_append_num_padded(mino_state *S, char *buf, size_t *len,
                                    size_t *cap, const char *s, size_t slen,
                                    long width, int left, int zero)
{
    size_t disp = (size_t)utf8_codepoint_count(s, slen);
    size_t pad = (width > 0 && (size_t)width > disp)
                     ? (size_t)width - disp : 0;
    size_t head = 0, k;
    if (left || !zero || pad == 0)
        return fmt_append_padded(S, buf, len, cap, s, slen, width, left);
    if (head < slen && (s[head] == '-' || s[head] == '+'
                        || s[head] == ' ' || s[head] == '(')) head++;
    if (head + 1 < slen && s[head] == '0'
        && (s[head + 1] == 'x' || s[head + 1] == 'X')) head += 2;
    if (head >= slen || s[head] < '0' || s[head] > '9')
        return fmt_append_padded(S, buf, len, cap, s, slen, width, left);
    buf = fmt_append(S, buf, len, cap, s, head);
    if (buf == NULL) return NULL;
    for (k = 0; k < pad; k++) {
        buf = fmt_append(S, buf, len, cap, "0", 1);
        if (buf == NULL) return NULL;
    }
    return fmt_append(S, buf, len, cap, s + head, slen - head);
}

/* Re-render a plain %lld decimal with ',' groups and/or the '('
 * negative style. `in` is nul-terminated; out must hold at least
 * strlen(in) + its comma count + 2 parens + nul (96 covers 64-bit). */
static void fmt_regroup_decimal(const char *in, char *out, size_t outsz,
                                 int comma, int paren)
{
    size_t n = strlen(in);
    size_t di = 0, start = 0, digits, k;
    int    neg = (n > 0 && in[0] == '-');
    if (neg) start = 1;
    digits = n - start;
    if (neg && di + 1 < outsz) out[di++] = paren ? '(' : '-';
    for (k = 0; k < digits; k++) {
        if (comma && k > 0 && (digits - k) % 3 == 0 && di + 1 < outsz)
            out[di++] = ',';
        if (di + 1 < outsz) out[di++] = in[start + k];
    }
    if (neg && paren && di + 1 < outsz) out[di++] = ')';
    out[di] = '\0';
}

/* Insert ',' groups into the leading integer-digit span of a
 * rendered decimal number ("-1234567.50" becomes "-1,234,567.50").
 * Digits before the '.' (or the end) group in threes; the sign and
 * everything from the '.' on copy through untouched. out must hold
 * strlen(in) + strlen(in)/3 + 1 bytes. */
static void fmt_group_thousands(const char *in, char *out)
{
    size_t di = 0, k, s0 = 0, digits = 0;
    if (in[0] == '-' || in[0] == '+' || in[0] == ' ')
        out[di++] = in[s0++];
    while (in[s0 + digits] >= '0' && in[s0 + digits] <= '9') digits++;
    for (k = 0; k < digits; k++) {
        if (k > 0 && (digits - k) % 3 == 0) out[di++] = ',';
        out[di++] = in[s0 + k];
    }
    strcpy(out + di, in + s0 + digits);
}

/* The Formatter's %g: `prec` total significant digits (default 6,
 * 0 promotes to 1), trailing zeros kept; scientific notation outside
 * [1e-4, 10^prec). C's %g strips zeros and picks the shorter form,
 * which prints 1.2345e-05 where the canon says 1.23450e-05. */
static void fmt_java_g(double x, long prec, char *out, size_t outsz)
{
    long   p = prec < 0 ? 6 : (prec == 0 ? 1 : prec);
    double ax = fabs(x);
    if (!isfinite(x)) {
        /* Defensive: format's %g early-outs non-finite values with
         * the canon spelling before calling here. The log10/floor
         * digit math below is undefined on them (a NaN-to-long
         * cast), so bail to C's rendering rather than reach it. */
        snprintf(out, outsz, "%f", x);
        return;
    }
    if (x != 0.0 && (ax < 1e-4 || ax >= pow(10.0, (double)p))) {
        snprintf(out, outsz, "%.*e", (int)(p - 1), x);
    } else {
        long decs = (x == 0.0) ? p - 1
                               : p - 1 - (long)floor(log10(ax));
        if (decs < 0) decs = 0;
        snprintf(out, outsz, "%.*f", (int)decs, x);
    }
}

/* Integer-directive argument: int or 64-bit-fitting bigint. A float
 * is an illegal conversion (canon throws; truncating was a silent
 * wrong answer), so anything else fails. */
static int fmt_arg_ll(const mino_val *v, long long *out)
{
    if (as_long(v, out)) return 1;
    if (v != NULL && mino_type_of(v) == MINO_BIGINT
        && mino_as_ll(v, out)) return 1;
    return 0;
}

static void fmt_ascii_upcase(char *s)
{
    for (; *s; s++)
        if (*s >= 'a' && *s <= 'z') *s = (char)(*s - 32);
}

/* Uppercase a UTF-8 buffer in place through the generated case
 * tables, matching upper-case. The tables preserve each codepoint's
 * byte length (asserted by the generator); a mapping that would
 * change length is left as-is, as are malformed lead bytes. */
static void fmt_utf8_upcase(char *s, size_t n)
{
    size_t pos = 0;
    while (pos < n) {
        size_t step = utf8_codepoint_step(s, n, pos);
        if (step == 1) {
            if (s[pos] >= 'a' && s[pos] <= 'z')
                s[pos] = (char)(s[pos] - 32);
        } else {
            char     enc[4];
            uint32_t up = mino_unicode_to_upper(utf8_decode(s + pos, step));
            if (utf8_encode(enc, up) == step)
                memcpy(s + pos, enc, step);
        }
        pos += step;
    }
}

/* Canon spelling for a non-finite double: "NaN" (never signed) or
 * "Infinity" honoring the '+', ' ', and '(' flags. Writes into `out`
 * (at least 16 bytes) and returns the length. Padding stays with the
 * caller: spaces only, never zeros. */
static size_t fmt_nonfinite(double d, int upper, int f_plus,
                            int f_space, int f_paren, char *out)
{
    if (isnan(d)) {
        strcpy(out, "NaN");
    } else if (d > 0.0) {
        out[0] = '\0';
        if (f_plus)       strcpy(out, "+");
        else if (f_space) strcpy(out, " ");
        strcat(out, "Infinity");
    } else if (f_paren) {
        strcpy(out, "(Infinity)");
    } else {
        strcpy(out, "-Infinity");
    }
    if (upper) fmt_ascii_upcase(out);
    return strlen(out);
}

/* Flag bits for the per-directive validation mask. */
enum {
    FMT_F_MINUS = 1u << 0, FMT_F_PLUS  = 1u << 1,
    FMT_F_SPACE = 1u << 2, FMT_F_ZERO  = 1u << 3,
    FMT_F_HASH  = 1u << 4, FMT_F_COMMA = 1u << 5,
    FMT_F_PAREN = 1u << 6
};

/* Per-directive allowed-flags mask, canon-probed pair by pair. The
 * general and char directives take only '-' ('#' would need an
 * extension hook mino does not have); the single-int-tier long
 * contract governs d/x/X/o, so the arbitrary-precision-only '+',
 * ' ', '(' forms stay illegal on x/X/o; ',' pairs only with the
 * decimal forms; '(' never pairs with the hex float directive. An
 * unknown spec allows everything and defers to the
 * unsupported-directive throw at dispatch. */
static unsigned fmt_allowed_flags(char spec)
{
    switch (spec) {
    case 's': case 'S': case 'b': case 'B':
    case 'c': case 'C': case '%':
        return FMT_F_MINUS;
    case 'd':
        return FMT_F_MINUS | FMT_F_PLUS | FMT_F_SPACE | FMT_F_ZERO
             | FMT_F_COMMA | FMT_F_PAREN;
    case 'x': case 'X': case 'o':
        return FMT_F_MINUS | FMT_F_ZERO | FMT_F_HASH;
    case 'e': case 'E':
        return FMT_F_MINUS | FMT_F_PLUS | FMT_F_SPACE | FMT_F_ZERO
             | FMT_F_HASH | FMT_F_PAREN;
    case 'f':
        return FMT_F_MINUS | FMT_F_PLUS | FMT_F_SPACE | FMT_F_ZERO
             | FMT_F_HASH | FMT_F_COMMA | FMT_F_PAREN;
    case 'g': case 'G':
        return FMT_F_MINUS | FMT_F_PLUS | FMT_F_SPACE | FMT_F_ZERO
             | FMT_F_COMMA | FMT_F_PAREN;
    case 'a': case 'A':
        return FMT_F_MINUS | FMT_F_PLUS | FMT_F_SPACE | FMT_F_ZERO
             | FMT_F_HASH;
    case 'n':
        return 0;
    default:
        return ~0u;
    }
}

mino_val *prim_format(mino_state *S, mino_val *args, mino_env *env)
{
    mino_val  *fmt_val;
    const char *fmt;
    size_t      fmt_len;
    mino_val  *arg_list;
    mino_val **argv = NULL;
    size_t      argc = 0;
    size_t      next_arg = 0;
    char   *buf = NULL;
    size_t  len = 0;
    size_t  cap = 0;
    size_t  i;
    const char *ekind = NULL, *ecode = NULL, *emsg = NULL;
    char        emsgbuf[64];
    (void)env;
    if (!mino_is_cons(args)) {
        return prim_throw_classified(S, "eval/arity", "MAR001",
            "format requires at least a format string");
    }
    fmt_val = args->as.cons.car;
    if (fmt_val == NULL || mino_type_of(fmt_val) != MINO_STRING) {
        return prim_throw_classified(S, "eval/type", "MTY001",
            "format: first argument must be a string");
    }
    fmt      = fmt_val->as.s.data;
    fmt_len  = fmt_val->as.s.len;
    arg_list = args->as.cons.cdr;
    {
        mino_val *walk = arg_list;
        while (mino_is_cons(walk)) { argc++; walk = walk->as.cons.cdr; }
        if (argc > 0) {
            size_t k = 0;
            argv = (mino_val **)malloc(argc * sizeof(*argv));
            if (argv == NULL) {
                return prim_throw_classified(S, "eval/out-of-memory",
                    "MOM001", "out of memory");
            }
            for (walk = arg_list; mino_is_cons(walk);
                 walk = walk->as.cons.cdr) {
                argv[k++] = walk->as.cons.car;
            }
        }
    }

    for (i = 0; i < fmt_len; i++) {
        size_t j;
        long   pos = -1, width = -1, prec = -1;
        int    f_minus = 0, f_plus = 0, f_space = 0, f_zero = 0,
               f_hash = 0, f_comma = 0, f_paren = 0;
        char   spec;
        mino_val *a = NULL;

        if (fmt[i] != '%') {
            buf = fmt_append(S, buf, &len, &cap, &fmt[i], 1);
            if (buf == NULL) { free(argv); return NULL; }
            continue;
        }
        j = i + 1;
        /* Positional prefix: digits followed by '$'. */
        {
            size_t k = j;
            long   v = 0;
            int    have = 0;
            while (k < fmt_len && fmt[k] >= '0' && fmt[k] <= '9'
                   && v < 1000000) {
                v = v * 10 + (fmt[k] - '0');
                k++; have = 1;
            }
            if (have && k < fmt_len && fmt[k] == '$' && v >= 1) {
                pos = v;
                j = k + 1;
            }
        }
        for (; j < fmt_len; j++) {
            char fc = fmt[j];
            int *fp;
            if      (fc == '-') fp = &f_minus;
            else if (fc == '+') fp = &f_plus;
            else if (fc == ' ') fp = &f_space;
            else if (fc == '0') fp = &f_zero;
            else if (fc == '#') fp = &f_hash;
            else if (fc == ',') fp = &f_comma;
            else if (fc == '(') fp = &f_paren;
            else break;
            if (*fp) {
                snprintf(emsgbuf, sizeof(emsgbuf),
                         "format: duplicate flag '%c'", fc);
                ekind = "eval/type"; ecode = "MTY001"; emsg = emsgbuf;
                goto fail;
            }
            *fp = 1;
        }
        if (j < fmt_len && fmt[j] >= '1' && fmt[j] <= '9') {
            width = 0;
            while (j < fmt_len && fmt[j] >= '0' && fmt[j] <= '9'
                   && width < 1000000) {
                width = width * 10 + (fmt[j] - '0');
                j++;
            }
        }
        if (j < fmt_len && fmt[j] == '.') {
            j++;
            prec = 0;
            while (j < fmt_len && fmt[j] >= '0' && fmt[j] <= '9'
                   && prec < 1000000) {
                prec = prec * 10 + (fmt[j] - '0');
                j++;
            }
        }
        if (j >= fmt_len) {
            ekind = "eval/type"; ecode = "MTY001";
            emsg  = "format: incomplete directive";
            goto fail;
        }
        spec = fmt[j];
        i = j; /* the loop's i++ steps past the spec */

        /* Validate the flags against the directive before anything
         * else; canon rejects an illegal pair ahead of the argument
         * list and argument conversion. */
        {
            unsigned allowed = fmt_allowed_flags(spec);
            unsigned have =
                  (f_minus ? FMT_F_MINUS : 0u)
                | (f_plus  ? FMT_F_PLUS  : 0u)
                | (f_space ? FMT_F_SPACE : 0u)
                | (f_zero  ? FMT_F_ZERO  : 0u)
                | (f_hash  ? FMT_F_HASH  : 0u)
                | (f_comma ? FMT_F_COMMA : 0u)
                | (f_paren ? FMT_F_PAREN : 0u);
            unsigned bad = have & ~allowed;
            if (bad != 0u) {
                const char *flag_chars = "-+ 0#,(";
                int b = 0;
                while ((bad & (1u << b)) == 0u) b++;
                snprintf(emsgbuf, sizeof(emsgbuf),
                         "format: illegal flag '%c' for directive '%c'",
                         flag_chars[b], spec);
                ekind = "eval/type"; ecode = "MTY001"; emsg = emsgbuf;
                goto fail;
            }
            /* The flag syntax rules apply to known directives; an
             * unknown spec falls through to the unsupported throw. */
            if (allowed != ~0u) {
                if (f_plus && f_space) {
                    ekind = "eval/type"; ecode = "MTY001";
                    emsg  = "format: the + and space flags are exclusive";
                    goto fail;
                }
                if (f_minus && f_zero) {
                    ekind = "eval/type"; ecode = "MTY001";
                    emsg  = "format: the - and 0 flags are exclusive";
                    goto fail;
                }
                if ((f_minus || f_zero) && width < 0) {
                    snprintf(emsgbuf, sizeof(emsgbuf),
                             "format: flag '%c' requires a width",
                             f_minus ? '-' : '0');
                    ekind = "eval/type"; ecode = "MTY001"; emsg = emsgbuf;
                    goto fail;
                }
            }
        }

        if (spec == '%') {
            buf = fmt_append(S, buf, &len, &cap, "%", 1);
            if (buf == NULL) { free(argv); return NULL; }
            continue;
        }
        if (spec == 'n') {
            buf = fmt_append(S, buf, &len, &cap, "\n", 1);
            if (buf == NULL) { free(argv); return NULL; }
            continue;
        }
        /* Every remaining directive consumes an argument. */
        {
            size_t idx = (pos > 0) ? (size_t)pos - 1 : next_arg++;
            if (idx >= argc) {
                ekind = "eval/arity"; ecode = "MAR001";
                emsg  = "format: not enough arguments for format string";
                goto fail;
            }
            a = argv[idx];
        }

        switch (spec) {
        case 's': case 'S': case 'b': case 'B': {
            const char *src;
            size_t      slen;
            char       *heap = NULL;
            mino_val  *sv = NULL;
            if (spec == 'b' || spec == 'B') {
                int truthy = !(mino_is_nil(a)
                               || (mino_type_of(a) == MINO_BOOL
                                   && !mino_val_bool_get(a)));
                src = truthy ? "true" : "false";
                slen = strlen(src);
            } else if (mino_is_nil(a)) {
                src = "null"; slen = 4;
            } else if (mino_type_of(a) == MINO_STRING) {
                src = a->as.s.data; slen = a->as.s.len;
            } else {
                sv = print_to_string(S, a);
                if (sv == NULL) { free(buf); free(argv); return NULL; }
                src = sv->as.s.data; slen = sv->as.s.len;
            }
            if (prec >= 0) {
                /* Precision counts codepoints, matching count; cut at
                 * the boundary, never mid-sequence. */
                size_t cut = utf8_skip_codepoints(src, slen, 0, prec);
                if (cut < slen) slen = cut;
            }
            if (spec == 'S' || spec == 'B') {
                heap = (char *)malloc(slen + 1);
                if (heap == NULL) {
                    ekind = "eval/out-of-memory"; ecode = "MOM001";
                    emsg  = "out of memory";
                    goto fail;
                }
                memcpy(heap, src, slen);
                heap[slen] = '\0';
                fmt_utf8_upcase(heap, slen);
                src = heap;
            }
            buf = fmt_append_padded(S, buf, &len, &cap, src, slen,
                                    width, f_minus);
            free(heap);
            if (buf == NULL) { free(argv); return NULL; }
            (void)sv;
            break;
        }
        case 'c': case 'C': {
            long long ll;
            uint32_t  cp;
            char      cb[4];
            size_t    cn;
            if (a != NULL && mino_type_of(a) == MINO_CHAR) {
                cp = (uint32_t)mino_val_char_get(a);
            } else if (as_long(a, &ll) && ll >= 0 && ll <= 0x10FFFF) {
                cp = (uint32_t)ll;
            } else {
                ekind = "eval/type"; ecode = "MTY001";
                emsg  = "format: %c expects a char or codepoint";
                goto fail;
            }
            if (spec == 'C') cp = mino_unicode_to_upper(cp);
            cn = utf8_encode(cb, cp);
            buf = fmt_append_padded(S, buf, &len, &cap, cb, cn,
                                    width, f_minus);
            if (buf == NULL) { free(argv); return NULL; }
            break;
        }
        case 'd': {
            long long n2;
            if (!fmt_arg_ll(a, &n2)) {
                ekind = "eval/type"; ecode = "MTY001";
                emsg  = "format: integer directive expects an integer";
                goto fail;
            }
            if (f_comma || f_paren) {
                char plain[32];
                char grouped[96];
                snprintf(plain, sizeof(plain), "%lld", n2);
                fmt_regroup_decimal(plain, grouped, sizeof(grouped),
                                    f_comma, f_paren);
                buf = fmt_append_num_padded(S, buf, &len, &cap, grouped,
                                            strlen(grouped), width,
                                            f_minus, f_zero);
                if (buf == NULL) { free(argv); return NULL; }
                break;
            }
            /* not grouped: fall through to the C-passthrough integer path */
        }
        MINO_FALLTHROUGH; /* into the x/X/o integer path */
        case 'x': case 'X': case 'o': {
            long long n2;
            char cdir[48];
            char tmp[64];
            size_t di = 0;
            int  tn;
            if (!fmt_arg_ll(a, &n2)) {
                ekind = "eval/type"; ecode = "MTY001";
                emsg  = "format: integer directive expects an integer";
                goto fail;
            }
            cdir[di++] = '%';
            if (f_minus) cdir[di++] = '-';
            if (f_plus)  cdir[di++] = '+';
            if (f_space) cdir[di++] = ' ';
            if (f_zero)  cdir[di++] = '0';
            if (f_hash)  cdir[di++] = '#';
            if (width >= 0)
                di += (size_t)snprintf(cdir + di, sizeof(cdir) - di,
                                       "%ld", width);
            if (prec >= 0)
                di += (size_t)snprintf(cdir + di, sizeof(cdir) - di,
                                       ".%ld", prec);
            cdir[di++] = 'l';
            cdir[di++] = 'l';
            cdir[di++] = spec;
            cdir[di]   = '\0';
            tn = snprintf(tmp, sizeof(tmp), cdir, n2);
            if (tn < 0) { free(buf); free(argv); return NULL; }
            {
                char *fsrc = tmp;
                char *fhtmp = NULL;
                if ((size_t)tn >= sizeof(tmp)) {
                    fhtmp = (char *)malloc((size_t)tn + 1);
                    if (fhtmp == NULL) { free(buf); free(argv); return NULL; }
                    snprintf(fhtmp, (size_t)tn + 1, cdir, n2);
                    fsrc = fhtmp;
                }
                buf = fmt_append(S, buf, &len, &cap, fsrc, (size_t)tn);
                free(fhtmp);
                if (buf == NULL) { free(argv); return NULL; }
            }
            break;
        }
        case 'f': case 'e': case 'E': {
            double d;
            char cdir[48];
            char tmp[128];
            size_t di = 0;
            int  tn;
            if (!as_double(a, &d)) {
                ekind = "eval/type"; ecode = "MTY001";
                emsg  = "format: float directive expects a number";
                goto fail;
            }
            if (!isfinite(d)) {
                char nf[16];
                size_t nn = fmt_nonfinite(d, spec == 'E', f_plus,
                                          f_space, f_paren, nf);
                buf = fmt_append_padded(S, buf, &len, &cap, nf, nn,
                                        width, f_minus);
                if (buf == NULL) { free(argv); return NULL; }
                break;
            }
            if (f_paren || f_comma) {
                /* Canon groups the integer digits under ',' and
                 * renders a finite negative inside parens under '('.
                 * Render without width, post-process, and let the
                 * numeric pad helper apply width and zero fill after
                 * the sign or '('. */
                char  *src = tmp;
                char  *heap = NULL;
                size_t sl;
                cdir[di++] = '%';
                if (f_plus)  cdir[di++] = '+';
                if (f_space) cdir[di++] = ' ';
                if (f_hash)  cdir[di++] = '#';
                if (prec >= 0)
                    di += (size_t)snprintf(cdir + di, sizeof(cdir) - di,
                                           ".%ld", prec);
                cdir[di++] = spec;
                cdir[di]   = '\0';
                tn = snprintf(tmp, sizeof(tmp), cdir, d);
                if (tn < 0) { free(buf); free(argv); return NULL; }
                if ((size_t)tn + 2 > sizeof(tmp)) {
                    heap = (char *)malloc((size_t)tn + 3);
                    if (heap == NULL) {
                        ekind = "eval/out-of-memory"; ecode = "MOM001";
                        emsg  = "out of memory";
                        goto fail;
                    }
                    snprintf(heap, (size_t)tn + 1, cdir, d);
                    src = heap;
                }
                sl = (size_t)tn;
                if (f_comma) {
                    /* Sized for every comma plus the paren pair. */
                    char *g2 = (char *)malloc(sl + sl / 3 + 4);
                    if (g2 == NULL) {
                        free(heap);
                        ekind = "eval/out-of-memory"; ecode = "MOM001";
                        emsg  = "out of memory";
                        goto fail;
                    }
                    fmt_group_thousands(src, g2);
                    free(heap);
                    heap = g2;
                    src  = g2;
                    sl   = strlen(g2);
                }
                if (f_paren && src[0] == '-') {
                    src[0] = '(';
                    src[sl++] = ')';
                    src[sl] = '\0';
                }
                buf = fmt_append_num_padded(S, buf, &len, &cap, src, sl,
                                            width, f_minus, f_zero);
                free(heap);
                if (buf == NULL) { free(argv); return NULL; }
                break;
            }
            cdir[di++] = '%';
            if (f_minus) cdir[di++] = '-';
            if (f_plus)  cdir[di++] = '+';
            if (f_space) cdir[di++] = ' ';
            if (f_zero)  cdir[di++] = '0';
            if (f_hash)  cdir[di++] = '#';
            if (width >= 0)
                di += (size_t)snprintf(cdir + di, sizeof(cdir) - di,
                                       "%ld", width);
            if (prec >= 0)
                di += (size_t)snprintf(cdir + di, sizeof(cdir) - di,
                                       ".%ld", prec);
            cdir[di++] = spec;
            cdir[di]   = '\0';
            tn = snprintf(tmp, sizeof(tmp), cdir, d);
            if (tn < 0) { free(buf); free(argv); return NULL; }
            {
                char *fsrc = tmp;
                char *fhtmp = NULL;
                if ((size_t)tn >= sizeof(tmp)) {
                    fhtmp = (char *)malloc((size_t)tn + 1);
                    if (fhtmp == NULL) { free(buf); free(argv); return NULL; }
                    snprintf(fhtmp, (size_t)tn + 1, cdir, d);
                    fsrc = fhtmp;
                }
                buf = fmt_append(S, buf, &len, &cap, fsrc, (size_t)tn);
                free(fhtmp);
                if (buf == NULL) { free(argv); return NULL; }
            }
            break;
        }
        case 'g': case 'G': {
            double d;
            char  *gtmp;
            size_t gsz;
            if (!as_double(a, &d)) {
                ekind = "eval/type"; ecode = "MTY001";
                emsg  = "format: float directive expects a number";
                goto fail;
            }
            if (!isfinite(d)) {
                char nf[16];
                size_t nn = fmt_nonfinite(d, spec == 'G', f_plus,
                                          f_space, f_paren, nf);
                buf = fmt_append_padded(S, buf, &len, &cap, nf, nn,
                                        width, f_minus);
                if (buf == NULL) { free(argv); return NULL; }
                break;
            }
            gsz = (size_t)(prec > 0 ? prec : 6) + 400;
            gtmp = (char *)malloc(gsz);
            if (gtmp == NULL) {
                ekind = "eval/out-of-memory"; ecode = "MOM001";
                emsg  = "out of memory";
                goto fail;
            }
            fmt_java_g(d, prec, gtmp, gsz);
            if (spec == 'G') fmt_ascii_upcase(gtmp);
            if (f_comma && strchr(gtmp, 'e') == NULL
                && strchr(gtmp, 'E') == NULL) {
                /* Canon groups only the decimal form; the scientific
                 * form stays ungrouped. Sized for every comma plus
                 * the paren pair. */
                size_t gl2 = strlen(gtmp);
                char  *g2 = (char *)malloc(gl2 + gl2 / 3 + 4);
                if (g2 == NULL) {
                    free(gtmp);
                    ekind = "eval/out-of-memory"; ecode = "MOM001";
                    emsg  = "out of memory";
                    goto fail;
                }
                fmt_group_thousands(gtmp, g2);
                free(gtmp);
                gtmp = g2;
            }
            if (f_paren && gtmp[0] == '-') {
                /* Canon renders a finite negative inside parens; the
                 * buffer leaves well over two bytes of slack past the
                 * longest render, so the ')' fits in place. */
                size_t gl = strlen(gtmp);
                gtmp[0] = '(';
                gtmp[gl] = ')';
                gtmp[gl + 1] = '\0';
            }
            buf = fmt_append_num_padded(S, buf, &len, &cap, gtmp,
                                        strlen(gtmp), width, f_minus,
                                        f_zero);
            free(gtmp);
            if (buf == NULL) { free(argv); return NULL; }
            break;
        }
        case 'a': case 'A': {
            double d;
            char  tmp[64];
            char *src = tmp;
            char *heap = NULL;
            char *plus;
            long  ap = (prec == 0) ? 1 : prec;
            if (!as_double(a, &d)) {
                ekind = "eval/type"; ecode = "MTY001";
                emsg  = "format: float directive expects a number";
                goto fail;
            }
            if (!isfinite(d)) {
                char nf[16];
                size_t nn = fmt_nonfinite(d, spec == 'A', f_plus,
                                          f_space, f_paren, nf);
                buf = fmt_append_padded(S, buf, &len, &cap, nf, nn,
                                        width, f_minus);
                if (buf == NULL) { free(argv); return NULL; }
                break;
            }
            if (d == 0.0 && ap < 0) {
                /* C's %a of zero omits the fraction; canon always
                 * keeps one fractional digit ("0x0.0p0"). With an
                 * explicit precision C keeps the fraction itself. */
                snprintf(tmp, sizeof(tmp), "%s%s",
                         signbit(d) ? "-"
                                    : (f_plus ? "+" : (f_space ? " " : "")),
                         "0x0.0p0");
                if (spec == 'A') fmt_ascii_upcase(tmp);
            } else {
                /* Thread the sign flags and the precision through;
                 * canon keeps precision 0 at one fractional digit.
                 * Width and zero fill stay with the pad helper. */
                char cdir[32];
                size_t di = 0;
                int  tn;
                cdir[di++] = '%';
                if (f_plus)  cdir[di++] = '+';
                if (f_space) cdir[di++] = ' ';
                if (ap >= 0)
                    di += (size_t)snprintf(cdir + di, sizeof(cdir) - di,
                                           ".%ld", ap);
                cdir[di++] = spec;
                cdir[di]   = '\0';
                tn = snprintf(tmp, sizeof(tmp), cdir, d);
                if (tn < 0) { free(buf); free(argv); return NULL; }
                if ((size_t)tn >= sizeof(tmp)) {
                    heap = (char *)malloc((size_t)tn + 1);
                    if (heap == NULL) {
                        ekind = "eval/out-of-memory"; ecode = "MOM001";
                        emsg  = "out of memory";
                        goto fail;
                    }
                    snprintf(heap, (size_t)tn + 1, cdir, d);
                    src = heap;
                }
            }
            /* Double/toHexString has no '+' on a positive exponent. */
            plus = strchr(src, spec == 'a' ? 'p' : 'P');
            if (plus != NULL && plus[1] == '+')
                memmove(plus + 1, plus + 2, strlen(plus + 2) + 1);
            buf = fmt_append_num_padded(S, buf, &len, &cap, src,
                                        strlen(src), width, f_minus,
                                        f_zero);
            free(heap);
            if (buf == NULL) { free(argv); return NULL; }
            break;
        }
        default:
            ekind = "eval/type"; ecode = "MTY001";
            emsg  = "format: unsupported directive";
            goto fail;
        }
    }
    {
        mino_val *result = mino_string_n(S, buf != NULL ? buf : "", len);
        free(buf);
        free(argv);
        return result;
    }
fail:
    free(buf);
    free(argv);
    return prim_throw_classified(S, ekind, ecode, emsg);
}

static mino_val *prim_read_string(mino_state *S, mino_val *args, mino_env *env)
{
    mino_val *s;
    mino_val *opts = NULL;
    mino_val *result;
    int         saved_mode = S->reader.reader_cond_mode;
    (void)env;
    if (!mino_is_cons(args)) {
        return prim_throw_classified(S, "eval/arity", "MAR001", "read-string requires one string argument");
    }
    /* Two-arg form: (read-string opts s). The opts map currently
     * recognises :read-cond → :allow / :preserve / :disallow. */
    if (mino_is_cons(args->as.cons.cdr)) {
        if (mino_is_cons(args->as.cons.cdr->as.cons.cdr)) {
            return prim_throw_classified(S, "eval/arity", "MAR001",
                "read-string takes one or two arguments");
        }
        opts = args->as.cons.car;
        s    = args->as.cons.cdr->as.cons.car;
    } else {
        s = args->as.cons.car;
    }
    if (s == NULL || mino_type_of(s) != MINO_STRING) {
        return prim_throw_classified(S, "eval/type", "MTY001", "read-string: argument must be a string");
    }
    if (opts != NULL && mino_type_of(opts) != MINO_NIL) {
        mino_val *rc;
        if (mino_type_of(opts) != MINO_MAP) {
            return prim_throw_classified(S, "eval/type", "MTY001",
                "read-string: opts must be a map");
        }
        rc = map_get_val(opts, mino_keyword(S, "read-cond"));
        if (rc != NULL) {
            if (mino_type_of(rc) != MINO_KEYWORD) {
                return prim_throw_classified(S, "eval/type", "MTY001",
                    "read-string: :read-cond must be a keyword");
            }
            if (strcmp(rc->as.s.data, "allow") == 0)         S->reader.reader_cond_mode = 0;
            else if (strcmp(rc->as.s.data, "preserve") == 0) S->reader.reader_cond_mode = 1;
            else if (strcmp(rc->as.s.data, "disallow") == 0) S->reader.reader_cond_mode = 2;
            else {
                return prim_throw_classified(S, "eval/contract", "MCT001",
                    "read-string: :read-cond must be :allow, :preserve, or :disallow");
            }
        }
    }
    clear_error(S);
    result = mino_read(S, s->as.s.data, NULL);
    S->reader.reader_cond_mode = saved_mode;
    if (result == NULL && mino_last_error(S) != NULL) {
        /* Throw parse errors as catchable exceptions so user code can
         * handle them via try/catch. */
        mino_val *ex = mino_string(S, mino_last_error(S));
        if (mino_current_ctx(S)->try_depth > 0) {
            mino_current_ctx(S)->try_stack[mino_current_ctx(S)->try_depth - 1].exception = ex;
            longjmp(mino_current_ctx(S)->try_stack[mino_current_ctx(S)->try_depth - 1].buf, 1);
        }
        /* No enclosing try — propagate as fatal error. */
        {
            char msg[512];
            snprintf(msg, sizeof(msg), "unhandled exception: %.*s",
                     (int)ex->as.s.len, ex->as.s.data);
            return prim_throw_classified(S, "eval/type", "MTY001", msg);
        }
    }
    return result != NULL ? result : mino_nil(S);
}

static mino_val *prim_pr_str(mino_state *S, mino_val *args, mino_env *env)
{
    /* The readable join consults the print-method hook exactly the
     * way pr does, so (pr-str x) always equals (with-out-str (pr x)),
     * custom methods included. */
    mino_val *result;
    print_dynvars_saved_t saved_dynvars;
    print_dynvars_resolve(S, env, &saved_dynvars);
    result = print_args_join(S, args, env, 1, 0);
    print_dynvars_restore(S, &saved_dynvars);
    return result;
}

static mino_val *prim_char_at(mino_state *S, mino_val *args, mino_env *env)
{
    mino_val *s, *idx_val;
    long long idx;
    (void)env;
    if (!mino_is_cons(args) || !mino_is_cons(args->as.cons.cdr)) {
        return prim_throw_classified(S, "eval/arity", "MAR001", "char-at requires two arguments");
    }
    s       = args->as.cons.car;
    idx_val = args->as.cons.cdr->as.cons.car;
    if (s == NULL || mino_type_of(s) != MINO_STRING
        || idx_val == NULL || !mino_val_int_p(idx_val)) {
        return prim_throw_classified(S, "eval/type", "MTY001", "char-at: requires a string and integer index");
    }
    idx = mino_val_int_get(idx_val);
    if (idx < 0 || (size_t)idx >= s->as.s.len) {
        return prim_throw_classified(S, "eval/bounds", "MBD001", "char-at: index out of range");
    }
    return mino_string_n(S, s->as.s.data + idx, 1);
}

/* ------------------------------------------------------------------------- */
/* String primitives                                                         */
/* ------------------------------------------------------------------------- */

/* Step one UTF-8 codepoint forward starting at byte index `pos` in
 * `data` (length `bytes`). Returns the byte length of the codepoint;
 * malformed leading bytes step by 1 to keep the walk bounded. */
size_t utf8_codepoint_step(const char *data, size_t bytes, size_t pos)
{
    unsigned char b;
    if (pos >= bytes) return 0;
    b = (unsigned char)data[pos];
    if (b < 0x80) return 1;
    if ((b & 0xE0) == 0xC0 && pos + 1 < bytes) return 2;
    if ((b & 0xF0) == 0xE0 && pos + 2 < bytes) return 3;
    if ((b & 0xF8) == 0xF0 && pos + 3 < bytes) return 4;
    return 1;
}

/* Walk `n` codepoints into `data` starting at `pos`; return the
 * resulting byte offset, capped at `bytes`. */
size_t utf8_skip_codepoints(const char *data, size_t bytes,
                            size_t pos, long long n)
{
    while (n > 0 && pos < bytes) {
        pos += utf8_codepoint_step(data, bytes, pos);
        n--;
    }
    return pos;
}

/* Count codepoints in [data, data+bytes). */
long long utf8_codepoint_count(const char *data, size_t bytes)
{
    long long count = 0;
    size_t pos = 0;
    while (pos < bytes) {
        pos += utf8_codepoint_step(data, bytes, pos);
        count++;
    }
    return count;
}

/* Lazy cached codepoint count for a MINO_STRING. See the declaration
 * in prim/internal.h for the caching discipline. */
long long mino_string_cp_count(mino_val *s)
{
    size_t cached;
    long long n;
    cached = s->as.s.ns_len;
    if (cached != 0) return (long long)cached;
    n = utf8_codepoint_count(s->as.s.data, s->as.s.len);
    /* Any positive long long fits size_t on every supported host
     * (both 64-bit; the cast would only matter on a 32-bit size_t,
     * which mino does not target). */
    if (n > 0) {
        s->as.s.ns_len = (size_t)n;
    }
    return n;
}

static mino_val *prim_subs(mino_state *S, mino_val *args, mino_env *env)
{
    mino_val *s_val;
    long long   start, end_idx;
    size_t      n;
    size_t      byte_start, byte_end;
    long long   total_cps;
    (void)env;
    arg_count(S, args, &n);
    if (n != 2 && n != 3) {
        return prim_throw_classified(S, "eval/arity", "MAR001", "subs requires 2 or 3 arguments");
    }
    s_val = args->as.cons.car;
    if (s_val == NULL || mino_type_of(s_val) != MINO_STRING) {
        return prim_throw_classified(S, "eval/type", "MTY001", "subs: first argument must be a string");
    }
    if (args->as.cons.cdr->as.cons.car == NULL
        || !mino_val_int_p(args->as.cons.cdr->as.cons.car)) {
        return prim_throw_classified(S, "eval/type", "MTY001", "subs: start index must be an integer");
    }
    start = mino_val_int_get(args->as.cons.cdr->as.cons.car);
    /* Indices are codepoint-counted, matching Clojure where strings
     * are sequences of chars (UTF-16 code units there, codepoints
     * here -- mino has no surrogates). The count comes from the lazy
     * per-string cache; when it equals the byte length the content is
     * pure ASCII and the codepoint indices are byte offsets directly,
     * skipping the per-call walks. */
    total_cps = mino_string_cp_count(s_val);
    if (n == 3) {
        if (args->as.cons.cdr->as.cons.cdr->as.cons.car == NULL
            || !mino_val_int_p(args->as.cons.cdr->as.cons.cdr->as.cons.car)) {
            return prim_throw_classified(S, "eval/type", "MTY001", "subs: end index must be an integer");
        }
        end_idx = mino_val_int_get(args->as.cons.cdr->as.cons.cdr->as.cons.car);
    } else {
        end_idx = total_cps;
    }
    if (start < 0 || end_idx < start || end_idx > total_cps) {
        return prim_throw_classified(S, "eval/bounds", "MBD001", "subs: index out of range");
    }
    if ((size_t)total_cps == s_val->as.s.len) {
        /* ASCII content: codepoint indices are byte offsets. */
        byte_start = (size_t)start;
        byte_end   = (size_t)end_idx;
    } else {
        byte_start = utf8_skip_codepoints(s_val->as.s.data, s_val->as.s.len,
                                          0, start);
        byte_end = utf8_skip_codepoints(s_val->as.s.data, s_val->as.s.len,
                                        byte_start, end_idx - start);
    }
    return mino_string_n(S, s_val->as.s.data + byte_start,
                         byte_end - byte_start);
}

static mino_val *prim_split(mino_state *S, mino_val *args, mino_env *env)
{
    mino_val  *s_val;
    mino_val  *sep_val;
    mino_val  *limit_val = NULL;
    const char  *s;
    size_t       slen;
    const char  *sep;
    size_t       sep_len;
    long long    limit = 0;       /* 0 / negative = no cap */
    mino_val **buf = NULL;
    size_t       cap = 0, len = 0;
    const char  *p;
    (void)env;
    if (!mino_is_cons(args) || !mino_is_cons(args->as.cons.cdr)) {
        return prim_throw_classified(S, "eval/arity", "MAR001", "split requires a string and a separator");
    }
    s_val   = args->as.cons.car;
    sep_val = args->as.cons.cdr->as.cons.car;
    if (mino_is_cons(args->as.cons.cdr->as.cons.cdr)) {
        limit_val = args->as.cons.cdr->as.cons.cdr->as.cons.car;
        if (mino_is_cons(args->as.cons.cdr->as.cons.cdr->as.cons.cdr)) {
            return prim_throw_classified(S, "eval/arity", "MAR001",
                "split takes at most 3 arguments");
        }
        if (limit_val == NULL || !mino_val_int_p(limit_val)) {
            return prim_throw_classified(S, "eval/type", "MTY001",
                "split: limit must be an integer");
        }
        limit = mino_val_int_get(limit_val);
    }
    if (s_val == NULL || mino_type_of(s_val) != MINO_STRING) {
        return prim_throw_classified(S, "eval/type", "MTY001",
            "split: first argument must be a string");
    }
    if (sep_val == NULL
        || (mino_type_of(sep_val) != MINO_STRING && mino_type_of(sep_val) != MINO_REGEX)) {
        return prim_throw_classified(S, "eval/type", "MTY001",
            "split: separator must be a string or regex");
    }
    s    = s_val->as.s.data;
    slen = s_val->as.s.len;
    /* Empty input: return [""] (a single empty-string element) per
     * Clojure / JVM String.split semantics. mino previously returned
     * an empty vector here. The single-empty form is what downstream
     * Clojure code expects from (str/split "" re). */
    if (slen == 0) {
        mino_val **buf1 = (mino_val **)gc_alloc_typed(S,
            GC_T_VALARR, 1 * sizeof(*buf1));
        if (buf1 == NULL) return NULL;
        buf1[0] = mino_string_n(S, "", 0);
        return mino_vector(S, buf1, 1);
    }
    if (mino_type_of(sep_val) == MINO_REGEX
        && sep_val->as.regex.source != NULL
        && mino_type_of(sep_val->as.regex.source) == MINO_STRING) {
        /* Regex separators: mirror java.util.regex.Pattern#split.
         * Pieces span [index, match-start); a zero-width match at the
         * very beginning contributes no leading empty piece; limit > 0
         * caps the piece count with the final piece absorbing the rest;
         * limit == 0 trims trailing empty pieces; limit < 0 keeps
         * them. */
        const char *pat_src = sep_val->as.regex.source->as.s.data;
        re_t        compiled;
        size_t      scan  = 0;   /* next search position */
        size_t      index = 0;   /* end of last consumed match */
        int         absorbed = 0;
        /* See prim_re_find for the rationale: the regex engine's
         * static-global match state requires every caller to be in
         * a state-safe window (state_lock held). */
        MINO_ASSERT_STATE_SAFE(S);
        compiled = re_compile(pat_src);
        if (compiled == NULL) {
            return prim_throw_classified(S, "eval/contract", "MCT001",
                "split: invalid regex pattern");
        }
        for (;;) {
            int    mlen = 0;
            int    idx;
            size_t abs_start, abs_end;
            if (scan > slen) break;
            idx = re_matchp(compiled, s + scan, &mlen);
            if (idx < 0) break;
            abs_start = scan + (size_t)idx;
            abs_end   = abs_start + (size_t)(mlen > 0 ? mlen : 0);
            if (limit <= 0 || (long long)len < limit - 1) {
                /* Leading zero-width match: no empty first piece. */
                if (!(index == 0 && abs_start == 0 && mlen <= 0)) {
                    if (len == cap) {
                        size_t new_cap;
                        mino_val **nb;
                        if (cap > SIZE_MAX / 2) {
                            set_eval_diag(S, mino_current_ctx(S)->eval_current_form, "internal", "MIN001", "split: too many pieces");
                            re_free(compiled);
                            return NULL;
                        }
                        new_cap = cap == 0 ? 8 : cap * 2;
                        if (new_cap > SIZE_MAX / sizeof(*nb)) {
                            set_eval_diag(S, mino_current_ctx(S)->eval_current_form, "internal", "MIN001", "split: too many pieces");
                            re_free(compiled);
                            return NULL;
                        }
                        nb = (mino_val **)gc_alloc_typed(S,
                            GC_T_VALARR, new_cap * sizeof(*nb));
                        if (nb == NULL) { re_free(compiled); return NULL; }
                        if (buf != NULL && len > 0)
                            memcpy(nb, buf, len * sizeof(*nb));
                        buf = nb;
                        cap = new_cap;
                    }
                    gc_pin((mino_val *)buf); /* keep buf alive across string alloc */
                    buf[len++] = mino_string_n(S, s + index,
                                               abs_start - index);
                    gc_unpin(1);
                    index = abs_end;
                }
            } else {
                /* Piece count is at limit - 1: the final piece absorbs
                 * everything from the last consumed end. */
                if (len == cap) {
                    size_t new_cap;
                    mino_val **nb;
                    if (cap > SIZE_MAX / 2) {
                        set_eval_diag(S, mino_current_ctx(S)->eval_current_form, "internal", "MIN001", "split: too many pieces");
                        re_free(compiled);
                        return NULL;
                    }
                    new_cap = cap == 0 ? 8 : cap * 2;
                    if (new_cap > SIZE_MAX / sizeof(*nb)) {
                        set_eval_diag(S, mino_current_ctx(S)->eval_current_form, "internal", "MIN001", "split: too many pieces");
                        re_free(compiled);
                        return NULL;
                    }
                    nb = (mino_val **)gc_alloc_typed(S,
                        GC_T_VALARR, new_cap * sizeof(*nb));
                    if (nb == NULL) { re_free(compiled); return NULL; }
                    if (buf != NULL && len > 0)
                        memcpy(nb, buf, len * sizeof(*nb));
                    buf = nb;
                    cap = new_cap;
                }
                gc_pin((mino_val *)buf); /* keep buf alive across string alloc */
                buf[len++] = mino_string_n(S, s + index, slen - index);
                gc_unpin(1);
                absorbed = 1;
                break;
            }
            /* Continue the scan past this match; bump one codepoint
             * past a zero-width site so the search advances without
             * splitting a multibyte UTF-8 sequence. */
            scan = (mlen > 0)
                ? abs_end
                : (abs_end >= slen
                       ? slen + 1
                       : abs_end + utf8_codepoint_step(s, slen, abs_end));
        }
        re_free(compiled);
        /* No (kept) match at all: the whole input is the only piece. */
        if (index == 0 && len == 0) {
            mino_val **buf1 = (mino_val **)gc_alloc_typed(S,
                GC_T_VALARR, 1 * sizeof(*buf1));
            if (buf1 == NULL) return NULL;
            buf1[0] = s_val;
            return mino_vector(S, buf1, 1);
        }
        if (!absorbed && (limit <= 0 || (long long)len < limit)) {
            if (len == cap) {
                size_t new_cap = cap == 0 ? 8 : cap * 2;
                mino_val **nb = (mino_val **)gc_alloc_typed(S,
                    GC_T_VALARR, new_cap * sizeof(*nb));
                if (nb == NULL) return NULL;
                if (buf != NULL && len > 0) memcpy(nb, buf, len * sizeof(*nb));
                buf = nb;
                cap = new_cap;
            }
            gc_pin((mino_val *)buf);
            buf[len++] = mino_string_n(S, s + index, slen - index);
            gc_unpin(1);
        }
        /* Only limit == 0 trims trailing empty pieces (JVM rule);
         * negative limits keep them. */
        if (limit == 0) {
            while (len > 0
                   && buf[len - 1] != NULL
                   && mino_type_of(buf[len - 1]) == MINO_STRING
                   && buf[len - 1]->as.s.len == 0) {
                len--;
            }
        }
        return mino_vector(S, buf, len);
    } else {
        sep     = sep_val->as.s.data;
        sep_len = sep_val->as.s.len;
    }
    p       = s;
    if (sep_len == 0) {
        /* Split into individual characters (codepoints, not bytes, so
         * multibyte UTF-8 sequences stay intact). limit > 0 caps the
         * piece count with the final piece absorbing the rest. */
        size_t pos = 0;
        while (pos < slen) {
            size_t step = utf8_codepoint_step(s, slen, pos);
            if (len == cap) {
                size_t new_cap = cap == 0 ? 8 : cap * 2;
                mino_val **nb = (mino_val **)gc_alloc_typed(S,
                    GC_T_VALARR, new_cap * sizeof(*nb));
                if (nb == NULL) return NULL;
                if (buf != NULL && len > 0) memcpy(nb, buf, len * sizeof(*nb));
                buf = nb;
                cap = new_cap;
            }
            if (limit > 0 && (long long)len + 1 == limit) {
                gc_pin((mino_val *)buf);
                buf[len++] = mino_string_n(S, s + pos, slen - pos);
                gc_unpin(1);
                pos = slen;
                break;
            }
            gc_pin((mino_val *)buf);
            buf[len++] = mino_string_n(S, s + pos, step);
            gc_unpin(1);
            pos += step;
        }
        if (buf == NULL) {
            buf = (mino_val **)gc_alloc_typed(S, GC_T_VALARR, sizeof(*buf));
            if (buf == NULL) return NULL;
        }
        return mino_vector(S, buf, len);
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
            mino_val **nb = (mino_val **)gc_alloc_typed(S,
                GC_T_VALARR, new_cap * sizeof(*nb));
            if (nb == NULL) return NULL;
            if (buf != NULL && len > 0) memcpy(nb, buf, len * sizeof(*nb));
            buf = nb;
            cap = new_cap;
        }
        /* Limit reached: emit one final item that absorbs the rest of
         * the string (matches canon's String.split(re, limit > 0). */
        if (limit > 0 && (long long)len + 1 == limit) {
            gc_pin((mino_val *)buf);
            buf[len++] = mino_string_n(S, p, (size_t)(s + slen - p));
            gc_unpin(1);
            break;
        }
        if (found != NULL) {
            gc_pin((mino_val *)buf);
            buf[len++] = mino_string_n(S, p, (size_t)(found - p));
            gc_unpin(1);
            p = found + sep_len;
        } else {
            gc_pin((mino_val *)buf);
            buf[len++] = mino_string_n(S, p, (size_t)(s + slen - p));
            gc_unpin(1);
            break;
        }
    }
    /* Only limit == 0 trims trailing empty pieces (JVM rule); negative
     * limits keep them. */
    if (limit == 0) {
        while (len > 0
               && buf[len - 1] != NULL
               && mino_type_of(buf[len - 1]) == MINO_STRING
               && buf[len - 1]->as.s.len == 0) {
            len--;
        }
    }
    return mino_vector(S, buf, len);
}

static mino_val *prim_join(mino_state *S, mino_val *args, mino_env *env)
{
    mino_val  *sep_val;
    mino_val  *coll;
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
        if (sep_val != NULL && mino_type_of(sep_val) == MINO_STRING) {
            sep     = sep_val->as.s.data;
            sep_len = sep_val->as.s.len;
        } else if (sep_val != NULL && mino_type_of(sep_val) != MINO_NIL) {
            return prim_throw_classified(S, "eval/type", "MTY001", "join: separator must be a string or nil");
        }
    } else {
        return prim_throw_classified(S, "eval/arity", "MAR001", "join requires 1 or 2 arguments");
    }
    if (coll == NULL || mino_type_of(coll) == MINO_NIL) {
        return mino_string(S, "");
    }
    seq_iter_init(S, &it, coll);
    while (!seq_iter_done(&it)) {
        mino_val *elem = seq_iter_val(S, &it);
        const char *part;
        size_t      part_len;
        size_t      need;
        if (elem == NULL || mino_type_of(elem) == MINO_NIL) {
            seq_iter_next(S, &it);
            continue;
        }
        if (mino_type_of(elem) == MINO_STRING) {
            part     = elem->as.s.data;
            part_len = elem->as.s.len;
        } else {
            /* Convert to string. */
            mino_val *str_a = mino_cons(S, elem, mino_nil(S));
            mino_val *str   = prim_str(S, str_a, env);
            if (str == NULL) { free(buf); return NULL; }
            part     = str->as.s.data;
            part_len = str->as.s.len;
        }
        need = buf_len + (first ? 0 : sep_len) + part_len + 1;
        if (need > buf_cap) {
            char *newbuf;
            buf_cap = buf_cap == 0 ? 128 : buf_cap;
            while (buf_cap < need) {
                if (buf_cap > SIZE_MAX / 2) {
                    free(buf);
                    set_eval_diag(S, mino_current_ctx(S)->eval_current_form, "internal", "MIN001", "join: result size overflow");
                    return NULL;
                }
                buf_cap *= 2;
            }
            newbuf = (char *)realloc(buf, buf_cap);
            if (newbuf == NULL) {
                free(buf);
                set_eval_diag(S, mino_current_ctx(S)->eval_current_form, "internal", "MIN001", "out of memory");
                return NULL;
            }
            buf = newbuf;
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
        mino_val *result = mino_string_n(S, buf != NULL ? buf : "", buf_len);
        free(buf);
        return result;
    }
}

/* Grow `*pbuf` so `*plen + n + 1` bytes fit, then append `n` bytes from
 * `src`. On OOM, frees `*pbuf` and returns -1 (caller must surface the
 * error to S). Used by the regex-replace and template-expansion paths. */
static int str_replace_buf_append(mino_state *S, char **pbuf,
                                  size_t *plen, size_t *pcap,
                                  const char *src, size_t n)
{
    size_t need;
    if (n > SIZE_MAX - *plen - 1) {
        free(*pbuf); *pbuf = NULL;
        set_eval_diag(S, mino_current_ctx(S)->eval_current_form,
                      "internal", "MIN001",
                      "str-replace: result size overflow");
        return -1;
    }
    need = *plen + n + 1;
    if (need > *pcap) {
        size_t new_cap = *pcap == 0 ? 256 : *pcap;
        char  *nb;
        while (new_cap < need) {
            if (new_cap > SIZE_MAX / 2) {
                free(*pbuf); *pbuf = NULL;
                set_eval_diag(S, mino_current_ctx(S)->eval_current_form,
                              "internal", "MIN001",
                              "str-replace: result size overflow");
                return -1;
            }
            new_cap *= 2;
        }
        nb = (char *)realloc(*pbuf, new_cap);
        if (nb == NULL) {
            free(*pbuf); *pbuf = NULL;
            set_eval_diag(S, mino_current_ctx(S)->eval_current_form,
                          "internal", "MIN001", "out of memory");
            return -1;
        }
        *pbuf = nb;
        *pcap = new_cap;
    }
    if (n > 0) memcpy(*pbuf + *plen, src, n);
    *plen += n;
    return 0;
}

/* JVM-style replacement-template expansion. Recognised escapes:
 *   \$  -> literal $
 *   \\  -> literal \
 *   $N  -> capture group N (0 = whole match, 1..9 = positional groups).
 * Other characters are copied verbatim. `text_base` plus the offsets in
 * `g` and `match_idx` resolves to absolute group bytes in the search
 * input. Returns 0 on success or -1 on error (S has the diag). */
static int str_replace_expand_template(mino_state *S,
                                       char **pbuf, size_t *plen, size_t *pcap,
                                       const char *tmpl, size_t tlen,
                                       const char *text_base,
                                       int match_idx, int match_len,
                                       const re_groups_t *g)
{
    size_t i = 0;
    while (i < tlen) {
        char c = tmpl[i];
        if (c == '\\' && i + 1 < tlen) {
            if (str_replace_buf_append(S, pbuf, plen, pcap, tmpl + i + 1, 1) < 0)
                return -1;
            i += 2;
        } else if (c == '$' && i + 1 < tlen
                   && tmpl[i + 1] >= '0' && tmpl[i + 1] <= '9') {
            int n = tmpl[i + 1] - '0';
            if (n == 0) {
                if (str_replace_buf_append(S, pbuf, plen, pcap,
                                           text_base + match_idx,
                                           (size_t)match_len) < 0)
                    return -1;
            } else if (n <= g->n) {
                int gs = g->starts[n - 1], ge = g->ends[n - 1];
                if (gs >= 0 && ge >= gs) {
                    if (str_replace_buf_append(S, pbuf, plen, pcap,
                                               text_base + gs,
                                               (size_t)(ge - gs)) < 0)
                        return -1;
                }
                /* unmatched group: contributes nothing, mirroring JVM */
            } else {
                free(*pbuf); *pbuf = NULL;
                prim_throw_classified(S, "eval/contract", "MCT001",
                    "str-replace: replacement references missing capture group");
                return -1;
            }
            i += 2;
        } else {
            if (str_replace_buf_append(S, pbuf, plen, pcap, &c, 1) < 0)
                return -1;
            i += 1;
        }
    }
    return 0;
}

/* Build the match value handed to a callable replacement: the whole-
 * match string when the pattern has no capture groups, or
 * `[whole g1 g2 ...]` when it does. Returns NULL on allocation failure
 * (diag already set). */
static mino_val *str_replace_match_arg(mino_state *S,
                                         const char *text_base,
                                         int match_idx, int match_len,
                                         const re_groups_t *g)
{
    if (g->n == 0) {
        return mino_string_n(S, text_base + match_idx, (size_t)match_len);
    }
    {
        mino_val *items[1 + RE_MAX_GROUPS];
        size_t      n = 1;
        int         i;
        mino_current_ctx(S)->gc_depth++;
        items[0] = mino_string_n(S, text_base + match_idx, (size_t)match_len);
        for (i = 0; i < g->n; i++) {
            if (g->starts[i] < 0 || g->ends[i] < 0
             || g->ends[i] < g->starts[i]) {
                items[n++] = mino_nil(S);
            } else {
                items[n++] = mino_string_n(S, text_base + g->starts[i],
                                           (size_t)(g->ends[i] - g->starts[i]));
            }
        }
        mino_current_ctx(S)->gc_depth--;
        return mino_vector(S, items, n);
    }
}

/* (str-replace s match replacement)
 *
 *   match=string : literal substring replacement (single-pass).
 *   match=regex  : regex-driven replacement. When `replacement` is a
 *                  string, $N references and \$ / \\ escapes are
 *                  expanded JVM-style; otherwise `replacement` is
 *                  called as a fn with the match value (a string when
 *                  the pattern has no groups, or `[whole g1 g2 ...]`
 *                  when it does) and its (str-coerced) result is used
 *                  literally. */
static mino_val *str_replace_impl(mino_state *S, mino_val *args,
                                    mino_env *env, int first_only)
{
    mino_val *s_val, *match_val, *repl_val;
    const char *s;
    size_t      slen;
    char       *buf = NULL;
    size_t      buf_len = 0, buf_cap = 0;
    (void)env;

    if (!mino_is_cons(args) || !mino_is_cons(args->as.cons.cdr)
        || !mino_is_cons(args->as.cons.cdr->as.cons.cdr)) {
        return prim_throw_classified(S, "eval/arity", "MAR001",
            "str-replace requires three arguments");
    }
    s_val     = args->as.cons.car;
    match_val = args->as.cons.cdr->as.cons.car;
    repl_val  = args->as.cons.cdr->as.cons.cdr->as.cons.car;
    if (s_val == NULL || mino_type_of(s_val) != MINO_STRING) {
        return prim_throw_classified(S, "eval/type", "MTY001",
            "str-replace: first argument must be a string");
    }
    if (match_val == NULL
        || (mino_type_of(match_val) != MINO_STRING
            && mino_type_of(match_val) != MINO_REGEX)) {
        return prim_throw_classified(S, "eval/type", "MTY001",
            "str-replace: match must be a string or regex");
    }
    s    = s_val->as.s.data;
    slen = s_val->as.s.len;

    /* ----- literal-string match: the original fast path ----- */
    if (mino_type_of(match_val) == MINO_STRING) {
        const char *match, *repl, *p;
        size_t mlen, rlen;
        if (repl_val == NULL || mino_type_of(repl_val) != MINO_STRING) {
            return prim_throw_classified(S, "eval/type", "MTY001",
                "str-replace: replacement must be a string when match is a string");
        }
        match = match_val->as.s.data; mlen = match_val->as.s.len;
        repl  = repl_val->as.s.data;  rlen = repl_val->as.s.len;
        if (mlen == 0) return s_val;
        buf_cap = slen + 256;
        buf = (char *)malloc(buf_cap);
        if (buf == NULL) {
            set_eval_diag(S, mino_current_ctx(S)->eval_current_form,
                          "internal", "MIN001", "out of memory");
            return NULL;
        }
        p = s;
        while (p < s + slen) {
            const char *found = NULL;
            const char *q;
            for (q = p; q + mlen <= s + slen; q++) {
                if (memcmp(q, match, mlen) == 0) { found = q; break; }
            }
            if (found != NULL) {
                size_t prefix_len = (size_t)(found - p);
                if (str_replace_buf_append(S, &buf, &buf_len, &buf_cap,
                                           p, prefix_len) < 0) return NULL;
                if (str_replace_buf_append(S, &buf, &buf_len, &buf_cap,
                                           repl, rlen) < 0) return NULL;
                p = found + mlen;
                if (first_only) {
                    size_t tail_len = (size_t)(s + slen - p);
                    if (str_replace_buf_append(S, &buf, &buf_len, &buf_cap,
                                               p, tail_len) < 0) return NULL;
                    break;
                }
            } else {
                size_t tail_len = (size_t)(s + slen - p);
                if (str_replace_buf_append(S, &buf, &buf_len, &buf_cap,
                                           p, tail_len) < 0) return NULL;
                break;
            }
        }
        {
            mino_val *result = mino_string_n(S, buf, buf_len);
            free(buf);
            return result;
        }
    }

    /* ----- regex match ----- */
    {
        const char *pat_src;
        re_t        compiled;
        size_t      pos = 0;
        int         repl_is_string = (repl_val != NULL
                                      && mino_type_of(repl_val) == MINO_STRING);
        const char *tmpl = repl_is_string ? repl_val->as.s.data  : NULL;
        size_t      tlen = repl_is_string ? repl_val->as.s.len   : 0;
        if (match_val->as.regex.source == NULL
            || mino_type_of(match_val->as.regex.source) != MINO_STRING) {
            return prim_throw_classified(S, "eval/contract", "MCT001",
                "str-replace: regex has no source pattern");
        }
        pat_src  = match_val->as.regex.source->as.s.data;
        MINO_ASSERT_STATE_SAFE(S);
        compiled = re_compile(pat_src);
        if (compiled == NULL) {
            return prim_throw_classified(S, "eval/contract", "MCT001",
                "str-replace: invalid regex pattern");
        }
        buf_cap = slen + 256;
        buf = (char *)malloc(buf_cap);
        if (buf == NULL) {
            re_free(compiled);
            set_eval_diag(S, mino_current_ctx(S)->eval_current_form,
                          "internal", "MIN001", "out of memory");
            return NULL;
        }
        while (pos <= slen) {
            int         match_len = 0;
            re_groups_t groups;
            int         idx;
            idx = re_matchp_groups(compiled, s + pos, &match_len, &groups);
            if (idx < 0) {
                /* No more matches: copy the tail and finish. */
                if (str_replace_buf_append(S, &buf, &buf_len, &buf_cap,
                                           s + pos, slen - pos) < 0) {
                    re_free(compiled); return NULL;
                }
                break;
            }
            /* Emit the prefix between `pos` and the match start. */
            if (str_replace_buf_append(S, &buf, &buf_len, &buf_cap,
                                       s + pos, (size_t)idx) < 0) {
                re_free(compiled); return NULL;
            }
            /* Emit the replacement: template expansion or fn call. */
            if (repl_is_string) {
                if (str_replace_expand_template(S, &buf, &buf_len, &buf_cap,
                                                tmpl, tlen,
                                                s + pos, idx, match_len,
                                                &groups) < 0) {
                    re_free(compiled);
                    return NULL;
                }
            } else {
                mino_val *argv1[1];
                mino_val *call_arg;
                mino_val *call_res;
                call_arg = str_replace_match_arg(S, s + pos, idx, match_len,
                                                 &groups);
                if (call_arg == NULL) {
                    free(buf); re_free(compiled); return NULL;
                }
                argv1[0] = call_arg;
                call_res = apply_callable_argv(S, repl_val, argv1, 1, env);
                if (call_res == NULL) {
                    free(buf); re_free(compiled); return NULL;
                }
                if (mino_type_of(call_res) == MINO_STRING) {
                    if (str_replace_buf_append(S, &buf, &buf_len, &buf_cap,
                                               call_res->as.s.data,
                                               call_res->as.s.len) < 0) {
                        re_free(compiled); return NULL;
                    }
                } else if (mino_type_of(call_res) == MINO_CHAR) {
                    /* Single-codepoint result: encode as full UTF-8 (1-4
                     * bytes) so non-ASCII codepoints are represented
                     * correctly rather than truncated to one byte. */
                    char utf8_tmp[4];
                    int  utf8_n;
                    unsigned cp = (unsigned)call_res->as.ch;
                    if (cp <= 0x7F) {
                        utf8_tmp[0] = (char)cp; utf8_n = 1;
                    } else if (cp <= 0x7FF) {
                        utf8_tmp[0] = (char)(0xC0 | (cp >> 6));
                        utf8_tmp[1] = (char)(0x80 | (cp & 0x3F));
                        utf8_n = 2;
                    } else if (cp <= 0xFFFF) {
                        utf8_tmp[0] = (char)(0xE0 | (cp >> 12));
                        utf8_tmp[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
                        utf8_tmp[2] = (char)(0x80 | (cp & 0x3F));
                        utf8_n = 3;
                    } else {
                        utf8_tmp[0] = (char)(0xF0 | (cp >> 18));
                        utf8_tmp[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
                        utf8_tmp[2] = (char)(0x80 | ((cp >> 6) & 0x3F));
                        utf8_tmp[3] = (char)(0x80 | (cp & 0x3F));
                        utf8_n = 4;
                    }
                    if (str_replace_buf_append(S, &buf, &buf_len, &buf_cap,
                                               utf8_tmp, (size_t)utf8_n) < 0) {
                        re_free(compiled); return NULL;
                    }
                } else {
                    free(buf); re_free(compiled);
                    return prim_throw_classified(S, "eval/type", "MTY001",
                        "str-replace: fn replacement must return a string or char");
                }
            }
            if (first_only) {
                size_t done = pos + (size_t)idx + (size_t)(match_len > 0
                                                            ? match_len : 0);
                if (str_replace_buf_append(S, &buf, &buf_len, &buf_cap,
                                           s + done, slen - done) < 0) {
                    re_free(compiled); return NULL;
                }
                break;
            }
            /* Advance past the match. For zero-width matches, step by
             * one byte to avoid an infinite loop -- copy the byte at
             * the match site so the result mirrors the input shape. */
            if (match_len <= 0) {
                if (pos + (size_t)idx >= slen) {
                    /* Zero-width match at end-of-string: nothing left. */
                    break;
                }
                if (str_replace_buf_append(S, &buf, &buf_len, &buf_cap,
                                           s + pos + idx, 1) < 0) {
                    re_free(compiled); return NULL;
                }
                pos += (size_t)idx + 1;
            } else {
                pos += (size_t)idx + (size_t)match_len;
            }
        }
        re_free(compiled);
        {
            mino_val *result = mino_string_n(S, buf, buf_len);
            free(buf);
            return result;
        }
    }
}

static mino_val *prim_starts_with_p(mino_state *S, mino_val *args, mino_env *env)
{
    mino_val *s, *prefix;
    (void)env;
    if (!mino_is_cons(args) || !mino_is_cons(args->as.cons.cdr)) {
        return prim_throw_classified(S, "eval/arity", "MAR001", "starts-with? requires two string arguments");
    }
    s      = args->as.cons.car;
    prefix = args->as.cons.cdr->as.cons.car;
    if (s == NULL || mino_type_of(s) != MINO_STRING
        || prefix == NULL || mino_type_of(prefix) != MINO_STRING) {
        return prim_throw_classified(S, "eval/type", "MTY001", "starts-with? requires two string arguments");
    }
    if (prefix->as.s.len > s->as.s.len) return mino_false(S);
    return memcmp(s->as.s.data, prefix->as.s.data, prefix->as.s.len) == 0
        ? mino_true(S) : mino_false(S);
}

static mino_val *prim_ends_with_p(mino_state *S, mino_val *args, mino_env *env)
{
    mino_val *s, *suffix;
    (void)env;
    if (!mino_is_cons(args) || !mino_is_cons(args->as.cons.cdr)) {
        return prim_throw_classified(S, "eval/arity", "MAR001", "ends-with? requires two string arguments");
    }
    s      = args->as.cons.car;
    suffix = args->as.cons.cdr->as.cons.car;
    if (s == NULL || mino_type_of(s) != MINO_STRING
        || suffix == NULL || mino_type_of(suffix) != MINO_STRING) {
        return prim_throw_classified(S, "eval/type", "MTY001", "ends-with? requires two string arguments");
    }
    if (suffix->as.s.len > s->as.s.len) return mino_false(S);
    return memcmp(s->as.s.data + s->as.s.len - suffix->as.s.len,
                  suffix->as.s.data, suffix->as.s.len) == 0
        ? mino_true(S) : mino_false(S);
}

static mino_val *prim_includes_p(mino_state *S, mino_val *args, mino_env *env)
{
    mino_val *s, *sub;
    const char *p;
    (void)env;
    if (!mino_is_cons(args) || !mino_is_cons(args->as.cons.cdr)) {
        return prim_throw_classified(S, "eval/arity", "MAR001", "includes? requires two string arguments");
    }
    s   = args->as.cons.car;
    sub = args->as.cons.cdr->as.cons.car;
    if (s == NULL || mino_type_of(s) != MINO_STRING
        || sub == NULL || mino_type_of(sub) != MINO_STRING) {
        return prim_throw_classified(S, "eval/type", "MTY001", "includes? requires two string arguments");
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

/* Case mapping over UTF-8 codepoints. ASCII-only strings keep the
 * byte-wise fast path; anything with a high byte decodes each
 * codepoint, maps it through the generated Unicode tables (ADR 31),
 * and re-encodes. Malformed sequences (a lone high byte) copy
 * verbatim rather than being re-encoded as a different codepoint. */
#include "unicode_case.h"

static uint32_t utf8_decode(const char *p, size_t len)
{
    unsigned char b = (unsigned char)p[0];
    if (b < 0x80) return b;
    if ((b & 0xE0) == 0xC0 && len >= 2)
        return ((uint32_t)(b & 0x1F) << 6)
             | (uint32_t)((unsigned char)p[1] & 0x3F);
    if ((b & 0xF0) == 0xE0 && len >= 3)
        return ((uint32_t)(b & 0x0F) << 12)
             | ((uint32_t)((unsigned char)p[1] & 0x3F) << 6)
             | (uint32_t)((unsigned char)p[2] & 0x3F);
    if ((b & 0xF8) == 0xF0 && len >= 4)
        return ((uint32_t)(b & 0x07) << 18)
             | ((uint32_t)((unsigned char)p[1] & 0x3F) << 12)
             | ((uint32_t)((unsigned char)p[2] & 0x3F) << 6)
             | (uint32_t)((unsigned char)p[3] & 0x3F);
    return b;
}

static size_t utf8_encode(char *p, uint32_t cp)
{
    if (cp <= 0x7F) {
        p[0] = (char)cp;
        return 1;
    }
    if (cp <= 0x7FF) {
        p[0] = (char)(0xC0 | (cp >> 6));
        p[1] = (char)(0x80 | (cp & 0x3F));
        return 2;
    }
    if (cp <= 0xFFFF) {
        p[0] = (char)(0xE0 | (cp >> 12));
        p[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
        p[2] = (char)(0x80 | (cp & 0x3F));
        return 3;
    }
    p[0] = (char)(0xF0 | (cp >> 18));
    p[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
    p[2] = (char)(0x80 | ((cp >> 6) & 0x3F));
    p[3] = (char)(0x80 | (cp & 0x3F));
    return 4;
}

static mino_val *string_case_map(mino_state *S, mino_val *args,
                                 int to_upper, const char *opname)
{
    mino_val *s;
    char     msg[80];
    size_t   i;

    if (!mino_is_cons(args) || mino_is_cons(args->as.cons.cdr)) {
        snprintf(msg, sizeof(msg), "%s requires one string argument", opname);
        return prim_throw_classified(S, "eval/arity", "MAR001", msg);
    }
    s = args->as.cons.car;
    if (s == NULL || mino_type_of(s) != MINO_STRING) {
        snprintf(msg, sizeof(msg), "%s requires one string argument", opname);
        return prim_throw_classified(S, "eval/type", "MTY001", msg);
    }

    /* ASCII fast path: no high byte, byte-wise casing is exact. */
    for (i = 0; i < s->as.s.len; i++) {
        if ((unsigned char)s->as.s.data[i] >= 0x80) break;
    }
    if (i == s->as.s.len) {
        char *buf = (char *)malloc(s->as.s.len + 1);
        mino_val *result;
        size_t j;
        if (buf == NULL && s->as.s.len > 0) {
            set_eval_diag(S, mino_current_ctx(S)->eval_current_form,
                          "internal", "MIN001", "out of memory");
            return NULL;
        }
        for (j = 0; j < s->as.s.len; j++) {
            buf[j] = (char)(to_upper
                ? toupper((unsigned char)s->as.s.data[j])
                : tolower((unsigned char)s->as.s.data[j]));
        }
        result = mino_string_n(S, buf, s->as.s.len);
        free(buf);
        return result;
    }

    /* Unicode path. Worst case every 2-byte codepoint maps to a
     * 4-byte one, so bound the buffer at twice the input length. */
    {
        char    *buf = (char *)malloc(2 * s->as.s.len + 4);
        size_t   pos = 0, out = 0;
        mino_val *result;
        if (buf == NULL) {
            set_eval_diag(S, mino_current_ctx(S)->eval_current_form,
                          "internal", "MIN001", "out of memory");
            return NULL;
        }
        while (pos < s->as.s.len) {
            size_t step = utf8_codepoint_step(s->as.s.data, s->as.s.len, pos);
            uint32_t cp;
            if (step <= 1 && (unsigned char)s->as.s.data[pos] >= 0x80) {
                /* malformed lead byte: copy verbatim */
                buf[out++] = s->as.s.data[pos];
                pos += 1;
                continue;
            }
            cp = utf8_decode(s->as.s.data + pos, step);
            cp = to_upper ? mino_unicode_to_upper(cp) : mino_unicode_to_lower(cp);
            out += utf8_encode(buf + out, cp);
            pos += step;
        }
        result = mino_string_n(S, buf, out);
        free(buf);
        return result;
    }
}

static mino_val *prim_upper_case(mino_state *S, mino_val *args, mino_env *env)
{
    (void)env;
    return string_case_map(S, args, 1, "upper-case");
}

static mino_val *prim_lower_case(mino_state *S, mino_val *args, mino_env *env)
{
    (void)env;
    return string_case_map(S, args, 0, "lower-case");
}

static mino_val *prim_trim(mino_state *S, mino_val *args, mino_env *env)
{
    mino_val *s;
    const char *start, *end_ptr;
    (void)env;
    if (!mino_is_cons(args) || mino_is_cons(args->as.cons.cdr)) {
        return prim_throw_classified(S, "eval/arity", "MAR001", "trim requires one string argument");
    }
    s = args->as.cons.car;
    if (s == NULL || mino_type_of(s) != MINO_STRING) {
        return prim_throw_classified(S, "eval/type", "MTY001", "trim requires one string argument");
    }
    start   = s->as.s.data;
    end_ptr = s->as.s.data + s->as.s.len;
    while (start < end_ptr && isspace((unsigned char)*start)) start++;
    while (end_ptr > start && isspace((unsigned char)*(end_ptr - 1))) end_ptr--;
    return mino_string_n(S, start, (size_t)(end_ptr - start));
}

/* Grow the buffer *buf to capacity *cap, ensuring at least need bytes fit.
 * Returns 1 on success, 0 on OOM (frees *buf, sets it to NULL).
 * Includes a SIZE_MAX/2 overflow guard. */
static int str_buf_grow(mino_state *S, char **buf, size_t *cap, size_t need)
{
    char *newbuf;
    size_t newcap = (*cap == 0) ? 128 : *cap;
    while (newcap < need) {
        if (newcap > SIZE_MAX / 2) {
            free(*buf);
            *buf = NULL;
            set_eval_diag(S, mino_current_ctx(S)->eval_current_form,
                          "internal", "MIN001", "str: result size overflow");
            return 0;
        }
        newcap *= 2;
    }
    newbuf = (char *)realloc(*buf, newcap);
    if (newbuf == NULL) {
        free(*buf);
        *buf = NULL;
        set_eval_diag(S, mino_current_ctx(S)->eval_current_form,
                      "internal", "MIN001", "out of memory");
        return 0;
    }
    *buf = newbuf;
    *cap = newcap;
    return 1;
}

/* Append nbytes bytes from src to the dynamic buffer (buf, len, cap).
 * Returns 1 on success, 0 on OOM (frees *buf and sets it to NULL). */
static int str_buf_append(mino_state *S, char **buf, size_t *len, size_t *cap,
                          const char *src, size_t nbytes)
{
    size_t need = *len + nbytes + 1;
    if (need > *cap && !str_buf_grow(S, buf, cap, need)) return 0;
    memcpy(*buf + *len, src, nbytes);
    *len += nbytes;
    return 1;
}

/* Render a bigdec value (without the trailing M) into fb[256].
 * Returns the number of bytes written (0 on error). */
static int str_fmt_bigdec(const mino_val *a, char *fb, int fbsz)
{
    char *digits = mino_bigint_to_cstr(a->as.bigdec.unscaled);
    int   fn2    = 0;
    if (digits == NULL) return 0;
    {
        int scale        = a->as.bigdec.scale;
        int neg          = (digits[0] == '-');
        int dlen         = (int)strlen(digits);
        int int_part_len = dlen - (neg ? 1 : 0) - scale;
        if (scale == 0) {
            fn2 = snprintf(fb, (size_t)fbsz, "%s", digits);
        } else if (int_part_len > 0) {
            int j, k = 0;
            for (j = 0; j < (neg ? 1 : 0) + int_part_len && k < fbsz - 1; j++)
                fb[k++] = digits[j];
            if (k < fbsz - 1) fb[k++] = '.';
            for (; j < dlen && k < fbsz - 1; j++) fb[k++] = digits[j];
            fb[k] = '\0';
            fn2 = k;
        } else {
            int pad, k = 0;
            if (neg && k < fbsz - 1) fb[k++] = '-';
            if (k + 2 < fbsz) { fb[k++] = '0'; fb[k++] = '.'; }
            for (pad = 0; pad < -int_part_len && k < fbsz - 1; pad++)
                fb[k++] = '0';
            {
                const char *src = digits + (neg ? 1 : 0);
                while (*src && k < fbsz - 1) fb[k++] = *src++;
            }
            fb[k] = '\0';
            fn2 = k;
        }
    }
    free(digits);
    return fn2;
}

/*
 * (str & args) — concatenate printed representations. Strings contribute
 * their raw content (no quotes); everything else uses the printer form.
 */
mino_val *prim_str(mino_state *S, mino_val *args, mino_env *env)
{
    char  *buf = NULL;
    size_t len = 0;
    size_t cap = 0;
    (void)env;
    while (mino_is_cons(args)) {
        mino_val *a = args->as.cons.car;
        /* `str` on a regex yields the bare pattern source (the
         * canonical toString), not the readable #"..." form the
         * printer emits. Unwrap to the source string so the plain
         * string arm below appends it raw. */
        if (a != NULL && mino_type_of(a) == MINO_REGEX
            && a->as.regex.source != NULL
            && mino_type_of(a->as.regex.source) == MINO_STRING) {
            a = a->as.regex.source;
        }
        if (a != NULL && mino_type_of(a) == MINO_STRING) {
            if (!str_buf_append(S, &buf, &len, &cap,
                                a->as.s.data, a->as.s.len)) return NULL;
        } else if (a != NULL && mino_type_of(a) == MINO_NIL) {
            /* nil contributes nothing. */
        } else if (a == NULL) {
            /* NULL treated as nil. */
        } else {
            /* Print to a temp buffer using the standard printer. */
            char tmp[256];
            int  n;
            switch (mino_type_of(a)) {
            case MINO_BOOL:
                n = snprintf(tmp, sizeof(tmp), "%s", mino_val_bool_get(a) ? "true" : "false");
                break;
            case MINO_INT:
                n = snprintf(tmp, sizeof(tmp), "%lld", mino_val_int_get(a));
                break;
            case MINO_BIGINT: {
                /* `str` strips the readable-form N suffix. */
                char *digits = mino_bigint_to_cstr(a);
                if (digits != NULL) {
                    size_t plen = strlen(digits);
                    int ok = str_buf_append(S, &buf, &len, &cap, digits, plen);
                    free(digits);
                    if (!ok) return NULL;
                }
                n = 0;
                break;
            }
            case MINO_BIGDEC: {
                /* `str` strips the readable-form M suffix. */
                char fb[256];
                int fn2 = str_fmt_bigdec(a, fb, (int)sizeof(fb));
                if (fn2 > 0 && !str_buf_append(S, &buf, &len, &cap,
                                                fb, (size_t)fn2)) return NULL;
                n = 0;
                break;
            }
            case MINO_CHAR: {
                /* str of a char emits the codepoint's UTF-8 encoding. */
                unsigned cp = (unsigned)mino_val_char_get(a);
                if (cp <= 0x7F) {
                    tmp[0] = (char)cp; n = 1;
                } else if (cp <= 0x7FF) {
                    tmp[0] = (char)(0xC0 | (cp >> 6));
                    tmp[1] = (char)(0x80 | (cp & 0x3F));
                    n = 2;
                } else if (cp <= 0xFFFF) {
                    tmp[0] = (char)(0xE0 | (cp >> 12));
                    tmp[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
                    tmp[2] = (char)(0x80 | (cp & 0x3F));
                    n = 3;
                } else {
                    tmp[0] = (char)(0xF0 | (cp >> 18));
                    tmp[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
                    tmp[2] = (char)(0x80 | ((cp >> 6) & 0x3F));
                    tmp[3] = (char)(0x80 | (cp & 0x3F));
                    n = 4;
                }
                tmp[n] = '\0';
                break;
            }
            case MINO_FLOAT: {
                /* Route through the printer's Double.toString formatter:
                 * JVM str of a double matches pr-str of it, including
                 * the scientific-notation thresholds and exponent form. */
                if (isnan(a->as.f)) {
                    n = snprintf(tmp, sizeof(tmp), "NaN");
                } else if (isinf(a->as.f)) {
                    n = snprintf(tmp, sizeof(tmp), "%sInfinity",
                                 a->as.f > 0 ? "" : "-");
                } else {
                    n = print_float_to_buf(tmp, sizeof(tmp), a->as.f);
                }
                break;
            }
            case MINO_SYMBOL:
            case MINO_KEYWORD: {
                size_t slen = a->as.s.len;
                int    off  = mino_type_of(a) == MINO_KEYWORD ? 1 : 0;
                if (off + slen + 1 > sizeof(tmp)) slen = sizeof(tmp) - off - 1;
                if (off) tmp[0] = ':';
                memcpy(tmp + off, a->as.s.data, slen);
                n = (int)(off + slen);
                tmp[n] = '\0';
                break;
            }
            case MINO_UUID: {
                /* JVM Clojure's `(str uuid)` returns the bare 36-char
                 * canonical form (no #uuid prefix). Match that, so
                 * (java.util.UUID/fromString (str u)) round-trips. */
                const unsigned char *b = a->as.uuid.bytes;
                n = snprintf(tmp, sizeof(tmp),
                    "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-"
                    "%02x%02x%02x%02x%02x%02x",
                    b[0],  b[1],  b[2],  b[3],  b[4],  b[5],  b[6],  b[7],
                    b[8],  b[9],  b[10], b[11], b[12], b[13], b[14], b[15]);
                break;
            }
            default: {
                /* Collections (vector, map, set, cons, lazy, atom) and
                 * opaque types: print via the standard printer so str
                 * produces readable output, not #<?>. */
                mino_val *printed = print_to_string(S, a);
                if (printed != NULL && mino_type_of(printed) == MINO_STRING) {
                    if (!str_buf_append(S, &buf, &len, &cap,
                                        printed->as.s.data,
                                        printed->as.s.len)) return NULL;
                    n = 0; /* already appended */
                } else {
                    n = snprintf(tmp, sizeof(tmp), "#<%s>",
                                 mino_type_of(a) == MINO_PRIM ? "prim" :
                                 mino_type_of(a) == MINO_FN   ? "fn" :
                                 mino_type_of(a) == MINO_MACRO ? "macro" :
                                 mino_type_of(a) == MINO_HANDLE ? "handle" : "?");
                }
                break;
            }
            }
            if (n > 0 && !str_buf_append(S, &buf, &len, &cap,
                                          tmp, (size_t)n)) return NULL;
        }
        args = args->as.cons.cdr;
    }
    {
        mino_val *result = mino_string_n(S, buf != NULL ? buf : "", len);
        free(buf);
        return result;
    }
}

/* mino_uuid_from_bytes -- copy 16 bytes into a freshly-allocated
 * MINO_UUID. */
mino_val *mino_uuid_from_bytes(mino_state *S, const unsigned char *b)
{
    mino_val *v = alloc_val(S, MINO_UUID);
    if (v == NULL) return NULL;
    memcpy(v->as.uuid.bytes, b, 16);
    return v;
}

/* mino_uuid_parse -- parse the canonical 8-4-4-4-12 hex form into
 * `out_bytes` (16 bytes). Returns 1 on success, 0 if the input is
 * malformed. Accepts upper-case and lower-case hex. */
static int hex_nibble(int c, unsigned *out)
{
    if (c >= '0' && c <= '9') { *out = (unsigned)(c - '0');      return 1; }
    if (c >= 'a' && c <= 'f') { *out = (unsigned)(c - 'a' + 10); return 1; }
    if (c >= 'A' && c <= 'F') { *out = (unsigned)(c - 'A' + 10); return 1; }
    return 0;
}
int mino_uuid_parse(const char *s, size_t len, unsigned char out[16])
{
    /* Layout: 8-4-4-4-12 with `-` at indices 8, 13, 18, 23. */
    static const int dashes[4] = {8, 13, 18, 23};
    size_t i;
    int    di = 0;
    int    bi = 0;
    if (len != 36) return 0;
    for (i = 0; i < 36; i++) {
        if (di < 4 && (int)i == dashes[di]) {
            if (s[i] != '-') return 0;
            di++;
        } else {
            unsigned hi, lo;
            if (!hex_nibble((unsigned char)s[i], &hi)) return 0;
            i++;
            if (i >= 36) return 0;
            if (!hex_nibble((unsigned char)s[i], &lo)) return 0;
            out[bi++] = (unsigned char)((hi << 4) | lo);
        }
    }
    return bi == 16;
}

/* (random-uuid) — generate a UUID v4. */
mino_val *prim_random_uuid(mino_state *S, mino_val *args,
                             mino_env *env)
{
    unsigned char bytes[16];
    int i;
    (void)env;
    if (mino_is_cons(args)) {
        return prim_throw_classified(S, "eval/arity", "MAR001",
            "random-uuid takes no arguments");
    }
    for (i = 0; i < 16; i += 8) {
        uint64_t r = state_rand64(S);
        bytes[i    ] = (unsigned char)(r      );
        bytes[i + 1] = (unsigned char)(r >>  8);
        bytes[i + 2] = (unsigned char)(r >> 16);
        bytes[i + 3] = (unsigned char)(r >> 24);
        bytes[i + 4] = (unsigned char)(r >> 32);
        bytes[i + 5] = (unsigned char)(r >> 40);
        bytes[i + 6] = (unsigned char)(r >> 48);
        bytes[i + 7] = (unsigned char)(r >> 56);
    }
    bytes[6] = (unsigned char)((bytes[6] & 0x0F) | 0x40); /* version 4 */
    bytes[8] = (unsigned char)((bytes[8] & 0x3F) | 0x80); /* variant 1 */
    return mino_uuid_from_bytes(S, bytes);
}

/* (parse-uuid s) — parse a canonical UUID string into a UUID value.
 * Strict canonical form (36 chars, dashes at 8/13/18/23). Throws on
 * non-string input; returns nil for strings that fail the strict
 * form. */
mino_val *prim_parse_uuid(mino_state *S, mino_val *args, mino_env *env)
{
    mino_val *s;
    unsigned char bytes[16];
    (void)env;
    if (!mino_is_cons(args) || mino_is_cons(args->as.cons.cdr)) {
        return prim_throw_classified(S, "eval/arity", "MAR001",
            "parse-uuid requires one argument");
    }
    s = args->as.cons.car;
    if (s == NULL || mino_type_of(s) != MINO_STRING) {
        return prim_throw_classified(S, "eval/type", "MTY001",
            "parse-uuid: argument must be a string");
    }
    if (!mino_uuid_parse(s->as.s.data, s->as.s.len, bytes)) {
        return mino_nil(S);
    }
    return mino_uuid_from_bytes(S, bytes);
}

/* Core string operations live in clojure.core: the always-available
 * conversion and formatting primitives that have no Clojure-side
 * namespace either (str, format, name, namespace, subs, ...). */
const mino_prim_def k_prims_string[] = {
    {"str",          prim_str,
     "Returns the string representation of the arguments concatenated."},
    {"pr-str",       prim_pr_str,
     "Returns a readable string representation of the arguments."},
    {"read-string",  prim_read_string,
     "Reads one form from the string."},
    {"format",       prim_format,
     "Returns a formatted string using a format specifier and arguments."},
    {"char-at",      prim_char_at,
     "Returns the character at the given index as a string."},
    {"subs",         prim_subs,
     "Returns a substring from start (inclusive) to end (exclusive)."},
    {"parse-uuid",   prim_parse_uuid,
     "Parses s as a UUID; returns a UUID value or nil if s is not a "
     "valid canonical UUID string."},
    {"random-uuid",  prim_random_uuid,
     "Returns a random UUID v4 string."},
};

const size_t k_prims_string_count =
    sizeof(k_prims_string) / sizeof(k_prims_string[0]);

static mino_val *prim_str_replace(mino_state *S, mino_val *args,
                             mino_env *env)
{
    return str_replace_impl(S, args, env, 0);
}

static mino_val *prim_str_replace_first(mino_state *S, mino_val *args,
                                   mino_env *env)
{
    return str_replace_impl(S, args, env, 1);
}

/* Operations that match Clojure's clojure.string namespace. mino
 * installs these into clojure.string directly so user code that
 * refers them in via :require works the way Clojure programmers
 * expect, and so :refer-clojure :exclude doesn't accidentally
 * shadow them in a fresh namespace. */
const mino_prim_def k_prims_clojure_string[] = {
    {"split",        prim_split,
     "Splits a string on a regex pattern."},
    {"join",         prim_join,
     "Returns a string of the items in coll joined by separator."},
    {"replace",      prim_str_replace,
     "Replaces all occurrences of match in s with replacement."},
    {"replace-first", prim_str_replace_first,
     "Replaces the first occurrence of match in s with replacement."},
    {"starts-with?", prim_starts_with_p,
     "Returns true if the string starts with the given prefix."},
    {"ends-with?",   prim_ends_with_p,
     "Returns true if the string ends with the given suffix."},
    {"includes?",    prim_includes_p,
     "Returns true if the string contains the given substring."},
    {"upper-case",   prim_upper_case,
     "Returns the string converted to upper case."},
    {"lower-case",   prim_lower_case,
     "Returns the string converted to lower case."},
    {"trim",         prim_trim,
     "Returns the string with leading and trailing whitespace removed."},
};

const size_t k_prims_clojure_string_count =
    sizeof(k_prims_clojure_string) / sizeof(k_prims_clojure_string[0]);
