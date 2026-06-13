(require "tests/test")

;; CPJIT inline-stencil <-> interpreter parity oracle.
;;
;; The 16 inlined arith / comparison / unary stencils each have a
;; fast path (inline tag check + arith + overflow check + tagged-int
;; store) and a slow path that bls into mino_jit_*_slow. Both paths
;; must produce results indistinguishable from the interpreter's
;; matching OP_*_II / OP_*_IK / OP_INC_I / etc. handler.
;;
;; The runner under `./mino` and `./mino_nojit` must produce the
;; same stdout bytes; the assertions below pin the literal expected
;; values so any divergence in either binary surfaces in its own
;; output.
;;
;; Each test fn is warmed past MINO_JIT_THRESHOLD (100) before the
;; recorded call so the JIT-enabled binary takes the native path on
;; the assertion. The nojit binary's path is the interpreter for
;; every call.
;;
;; OP_LOOP_INT_LT is intentionally excluded -- the descriptor table
;; omits it (interpreter inline fast path beat the stencil by 17%).
;; Loop-shape parity for OP_LOOP_INT_DEC / _DEC_INC / _LT_INC is
;; covered by the loop-shape section at the bottom of this file;
;; broader loop coverage continues to come from the realistic_bench
;; and diff_random rows.

(def +max+ 1152921504606846975)    ; (1 << 60) - 1
(def +min+ -1152921504606846976)   ; -(1 << 60)
(def +warm+ 200)                   ; > MINO_JIT_THRESHOLD (100)

;; ---- Test helper fns (top-level so the bc compiler emits each
;; ---- body once and the per-fn hot counter accumulates across
;; ---- tests). Each fn body has the matching OP_*_II / OP_*_IK
;; ---- shape the compiler emits when both operands are int-typed
;; ---- and the immediate (where applicable) fits in 8-bit signed.

(defn p-add-ii [a b] (+ a b))
(defn p-sub-ii [a b] (- a b))
(defn p-mul-ii [a b] (* a b))
(defn p-lt-ii  [a b] (< a b))
(defn p-le-ii  [a b] (<= a b))
(defn p-gt-ii  [a b] (> a b))
(defn p-ge-ii  [a b] (>= a b))
(defn p-eq-ii  [a b] (= a b))

(defn p-inc-i      [a] (inc a))
(defn p-dec-i      [a] (dec a))
(defn p-zero-int-p [a] (zero? a))

(defn p-mod-ii  [a b] (mod a b))
(defn p-quot-ii [a b] (quot a b))
(defn p-rem-ii  [a b] (rem a b))

(defn p-band-ii [a b] (bit-and a b))
(defn p-bor-ii  [a b] (bit-or a b))
(defn p-bxor-ii [a b] (bit-xor a b))
(defn p-shl-ii  [a b] (bit-shift-left a b))
(defn p-shr-ii  [a b] (bit-shift-right a b))
(defn p-ushr-ii [a b] (unsigned-bit-shift-right a b))

(defn p-pos-p   [a] (pos? a))
(defn p-neg-p   [a] (neg? a))
(defn p-even-p  [a] (even? a))
(defn p-odd-p   [a] (odd? a))
(defn p-bnot-i  [a] (bit-not a))

;; IK variants: rhs immediate fits in signed 8-bit, lhs comes from a
;; param. The bc compiler folds the literal into the C field of the
;; encoded instruction, so `(+ a 1)` emits OP_ADD_IK with sC=1.
(defn p-add-ik-1   [a] (+ a 1))
(defn p-add-ik-127 [a] (+ a 127))
(defn p-sub-ik-1   [a] (- a 1))
(defn p-sub-ik-128 [a] (- a 128))
(defn p-lt-ik-10   [a] (< a 10))
(defn p-le-ik-10   [a] (<= a 10))
(defn p-eq-ik-7    [a] (= a 7))

(defn- warm-all []
  (dotimes [_ +warm+]
    (p-add-ii 1 2) (p-sub-ii 5 3) (p-mul-ii 4 6)
    (p-lt-ii 1 2)  (p-le-ii 2 2)  (p-gt-ii 3 1)
    (p-ge-ii 3 3)  (p-eq-ii 4 4)
    (p-inc-i 1)    (p-dec-i 1)    (p-zero-int-p 0)
    (p-add-ik-1 3) (p-add-ik-127 3) (p-sub-ik-1 3)
    (p-sub-ik-128 3) (p-lt-ik-10 5) (p-le-ik-10 10) (p-eq-ik-7 7)
    (p-mod-ii 10 3) (p-quot-ii 10 3) (p-rem-ii 10 3)
    (p-band-ii 7 3) (p-bor-ii 4 2) (p-bxor-ii 6 5)
    (p-shl-ii 1 3) (p-shr-ii 16 2) (p-ushr-ii 16 2)
    (p-pos-p 1) (p-neg-p -1) (p-even-p 2) (p-odd-p 3) (p-bnot-i 0)))

(deftest -aaa-warm-all
  ;; Runs first (alphabetical leading-dash); warms every parity fn
  ;; past MINO_JIT_THRESHOLD so the assertions below take the
  ;; JIT-compiled path on `./mino` and the interpreter path on
  ;; `./mino_nojit`.
  (warm-all)
  (is true))

;; ---- Section 1: range boundaries for II / IK arith --------------

(deftest add-ii-normal
  (is (= 12 (p-add-ii 5 7))))

(deftest add-ii-max-no-overflow
  (is (= +max+ (p-add-ii +max+ 0))))

(deftest add-ii-max-overflow
  ;; Promotes to boxed MAX + 1.
  (is (= (inc +max+) (p-add-ii +max+ 1))))

(deftest add-ii-min-underflow
  (is (= (dec +min+) (p-add-ii +min+ -1))))

(deftest sub-ii-normal
  (is (= 3 (p-sub-ii 10 7))))

(deftest sub-ii-max-overflow
  ;; MAX - (-1) overflows to boxed MAX + 1.
  (is (= (inc +max+) (p-sub-ii +max+ -1))))

(deftest sub-ii-min-underflow
  (is (= (dec +min+) (p-sub-ii +min+ 1))))

(deftest mul-ii-normal
  (is (= 42 (p-mul-ii 6 7))))

(deftest mul-ii-overflow-throws
  ;; MAX * MAX overflows past mino's bigint range; both tiers throw.
  (is (thrown? (p-mul-ii +max+ +max+))))

(deftest mul-ii-negative
  (is (= -42 (p-mul-ii -6 7))))

(deftest add-ik-1-max-overflow
  ;; OP_ADD_IK with sC = 1 should overflow the same way OP_ADD_II does.
  (is (= (inc +max+) (p-add-ik-1 +max+))))

(deftest add-ik-127-max-overflow
  (is (= (+ +max+ 127) (p-add-ik-127 +max+))))

(deftest sub-ik-1-min-underflow
  (is (= (dec +min+) (p-sub-ik-1 +min+))))

(deftest sub-ik-128-min-underflow
  (is (= (- +min+ 128) (p-sub-ik-128 +min+))))

;; ---- Section 2: tag-miss for all 16 inlined ops -----------------
;;
;; Each test calls a parity fn with a non-int operand. The JIT's
;; inline tag check observes the non-MINO_TAG_INT and falls through
;; to mino_jit_binop_slow / unop_slow / binop_k_slow; the interpreter
;; takes the same prim_* slow path. Either way the result (or
;; coerced result) is what the canonical prim returns.

(deftest add-ii-tag-miss-double
  ;; prim_add coerces both to double.
  (is (= 6.5 (p-add-ii 5 1.5))))

(deftest sub-ii-tag-miss-double
  (is (= 0.5 (p-sub-ii 2 1.5))))

(deftest mul-ii-tag-miss-double
  (is (= 7.5 (p-mul-ii 3 2.5))))

(deftest lt-ii-tag-miss-double
  (is (= true (p-lt-ii 1 1.5))))

(deftest le-ii-tag-miss-double
  (is (= true (p-le-ii 1.5 2))))

(deftest gt-ii-tag-miss-double
  (is (= true (p-gt-ii 2.5 1))))

(deftest ge-ii-tag-miss-double
  (is (= true (p-ge-ii 2 1.5))))

(deftest eq-ii-tag-miss-double
  ;; mino's `=` is identity-equal across :int / :double, matching
  ;; JVM Clojure -- use `==` for numeric equality.
  (is (= false (p-eq-ii 1 1.0))))

(deftest inc-i-tag-miss-double
  (is (= 2.5 (p-inc-i 1.5))))

(deftest dec-i-tag-miss-double
  (is (= 0.5 (p-dec-i 1.5))))

(deftest zero-int-p-tag-miss-double
  ;; zero? on 0.0 is true regardless of underlying type.
  (is (= true (p-zero-int-p 0.0))))

(deftest add-ik-1-tag-miss-double
  (is (= 2.5 (p-add-ik-1 1.5))))

(deftest sub-ik-1-tag-miss-double
  (is (= 0.5 (p-sub-ik-1 1.5))))

(deftest lt-ik-10-tag-miss-double
  (is (= true (p-lt-ik-10 1.5))))

(deftest le-ik-10-tag-miss-double
  (is (= true (p-le-ik-10 1.5))))

(deftest eq-ik-7-tag-miss-double
  ;; mino's `=` is identity-equal across :int / :double.
  (is (= false (p-eq-ik-7 7.0))))

;; ---- Section 3: comparison-result identity for 8 cmp ops ---------

(deftest lt-ii-equal-false
  (is (= false (p-lt-ii 3 3))))

(deftest lt-ii-max-boundary
  (is (= true (p-lt-ii (dec +max+) +max+))))

(deftest le-ii-equal-true
  (is (= true (p-le-ii 3 3))))

(deftest gt-ii-equal-false
  (is (= false (p-gt-ii 3 3))))

(deftest ge-ii-min-boundary
  (is (= true (p-ge-ii (inc +min+) +min+))))

(deftest eq-ii-max-self
  (is (= true (p-eq-ii +max+ +max+))))

(deftest eq-ii-min-self
  (is (= true (p-eq-ii +min+ +min+))))

(deftest eq-ii-distinct
  (is (= false (p-eq-ii +max+ +min+))))

;; ---- Section 4: unary boundaries (INC_I / DEC_I / ZERO_INT_P) ----

(deftest inc-i-zero
  (is (= 1 (p-inc-i 0))))

(deftest inc-i-max-overflow
  (is (= (inc +max+) (p-inc-i +max+))))

(deftest dec-i-zero
  (is (= -1 (p-dec-i 0))))

(deftest dec-i-min-underflow
  (is (= (dec +min+) (p-dec-i +min+))))

(deftest zero-int-p-zero
  (is (= true (p-zero-int-p 0))))

(deftest zero-int-p-one
  (is (= false (p-zero-int-p 1))))

(deftest zero-int-p-max
  (is (= false (p-zero-int-p +max+))))

(deftest zero-int-p-min
  (is (= false (p-zero-int-p +min+))))

;; ---- Section 4b: mod / quot / rem on tagged ints -----------------

(deftest mod-ii-normal-positive
  (is (= 1 (p-mod-ii 10 3))))

(deftest mod-ii-clojure-sign
  ;; mod follows the divisor's sign (Clojure / Knuth).
  (is (= 2 (p-mod-ii -10 3))))

(deftest mod-ii-divide-by-zero-throws
  (is (thrown? (p-mod-ii 10 0))))

(deftest mod-ii-min-by-neg-one-tag-escape
  ;; The MIN/-1 corner forces the bail-out (the inline divide would
  ;; UB-overflow). prim_mod handles it through the boxed path and
  ;; returns 0 (any number is divisible by -1).
  (is (= 0 (p-mod-ii +min+ -1))))

(deftest quot-ii-normal
  (is (= 3 (p-quot-ii 10 3))))

(deftest quot-ii-truncates-toward-zero
  (is (= -3 (p-quot-ii -10 3))))

(deftest quot-ii-divide-by-zero-throws
  (is (thrown? (p-quot-ii 10 0))))

(deftest rem-ii-normal
  (is (= 1 (p-rem-ii 10 3))))

(deftest rem-ii-c-sign
  ;; rem follows the dividend's sign (C / Java's %).
  (is (= -1 (p-rem-ii -10 3))))

(deftest rem-ii-divide-by-zero-throws
  (is (thrown? (p-rem-ii 10 0))))

;; ---- Section 4c: bitwise int ops ---------------------------------

(deftest band-ii-normal
  (is (= 3 (p-band-ii 7 3))))

(deftest band-ii-all-bits
  (is (= 0 (p-band-ii 0 +max+))))

(deftest bor-ii-normal
  (is (= 7 (p-bor-ii 5 2))))

(deftest bxor-ii-normal
  (is (= 6 (p-bxor-ii 5 3))))

(deftest bxor-ii-self
  (is (= 0 (p-bxor-ii 42 42))))

(deftest shl-ii-normal
  (is (= 8 (p-shl-ii 1 3))))

(deftest shl-ii-overflow-tag-escape
  ;; Shifting MAX-fitting value far enough lands outside the inline
  ;; range; slow path's mino_int_wrap matches the prim's wrap semantics.
  (is (= (p-shl-ii (quot +max+ 2) 2)
         (bit-shift-left (quot +max+ 2) 2))))

(deftest shr-ii-normal
  (is (= 4 (p-shr-ii 16 2))))

(deftest shr-ii-negative
  ;; Arithmetic right shift preserves the sign bit.
  (is (= -1 (p-shr-ii -1 63))))

(deftest ushr-ii-normal
  (is (= 4 (p-ushr-ii 16 2))))

(deftest ushr-ii-negative
  ;; Unsigned right shift of -1 by 1 = 0x7fff... -- outside tagged
  ;; range; slow path's mino_int_wrap matches the prim's semantics.
  (is (= (unsigned-bit-shift-right -1 1) (p-ushr-ii -1 1))))

;; ---- Section 4d: unary int predicates / bnot ---------------------

(deftest pos-p-positive
  (is (= true (p-pos-p 5))))
(deftest pos-p-zero
  (is (= false (p-pos-p 0))))
(deftest pos-p-negative
  (is (= false (p-pos-p -5))))

(deftest neg-p-negative
  (is (= true (p-neg-p -5))))
(deftest neg-p-zero
  (is (= false (p-neg-p 0))))
(deftest neg-p-positive
  (is (= false (p-neg-p 5))))

(deftest even-p-even
  (is (= true (p-even-p 8))))
(deftest even-p-odd
  (is (= false (p-even-p 7))))
(deftest even-p-zero
  (is (= true (p-even-p 0))))

(deftest odd-p-odd
  (is (= true (p-odd-p 7))))
(deftest odd-p-even
  (is (= false (p-odd-p 8))))

(deftest bnot-i-zero
  (is (= -1 (p-bnot-i 0))))
(deftest bnot-i-positive
  (is (= -8 (p-bnot-i 7))))
(deftest bnot-i-negative
  (is (= 7 (p-bnot-i -8))))

;; ---- Section 5: loop-shape parity --------------------------------
;;
;; Each loop fn exercises one of the fused OP_LOOP_INT_* opcodes
;; (DEC, DEC_INC, LT_INC). The interpreter and JIT must produce
;; identical results on:
;;   - the normal exit (zero / lt) path,
;;   - the n=0 / n=1 boundaries,
;;   - the slow-path through prim_dec when the counter would
;;     underflow MINO_TAGGED_INT_MIN.

(defn p-loop-dec [n]
  (loop [i n] (if (zero? i) i (recur (dec i)))))

(defn p-loop-dec-inc [n]
  (loop [i 0 j n] (if (zero? j) i (recur (inc i) (dec j)))))

(defn p-loop-lt-inc [n]
  (loop [i 0 k 0] (if (< i n) (recur (inc i) (inc k)) k)))

(defn- warm-loops []
  (dotimes [_ +warm+]
    (p-loop-dec 100)
    (p-loop-dec-inc 100)
    (p-loop-lt-inc 100)))

(deftest -aab-warm-loops
  (warm-loops)
  (is true))

(deftest loop-dec-zero
  (is (= 0 (p-loop-dec 0))))

(deftest loop-dec-one
  (is (= 0 (p-loop-dec 1))))

(deftest loop-dec-1m
  (is (= 0 (p-loop-dec 1000000))))

(deftest loop-dec-inc-zero
  (is (= 0 (p-loop-dec-inc 0))))

(deftest loop-dec-inc-one
  (is (= 1 (p-loop-dec-inc 1))))

(deftest loop-dec-inc-100
  (is (= 100 (p-loop-dec-inc 100))))

(deftest loop-dec-inc-1m
  (is (= 1000000 (p-loop-dec-inc 1000000))))

(deftest loop-lt-inc-zero
  (is (= 0 (p-loop-lt-inc 0))))

(deftest loop-lt-inc-100
  (is (= 100 (p-loop-lt-inc 100))))

(deftest loop-lt-inc-1m
  (is (= 1000000 (p-loop-lt-inc 1000000))))

;; ---- Section 6: ACC-variant loop parity ---------------------------
;;
;; OP_LOOP_INT_LT_ACC and OP_LOOP_INT_DEC_ACC carry an accumulator
;; register alongside the loop counter. Both the interpreter and the
;; JIT stencil must produce the same accumulated result.

(defn p-loop-lt-acc  [n] (loop [i 0 s 0] (if (< i n) (recur (inc i) (+ s i)) s)))
(defn p-loop-dec-acc [n] (loop [i n s 0] (if (zero? i) s (recur (dec i) (+ s i)))))

(defn- warm-acc-loops []
  (dotimes [_ +warm+]
    (p-loop-lt-acc 100)
    (p-loop-dec-acc 100)))

(deftest -aab-warm-acc-loops
  (warm-acc-loops)
  (is true))

(deftest loop-lt-acc-zero
  (is (= 0 (p-loop-lt-acc 0))))

(deftest loop-lt-acc-one
  (is (= 0 (p-loop-lt-acc 1))))

(deftest loop-lt-acc-10
  (is (= 45 (p-loop-lt-acc 10))))

(deftest loop-lt-acc-1m
  (is (= (reduce + (range 1000000)) (p-loop-lt-acc 1000000))))

(deftest loop-dec-acc-zero
  (is (= 0 (p-loop-dec-acc 0))))

(deftest loop-dec-acc-one
  (is (= 1 (p-loop-dec-acc 1))))

(deftest loop-dec-acc-10
  (is (= 55 (p-loop-dec-acc 10))))

(deftest loop-dec-acc-1m
  (is (= (reduce + (range 1 1000001)) (p-loop-dec-acc 1000000))))

(run-tests-and-exit)
