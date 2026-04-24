/*
 * prim_bignum.c -- arbitrary-precision integer support.
 *
 * Backed by vendored imath (src/vendor/imath.c). The mpz_t struct for
 * each MINO_BIGINT value is malloc-owned and referenced through the
 * opaque `v->as.bigint.mpz` pointer. The cell's digit storage (managed
 * by imath internally) is freed by mino_bigint_free, which the minor
 * and major sweep loops call when a bigint cell becomes unreachable.
 *
 * Constructors return GC-owned cells. The cell's mpz_t is allocated
 * and initialised BEFORE the mino_val_t so that, once alloc_val
 * returns, the pointer store is unconditional and no partially-typed
 * cell can be observed by a subsequent collector.
 */

#include "prim_internal.h"
#include "vendor/imath.h"

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

/* Hash: reduce the bigint modulo (2^31 - 1) using imath, then fold via
 * the same FNV-style mixer used elsewhere. Cross-tier hash equivalence
 * is NOT required in this phase; ints and bigints hash separately
 * and never compare equal under `=` (they do under `==`, covered in
 * Phase C.3 with tower dispatch). */
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
