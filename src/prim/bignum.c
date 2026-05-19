/*
 * bignum.c -- arbitrary-precision integer support.
 *
 * Backed by vendored imath (src/vendor/imath/imath.c). The mpz_t
 * struct for each MINO_BIGINT value is malloc-owned and referenced
 * through the opaque `v->as.bigint.mpz` pointer. The cell's digit
 * storage (managed by imath internally) is freed by mino_bigint_free,
 * which the minor and major sweep loops call when a bigint cell
 * becomes unreachable.
 *
 * Constructors return GC-owned cells. The cell's mpz_t is allocated
 * and initialised BEFORE the mino_val_t so that, once alloc_val
 * returns, the pointer store is unconditional and no partially-typed
 * cell can be observed by a subsequent collector.
 */

#include "prim/internal.h"
#include "mino.h"
#include "imath.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Shortest-decimal-double printer. Iterates precision 1..17 with
 * %.Xg, parses the result back via strtod, and accepts the first
 * precision whose round-trip is bit-identical to the input. Buffer
 * must hold at least 32 bytes. Writes the canonical "NaN" /
 * "Infinity" / "-Infinity" tokens for non-finite inputs. */
int mino_double_shortest(double d, char *out, size_t outsz)
{
    int prec;
    double pinf = (double)(1.0 / 0.0);
    if (d != d)     return snprintf(out, outsz, "NaN");
    if (d == pinf)  return snprintf(out, outsz, "Infinity");
    if (d == -pinf) return snprintf(out, outsz, "-Infinity");
    for (prec = 1; prec <= 17; prec++) {
        char    buf[64];
        char   *end;
        double  parsed;
        snprintf(buf, sizeof(buf), "%.*g", prec, d);
        parsed = strtod(buf, &end);
        if (memcmp(&parsed, &d, sizeof(double)) == 0) {
            return snprintf(out, outsz, "%s", buf);
        }
    }
    return snprintf(out, outsz, "%.17g", d);
}

/* ------------------------------------------------------------------------- */
/* Internal helpers                                                          */
/* ------------------------------------------------------------------------- */

mp_int bigint_alloc_zeroed(void)
{
    mp_int z = (mp_int)malloc(sizeof(mpz_t));
    if (z == NULL) return NULL;
    if (mp_int_init(z) != MP_OK) {
        free(z);
        return NULL;
    }
    return z;
}

mino_val_t *bigint_wrap(mino_state_t *S, mp_int z)
{
    mino_val_t *v = alloc_val(S, MINO_BIGINT);
    v->as.bigint.mpz = z;
    return v;
}

/* Public: free the mpz_t owned by a MINO_BIGINT cell. Called from the
 * GC sweep paths. Safe to call with NULL mpz (defensive). */
void mino_bigint_free(mino_val_t *v)
{
    mp_int z;
    if (v == NULL || mino_type_of(v) != MINO_BIGINT) return;
    z = (mp_int)v->as.bigint.mpz;
    if (z == NULL) return;
    mp_int_clear(z);
    free(z);
    v->as.bigint.mpz = NULL;
}

/* ------------------------------------------------------------------------- */
/* Public constructors (mino.h)                                              */
/* ------------------------------------------------------------------------- */

mino_val_t *mino_bigint_from_ll(mino_state_t *S, long long n)
{
    mp_int z = bigint_alloc_zeroed();
    if (z == NULL) {
        return prim_throw_classified(S, "eval/out-of-memory", "MOM001",
                                     "out of memory building bigint");
    }
    /* mp_small is `long` in imath. On LP64 this is 64-bit so direct
     * assignment is safe; we guard by splitting via magnitude so the
     * ILP32 path (where long is 32-bit) still produces correct values. */
    if (n >= LONG_MIN && n <= LONG_MAX) {
        if (mp_int_set_value(z, (mp_small)n) != MP_OK) {
            mp_int_clear(z); free(z);
            return prim_throw_classified(S, "eval/out-of-memory", "MOM001",
                                         "out of memory building bigint");
        }
    } else {
        /* Unlikely on LP64; fall back to string conversion. */
        char   buf[32];
        mp_result rr;
        snprintf(buf, sizeof(buf), "%lld", n);
        rr = mp_int_read_string(z, 10, buf);
        if (rr != MP_OK) {
            mp_int_clear(z); free(z);
            return prim_throw_classified(S, "eval/out-of-memory", "MOM001",
                                         "out of memory building bigint");
        }
    }
    return bigint_wrap(S, z);
}

mino_val_t *mino_bigint_from_string_n(mino_state_t *S, const char *s, size_t len)
{
    mp_int z;
    char  *buf;
    mp_result rr;
    if (s == NULL || len == 0) return NULL;
    buf = (char *)malloc(len + 1);
    if (buf == NULL) {
        return prim_throw_classified(S, "eval/out-of-memory", "MOM001",
                                     "out of memory building bigint");
    }
    memcpy(buf, s, len);
    buf[len] = '\0';
    z = bigint_alloc_zeroed();
    if (z == NULL) {
        free(buf);
        return prim_throw_classified(S, "eval/out-of-memory", "MOM001",
                                     "out of memory building bigint");
    }
    rr = mp_int_read_string(z, 10, buf);
    free(buf);
    if (rr != MP_OK) {
        mp_int_clear(z); free(z);
        return NULL;
    }
    return bigint_wrap(S, z);
}

mino_val_t *mino_bigint_from_string(mino_state_t *S, const char *s)
{
    if (s == NULL) return NULL;
    return mino_bigint_from_string_n(S, s, strlen(s));
}

/* ------------------------------------------------------------------------- */
/* Equality, hashing, comparison, printing                                    */
/* ------------------------------------------------------------------------- */

int mino_bigint_equals(const mino_val_t *a, const mino_val_t *b)
{
    if (a == NULL || b == NULL) return 0;
    if (mino_type_of(a) != MINO_BIGINT || mino_type_of(b) != MINO_BIGINT) return 0;
    return mp_int_compare((mp_int)a->as.bigint.mpz,
                          (mp_int)b->as.bigint.mpz) == 0;
}

int mino_bigint_equals_ll(const mino_val_t *a, long long n)
{
    if (a == NULL || mino_type_of(a) != MINO_BIGINT) return 0;
    if (n >= LONG_MIN && n <= LONG_MAX) {
        return mp_int_compare_value((mp_int)a->as.bigint.mpz,
                                    (mp_small)n) == 0;
    }
    {
        /* Construct a temporary bigint for comparison. */
        mpz_t t;
        char  buf[32];
        int   r;
        if (mp_int_init(&t) != MP_OK) return 0;
        snprintf(buf, sizeof(buf), "%lld", n);
        if (mp_int_read_string(&t, 10, buf) != MP_OK) {
            mp_int_clear(&t);
            return 0;
        }
        r = mp_int_compare((mp_int)a->as.bigint.mpz, &t);
        mp_int_clear(&t);
        return r == 0;
    }
}

int mino_bigint_cmp(const mino_val_t *a, const mino_val_t *b)
{
    if (a == NULL || b == NULL) return 0;
    if (mino_type_of(a) != MINO_BIGINT || mino_type_of(b) != MINO_BIGINT) return 0;
    return mp_int_compare((mp_int)a->as.bigint.mpz,
                          (mp_int)b->as.bigint.mpz);
}

/* Upper-magnitude bigint hash: reduce modulo (2^31 - 1) using imath,
 * then fold via the same FNV-style mixer used elsewhere. The fits-in-
 * long-long path is short-circuited in hash_val before reaching here
 * so that (= 1 1N) ⇒ matching hashes via hash_long_long_bytes; this
 * function only fires when the bigint is genuinely larger than ll. */
uint32_t mino_bigint_hash(const mino_val_t *v)
{
    mp_small r = 0;
    uint32_t h;
    if (v == NULL || mino_type_of(v) != MINO_BIGINT) return 0;
    /* Use a prime that fits mp_small comfortably. */
    mp_int_div_value((mp_int)v->as.bigint.mpz, (mp_small)2147483647L, NULL, &r);
    h = (uint32_t)(r < 0 ? -r : r);
    /* Fold the sign so that n and -n hash differently. */
    if (mp_int_compare_zero((mp_int)v->as.bigint.mpz) < 0) {
        h = ~h;
    }
    /* Tag with MINO_BIGINT to stay distinct from int64/float hashes. */
    return h ^ 0x9e3779b9u;
}

/* Try to extract the value of v as a long long. Returns 1 if v is a
 * MINO_INT or a MINO_BIGINT whose magnitude fits in long long, with
 * *out set to the value. Returns 0 otherwise (including nil / non-
 * numeric types); *out is left unchanged on failure. */
int mino_as_ll(const mino_val_t *v, long long *out)
{
    if (v == NULL || out == NULL) return 0;
    if (mino_val_int_p(v)) {
        *out = mino_val_int_get(v);
        return 1;
    }
    if (mino_type_of(v) == MINO_BIGINT && v->as.bigint.mpz != NULL) {
        mp_int    z = (mp_int)v->as.bigint.mpz;
        mp_small  s;
        if (mp_int_to_int(z, &s) != MP_OK) return 0;
        /* mp_small is long; long and long long coincide on LP64 but we
         * stay conservative in case of ILP32. */
        *out = (long long)s;
        return 1;
    }
    return 0;
}

/* Serialise a bigint to a NUL-terminated base-10 C string. The caller
 * takes ownership of the returned buffer and must free() it. Returns
 * NULL on allocation failure or if v is not a bigint. Used by clone.c
 * for cross-state transfer without sharing imath allocations. */
char *mino_bigint_to_cstr(const mino_val_t *v)
{
    mp_result lenr;
    int  len;
    char *buf;
    if (v == NULL || mino_type_of(v) != MINO_BIGINT || v->as.bigint.mpz == NULL) {
        return NULL;
    }
    lenr = mp_int_string_len((mp_int)v->as.bigint.mpz, 10);
    if (lenr < 1) return NULL;
    len = (int)lenr;
    buf = (char *)malloc((size_t)len + 1);
    if (buf == NULL) return NULL;
    if (mp_int_to_string((mp_int)v->as.bigint.mpz, 10, buf, len) != MP_OK) {
        free(buf);
        return NULL;
    }
    return buf;
}

void mino_bigint_print(mino_state_t *S, const mino_val_t *v, FILE *out)
{
    mp_int z;
    mp_result lenr;
    int  len;
    char *buf;
    (void)S;
    if (v == NULL || mino_type_of(v) != MINO_BIGINT || v->as.bigint.mpz == NULL) {
        fputs("0N", out);
        return;
    }
    z = (mp_int)v->as.bigint.mpz;
    lenr = mp_int_string_len(z, 10);
    if (lenr < 1) { fputs("0N", out); return; }
    len = (int)lenr;
    buf = (char *)malloc((size_t)len + 1);
    if (buf == NULL) { fputs("0N", out); return; }
    if (mp_int_to_string(z, 10, buf, len) != MP_OK) {
        free(buf);
        fputs("0N", out);
        return;
    }
    fputs(buf, out);
    fputc('N', out);
    free(buf);
}

/* ------------------------------------------------------------------------- */
/* User-facing primitives                                                     */
/* ------------------------------------------------------------------------- */

/* (bigint x) -- coerce int, bigint, or numeric string to a bigint. */
mino_val_t *prim_bigint(mino_state_t *S, mino_val_t *args, mino_env_t *env)
{
    mino_val_t *x;
    (void)env;
    if (!mino_is_cons(args) || mino_is_cons(args->as.cons.cdr)) {
        return prim_throw_classified(S, "eval/arity", "MAR001",
                                     "bigint requires one argument");
    }
    x = args->as.cons.car;
    if (x == NULL) {
        return prim_throw_classified(S, "eval/type", "MTY001",
                                     "bigint argument is nil");
    }
    switch (mino_type_of(x)) {
    case MINO_INT:
        return mino_bigint_from_ll(S, mino_val_int_get(x));
    case MINO_BIGINT: {
        /* Copy into a fresh bigint. */
        mp_int z = bigint_alloc_zeroed();
        if (z == NULL) {
            return prim_throw_classified(S, "eval/out-of-memory", "MOM001",
                                         "out of memory building bigint");
        }
        if (mp_int_copy((mp_int)x->as.bigint.mpz, z) != MP_OK) {
            mp_int_clear(z); free(z);
            return prim_throw_classified(S, "eval/out-of-memory", "MOM001",
                                         "out of memory building bigint");
        }
        return bigint_wrap(S, z);
    }
    case MINO_FLOAT: {
        /* JVM Clojure's (bigint d) goes through BigDecimal.valueOf(d),
         * which uses Double.toString -- the shortest decimal that
         * round-trips. We do the same so (bigint 1.79e308) yields the
         * full 309-digit integer rather than saturating through long. */
        double      f = x->as.f;
        char        buf[64];
        mino_val_t *bd, *unscaled;
        int         scale;
        if (f != f || f == (double)(1.0 / 0.0) || f == -(double)(1.0 / 0.0)) {
            return prim_throw_classified(S, "eval/type", "MTY001",
                                         "cannot convert non-finite double to bigint");
        }
        mino_double_shortest(f, buf, sizeof(buf));
        bd = mino_bigdec_from_string(S, buf);
        if (bd == NULL) {
            return prim_throw_classified(S, "eval/type", "MTY001",
                                         "bigint: failed to convert double");
        }
        unscaled = bd->as.bigdec.unscaled;
        scale    = bd->as.bigdec.scale;
        if (scale <= 0) {
            /* mino_bigdec_from_string already multiplied unscaled by
             * 10^|scale| when scale was negative, leaving scale=0. */
            return unscaled;
        }
        /* scale > 0: truncate fractional part via integer division. */
        {
            mpz_t       pw;
            mino_val_t *out;
            if (mp_int_init(&pw) != MP_OK) {
                return prim_throw_classified(S, "eval/out-of-memory",
                                             "MOM001", "out of memory");
            }
            if (mp_int_set_value(&pw, 10) != MP_OK
                || mp_int_expt(&pw, scale, &pw) != MP_OK) {
                mp_int_clear(&pw);
                return prim_throw_classified(S, "eval/out-of-memory",
                                             "MOM001", "out of memory");
            }
            out = mino_bigint_from_ll(S, 0);
            if (out == NULL) { mp_int_clear(&pw); return NULL; }
            if (mp_int_div((mp_int)unscaled->as.bigint.mpz, &pw,
                           (mp_int)out->as.bigint.mpz, NULL) != MP_OK) {
                mp_int_clear(&pw);
                return prim_throw_classified(S, "eval/out-of-memory",
                                             "MOM001", "out of memory");
            }
            mp_int_clear(&pw);
            return out;
        }
    }
    case MINO_STRING: {
        mino_val_t *r = mino_bigint_from_string_n(S, x->as.s.data, x->as.s.len);
        if (r == NULL) {
            return prim_throw_classified(S, "eval/type", "MTY001",
                                         "bigint: invalid numeric string");
        }
        return r;
    }
    default:
        return prim_throw_classified(S, "eval/type", "MTY001",
                                     "bigint: argument must be integer, "
                                     "float, string, or bigint");
    }
}

/* (biginteger x) -- alias for (bigint x). In Clojure both exist and
 * return the same underlying arbitrary-precision representation. */
mino_val_t *prim_biginteger(mino_state_t *S, mino_val_t *args, mino_env_t *env)
{
    return prim_bigint(S, args, env);
}

/* (bigint? x) */
mino_val_t *prim_bigint_p(mino_state_t *S, mino_val_t *args, mino_env_t *env)
{
    (void)env;
    if (!mino_is_cons(args) || mino_is_cons(args->as.cons.cdr)) {
        return prim_throw_classified(S, "eval/arity", "MAR001",
                                     "bigint? requires one argument");
    }
    {
        mino_val_t *x = args->as.cons.car;
        return (x != NULL && mino_type_of(x) == MINO_BIGINT)
            ? mino_true(S) : mino_false(S);
    }
}

/* ------------------------------------------------------------------------- */
/* Arithmetic helpers used by the promoting tower primitives (+' -' *')      */
/*                                                                           */
/* Each helper takes values that are either MINO_INT or MINO_BIGINT (the     */
/* caller is expected to classify operands before calling). Results are      */
/* always GC-owned MINO_BIGINT cells — callers that want the "fits-in-long"  */
/* narrowing are responsible for it (Clojure doesn't narrow +' output back   */
/* to long either, so returning a bigint is the expected shape).             */
/*                                                                           */
/* On allocation or imath failure these return NULL after raising an         */
/* out-of-memory error through prim_throw_classified.                        */
/* ------------------------------------------------------------------------- */

/* Borrowed view into a value as a bigint. If the value is already a bigint
 * the existing mp_int is returned. If it's a long, a scratch mp_int is
 * initialised in *scratch and returned. Caller tracks ownership via
 * `owns_scratch` and must mp_int_clear / nothing based on it. */
static int bigint_view(const mino_val_t *v, mpz_t *scratch, mp_int *out,
                       int *owns_scratch)
{
    if (v == NULL) return 0;
    if (mino_type_of(v) == MINO_BIGINT && v->as.bigint.mpz != NULL) {
        *out = (mp_int)v->as.bigint.mpz;
        *owns_scratch = 0;
        return 1;
    }
    if (mino_val_int_p(v)) {
        if (mp_int_init(scratch) != MP_OK) return 0;
        if (mino_val_int_get(v) >= LONG_MIN && mino_val_int_get(v) <= LONG_MAX) {
            if (mp_int_set_value(scratch, (mp_small)mino_val_int_get(v)) != MP_OK) {
                mp_int_clear(scratch);
                return 0;
            }
        } else {
            char buf[32];
            snprintf(buf, sizeof(buf), "%lld", mino_val_int_get(v));
            if (mp_int_read_string(scratch, 10, buf) != MP_OK) {
                mp_int_clear(scratch);
                return 0;
            }
        }
        *out = scratch;
        *owns_scratch = 1;
        return 1;
    }
    return 0;
}

typedef mp_result (*bigint_binop_fn)(mp_int, mp_int, mp_int);

static mino_val_t *bigint_binop(mino_state_t *S, const mino_val_t *a,
                                const mino_val_t *b, bigint_binop_fn op,
                                const char *opname)
{
    mpz_t  as_buf, bs_buf;
    mp_int av = NULL, bv = NULL;
    int    owns_a = 0, owns_b = 0;
    mp_int rz;
    mino_val_t *out;
    (void)opname;
    if (!bigint_view(a, &as_buf, &av, &owns_a) ||
        !bigint_view(b, &bs_buf, &bv, &owns_b)) {
        if (owns_a) mp_int_clear(&as_buf);
        return prim_throw_classified(S, "eval/out-of-memory", "MOM001",
                                     "out of memory in bigint arithmetic");
    }
    rz = bigint_alloc_zeroed();
    if (rz == NULL) {
        if (owns_a) mp_int_clear(&as_buf);
        if (owns_b) mp_int_clear(&bs_buf);
        return prim_throw_classified(S, "eval/out-of-memory", "MOM001",
                                     "out of memory in bigint arithmetic");
    }
    if (op(av, bv, rz) != MP_OK) {
        if (owns_a) mp_int_clear(&as_buf);
        if (owns_b) mp_int_clear(&bs_buf);
        mp_int_clear(rz); free(rz);
        return prim_throw_classified(S, "eval/out-of-memory", "MOM001",
                                     "out of memory in bigint arithmetic");
    }
    if (owns_a) mp_int_clear(&as_buf);
    if (owns_b) mp_int_clear(&bs_buf);
    out = bigint_wrap(S, rz);
    return out;
}

mino_val_t *mino_bigint_add(mino_state_t *S, const mino_val_t *a,
                            const mino_val_t *b)
{
    return bigint_binop(S, a, b, mp_int_add, "+'");
}

mino_val_t *mino_bigint_sub(mino_state_t *S, const mino_val_t *a,
                            const mino_val_t *b)
{
    return bigint_binop(S, a, b, mp_int_sub, "-'");
}

mino_val_t *mino_bigint_mul(mino_state_t *S, const mino_val_t *a,
                            const mino_val_t *b)
{
    return bigint_binop(S, a, b, mp_int_mul, "*'");
}

/* mino_bigint_quotrem -- truncated integer division. Sign of remainder
 * matches the sign of the dividend (`a`); sign of quotient is the
 * usual product-of-signs. Either q_out or r_out may be NULL when only
 * one half is wanted. Returns -1 on b == 0 (with diag set) or
 * allocation failure; 0 on success. */
int mino_bigint_quotrem(mino_state_t *S, const mino_val_t *a,
                        const mino_val_t *b, mino_val_t **q_out,
                        mino_val_t **r_out)
{
    mpz_t  as_buf, bs_buf;
    mp_int av = NULL, bv = NULL;
    int    owns_a = 0, owns_b = 0;
    mp_int qz = NULL, rz = NULL;
    if (q_out) *q_out = NULL;
    if (r_out) *r_out = NULL;
    if (!bigint_view(a, &as_buf, &av, &owns_a) ||
        !bigint_view(b, &bs_buf, &bv, &owns_b)) {
        if (owns_a) mp_int_clear(&as_buf);
        prim_throw_classified(S, "eval/out-of-memory", "MOM001",
                              "out of memory in bigint quotrem");
        return -1;
    }
    if (mp_int_compare_zero(bv) == 0) {
        if (owns_a) mp_int_clear(&as_buf);
        if (owns_b) mp_int_clear(&bs_buf);
        prim_throw_classified(S, "eval/type", "MTY001", "division by zero");
        return -1;
    }
    if (q_out) {
        qz = bigint_alloc_zeroed();
        if (qz == NULL) goto oom;
    }
    if (r_out) {
        rz = bigint_alloc_zeroed();
        if (rz == NULL) goto oom;
    }
    if (mp_int_div(av, bv, qz, rz) != MP_OK) {
        goto oom;
    }
    if (owns_a) mp_int_clear(&as_buf);
    if (owns_b) mp_int_clear(&bs_buf);
    if (q_out) *q_out = bigint_wrap(S, qz);
    if (r_out) *r_out = bigint_wrap(S, rz);
    return 0;
oom:
    if (owns_a) mp_int_clear(&as_buf);
    if (owns_b) mp_int_clear(&bs_buf);
    if (qz != NULL) { mp_int_clear(qz); free(qz); }
    if (rz != NULL) { mp_int_clear(rz); free(rz); }
    prim_throw_classified(S, "eval/out-of-memory", "MOM001",
                          "out of memory in bigint quotrem");
    return -1;
}

/* mino_bigint_quot / _rem -- thin wrappers, return only one half. */
mino_val_t *mino_bigint_quot(mino_state_t *S, const mino_val_t *a,
                             const mino_val_t *b)
{
    mino_val_t *q = NULL;
    if (mino_bigint_quotrem(S, a, b, &q, NULL) != 0) return NULL;
    return q;
}

mino_val_t *mino_bigint_rem(mino_state_t *S, const mino_val_t *a,
                            const mino_val_t *b)
{
    mino_val_t *r = NULL;
    if (mino_bigint_quotrem(S, a, b, NULL, &r) != 0) return NULL;
    return r;
}

/* mino_bigint_mod -- floored division remainder. Sign of result matches
 * the sign of the divisor (`b`). Computed as: r = trunc-rem; if r != 0
 * and (r and b have opposite signs) then r += b. */
mino_val_t *mino_bigint_mod(mino_state_t *S, const mino_val_t *a,
                            const mino_val_t *b)
{
    mino_val_t *r = NULL;
    int sr, sb;
    if (mino_bigint_quotrem(S, a, b, NULL, &r) != 0) return NULL;
    sr = mp_int_compare_zero((mp_int)r->as.bigint.mpz);
    if (mino_val_int_p(b)) {
        sb = (mino_val_int_get(b) > 0) - (mino_val_int_get(b) < 0);
    } else {
        sb = mp_int_compare_zero((mp_int)b->as.bigint.mpz);
    }
    if (sr != 0 && ((sr < 0) != (sb < 0))) {
        return mino_bigint_add(S, r, b);
    }
    return r;
}

mino_val_t *mino_bigint_neg(mino_state_t *S, const mino_val_t *a)
{
    mpz_t  as_buf;
    mp_int av = NULL;
    int    owns_a = 0;
    mp_int rz;
    if (!bigint_view(a, &as_buf, &av, &owns_a)) {
        return prim_throw_classified(S, "eval/out-of-memory", "MOM001",
                                     "out of memory in bigint arithmetic");
    }
    rz = bigint_alloc_zeroed();
    if (rz == NULL) {
        if (owns_a) mp_int_clear(&as_buf);
        return prim_throw_classified(S, "eval/out-of-memory", "MOM001",
                                     "out of memory in bigint arithmetic");
    }
    if (mp_int_neg(av, rz) != MP_OK) {
        if (owns_a) mp_int_clear(&as_buf);
        mp_int_clear(rz); free(rz);
        return prim_throw_classified(S, "eval/out-of-memory", "MOM001",
                                     "out of memory in bigint arithmetic");
    }
    if (owns_a) mp_int_clear(&as_buf);
    return bigint_wrap(S, rz);
}

/* Convert a bigint to the closest representable double. Used when an
 * arithmetic operand sequence mixes bigints with floats; the bigint tier
 * then collapses to the double tier for the remainder of the computation
 * (matching Clojure's tower fallback). */
double mino_bigint_to_double(const mino_val_t *v)
{
    mp_result lenr;
    int  len;
    char *buf;
    double d;
    if (v == NULL || mino_type_of(v) != MINO_BIGINT || v->as.bigint.mpz == NULL) {
        return 0.0;
    }
    lenr = mp_int_string_len((mp_int)v->as.bigint.mpz, 10);
    if (lenr < 1) return 0.0;
    len = (int)lenr;
    buf = (char *)malloc((size_t)len + 1);
    if (buf == NULL) return 0.0;
    if (mp_int_to_string((mp_int)v->as.bigint.mpz, 10, buf, len) != MP_OK) {
        free(buf);
        return 0.0;
    }
    d = strtod(buf, NULL);
    free(buf);
    return d;
}


const mino_prim_def k_prims_bignum[] = {
    {"bigint",      prim_bigint,
     "Coerces a value to an arbitrary-precision integer. Accepts int, "
     "bigint, float (truncated toward zero), or a base-10 string."},
    {"biginteger",  prim_biginteger,
     "Alias of bigint. Coerces a value to an arbitrary-precision integer."},
    {"bigint?",     prim_bigint_p,
     "Returns true if x is an arbitrary-precision integer."},
    {"numerator",   prim_numerator,
     "Returns the numerator of a rational number."},
    {"denominator", prim_denominator,
     "Returns the denominator of a rational number."},
    {"ratio?",      prim_ratio_p,
     "Returns true if x is a ratio."},
    {"rational?",   prim_rational_p,
     "Returns true if x is a rational number (int, bigint, or ratio)."},
    {"rationalize", prim_rationalize,
     "Returns the rational value nearest to the argument."},
    {"bigdec",      prim_bigdec,
     "Coerces a value to an arbitrary-precision decimal."},
    {"decimal?",    prim_decimal_p,
     "Returns true if x is an arbitrary-precision decimal."},
};

const size_t k_prims_bignum_count =
    sizeof(k_prims_bignum) / sizeof(k_prims_bignum[0]);

void mino_install_bignum(mino_state_t *S, mino_env_t *env)
{
    mino_env_t *core_env = ns_env_ensure(S, "clojure.core");
    (void)env;
    prim_install_table_with_capability(S, core_env, "clojure.core",
                                       k_prims_bignum, k_prims_bignum_count,
                                       "bignum");
    S->caps_installed |= MINO_CAP_BIGNUM;
}
