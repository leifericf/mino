/*
 * interop_test.c -- test driver for host interop system.
 *
 * Registers mock capabilities (Counter type, Math statics), then runs
 * tests/interop_test.mino which exercises all four host primitives
 * and verifies error handling.
 *
 * Build: make test-interop
 */

#include "mino.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void free_finalizer(void *ptr, const char *tag)
{
    (void)tag;
    free(ptr);
}

/* --- Mock Counter type ---
 * A simple counter: new creates with value 0, inc adds 1, get returns
 * current value, "value" getter returns current value. */

typedef struct {
    long long val;
} counter_t;

static mino_val_t *counter_new(mino_state_t *S, mino_val_t *target,
                                mino_val_t *args, void *ctx)
{
    counter_t *c;
    (void)target; (void)args; (void)ctx;
    c = (counter_t *)malloc(sizeof(*c));
    if (c == NULL) return NULL;
    c->val = 0;
    return mino_handle_ex(S, c, "Counter", free_finalizer);
}

static mino_val_t *counter_inc(mino_state_t *S, mino_val_t *target,
                                mino_val_t *args, void *ctx)
{
    counter_t *c;
    (void)S; (void)args; (void)ctx;
    c = (counter_t *)mino_handle_ptr(target);
    c->val++;
    return target;
}

static mino_val_t *counter_get(mino_state_t *S, mino_val_t *target,
                                mino_val_t *args, void *ctx)
{
    counter_t *c;
    (void)args; (void)ctx;
    c = (counter_t *)mino_handle_ptr(target);
    return mino_int(S, c->val);
}

static mino_val_t *counter_add(mino_state_t *S, mino_val_t *target,
                                mino_val_t *args, void *ctx)
{
    counter_t *c;
    long long n;
    (void)S; (void)ctx;
    c = (counter_t *)mino_handle_ptr(target);
    if (!mino_is_cons(args) || !mino_to_int(args->as.cons.car, &n))
        return NULL;
    c->val += n;
    return target;
}

static mino_val_t *counter_value_getter(mino_state_t *S, mino_val_t *target,
                                         mino_val_t *args, void *ctx)
{
    counter_t *c;
    (void)args; (void)ctx;
    c = (counter_t *)mino_handle_ptr(target);
    return mino_int(S, c->val);
}

/* --- Mock Math statics --- */

static mino_val_t *math_add(mino_state_t *S, mino_val_t *target,
                             mino_val_t *args, void *ctx)
{
    long long a, b;
    (void)target; (void)ctx;
    if (!mino_is_cons(args) || !mino_to_int(args->as.cons.car, &a))
        return NULL;
    args = args->as.cons.cdr;
    if (!mino_is_cons(args) || !mino_to_int(args->as.cons.car, &b))
        return NULL;
    return mino_int(S, a + b);
}

static mino_val_t *math_pi(mino_state_t *S, mino_val_t *target,
                            mino_val_t *args, void *ctx)
{
    (void)target; (void)args; (void)ctx;
    return mino_float(S, 3.14159265358979323846);
}

/* Resolver for require. */
static const char *resolve_module(const char *name, void *ctx)
{
    char *buf;
    size_t len;
    (void)ctx;
    len = strlen(name) + 6; /* name + ".mino" + NUL */
    buf = (char *)malloc(len);
    if (buf == NULL) return NULL;
    snprintf(buf, len, "%s.mino", name);
    return buf;
}

int main(int argc, char **argv)
{
    mino_state_t *S;
    mino_env_t   *env;
    mino_val_t   *result;

    (void)argc;
    (void)argv;

    S   = mino_state_new();
    env = mino_new(S);
    mino_set_resolver(S, resolve_module, NULL);

    /* Register Counter type. */
    mino_host_register_ctor(S, "Counter", 0, counter_new, NULL);
    mino_host_register_method(S, "Counter", "inc", 0, counter_inc, NULL);
    mino_host_register_method(S, "Counter", "get", 0, counter_get, NULL);
    mino_host_register_method(S, "Counter", "add", 1, counter_add, NULL);
    mino_host_register_getter(S, "Counter", "value", counter_value_getter, NULL);

    /* Register Math statics. */
    mino_host_register_static(S, "Math", "add", 2, math_add, NULL);
    mino_host_register_static(S, "Math", "pi", 0, math_pi, NULL);

    /* Enable interop. */
    mino_host_enable(S);

    /* Run the mino test file. */
    result = mino_load_file(S, "tests/interop_test.mino", env);
    if (result == NULL) {
        fprintf(stderr, "interop test failed: %s\n", mino_last_error(S));
        mino_state_free(S);
        return 1;
    }

    mino_state_free(S);
    return 0;
}
