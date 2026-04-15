/*
 * actor_test.c -- exercise the mino_actor API.
 *
 * Build:
 *   cc -std=c99 -I.. -o actor_test actor_test.c ../mino.o ../re.o -lm -lpthread
 * Run:
 *   ./actor_test
 */

#include "mino.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define ASSERT(cond, msg) do { \
    if (!(cond)) { fprintf(stderr, "FAIL: %s\n", msg); return 1; } \
} while (0)

int main(void)
{
    mino_state_t *S = mino_state_new();
    mino_env_t *env = mino_new(S);
    mino_install_io(S, env);

    /* ---- Test 1: create and free an actor ---- */
    {
        mino_actor_t *a = mino_actor_new();
        ASSERT(a != NULL, "actor new");
        ASSERT(mino_actor_state(a) != NULL, "actor state");
        ASSERT(mino_actor_env(a) != NULL, "actor env");
        ASSERT(mino_actor_mailbox(a) != NULL, "actor mailbox");
        mino_actor_free(a);
    }

    /* ---- Test 2: send and receive ---- */
    {
        mino_actor_t *a = mino_actor_new();
        mino_val_t *msg;
        ASSERT(a != NULL, "actor new for send/recv");

        /* Empty recv returns nil. */
        msg = mino_actor_recv(a);
        ASSERT(msg == NULL, "empty recv is NULL");

        /* Send from host state, receive in actor's state. */
        mino_actor_send(a, S, mino_int(S, 42));
        msg = mino_actor_recv(a);
        ASSERT(msg != NULL, "recv not null");
        ASSERT(msg->type == MINO_INT && msg->as.i == 42, "recv value");

        /* Queue is empty again. */
        msg = mino_actor_recv(a);
        ASSERT(msg == NULL, "empty after drain");

        mino_actor_free(a);
    }

    /* ---- Test 3: eval in actor context ---- */
    {
        mino_actor_t *a = mino_actor_new();
        mino_val_t *result;
        ASSERT(a != NULL, "actor new for eval");

        /* Define a function in the actor's state. */
        result = mino_eval_string(mino_actor_state(a),
            "(def double (fn (x) (* x 2)))", mino_actor_env(a));
        ASSERT(result != NULL, "define double");

        /* Send a value, receive it, eval. */
        mino_actor_send(a, S, mino_int(S, 21));
        {
            mino_val_t *msg = mino_actor_recv(a);
            mino_val_t *fn, *call_args, *call_result;
            ASSERT(msg != NULL && msg->type == MINO_INT, "recv for eval");

            fn = mino_env_get(mino_actor_env(a), "double");
            ASSERT(fn != NULL, "lookup double");

            call_args = mino_cons(mino_actor_state(a), msg,
                                  mino_nil(mino_actor_state(a)));
            call_result = mino_call(mino_actor_state(a), fn, call_args,
                                    mino_actor_env(a));
            ASSERT(call_result != NULL, "call double");
            ASSERT(call_result->type == MINO_INT && call_result->as.i == 42,
                   "double 21 = 42");
        }

        mino_actor_free(a);
    }

    /* ---- Test 4: multiple messages in order ---- */
    {
        mino_actor_t *a = mino_actor_new();
        int i;
        ASSERT(a != NULL, "actor new for multi");

        for (i = 0; i < 5; i++) {
            mino_actor_send(a, S, mino_int(S, (long long)i));
        }
        for (i = 0; i < 5; i++) {
            mino_val_t *msg = mino_actor_recv(a);
            ASSERT(msg != NULL && msg->type == MINO_INT && msg->as.i == i,
                   "ordered recv");
        }

        mino_actor_free(a);
    }

    /* ---- Test 5: two actors, independent state ---- */
    {
        mino_actor_t *a1 = mino_actor_new();
        mino_actor_t *a2 = mino_actor_new();
        mino_val_t *r1, *r2;
        ASSERT(a1 != NULL && a2 != NULL, "two actors");

        mino_eval_string(mino_actor_state(a1), "(def x 10)",
                         mino_actor_env(a1));
        mino_eval_string(mino_actor_state(a2), "(def x 20)",
                         mino_actor_env(a2));

        r1 = mino_eval_string(mino_actor_state(a1), "x",
                              mino_actor_env(a1));
        r2 = mino_eval_string(mino_actor_state(a2), "x",
                              mino_actor_env(a2));
        ASSERT(r1 != NULL && r1->type == MINO_INT && r1->as.i == 10,
               "a1 x = 10");
        ASSERT(r2 != NULL && r2->type == MINO_INT && r2->as.i == 20,
               "a2 x = 20");

        mino_actor_free(a1);
        mino_actor_free(a2);
    }

    /* ---- Test 6: spawn from mino code (structured form) ---- */
    {
        mino_val_t *handle = mino_eval_string(S,
            "(spawn (def greet (fn (name) (str \"hello \" name))))",
            env);
        ASSERT(handle != NULL, "spawn returned");
        ASSERT(handle->type == MINO_HANDLE, "spawn returns handle");
        ASSERT(strcmp(handle->as.handle.tag, "actor") == 0, "spawn tag");
    }

    /* ---- Test 7: send! from mino code ---- */
    {
        mino_val_t *result = mino_eval_string(S,
            "(let (a (spawn nil)) (send! a 42))", env);
        ASSERT(result != NULL, "send! returned");
    }

    mino_env_free(S, env);
    mino_state_free(S);

    printf("all actor tests passed\n");
    return 0;
}
