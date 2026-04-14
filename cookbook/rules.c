/*
 * rules.c — a tiny rules engine driven by mino expressions.
 *
 * Demonstrates: registering host functions, evaluating user-defined
 * predicates, using mino as a decision layer between C data and actions.
 *
 * Build:  cc -std=c99 -I.. -o rules rules.c ../mino.c
 * Run:    ./rules
 */

#include "mino.h"
#include <stdio.h>
#include <string.h>

/* --- Simulated application state ---------------------------------------- */

typedef struct {
    const char *user;
    int         age;
    int         purchases;
    double      balance;
} customer_t;

static customer_t current_customer;

/* Host functions exposed to the rules engine. */

static mino_val_t *host_age(mino_val_t *args, mino_env_t *env)
{
    (void)args; (void)env;
    return mino_int(current_customer.age);
}

static mino_val_t *host_purchases(mino_val_t *args, mino_env_t *env)
{
    (void)args; (void)env;
    return mino_int(current_customer.purchases);
}

static mino_val_t *host_balance(mino_val_t *args, mino_env_t *env)
{
    (void)args; (void)env;
    return mino_float(current_customer.balance);
}

/* --- Rules are mino expressions returning a discount tier keyword ------- */

static const char *rules_src =
    "(def discount-tier\n"
    "  (fn ()\n"
    "    (cond\n"
    "      (> (purchases) 50) :gold\n"
    "      (> (purchases) 10) :silver\n"
    "      (> (age) 65)       :senior\n"
    "      :else              :standard)))\n";

int main(void)
{
    mino_env_t *env = mino_new();
    mino_val_t *result;

    /* Register host accessors. */
    mino_register_fn(env, "age",       host_age);
    mino_register_fn(env, "purchases", host_purchases);
    mino_register_fn(env, "balance",   host_balance);

    /* Load the rules. */
    if (mino_eval_string(rules_src, env) == NULL) {
        fprintf(stderr, "rules error: %s\n", mino_last_error());
        return 1;
    }

    /* Evaluate for different customers. */
    {
        static const customer_t customers[] = {
            { "Alice",  30,  5,  120.0 },
            { "Bob",    70, 12, 3400.0 },
            { "Carol",  25, 55,  800.0 },
        };
        size_t i;
        for (i = 0; i < sizeof(customers)/sizeof(customers[0]); i++) {
            current_customer = customers[i];
            result = mino_eval_string("(discount-tier)", env);
            if (result == NULL) {
                fprintf(stderr, "eval error: %s\n", mino_last_error());
                continue;
            }
            printf("%-8s -> ", customers[i].user);
            mino_println(result);
        }
    }

    mino_env_free(env);
    return 0;
}
