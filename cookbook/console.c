/*
 * console.c — game scripting console.
 *
 * Demonstrates: mutable host state exposed through handles and host
 * functions, execution limits to prevent runaway scripts, the REPL
 * handle for interactive command entry.
 *
 * Build:  cc -std=c99 -I.. -o console console.c ../mino.c
 * Run:    ./console
 */

#include "mino.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

/* --- Simulated game world ----------------------------------------------- */

typedef struct {
    double x, y;
    int    hp;
    int    max_hp;
    const char *name;
} entity_t;

static entity_t player = { 0.0, 0.0, 100, 100, "player" };

/* --- Host functions ----------------------------------------------------- */

static mino_val_t *host_pos(mino_val_t *args, mino_env_t *env)
{
    mino_val_t *items[2];
    (void)args; (void)env;
    items[0] = mino_float(player.x);
    items[1] = mino_float(player.y);
    return mino_vector(items, 2);
}

static mino_val_t *host_move(mino_val_t *args, mino_env_t *env)
{
    double dx, dy;
    (void)env;
    if (!mino_is_cons(args) || !mino_is_cons(args->as.cons.cdr)) {
        return mino_nil();
    }
    if (!mino_to_float(args->as.cons.car, &dx)) {
        long long ix;
        if (mino_to_int(args->as.cons.car, &ix)) dx = (double)ix;
        else return mino_nil();
    }
    if (!mino_to_float(args->as.cons.cdr->as.cons.car, &dy)) {
        long long iy;
        if (mino_to_int(args->as.cons.cdr->as.cons.car, &iy)) dy = (double)iy;
        else return mino_nil();
    }
    player.x += dx;
    player.y += dy;
    return host_pos(args, env);
}

static mino_val_t *host_hp(mino_val_t *args, mino_env_t *env)
{
    (void)args; (void)env;
    return mino_int(player.hp);
}

static mino_val_t *host_heal(mino_val_t *args, mino_env_t *env)
{
    long long amount;
    (void)env;
    if (mino_is_cons(args) && mino_to_int(args->as.cons.car, &amount)) {
        player.hp += (int)amount;
        if (player.hp > player.max_hp) player.hp = player.max_hp;
    }
    return mino_int(player.hp);
}

static mino_val_t *host_damage(mino_val_t *args, mino_env_t *env)
{
    long long amount;
    (void)env;
    if (mino_is_cons(args) && mino_to_int(args->as.cons.car, &amount)) {
        player.hp -= (int)amount;
        if (player.hp < 0) player.hp = 0;
    }
    return mino_int(player.hp);
}

static mino_val_t *host_status(mino_val_t *args, mino_env_t *env)
{
    mino_val_t *keys[3], *vals[3];
    (void)args; (void)env;
    keys[0] = mino_keyword("pos");
    vals[0] = host_pos(args, env);
    keys[1] = mino_keyword("hp");
    vals[1] = mino_int(player.hp);
    keys[2] = mino_keyword("name");
    vals[2] = mino_string(player.name);
    return mino_map(keys, vals, 3);
}

int main(void)
{
    mino_env_t  *env  = mino_new();

    /* Install I/O so scripts can use println. */
    mino_install_io(env);

    /* Limit scripts to 100k eval steps to prevent infinite loops. */
    mino_set_limit(MINO_LIMIT_STEPS, 100000);

    /* Register game functions. */
    mino_register_fn(env, "pos",    host_pos);
    mino_register_fn(env, "move",   host_move);
    mino_register_fn(env, "hp",     host_hp);
    mino_register_fn(env, "heal",   host_heal);
    mino_register_fn(env, "damage", host_damage);
    mino_register_fn(env, "status", host_status);

    /* Define some convenience commands in mino. */
    mino_eval_string(
        "(def teleport (fn (x y)\n"
        "  (move (- x (first (pos)))\n"
        "        (- y (nth (pos) 1)))))\n"
        "(def full-heal (fn () (heal 9999)))\n",
        env);

    /* Run a scripted demo instead of an interactive loop. */
    printf("=== game console demo ===\n\n");

    {
        static const char *commands[] = {
            "(status)",
            "(move 5 3)",
            "(damage 30)",
            "(heal 10)",
            "(teleport 100 200)",
            "(full-heal)",
            "(status)",
        };
        size_t i;
        for (i = 0; i < sizeof(commands)/sizeof(commands[0]); i++) {
            mino_val_t *result;
            printf("> %s\n", commands[i]);
            result = mino_eval_string(commands[i], env);
            if (result == NULL) {
                printf("error: %s\n", mino_last_error());
            } else {
                mino_println(result);
            }
            printf("\n");
        }
    }

    mino_env_free(env);
    return 0;
}
