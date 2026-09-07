/*
 * embed_hpp_test.cpp -- smoke test for the C++ RAII wrappers in mino.hpp.
 *
 * Build (from repo root):
 *   c++ -std=c++14 -Isrc -o embed_hpp_test \
 *       tests/embed_hpp_test.cpp src/SRC.o -lm -lpthread
 * Run: ./embed_hpp_test
 *
 * The load-bearing case is move: mino::state, mino::env, and mino::pin
 * are move-only, and a moved pin or env must still release against the
 * state it was constructed with, even after the state wrapper itself
 * has been moved. A wrapper that dereferenced the state wrapper at
 * destruction time would null-deref once the wrapper moved.
 */

#include "mino.h"
#include "mino.hpp"

#include <cstdio>
#include <string>
#include <utility>

static int failures = 0;

#define REQUIRE(cond, msg)                                         \
    do {                                                           \
        if (!(cond)) {                                             \
            std::fprintf(stderr, "FAIL (%s:%d): %s\n",             \
                         __FILE__, __LINE__, (msg));               \
            failures++;                                            \
        }                                                          \
    } while (0)

/* A pin outlives a move of the state wrapper. The pin was constructed
 * against `s1`; moving `s1` into `s2` transfers ownership of the raw
 * mino_state to `s2`, but the pin must still release its root against
 * the same live runtime. If the pin held a pointer to the state
 * wrapper, `*state_` would read the moved-from wrapper (raw pointer
 * nulled) and mino_unroot would dereference NULL. */
static void test_pin_survives_state_move(void)
{
    /* s2 is declared before the pin so the pin destructs first, while
     * the runtime s2 owns is still alive. The move happens while the
     * pin is alive: with the wrapper-pointer bug the pin's state_ reads
     * the moved-from wrapper (raw pointer nulled) at destruct time and
     * null-derefs; with the raw-pointer fix it releases cleanly. */
    mino::state s2;
    {
        mino::pin   p;
        mino::state s1;
        p = mino::pin(s1, mino_int(s1, 42));
        long long n = 0;
        REQUIRE(mino_to_int(p.get(), &n) && n == 42,
                "pin/move: pin derefs its value before the move");
        s2 = std::move(s1);
        /* s1 is now moved-from; its raw pointer is null. The pin still
         * roots against the live runtime now owned by s2. p destructs
         * at scope end, before s2 frees the runtime. */
    }
    (void)s2;
}

/* An env constructed against a state releases correctly after the state
 * wrapper is moved. Same failure mode as the pin: the env destructor
 * calls mino_env_free(*state_, ptr_). */
static void test_env_survives_state_move(void)
{
    mino::state s2;
    mino::state s1;
    {
        mino::env e(s1);
        e.set("x", mino_int(s1, 7));
        long long n = 0;
        REQUIRE(mino_to_int(e.get("x"), &n) && n == 7,
                "env/move: env reads its binding before the move");
        s2 = std::move(s1);
        /* e destructs at scope end, against the runtime s2 now owns,
         * while both s1 (moved-from, no-op destructor) and s2 are still
         * in scope. With the wrapper-pointer bug e's state_ reads the
         * moved-from s1 wrapper and null-derefs in mino_env_free. */
    }
    (void)s2;
}

/* Moving a pin transfers the root; the source becomes empty and only
 * the destination releases. Exercises pin's move ctor and the raw
 * state capture surviving that move too. */
static void test_pin_move_transfers_root(void)
{
    mino::state s;
    mino::pin   a(s, mino_int(s, 99));
    mino::pin   b(std::move(a));
    REQUIRE(a.empty(), "pin/move: moved-from pin is empty");
    REQUIRE(!b.empty(), "pin/move: moved-to pin holds the root");
    {
        long long n = 0;
        REQUIRE(mino_to_int(b.get(), &n) && n == 99,
                "pin/move: moved-to pin still derefs its value");
    }
}

int main(void)
{
    test_pin_survives_state_move();
    test_env_survives_state_move();
    test_pin_move_transfers_root();

    if (failures == 0) {
        std::printf("embed_hpp_test: all checks passed\n");
        return 0;
    }
    std::fprintf(stderr, "embed_hpp_test: %d failure(s)\n", failures);
    return 1;
}
