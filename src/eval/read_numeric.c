/*
 * read_numeric.c -- numeric literal parser for the mino reader.
 *
 * Handles all Clojure numeric literal forms:
 *   integer (decimal, octal 0-prefix, hex 0x-prefix, arbitrary radix NrDIGITS)
 *   floating-point (with . or e/E exponent)
 *   ratio (digits/digits)
 *   bigint (N suffix: 42N)
 *   bigdec (M suffix: 1.5M)
 *
 * Extracted from read.c to keep translation-unit sizes within the
 * 1100-line style limit.  Entry point: try_parse_numeric.
 */

#include "eval/read_internal.h"
#include <string.h>

mino_val *try_parse_numeric(mino_state *S, const char *start,
                             size_t len, int *err)
{
    char        stack_buf[64];
    char       *buf            = stack_buf;
    char       *heap_buf       = NULL;
    char       *endp           = NULL;
    size_t      scan_start     = 0;
    size_t      num_len        = len;
    int         has_dot_or_exp = 0;
    int         looks_numeric  = 1;
    mino_val *out            = NULL;
    size_t      i;

    *err = 0;
    /* Bigint / bigdec literals can run hundreds of digits long; fall
     * back to a heap buffer so the parser doesn't punt on them. */
    if (len + 1 > sizeof(stack_buf)) {
        heap_buf = (char *)malloc(len + 1);
        if (heap_buf == NULL) return NULL;
        buf = heap_buf;
    }
    memcpy(buf, start, len);
    buf[len] = '\0';
#define TRY_PARSE_RETURN(v) do { out = (v); goto try_parse_done; } while (0)

    /* Sign prefix. */
    if (buf[0] == '+' || buf[0] == '-') scan_start = 1;
    if (scan_start == len) looks_numeric = 0;

    /* Hex: 0x... or [+-]0x..., with an optional bigint N suffix.
     * Magnitudes past the long-long range promote to bigint (matching
     * the decimal path) instead of saturating at LLONG_MAX silently. */
    if (looks_numeric && scan_start + 2 < len
        && buf[scan_start] == '0'
        && (buf[scan_start + 1] == 'x' || buf[scan_start + 1] == 'X')) {
        size_t digits_start = scan_start + 2;
        size_t digits_end   = len;
        int    force_big    = 0;
        int    all_hex;
        if (buf[len - 1] == 'N' && len - 1 > digits_start) {
            force_big  = 1;
            digits_end = len - 1;
        }
        all_hex = (digits_end > digits_start);
        for (i = digits_start; i < digits_end; i++) {
            if (!isxdigit((unsigned char)buf[i])) { all_hex = 0; break; }
        }
        if (all_hex) {
            char      saved = buf[digits_end];
            long long n;
            buf[digits_end] = '\0';
            errno = 0;
            n = strtoll(buf, &endp, 16);
            buf[digits_end] = saved;
            /* musl's strtoll leaves endp short of the digit end on
             * overflow (glibc and macOS consume the full run), so
             * ERANGE alone must also qualify -- the run was verified
             * all-hex above. */
            if (endp == buf + digits_end || errno == ERANGE) {
                if (!force_big && errno != ERANGE)
                    TRY_PARSE_RETURN(mino_int_wrap(S, n));
                {
                    mino_val *bi = mino_bigint_from_digits_base(
                        S, buf + digits_start, digits_end - digits_start,
                        16, scan_start > 0 && buf[0] == '-');
                    if (bi != NULL) TRY_PARSE_RETURN(bi);
                }
            }
        }
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
                    /* Parse the digits after 'r' in the given base.
                     * `buf` is NUL-terminated, so the digit run can be
                     * scanned in place at any length. Out-of-range
                     * magnitudes promote to bigint rather than
                     * saturating at the long-long bound silently. */
                    const char *digits   = r_pos + 1;
                    size_t      dlen     = len - (size_t)(r_pos - buf) - 1;
                    int         sign_neg = (scan_start > 0 && buf[0] == '-');
                    long long   n;
                    errno = 0;
                    n = strtoll(digits, &endp, (int)base);
                    {
                        int radix_ok = (endp == digits + dlen);
                        if (!radix_ok && errno == ERANGE) {
                            /* musl stops endp early on overflow;
                             * verify the digit run in the base
                             * ourselves before promoting. */
                            size_t k;
                            radix_ok = dlen > 0;
                            for (k = 0; k < dlen; k++) {
                                int c = (unsigned char)digits[k];
                                int v = -1;
                                if (c >= '0' && c <= '9') v = c - '0';
                                else if (c >= 'a' && c <= 'z') v = c - 'a' + 10;
                                else if (c >= 'A' && c <= 'Z') v = c - 'A' + 10;
                                if (v < 0 || v >= (int)base) {
                                    radix_ok = 0;
                                    break;
                                }
                            }
                        }
                        if (!radix_ok) goto radix_done;
                    }
                    {
                        if (errno != ERANGE)
                            TRY_PARSE_RETURN(
                                mino_int_wrap(S, sign_neg ? -n : n));
                        {
                            mino_val *bi = mino_bigint_from_digits_base(
                                S, digits, dlen, (int)base, sign_neg);
                            if (bi != NULL) TRY_PARSE_RETURN(bi);
                        }
                    }
                radix_done: ;
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
                mino_val *num_bi = mino_bigint_from_string_n(
                    S, buf, num_str_len);
                mino_val *den_bi;
                if (num_bi == NULL) {
                    set_reader_diag(S, MRE008,
                                    "invalid ratio literal",
                                    S->reader.reader_line, S->reader.reader_col);
                    *err = 1;
                    goto try_parse_done;
                }
                den_bi = mino_bigint_from_string_n(
                    S, slash + 1, den_str_len);
                if (den_bi == NULL) {
                    set_reader_diag(S, MRE008,
                                    "invalid ratio literal",
                                    S->reader.reader_line, S->reader.reader_col);
                    *err = 1;
                    goto try_parse_done;
                }
                TRY_PARSE_RETURN(mino_ratio_make(S, num_bi, den_bi));
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
            mino_val *bi;
            /* Leading-zero magnitudes are octal, like the bare-int
             * path below: 010N is 8N. */
            if (buf[digit_start] == '0' && num_len > digit_start + 1) {
                int all_oct = 1;
                for (i = digit_start + 1; i < num_len; i++) {
                    if (buf[i] < '0' || buf[i] > '7') { all_oct = 0; break; }
                }
                if (all_oct) {
                    bi = mino_bigint_from_digits_base(
                        S, buf + digit_start + 1, num_len - digit_start - 1,
                        8, scan_start > 0 && buf[0] == '-');
                    if (bi != NULL) TRY_PARSE_RETURN(bi);
                }
            } else {
                buf[num_len] = '\0';
                bi = mino_bigint_from_string_n(S, buf, num_len);
                buf[num_len] = 'N';
                if (bi != NULL) TRY_PARSE_RETURN(bi);
            }
        }
        num_len = len;
    }

    /* Bigdec M suffix: 1.5M -> arbitrary-precision decimal. */
    if (looks_numeric && len > 1 && buf[len - 1] == 'M') {
        mino_val *bd;
        num_len = len - 1;
        buf[num_len] = '\0';
        bd = mino_bigdec_from_string(S, buf);
        buf[num_len] = 'M';
        num_len = len;
        if (bd != NULL) TRY_PARSE_RETURN(bd);
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
    /* Leading-zero integers are octal: 010 is 8, 0377 is 255. A non-
     * octal digit after the leading zero (08, 09) is malformed; let
     * the token fall through so the caller's digit-leading guard
     * raises invalid-number. Out-of-range magnitudes promote to
     * bigint like every other integer form. */
    if (looks_numeric && !has_dot_or_exp
        && buf[scan_start] == '0' && len > scan_start + 1) {
        const char *digits   = buf + scan_start + 1;
        size_t      dlen     = len - scan_start - 1;
        int         sign_neg = (scan_start > 0 && buf[0] == '-');
        int         all_oct  = 1;
        for (i = 0; i < dlen; i++) {
            if (digits[i] < '0' || digits[i] > '7') { all_oct = 0; break; }
        }
        if (all_oct) {
            long long n;
            errno = 0;
            n = strtoll(digits, &endp, 8);
            /* See the hex arm: musl stops endp early on overflow; the
             * run was verified all-octal above. */
            if (endp == digits + dlen || errno == ERANGE) {
                if (errno != ERANGE)
                    TRY_PARSE_RETURN(mino_int_wrap(S, sign_neg ? -n : n));
                {
                    mino_val *bi = mino_bigint_from_digits_base(
                        S, digits, dlen, 8, sign_neg);
                    if (bi != NULL) TRY_PARSE_RETURN(bi);
                }
            }
        }
        looks_numeric = 0;
    }
    if (looks_numeric) {
        if (has_dot_or_exp) {
            double d = strtod(buf, &endp);
            if (endp == buf + len)
                TRY_PARSE_RETURN(mino_float(S, d));
        } else {
            long long n;
            errno = 0;
            n = strtoll(buf, &endp, 10);
            {
                int dec_ok = (endp == buf + len);
                if (!dec_ok && errno == ERANGE) {
                    /* musl stops endp early on overflow; verify the
                     * digit run ourselves before promoting. */
                    size_t k;
                    dec_ok = len > scan_start;
                    for (k = scan_start; k < len; k++) {
                        if (!isdigit((unsigned char)buf[k])) {
                            dec_ok = 0;
                            break;
                        }
                    }
                }
                if (!dec_ok) goto try_parse_done;
            }
            {
                /* strtoll saturates at LLONG_MIN/LLONG_MAX with
                 * errno=ERANGE on overflow. Promote to bigint so
                 * literals like 9223372036854775808 don't silently
                 * collapse into the long range. */
                if (errno == ERANGE) {
                    mino_val *bi = mino_bigint_from_string_n(S, buf, len);
                    if (bi != NULL) TRY_PARSE_RETURN(bi);
                }
                TRY_PARSE_RETURN(mino_int_wrap(S, n));
            }
        }
    }
try_parse_done:
    if (heap_buf != NULL) free(heap_buf);
    return out;
#undef TRY_PARSE_RETURN
}
