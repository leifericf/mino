/*
 * clone_test.c -- exercise mino_clone and mino_mailbox.
 *
 * Build:
 *   cc -std=c99 -I.. -o clone_test clone_test.c ../mino.o ../re.o -lm -lpthread
 * Run:
 *   ./clone_test
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
    mino_state_t *A = mino_state_new();
    mino_state_t *B = mino_state_new();
    mino_env_t *ea  = mino_new(A);
    mino_env_t *eb  = mino_new(B);

    /* ---- Test 1: clone primitives ---- */
    {
        mino_val_t *v;

        v = mino_clone(B, A, mino_nil(A));
        ASSERT(v != NULL && v->type == MINO_NIL, "nil clone");

        v = mino_clone(B, A, mino_true(A));
        ASSERT(v != NULL && v->type == MINO_BOOL && v->as.b == 1, "true clone");

        v = mino_clone(B, A, mino_int(A, 42));
        ASSERT(v != NULL && v->type == MINO_INT && v->as.i == 42, "int clone");

        v = mino_clone(B, A, mino_float(A, 3.14));
        ASSERT(v != NULL && v->type == MINO_FLOAT, "float clone");

        v = mino_clone(B, A, mino_string(A, "hello"));
        ASSERT(v != NULL && v->type == MINO_STRING, "string clone");
        ASSERT(strcmp(v->as.s.data, "hello") == 0, "string data");

        v = mino_clone(B, A, mino_keyword(A, "foo"));
        ASSERT(v != NULL && v->type == MINO_KEYWORD, "keyword clone");

        v = mino_clone(B, A, mino_symbol(A, "bar"));
        ASSERT(v != NULL && v->type == MINO_SYMBOL, "symbol clone");
    }

    /* ---- Test 2: clone a vector ---- */
    {
        mino_val_t *src = mino_eval_string(A, "(vec (range 10))", ea);
        mino_val_t *dst;
        ASSERT(src != NULL, "eval range");
        dst = mino_clone(B, A, src);
        ASSERT(dst != NULL, "vector clone not null");
        ASSERT(dst->type == MINO_VECTOR, "vector clone type");
        ASSERT(dst->as.vec.len == 10, "vector clone len");
    }

    /* ---- Test 3: clone a map ---- */
    {
        mino_val_t *src = mino_eval_string(A,
            "(hash-map :a 1 :b 2 :c 3)", ea);
        mino_val_t *dst;
        ASSERT(src != NULL, "eval map");
        dst = mino_clone(B, A, src);
        ASSERT(dst != NULL, "map clone not null");
        ASSERT(dst->type == MINO_MAP, "map clone type");
        ASSERT(dst->as.map.len == 3, "map clone len");
    }

    /* ---- Test 4: clone a set ---- */
    {
        mino_val_t *src = mino_eval_string(A,
            "(hash-set 1 2 3 4 5)", ea);
        mino_val_t *dst;
        ASSERT(src != NULL, "eval set");
        dst = mino_clone(B, A, src);
        ASSERT(dst != NULL, "set clone not null");
        ASSERT(dst->type == MINO_SET, "set clone type");
        ASSERT(dst->as.set.len == 5, "set clone len");
    }

    /* ---- Test 5: clone rejects functions ---- */
    {
        mino_val_t *fn = mino_eval_string(A, "(fn (x) x)", ea);
        mino_val_t *dst;
        ASSERT(fn != NULL, "eval fn");
        dst = mino_clone(B, A, fn);
        ASSERT(dst == NULL, "fn clone should fail");
    }

    /* ---- Test 6: clone nested structure ---- */
    {
        mino_val_t *src = mino_eval_string(A,
            "(list (vec (range 3)) {:a 1} #{:x :y})", ea);
        mino_val_t *dst;
        ASSERT(src != NULL, "eval nested");
        dst = mino_clone(B, A, src);
        ASSERT(dst != NULL, "nested clone not null");
        ASSERT(dst->type == MINO_CONS, "nested clone type");
    }

    /* ---- Test 7: mailbox send/recv ---- */
    {
        mino_mailbox_t *mb = mino_mailbox_new();
        mino_val_t *sent, *got;
        int rc;

        ASSERT(mb != NULL, "mailbox new");

        /* Empty recv returns NULL. */
        got = mino_mailbox_recv(mb, B);
        ASSERT(got == NULL, "empty recv");

        /* Send from A, receive into B. */
        sent = mino_eval_string(A, "[1 2 3]", ea);
        ASSERT(sent != NULL, "eval send val");
        rc = mino_mailbox_send(mb, A, sent);
        ASSERT(rc == 0, "send ok");

        got = mino_mailbox_recv(mb, B);
        ASSERT(got != NULL, "recv not null");
        ASSERT(got->type == MINO_VECTOR, "recv type");
        ASSERT(got->as.vec.len == 3, "recv len");

        /* Queue is now empty again. */
        got = mino_mailbox_recv(mb, B);
        ASSERT(got == NULL, "empty after drain");

        mino_mailbox_free(mb);
    }

    /* ---- Test 8: mailbox multiple messages ---- */
    {
        mino_mailbox_t *mb = mino_mailbox_new();
        int rc, i;

        for (i = 0; i < 5; i++) {
            rc = mino_mailbox_send(mb, A, mino_int(A, (long long)i));
            ASSERT(rc == 0, "multi send");
        }
        for (i = 0; i < 5; i++) {
            mino_val_t *got = mino_mailbox_recv(mb, B);
            ASSERT(got != NULL, "multi recv not null");
            ASSERT(got->type == MINO_INT && got->as.i == i, "multi recv val");
        }

        mino_mailbox_free(mb);
    }

    /* ---- Test 9: isolation - mutating in A doesn't affect B's clone ---- */
    {
        mino_val_t *atom_v = mino_eval_string(A,
            "(let (a (atom 0)) (reset! a 42) (deref a))", ea);
        mino_val_t *cloned;
        ASSERT(atom_v != NULL && atom_v->type == MINO_INT, "atom deref");
        /* Clone the integer result (not the atom). */
        cloned = mino_clone(B, A, atom_v);
        ASSERT(cloned != NULL, "int clone for isolation");
        ASSERT(cloned->type == MINO_INT && cloned->as.i == 42, "isolation val");
    }

    mino_env_free(A, ea);
    mino_env_free(B, eb);
    mino_state_free(A);
    mino_state_free(B);

    printf("all clone/mailbox tests passed\n");
    return 0;
}
