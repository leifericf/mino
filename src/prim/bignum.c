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
#include "imath.h"

#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------------------- */
/* Internal helpers                                                          */
/* ------------------------------------------------------------------------- */

static mp_int bigint_alloc_zeroed(void)
{
    mp_int z = (mp_int)malloc(sizeof(mpz_t));
    if (z == NULL) return NULL;
    if (mp_int_init(z) != MP_OK) {
        free(z);
        return NULL;
    }
    return z;
}

static mino_val_t *bigint_wrap(mino_state_t *S, mp_int z)
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
    if (v == NULL || v->type != MINO_BIGINT) return;
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
    if (a->type != MINO_BIGINT || b->type != MINO_BIGINT) return 0;
    return mp_int_compare((mp_int)a->as.bigint.mpz,
                          (mp_int)b->as.bigint.mpz) == 0;
}

int mino_bigint_equals_ll(const mino_val_t *a, long long n)
{
    if (a == NULL || a->type != MINO_BIGINT) return 0;
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
    if (a->type != MINO_BIGINT || b->type != MINO_BIGINT) return 0;
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
    if (v == NULL || v->type != MINO_BIGINT) return 0;
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
    if (v->type == MINO_INT) {
        *out = v->as.i;
        return 1;
    }
    if (v->type == MINO_BIGINT && v->as.bigint.mpz != NULL) {
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
    if (v == NULL || v->type != MINO_BIGINT || v->as.bigint.mpz == NULL) {
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
    if (v == NULL || v->type != MINO_BIGINT || v->as.bigint.mpz == NULL) {
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
    switch (x->type) {
    case MINO_INT:
        return mino_bigint_from_ll(S, x->as.i);
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
        /* Truncate toward zero, matching Clojure's bigint on doubles. */
        double      f = x->as.f;
        long long   n;
        if (f != f || f == (double)(1.0 / 0.0) || f == -(double)(1.0 / 0.0)) {
            return prim_throw_classified(S, "eval/type", "MTY001",
                                         "cannot convert non-finite double to bigint");
        }
        if (f >= 0) f = f >= 0 ? (double)((long long)f) : f;
        else        f = -(double)((long long)(-f));
        n = (long long)f;
        return mino_bigint_from_ll(S, n);
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
        return (x != NULL && x->type == MINO_BIGINT)
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
    if (v->type == MINO_BIGINT && v->as.bigint.mpz != NULL) {
        *out = (mp_int)v->as.bigint.mpz;
        *owns_scratch = 0;
        return 1;
    }
    if (v->type == MINO_INT) {
        if (mp_int_init(scratch) != MP_OK) return 0;
        if (v->as.i >= LONG_MIN && v->as.i <= LONG_MAX) {
            if (mp_int_set_value(scratch, (mp_small)v->as.i) != MP_OK) {
                mp_int_clear(scratch);
                return 0;
            }
        } else {
            char buf[32];
            snprintf(buf, sizeof(buf), "%lld", v->as.i);
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
    if (v == NULL || v->type != MINO_BIGINT || v->as.bigint.mpz == NULL) {
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

/* ------------------------------------------------------------------------- */
/* MINO_RATIO and MINO_BIGDEC                                                */
/*                                                                           */
/* Both types are built on top of MINO_BIGINT cells; they hold pointers to   */
/* GC-allocated bigints and rely on the regular tracer to keep the children  */
/* alive. Ratios always live in canonical form (gcd-reduced, positive        */
/* denominator); the constructors enforce that and downgrade to int /        */
/* bigint when the denominator collapses to one.                             */
/* ------------------------------------------------------------------------- */

/* Coerce a value to a fresh GC-owned MINO_BIGINT. Accepts MINO_INT and
 * MINO_BIGINT; returns NULL with a thrown error on any other type. */
static mino_val_t *to_bigint(mino_state_t *S, const mino_val_t *v)
{
    if (v == NULL) return NULL;
    if (v->type == MINO_BIGINT) {
        mp_int z = bigint_alloc_zeroed();
        if (z == NULL) {
            return prim_throw_classified(S, "eval/out-of-memory", "MOM001",
                                         "out of memory");
        }
        if (mp_int_copy((mp_int)v->as.bigint.mpz, z) != MP_OK) {
            mp_int_clear(z); free(z);
            return prim_throw_classified(S, "eval/out-of-memory", "MOM001",
                                         "out of memory");
        }
        return bigint_wrap(S, z);
    }
    if (v->type == MINO_INT) {
        return mino_bigint_from_ll(S, v->as.i);
    }
    return NULL;
}

/* Try to narrow a bigint cell back to a long long. Returns 1 on success
 * with *out set; 0 if the bigint doesn't fit. */
static int bigint_fits_ll(const mino_val_t *v, long long *out)
{
    return mino_as_ll(v, out);
}

mino_val_t *mino_ratio_make_unchecked(mino_state_t *S, mino_val_t *num,
                                      mino_val_t *denom)
{
    mino_val_t *v;
    if (num == NULL || denom == NULL ||
        num->type != MINO_BIGINT || denom->type != MINO_BIGINT) {
        return prim_throw_classified(S, "internal", "MIN001",
                                     "ratio components must be bigints");
    }
    v = alloc_val(S, MINO_RATIO);
    v->as.ratio.num   = num;
    v->as.ratio.denom = denom;
    return v;
}

/* Build a canonical ratio from two arbitrary numeric (int or bigint)
 * values. Returns int / bigint when the result is integer, otherwise a
 * MINO_RATIO. Throws on division-by-zero. */
mino_val_t *mino_ratio_make(mino_state_t *S, mino_val_t *num,
                            mino_val_t *denom)
{
    mino_val_t *bnum, *bdenom;
    mp_int      g;
    mpz_t       g_buf;
    bnum   = to_bigint(S, num);
    if (bnum == NULL) {
        return prim_throw_classified(S, "eval/type", "MTY001",
                                     "ratio numerator must be an integer");
    }
    bdenom = to_bigint(S, denom);
    if (bdenom == NULL) {
        return prim_throw_classified(S, "eval/type", "MTY001",
                                     "ratio denominator must be an integer");
    }
    if (mp_int_compare_zero((mp_int)bdenom->as.bigint.mpz) == 0) {
        return prim_throw_classified(S, "eval/type", "MTY001",
                                     "ratio: division by zero");
    }
    /* Normalise sign: denom always positive. */
    if (mp_int_compare_zero((mp_int)bdenom->as.bigint.mpz) < 0) {
        mp_int_neg((mp_int)bnum->as.bigint.mpz,   (mp_int)bnum->as.bigint.mpz);
        mp_int_neg((mp_int)bdenom->as.bigint.mpz, (mp_int)bdenom->as.bigint.mpz);
    }
    /* Reduce by gcd. mp_int_gcd produces the unsigned gcd. */
    if (mp_int_init(&g_buf) != MP_OK) {
        return prim_throw_classified(S, "eval/out-of-memory", "MOM001",
                                     "out of memory");
    }
    g = &g_buf;
    if (mp_int_gcd((mp_int)bnum->as.bigint.mpz,
                   (mp_int)bdenom->as.bigint.mpz, g) != MP_OK) {
        mp_int_clear(g);
        return prim_throw_classified(S, "eval/out-of-memory", "MOM001",
                                     "out of memory in ratio reduce");
    }
    /* If gcd > 1, divide both num and denom by g. */
    if (mp_int_compare_value(g, 1) > 0) {
        mpz_t   r_buf;
        if (mp_int_init(&r_buf) != MP_OK) {
            mp_int_clear(g);
            return prim_throw_classified(S, "eval/out-of-memory", "MOM001",
                                         "out of memory");
        }
        mp_int_div((mp_int)bnum->as.bigint.mpz, g,
                   (mp_int)bnum->as.bigint.mpz, &r_buf);
        mp_int_div((mp_int)bdenom->as.bigint.mpz, g,
                   (mp_int)bdenom->as.bigint.mpz, &r_buf);
        mp_int_clear(&r_buf);
    }
    mp_int_clear(g);
    /* If denominator collapsed to 1, result is integer-valued. Narrow
     * to MINO_INT when it fits, else keep as MINO_BIGINT. */
    if (mp_int_compare_value((mp_int)bdenom->as.bigint.mpz, 1) == 0) {
        long long ll;
        if (bigint_fits_ll(bnum, &ll)) return mino_int(S, ll);
        return bnum;
    }
    return mino_ratio_make_unchecked(S, bnum, bdenom);
}

mino_val_t *mino_ratio_from_ll(mino_state_t *S, long long num, long long denom)
{
    mino_val_t *bn = mino_bigint_from_ll(S, num);
    mino_val_t *bd;
    if (bn == NULL) return NULL;
    bd = mino_bigint_from_ll(S, denom);
    if (bd == NULL) return NULL;
    return mino_ratio_make(S, bn, bd);
}

void mino_ratio_print(mino_state_t *S, const mino_val_t *v, FILE *out)
{
    if (v == NULL || v->type != MINO_RATIO) {
        fputs("0/1", out);
        return;
    }
    /* Print numerator as a plain integer (no N suffix), then "/", then
     * the denominator. The form `1/2` re-reads as the same ratio. */
    {
        char *n = mino_bigint_to_cstr(v->as.ratio.num);
        char *d = mino_bigint_to_cstr(v->as.ratio.denom);
        if (n == NULL || d == NULL) {
            free(n); free(d);
            fputs("0/1", out);
            return;
        }
        fputs(n, out);
        fputc('/', out);
        fputs(d, out);
        free(n); free(d);
    }
    (void)S;
}

int mino_ratio_equals(const mino_val_t *a, const mino_val_t *b)
{
    if (a == NULL || b == NULL) return 0;
    if (a->type != MINO_RATIO || b->type != MINO_RATIO) return 0;
    return mino_bigint_equals(a->as.ratio.num,   b->as.ratio.num)
        && mino_bigint_equals(a->as.ratio.denom, b->as.ratio.denom);
}

/* Compare two ratios. Sign of (a.num * b.denom - b.num * a.denom).
 * Both denominators are positive so cross-multiplication is safe. */
int mino_ratio_cmp(const mino_val_t *a, const mino_val_t *b)
{
    mpz_t lhs, rhs;
    int   r;
    if (a == NULL || b == NULL) return 0;
    if (a->type != MINO_RATIO || b->type != MINO_RATIO) return 0;
    if (mp_int_init(&lhs) != MP_OK) return 0;
    if (mp_int_init(&rhs) != MP_OK) { mp_int_clear(&lhs); return 0; }
    mp_int_mul((mp_int)a->as.ratio.num->as.bigint.mpz,
               (mp_int)b->as.ratio.denom->as.bigint.mpz, &lhs);
    mp_int_mul((mp_int)b->as.ratio.num->as.bigint.mpz,
               (mp_int)a->as.ratio.denom->as.bigint.mpz, &rhs);
    r = mp_int_compare(&lhs, &rhs);
    mp_int_clear(&lhs);
    mp_int_clear(&rhs);
    return r;
}

uint32_t mino_ratio_hash(const mino_val_t *v)
{
    uint32_t h;
    if (v == NULL || v->type != MINO_RATIO) return 0;
    h = mino_bigint_hash(v->as.ratio.num);
    h ^= mino_bigint_hash(v->as.ratio.denom) * 0x85ebca77u;
    /* Tag with a ratio-specific salt so a ratio doesn't collide with a
     * plain bigint of the same numerator. */
    return h ^ 0x5bd1e995u;
}

double mino_ratio_to_double(const mino_val_t *v)
{
    double n, d;
    if (v == NULL || v->type != MINO_RATIO) return 0.0;
    n = mino_bigint_to_double(v->as.ratio.num);
    d = mino_bigint_to_double(v->as.ratio.denom);
    if (d == 0.0) return 0.0;
    return n / d;
}

/* (numerator r) — accepts ratio (returns its numerator) or integer
 * (returns the integer; integers act as r/1). */
mino_val_t *prim_numerator(mino_state_t *S, mino_val_t *args, mino_env_t *env)
{
    mino_val_t *x;
    (void)env;
    if (!mino_is_cons(args) || mino_is_cons(args->as.cons.cdr)) {
        return prim_throw_classified(S, "eval/arity", "MAR001",
                                     "numerator requires one argument");
    }
    x = args->as.cons.car;
    if (x == NULL)
        return prim_throw_classified(S, "eval/type", "MTY001",
                                     "numerator: nil");
    if (x->type == MINO_RATIO) {
        long long ll;
        if (mino_as_ll(x->as.ratio.num, &ll)) return mino_int(S, ll);
        return x->as.ratio.num;
    }
    if (x->type == MINO_INT || x->type == MINO_BIGINT) return x;
    return prim_throw_classified(S, "eval/type", "MTY001",
                                 "numerator: argument must be a rational");
}

mino_val_t *prim_denominator(mino_state_t *S, mino_val_t *args, mino_env_t *env)
{
    mino_val_t *x;
    (void)env;
    if (!mino_is_cons(args) || mino_is_cons(args->as.cons.cdr)) {
        return prim_throw_classified(S, "eval/arity", "MAR001",
                                     "denominator requires one argument");
    }
    x = args->as.cons.car;
    if (x == NULL)
        return prim_throw_classified(S, "eval/type", "MTY001",
                                     "denominator: nil");
    if (x->type == MINO_RATIO) {
        long long ll;
        if (mino_as_ll(x->as.ratio.denom, &ll)) return mino_int(S, ll);
        return x->as.ratio.denom;
    }
    if (x->type == MINO_INT || x->type == MINO_BIGINT)
        return mino_int(S, 1);
    return prim_throw_classified(S, "eval/type", "MTY001",
                                 "denominator: argument must be a rational");
}

/* (ratio? x) */
mino_val_t *prim_ratio_p(mino_state_t *S, mino_val_t *args, mino_env_t *env)
{
    (void)env;
    if (!mino_is_cons(args) || mino_is_cons(args->as.cons.cdr)) {
        return prim_throw_classified(S, "eval/arity", "MAR001",
                                     "ratio? requires one argument");
    }
    {
        mino_val_t *x = args->as.cons.car;
        return (x != NULL && x->type == MINO_RATIO)
            ? mino_true(S) : mino_false(S);
    }
}

/* (rational? x) — true for int / bigint / ratio. */
mino_val_t *prim_rational_p(mino_state_t *S, mino_val_t *args, mino_env_t *env)
{
    (void)env;
    if (!mino_is_cons(args) || mino_is_cons(args->as.cons.cdr)) {
        return prim_throw_classified(S, "eval/arity", "MAR001",
                                     "rational? requires one argument");
    }
    {
        mino_val_t *x = args->as.cons.car;
        if (x == NULL) return mino_false(S);
        return (x->type == MINO_INT
             || x->type == MINO_BIGINT
             || x->type == MINO_RATIO) ? mino_true(S) : mino_false(S);
    }
}

/* (rationalize x) — coerce a float to the nearest representable ratio.
 * For numeric kinds that already exact (int, bigint, ratio), returns
 * the value unchanged. For floats, decomposes the IEEE-754 value into
 * mantissa * 2^exp and reduces. */
mino_val_t *prim_rationalize(mino_state_t *S, mino_val_t *args, mino_env_t *env)
{
    mino_val_t *x;
    (void)env;
    if (!mino_is_cons(args) || mino_is_cons(args->as.cons.cdr)) {
        return prim_throw_classified(S, "eval/arity", "MAR001",
                                     "rationalize requires one argument");
    }
    x = args->as.cons.car;
    if (x == NULL)
        return prim_throw_classified(S, "eval/type", "MTY001",
                                     "rationalize: nil");
    if (x->type == MINO_INT || x->type == MINO_BIGINT
        || x->type == MINO_RATIO) {
        return x;
    }
    if (x->type == MINO_FLOAT) {
        /* Use the fact that any finite double is m * 2^e for some
         * 53-bit signed mantissa m and integer exponent e. Build a
         * ratio numerator / denominator from that decomposition. */
        double      d = x->as.f;
        int         exp;
        double      mant;
        long long   m;
        mino_val_t *bn, *bd;
        if (d != d) /* NaN */
            return prim_throw_classified(S, "eval/type", "MTY001",
                                         "rationalize: NaN");
        if (d == 0.0) return mino_int(S, 0);
        mant = frexp(d, &exp);
        /* mant in [0.5, 1.0); shift to integer mantissa. */
        m = (long long)(mant * 9007199254740992.0); /* 2^53 */
        exp -= 53;
        bn = mino_bigint_from_ll(S, m);
        if (bn == NULL) return NULL;
        if (exp >= 0) {
            mpz_t pw;
            if (mp_int_init(&pw) != MP_OK) {
                return prim_throw_classified(S, "eval/out-of-memory", "MOM001",
                                             "out of memory");
            }
            if (mp_int_set_value(&pw, 2) != MP_OK ||
                mp_int_expt(&pw, exp, &pw) != MP_OK) {
                mp_int_clear(&pw);
                return prim_throw_classified(S, "eval/out-of-memory", "MOM001",
                                             "out of memory");
            }
            mp_int_mul((mp_int)bn->as.bigint.mpz, &pw,
                       (mp_int)bn->as.bigint.mpz);
            mp_int_clear(&pw);
            return mino_ratio_make(S, bn, mino_int(S, 1));
        } else {
            int neg_exp = -exp;
            bd = mino_bigint_from_ll(S, 1);
            if (bd == NULL) return NULL;
            {
                mpz_t pw;
                if (mp_int_init(&pw) != MP_OK) {
                    return prim_throw_classified(S, "eval/out-of-memory",
                                                 "MOM001", "out of memory");
                }
                if (mp_int_set_value(&pw, 2) != MP_OK ||
                    mp_int_expt(&pw, neg_exp, &pw) != MP_OK) {
                    mp_int_clear(&pw);
                    return prim_throw_classified(S, "eval/out-of-memory",
                                                 "MOM001", "out of memory");
                }
                mp_int_copy(&pw, (mp_int)bd->as.bigint.mpz);
                mp_int_clear(&pw);
            }
            return mino_ratio_make(S, bn, bd);
        }
    }
    return prim_throw_classified(S, "eval/type", "MTY001",
                                 "rationalize: argument must be numeric");
}

/* Tower-arithmetic helpers for ratios. Each takes any numeric pair
 * (a, b) where at least one operand is a ratio (or both are) and
 * returns a canonicalised ratio / integer. */

static mino_val_t *as_ratio_pair(mino_state_t *S, const mino_val_t *v,
                                 mino_val_t **out_num, mino_val_t **out_denom)
{
    if (v == NULL) return NULL;
    if (v->type == MINO_RATIO) {
        *out_num   = v->as.ratio.num;
        *out_denom = v->as.ratio.denom;
        return (mino_val_t *)v;
    }
    if (v->type == MINO_INT || v->type == MINO_BIGINT) {
        mino_val_t *bn = to_bigint(S, v);
        if (bn == NULL) return NULL;
        *out_num   = bn;
        *out_denom = mino_bigint_from_ll(S, 1);
        return bn;
    }
    return NULL;
}

mino_val_t *mino_ratio_add(mino_state_t *S, const mino_val_t *a,
                           const mino_val_t *b)
{
    mino_val_t *an, *ad, *bn, *bd;
    mino_val_t *cross1, *cross2, *new_num, *new_den;
    if (as_ratio_pair(S, a, &an, &ad) == NULL ||
        as_ratio_pair(S, b, &bn, &bd) == NULL) {
        return NULL;
    }
    /* a/b + c/d = (a*d + c*b) / (b*d) */
    cross1  = mino_bigint_mul(S, an, bd); if (cross1 == NULL) return NULL;
    cross2  = mino_bigint_mul(S, bn, ad); if (cross2 == NULL) return NULL;
    new_num = mino_bigint_add(S, cross1, cross2); if (new_num == NULL) return NULL;
    new_den = mino_bigint_mul(S, ad, bd); if (new_den == NULL) return NULL;
    return mino_ratio_make(S, new_num, new_den);
}

mino_val_t *mino_ratio_sub(mino_state_t *S, const mino_val_t *a,
                           const mino_val_t *b)
{
    mino_val_t *an, *ad, *bn, *bd;
    mino_val_t *cross1, *cross2, *new_num, *new_den;
    if (as_ratio_pair(S, a, &an, &ad) == NULL ||
        as_ratio_pair(S, b, &bn, &bd) == NULL) {
        return NULL;
    }
    cross1  = mino_bigint_mul(S, an, bd); if (cross1 == NULL) return NULL;
    cross2  = mino_bigint_mul(S, bn, ad); if (cross2 == NULL) return NULL;
    new_num = mino_bigint_sub(S, cross1, cross2); if (new_num == NULL) return NULL;
    new_den = mino_bigint_mul(S, ad, bd); if (new_den == NULL) return NULL;
    return mino_ratio_make(S, new_num, new_den);
}

mino_val_t *mino_ratio_mul(mino_state_t *S, const mino_val_t *a,
                           const mino_val_t *b)
{
    mino_val_t *an, *ad, *bn, *bd;
    mino_val_t *new_num, *new_den;
    if (as_ratio_pair(S, a, &an, &ad) == NULL ||
        as_ratio_pair(S, b, &bn, &bd) == NULL) {
        return NULL;
    }
    new_num = mino_bigint_mul(S, an, bn); if (new_num == NULL) return NULL;
    new_den = mino_bigint_mul(S, ad, bd); if (new_den == NULL) return NULL;
    return mino_ratio_make(S, new_num, new_den);
}

mino_val_t *mino_ratio_div(mino_state_t *S, const mino_val_t *a,
                           const mino_val_t *b)
{
    mino_val_t *an, *ad, *bn, *bd;
    mino_val_t *new_num, *new_den;
    if (as_ratio_pair(S, a, &an, &ad) == NULL ||
        as_ratio_pair(S, b, &bn, &bd) == NULL) {
        return NULL;
    }
    if (mp_int_compare_zero((mp_int)bn->as.bigint.mpz) == 0) {
        return prim_throw_classified(S, "eval/type", "MTY001",
                                     "divide by zero");
    }
    /* a/b / c/d = (a*d) / (b*c) */
    new_num = mino_bigint_mul(S, an, bd); if (new_num == NULL) return NULL;
    new_den = mino_bigint_mul(S, ad, bn); if (new_den == NULL) return NULL;
    return mino_ratio_make(S, new_num, new_den);
}

/* ------------------------------------------------------------------------- */
/* MINO_BIGDEC                                                                */
/* ------------------------------------------------------------------------- */

mino_val_t *mino_bigdec_make(mino_state_t *S, mino_val_t *unscaled, int scale)
{
    mino_val_t *v;
    if (unscaled == NULL || unscaled->type != MINO_BIGINT) {
        return prim_throw_classified(S, "internal", "MIN001",
                                     "bigdec: unscaled must be a bigint");
    }
    v = alloc_val(S, MINO_BIGDEC);
    v->as.bigdec.unscaled = unscaled;
    v->as.bigdec.scale    = scale;
    return v;
}

/* Parse a base-10 decimal numeric string into a bigdec. Recognises:
 *   optional sign, integer part, '.' fractional part, 'e'/'E' exponent.
 * Returns NULL on parse failure. */
mino_val_t *mino_bigdec_from_string(mino_state_t *S, const char *s)
{
    const char *p, *frac_start;
    char       *digits;
    size_t      digit_count, dlen;
    int         sign = 1, frac_len = 0, has_dot = 0, exp = 0;
    mino_val_t *unscaled;
    if (s == NULL || *s == '\0') return NULL;
    p = s;
    if (*p == '+') p++;
    else if (*p == '-') { sign = -1; p++; }
    /* Count digits and detect dot / exponent. Build a digits-only buffer
     * (sign attached afterwards) so mp_int_read_string sees a valid
     * integer literal even if the source had a '.'. */
    digits = (char *)malloc(strlen(p) + 2);
    if (digits == NULL) return NULL;
    digit_count = 0;
    if (sign < 0) digits[digit_count++] = '-';
    frac_start = NULL;
    while (*p) {
        if (*p >= '0' && *p <= '9') {
            digits[digit_count++] = *p++;
            if (has_dot) frac_len++;
        } else if (*p == '.' && !has_dot) {
            has_dot = 1;
            frac_start = p;
            p++;
        } else if ((*p == 'e' || *p == 'E') && (p != s)) {
            char *end;
            long  e = strtol(p + 1, &end, 10);
            if (end == p + 1) { free(digits); return NULL; }
            exp = (int)e;
            p = end;
            break;
        } else {
            free(digits); return NULL;
        }
    }
    if (*p != '\0') { free(digits); return NULL; }
    if (digit_count == (size_t)(sign < 0 ? 1 : 0)) {
        /* No digits at all. */
        free(digits); return NULL;
    }
    digits[digit_count] = '\0';
    dlen = digit_count;
    unscaled = mino_bigint_from_string_n(S, digits, dlen);
    free(digits);
    (void)frac_start;
    if (unscaled == NULL) return NULL;
    /* The decimal scale is (frac_len - exp). Negative scale means
     * trailing zeros — scale up by multiplying unscaled by 10^|scale|. */
    {
        int scale = frac_len - exp;
        if (scale < 0) {
            mpz_t pw;
            if (mp_int_init(&pw) != MP_OK) {
                return prim_throw_classified(S, "eval/out-of-memory",
                                             "MOM001", "out of memory");
            }
            if (mp_int_set_value(&pw, 10) != MP_OK ||
                mp_int_expt(&pw, -scale, &pw) != MP_OK) {
                mp_int_clear(&pw);
                return prim_throw_classified(S, "eval/out-of-memory",
                                             "MOM001", "out of memory");
            }
            mp_int_mul((mp_int)unscaled->as.bigint.mpz, &pw,
                       (mp_int)unscaled->as.bigint.mpz);
            mp_int_clear(&pw);
            scale = 0;
        }
        return mino_bigdec_make(S, unscaled, scale);
    }
}

/* Print a bigdec as "123.45M" — Clojure's bigdec literal form. */
void mino_bigdec_print(mino_state_t *S, const mino_val_t *v, FILE *out)
{
    char *digits;
    int   scale;
    int   neg;
    int   len;
    if (v == NULL || v->type != MINO_BIGDEC) {
        fputs("0M", out); return;
    }
    digits = mino_bigint_to_cstr(v->as.bigdec.unscaled);
    if (digits == NULL) { fputs("0M", out); return; }
    scale = v->as.bigdec.scale;
    neg   = (digits[0] == '-');
    len   = (int)strlen(digits);
    if (scale == 0) {
        fputs(digits, out);
        fputc('M', out);
    } else {
        int int_part_len = len - (neg ? 1 : 0) - scale;
        if (int_part_len > 0) {
            /* Insert '.' before the last `scale` digits. */
            int i;
            for (i = 0; i < (neg ? 1 : 0) + int_part_len; i++) fputc(digits[i], out);
            fputc('.', out);
            for (; i < len; i++) fputc(digits[i], out);
            fputc('M', out);
        } else {
            /* Need leading "0." and zero-padding. */
            int pad;
            if (neg) fputc('-', out);
            fputs("0.", out);
            for (pad = 0; pad < -int_part_len; pad++) fputc('0', out);
            fputs(digits + (neg ? 1 : 0), out);
            fputc('M', out);
        }
    }
    free(digits);
    (void)S;
}

/* Equality for bigdecs is "same unscaled and same scale" — Clojure's
 * BigDecimal.equals is type-strict on representation. (1.00M) and
 * (1.0M) are NOT equal under = even though they're numerically equal.
 * Use `==` for cross-tier numeric equality. */
int mino_bigdec_equals(const mino_val_t *a, const mino_val_t *b)
{
    if (a == NULL || b == NULL) return 0;
    if (a->type != MINO_BIGDEC || b->type != MINO_BIGDEC) return 0;
    if (a->as.bigdec.scale != b->as.bigdec.scale) return 0;
    return mino_bigint_equals(a->as.bigdec.unscaled, b->as.bigdec.unscaled);
}

uint32_t mino_bigdec_hash(const mino_val_t *v)
{
    uint32_t h;
    if (v == NULL || v->type != MINO_BIGDEC) return 0;
    h = mino_bigint_hash(v->as.bigdec.unscaled);
    h ^= ((uint32_t)v->as.bigdec.scale) * 0x27d4eb2du;
    return h ^ 0x165667b1u;
}

double mino_bigdec_to_double(const mino_val_t *v)
{
    double d;
    int    scale;
    if (v == NULL || v->type != MINO_BIGDEC) return 0.0;
    d = mino_bigint_to_double(v->as.bigdec.unscaled);
    scale = v->as.bigdec.scale;
    while (scale > 0)  { d /= 10.0; scale--; }
    while (scale < 0)  { d *= 10.0; scale++; }
    return d;
}

/* (bigdec x) — coerce. */
mino_val_t *prim_bigdec(mino_state_t *S, mino_val_t *args, mino_env_t *env)
{
    mino_val_t *x;
    (void)env;
    if (!mino_is_cons(args) || mino_is_cons(args->as.cons.cdr)) {
        return prim_throw_classified(S, "eval/arity", "MAR001",
                                     "bigdec requires one argument");
    }
    x = args->as.cons.car;
    if (x == NULL)
        return prim_throw_classified(S, "eval/type", "MTY001",
                                     "bigdec: nil");
    if (x->type == MINO_BIGDEC) return x;
    if (x->type == MINO_INT) {
        mino_val_t *u = mino_bigint_from_ll(S, x->as.i);
        if (u == NULL) return NULL;
        return mino_bigdec_make(S, u, 0);
    }
    if (x->type == MINO_BIGINT) {
        mino_val_t *u = to_bigint(S, x);
        if (u == NULL) return NULL;
        return mino_bigdec_make(S, u, 0);
    }
    if (x->type == MINO_FLOAT) {
        char buf[64];
        snprintf(buf, sizeof(buf), "%.17g", x->as.f);
        return mino_bigdec_from_string(S, buf);
    }
    if (x->type == MINO_STRING) {
        mino_val_t *r = mino_bigdec_from_string(S, x->as.s.data);
        if (r == NULL)
            return prim_throw_classified(S, "eval/type", "MTY001",
                                         "bigdec: invalid numeric string");
        return r;
    }
    if (x->type == MINO_RATIO) {
        /* Convert via float then string. Lossy; the user can pre-call
         * with-precision once it's wired. */
        char buf[64];
        snprintf(buf, sizeof(buf), "%.17g", mino_ratio_to_double(x));
        return mino_bigdec_from_string(S, buf);
    }
    return prim_throw_classified(S, "eval/type", "MTY001",
                                 "bigdec: unsupported argument type");
}

/* (decimal? x) — true for bigdec only. */
mino_val_t *prim_decimal_p(mino_state_t *S, mino_val_t *args, mino_env_t *env)
{
    (void)env;
    if (!mino_is_cons(args) || mino_is_cons(args->as.cons.cdr)) {
        return prim_throw_classified(S, "eval/arity", "MAR001",
                                     "decimal? requires one argument");
    }
    {
        mino_val_t *x = args->as.cons.car;
        return (x != NULL && x->type == MINO_BIGDEC)
            ? mino_true(S) : mino_false(S);
    }
}

/* ------------------------------------------------------------------------- */
/* Tier promotion helpers + bigdec arithmetic                                 */
/* ------------------------------------------------------------------------- */

/* Promote int / bigint / ratio to bigdec at scale 0. Lossless. */
mino_val_t *mino_to_bigdec(mino_state_t *S, const mino_val_t *v)
{
    if (v == NULL) return NULL;
    if (v->type == MINO_BIGDEC) return (mino_val_t *)v;
    if (v->type == MINO_INT) {
        mino_val_t *u = mino_bigint_from_ll(S, v->as.i);
        if (u == NULL) return NULL;
        return mino_bigdec_make(S, u, 0);
    }
    if (v->type == MINO_BIGINT) {
        mino_val_t *u = to_bigint(S, v);
        if (u == NULL) return NULL;
        return mino_bigdec_make(S, u, 0);
    }
    /* Ratio promotion to bigdec is non-exact in general (e.g. 1/3) so the
     * tower-dispatch caller drops to float when ratio meets bigdec. */
    return NULL;
}

/* Scale an unscaled bigint up by 10^delta in place; delta must be >= 0. */
static int bigint_mul_pow10(mino_val_t *bi, int delta)
{
    if (delta <= 0) return 1;
    {
        mpz_t pw;
        if (mp_int_init(&pw) != MP_OK) return 0;
        if (mp_int_set_value(&pw, 10) != MP_OK ||
            mp_int_expt(&pw, delta, &pw) != MP_OK) {
            mp_int_clear(&pw);
            return 0;
        }
        mp_int_mul((mp_int)bi->as.bigint.mpz, &pw,
                   (mp_int)bi->as.bigint.mpz);
        mp_int_clear(&pw);
    }
    return 1;
}

/* Bigdec add / sub: align scales then add unscaled bigints. */
mino_val_t *mino_bigdec_add(mino_state_t *S, const mino_val_t *a,
                            const mino_val_t *b)
{
    int        sa, sb, smax;
    mino_val_t *au, *bu, *result;
    if (a == NULL || b == NULL || a->type != MINO_BIGDEC || b->type != MINO_BIGDEC) {
        return prim_throw_classified(S, "eval/type", "MTY001",
                                     "bigdec add: bigdec operands required");
    }
    sa = a->as.bigdec.scale;
    sb = b->as.bigdec.scale;
    smax = sa > sb ? sa : sb;
    au = to_bigint(S, a->as.bigdec.unscaled);
    if (au == NULL) return NULL;
    bu = to_bigint(S, b->as.bigdec.unscaled);
    if (bu == NULL) return NULL;
    if (!bigint_mul_pow10(au, smax - sa) || !bigint_mul_pow10(bu, smax - sb)) {
        return prim_throw_classified(S, "eval/out-of-memory", "MOM001",
                                     "out of memory in bigdec add");
    }
    result = mino_bigint_add(S, au, bu);
    if (result == NULL) return NULL;
    return mino_bigdec_make(S, result, smax);
}

mino_val_t *mino_bigdec_sub(mino_state_t *S, const mino_val_t *a,
                            const mino_val_t *b)
{
    int        sa, sb, smax;
    mino_val_t *au, *bu, *result;
    if (a == NULL || b == NULL || a->type != MINO_BIGDEC || b->type != MINO_BIGDEC) {
        return prim_throw_classified(S, "eval/type", "MTY001",
                                     "bigdec sub: bigdec operands required");
    }
    sa = a->as.bigdec.scale;
    sb = b->as.bigdec.scale;
    smax = sa > sb ? sa : sb;
    au = to_bigint(S, a->as.bigdec.unscaled);
    if (au == NULL) return NULL;
    bu = to_bigint(S, b->as.bigdec.unscaled);
    if (bu == NULL) return NULL;
    if (!bigint_mul_pow10(au, smax - sa) || !bigint_mul_pow10(bu, smax - sb)) {
        return prim_throw_classified(S, "eval/out-of-memory", "MOM001",
                                     "out of memory in bigdec sub");
    }
    result = mino_bigint_sub(S, au, bu);
    if (result == NULL) return NULL;
    return mino_bigdec_make(S, result, smax);
}

mino_val_t *mino_bigdec_mul(mino_state_t *S, const mino_val_t *a,
                            const mino_val_t *b)
{
    mino_val_t *result;
    if (a == NULL || b == NULL || a->type != MINO_BIGDEC || b->type != MINO_BIGDEC) {
        return prim_throw_classified(S, "eval/type", "MTY001",
                                     "bigdec mul: bigdec operands required");
    }
    result = mino_bigint_mul(S, a->as.bigdec.unscaled, b->as.bigdec.unscaled);
    if (result == NULL) return NULL;
    return mino_bigdec_make(S, result, a->as.bigdec.scale + b->as.bigdec.scale);
}

mino_val_t *mino_bigdec_neg(mino_state_t *S, const mino_val_t *a)
{
    mino_val_t *u;
    if (a == NULL || a->type != MINO_BIGDEC) {
        return prim_throw_classified(S, "eval/type", "MTY001",
                                     "bigdec neg: bigdec operand required");
    }
    u = mino_bigint_neg(S, a->as.bigdec.unscaled);
    if (u == NULL) return NULL;
    return mino_bigdec_make(S, u, a->as.bigdec.scale);
}

/* Compare two bigdecs. Returns -1, 0, or 1. */
int mino_bigdec_cmp(const mino_val_t *a, const mino_val_t *b)
{
    int sa = a->as.bigdec.scale;
    int sb = b->as.bigdec.scale;
    if (sa == sb) {
        return mino_bigint_cmp(a->as.bigdec.unscaled, b->as.bigdec.unscaled);
    }
    /* Multiply the lower-scale unscaled by 10^(diff), then compare. */
    {
        mpz_t lhs, rhs, pw;
        int   r;
        if (mp_int_init(&lhs) != MP_OK) return 0;
        if (mp_int_init(&rhs) != MP_OK) { mp_int_clear(&lhs); return 0; }
        if (mp_int_init(&pw) != MP_OK) {
            mp_int_clear(&lhs); mp_int_clear(&rhs); return 0;
        }
        mp_int_copy((mp_int)a->as.bigdec.unscaled->as.bigint.mpz, &lhs);
        mp_int_copy((mp_int)b->as.bigdec.unscaled->as.bigint.mpz, &rhs);
        if (sa < sb) {
            mp_int_set_value(&pw, 10);
            mp_int_expt(&pw, sb - sa, &pw);
            mp_int_mul(&lhs, &pw, &lhs);
        } else {
            mp_int_set_value(&pw, 10);
            mp_int_expt(&pw, sa - sb, &pw);
            mp_int_mul(&rhs, &pw, &rhs);
        }
        r = mp_int_compare(&lhs, &rhs);
        mp_int_clear(&lhs); mp_int_clear(&rhs); mp_int_clear(&pw);
        return r;
    }
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
