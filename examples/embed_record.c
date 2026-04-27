/*
 * embed_record.c -- demonstrates the host-side record-type API.
 *
 * Defines a Vec3 type from C, builds an instance from C, hands it
 * to script, runs a protocol method through the script-side
 * dispatch, and reads field values back from C. Used as a smoke
 * test that mino_defrecord / mino_record / mino_record_field stay
 * symmetrical with how script-side defrecord/->Vec3/(:x v) work.
 *
 * Build (from repo root):
 *   cc -std=c99 -Isrc -Isrc/public -Isrc/runtime -Isrc/gc -Isrc/eval \
 *      -Isrc/collections -Isrc/prim -Isrc/async -Isrc/interop \
 *      -Isrc/diag -Isrc/vendor/imath \
 *      -o embed_record examples/embed_record.c <objects from `mino task build`> -lm
 */

#include "mino.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int read_long(const mino_val_t *v, long long *out)
{
    return mino_to_int(v, out);
}

int main(void)
{
    mino_state_t *S;
    mino_env_t   *env;
    mino_val_t   *Vec3, *v, *x, *y, *z, *result;
    mino_val_t   *vals[3];
    const char   *fields[3] = {"x", "y", "z"};
    long long     lx, ly, lz;

    S = mino_state_new();
    if (S == NULL) {
        fprintf(stderr, "state_new failed\n");
        return 1;
    }
    env = mino_env_new(S);
    mino_install_core(S, env);

    /* Define the record type from C. The constructor is idempotent
     * by (ns, name); a re-call returns the existing type. */
    Vec3 = mino_defrecord(S, "user", "Vec3", fields, 3);
    if (Vec3 == NULL || !mino_is_record_type(Vec3)) {
        fprintf(stderr, "mino_defrecord failed\n");
        return 1;
    }
    /* Bind the type to the script name so extend-type can reference
     * it as a symbol. */
    mino_env_set(S, env, "Vec3", Vec3);

    /* Build a record value from C and bind it to v. */
    vals[0] = mino_int(S, 1);
    vals[1] = mino_int(S, 2);
    vals[2] = mino_int(S, 3);
    v = mino_record(S, Vec3, vals, 3);
    if (v == NULL || !mino_is_record(v)) {
        fprintf(stderr, "mino_record failed\n");
        return 1;
    }
    mino_env_set(S, env, "v", v);

    /* Script side: define a Magnitude-squared protocol, extend it
     * onto Vec3, and call it on the C-built instance. */
    result = mino_eval_string(S,
        "(do"
        "  (defprotocol IMag (mag2 [x]))"
        "  (extend-type Vec3 IMag"
        "    (mag2 [w] (+ (* (:x w) (:x w)) (* (:y w) (:y w)) (* (:z w) (:z w)))))"
        "  (mag2 v))",
        env);
    if (result == NULL) {
        fprintf(stderr, "eval failed: %s\n", mino_last_error(S));
        return 1;
    }
    {
        long long got;
        if (!read_long(result, &got)) {
            fprintf(stderr, "expected integer result\n");
            return 1;
        }
        printf("script mag2(1,2,3) = %lld (expected 14)\n", got);
        if (got != 14) return 1;
    }

    /* Read fields back from C. */
    x = mino_record_field(v, "x");
    y = mino_record_field(v, "y");
    z = mino_record_field(v, "z");
    if (x == NULL || y == NULL || z == NULL) {
        fprintf(stderr, "mino_record_field returned NULL\n");
        return 1;
    }
    if (!read_long(x, &lx) || !read_long(y, &ly) || !read_long(z, &lz)) {
        fprintf(stderr, "field values not integers\n");
        return 1;
    }
    printf("C-side fields: x=%lld y=%lld z=%lld\n", lx, ly, lz);
    if (lx != 1 || ly != 2 || lz != 3) return 1;

    /* Confirm mino_record_field rejects undeclared keys. */
    if (mino_record_field(v, "w") != NULL) {
        fprintf(stderr, "expected NULL for undeclared field 'w'\n");
        return 1;
    }

    /* Round-trip identity: idempotent intern. */
    {
        mino_val_t *Vec3b = mino_defrecord(S, "user", "Vec3", fields, 3);
        if (Vec3b != Vec3) {
            fprintf(stderr, "mino_defrecord not idempotent\n");
            return 1;
        }
    }

    printf("ok\n");
    mino_env_free(S, env);
    mino_state_free(S);
    return 0;
}
