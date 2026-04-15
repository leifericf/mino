/*
 * perf_profile.c -- measure core operation costs.
 */

#include "mino.h"
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

static double now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000.0 + ts.tv_nsec / 1e6;
}

#define BENCH(label, count, body) do { \
    double _t0 = now_ms(); \
    int _i; \
    for (_i = 0; _i < (count); _i++) { body; } \
    double _t1 = now_ms(); \
    printf("  %-45s %8d in %7.1fms  (%6.2f us/op)\n", \
           label, count, _t1 - _t0, (_t1 - _t0) * 1000.0 / (count)); \
} while(0)

int main(void)
{
    mino_state_t *S = mino_state_new();
    mino_env_t *env = mino_new(S);
    double t0, t1;

    printf("mino performance profile\n");
    printf("========================\n\n");

    /* --- State/env lifecycle --- */
    printf("Lifecycle:\n");
    BENCH("mino_state_new + state_free", 1000, {
        mino_state_t *s = mino_state_new();
        mino_state_free(s);
    });

    BENCH("mino_state_new + mino_new + free", 100, {
        mino_state_t *s = mino_state_new();
        mino_env_t *e = mino_new(s);
        mino_env_free(s, e);
        mino_state_free(s);
    });

    BENCH("mino_env_clone + free", 1000, {
        mino_env_t *c = mino_env_clone(S, env);
        mino_env_free(S, c);
    });

    /* --- Value construction --- */
    printf("\nValue construction:\n");
    BENCH("mino_int", 100000, {
        (void)mino_int(S, (long long)_i);
    });

    BENCH("mino_string (short)", 100000, {
        (void)mino_string(S, "hello");
    });

    BENCH("mino_symbol (interned)", 100000, {
        (void)mino_symbol(S, "test-sym");
    });

    BENCH("mino_cons", 100000, {
        (void)mino_cons(S, mino_int(S, 1), mino_nil(S));
    });

    /* --- Eval --- */
    printf("\nEval (simple expressions):\n");
    BENCH("eval_string: (+ 1 2)", 10000, {
        (void)mino_eval_string(S, "(+ 1 2)", env);
    });

    BENCH("eval_string: (+ 1 2 3 4 5)", 10000, {
        (void)mino_eval_string(S, "(+ 1 2 3 4 5)", env);
    });

    BENCH("eval_string: (let (x 1) x)", 10000, {
        (void)mino_eval_string(S, "(let (x 1) x)", env);
    });

    BENCH("eval_string: (if true 1 2)", 10000, {
        (void)mino_eval_string(S, "(if true 1 2)", env);
    });

    /* --- Collections --- */
    printf("\nCollections:\n");
    BENCH("eval: (vector 1 2 3 4 5)", 10000, {
        (void)mino_eval_string(S, "(vector 1 2 3 4 5)", env);
    });

    BENCH("eval: (hash-map :a 1 :b 2 :c 3)", 10000, {
        (void)mino_eval_string(S, "(hash-map :a 1 :b 2 :c 3)", env);
    });

    BENCH("eval: (conj [1 2 3] 4)", 10000, {
        (void)mino_eval_string(S, "(conj [1 2 3] 4)", env);
    });

    BENCH("eval: (get {:a 1 :b 2 :c 3} :b)", 10000, {
        (void)mino_eval_string(S, "(get {:a 1 :b 2 :c 3} :b)", env);
    });

    /* --- Larger operations --- */
    printf("\nBulk operations:\n");
    BENCH("eval: (into [] (range 100))", 1000, {
        (void)mino_eval_string(S, "(into [] (range 100))", env);
    });

    BENCH("eval: (reduce + 0 (range 100))", 1000, {
        (void)mino_eval_string(S, "(reduce + 0 (range 100))", env);
    });

    BENCH("eval: (into [] (range 1000))", 100, {
        (void)mino_eval_string(S, "(into [] (range 1000))", env);
    });

    BENCH("eval: (reduce + 0 (range 1000))", 100, {
        (void)mino_eval_string(S, "(reduce + 0 (range 1000))", env);
    });

    BENCH("eval: (count (into {} (map ...))) 100 keys", 100, {
        (void)mino_eval_string(S,
            "(count (reduce (fn (m i) (assoc m (keyword (str \"k\" i)) i)) {} (range 100)))",
            env);
    });

    /* --- Eager variants --- */
    printf("\nEager collection builders:\n");
    BENCH("eval: (rangev 100)", 1000, {
        (void)mino_eval_string(S, "(rangev 100)", env);
    });
    BENCH("eval: (rangev 1000)", 100, {
        (void)mino_eval_string(S, "(rangev 1000)", env);
    });
    BENCH("eval: (mapv inc (rangev 100))", 1000, {
        (void)mino_eval_string(S, "(mapv inc (rangev 100))", env);
    });
    BENCH("eval: (mapv inc (rangev 1000))", 100, {
        (void)mino_eval_string(S, "(mapv inc (rangev 1000))", env);
    });
    BENCH("eval: (filterv odd? (rangev 100))", 1000, {
        (void)mino_eval_string(S, "(filterv odd? (rangev 100))", env);
    });
    BENCH("eval: (reduce + 0 (rangev 1000))", 100, {
        (void)mino_eval_string(S, "(reduce + 0 (rangev 1000))", env);
    });

    /* --- Function calls --- */
    printf("\nFunction calls:\n");
    mino_eval_string(S, "(defn add1 (x) (+ x 1))", env);
    BENCH("eval: (add1 42) -- user fn call", 10000, {
        (void)mino_eval_string(S, "(add1 42)", env);
    });

    mino_eval_string(S, "(defn fib (n) (if (< n 2) n (+ (fib (- n 1)) (fib (- n 2)))))", env);
    BENCH("eval: (fib 20) -- recursive", 10, {
        (void)mino_eval_string(S, "(fib 20)", env);
    });

    BENCH("eval: loop/recur 10000 iterations", 100, {
        (void)mino_eval_string(S,
            "(loop (i 0 acc 0) (if (= i 10000) acc (recur (+ i 1) (+ acc i))))",
            env);
    });

    /* --- Ref/GC --- */
    printf("\nRef/GC:\n");
    BENCH("mino_ref + deref + unref", 100000, {
        mino_ref_t *r = mino_ref(S, mino_int(S, 42));
        (void)mino_deref(r);
        mino_unref(S, r);
    });

    /* --- Clone --- */
    printf("\nClone:\n");
    {
        mino_state_t *dst = mino_state_new();
        mino_val_t *small = mino_eval_string(S, "[1 2 3 4 5]", env);
        mino_val_t *medium = mino_eval_string(S, "(into [] (range 100))", env);
        mino_val_t *nested = mino_eval_string(S,
            "{:a [1 2 3] :b {:c 4 :d [5 6]} :e \"hello\"}", env);

        BENCH("clone: 5-element vector", 10000, {
            (void)mino_clone(dst, S, small);
        });
        BENCH("clone: 100-element vector", 1000, {
            (void)mino_clone(dst, S, medium);
        });
        BENCH("clone: nested map", 10000, {
            (void)mino_clone(dst, S, nested);
        });
        mino_state_free(dst);
    }

    /* --- Mailbox --- */
    printf("\nMailbox:\n");
    {
        mino_state_t *s2 = mino_state_new();
        mino_mailbox_t *mb = mino_mailbox_new();
        BENCH("mailbox: send+recv int", 10000, {
            mino_mailbox_send(mb, S, mino_int(S, 42));
            (void)mino_mailbox_recv(mb, s2);
        });
        BENCH("mailbox: send+recv 5-element vector", 1000, {
            mino_val_t *v = mino_eval_string(S, "[1 2 3 4 5]", env);
            mino_mailbox_send(mb, S, v);
            (void)mino_mailbox_recv(mb, s2);
        });
        mino_mailbox_free(mb);
        mino_state_free(s2);
    }

    /* --- REPL handle --- */
    printf("\nREPL handle:\n");
    {
        mino_repl_t *repl = mino_repl_new(S, env);
        mino_val_t *out;
        BENCH("repl_feed: (+ 1 2)", 10000, {
            mino_repl_feed(repl, "(+ 1 2)", &out);
        });
        mino_repl_free(repl);
    }

    /* --- Reader --- */
    printf("\nReader:\n");
    BENCH("mino_read: simple form", 100000, {
        (void)mino_read(S, "(+ 1 2)", NULL);
    });
    BENCH("mino_read: nested form", 10000, {
        (void)mino_read(S, "(if (< x 10) (+ x 1) (* x 2))", NULL);
    });

    mino_env_free(S, env);
    mino_state_free(S);

    printf("\nDone.\n");
    return 0;
}
