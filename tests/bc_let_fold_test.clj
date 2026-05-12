(require "tests/test")

;; Phase E1: let-binding fold-through. A let binding whose RHS folds
;; at compile time (a literal, or a pure-prim call over folded args)
;; carries a compile-time value on the local. Subsequent pure-prim
;; calls that name the local in argument position substitute the
;; folded value and fold themselves. The bc compiler's `has_folds`
;; bit + `compile_ic_gen` soundness check still gate the resulting
;; OP_LOAD_K against any later redef of `+` / `*` etc, so the
;; semantic answer is the same as if the fold never happened.

(deftest let-fold-through-literal-binding
  (testing "literal binding propagates"
    (defn lf1 [] (let [x 7] (+ x 3)))
    (is (= 10 (lf1))))
  (testing "two literal bindings combine"
    (defn lf2 [] (let [x 7 y 3] (+ x y)))
    (is (= 10 (lf2))))
  (testing "binding shadows outer fold"
    (defn lf3 [] (let [x 5] (let [x 10] x)))
    (is (= 10 (lf3)))))

(deftest let-fold-through-rhs-fold
  (testing "(+ 1 2) on RHS folds, propagates as 3"
    (defn ff1 [] (let [x (+ 1 2)] (* x x)))
    (is (= 9 (ff1))))
  (testing "nested let with intermediate fold"
    (defn ff2 [] (let [a 4
                       b (* a 2)]
                   (- b a)))
    (is (= 4 (ff2))))
  (testing "chain through several bindings"
    (defn ff3 [] (let [a 2
                       b (* a a)
                       c (+ b 1)]
                   (* c c)))
    ;; 2 * 2 = 4; 4 + 1 = 5; 5 * 5 = 25.
    (is (= 25 (ff3)))))

(deftest let-fold-respects-non-foldable
  (testing "fn-call arg doesn't fold but local result still right"
    (defn nf1 [n] (let [x (* n 2)] (+ x 1)))
    (is (= 9 (nf1 4))))
  (testing "mix of foldable and runtime"
    (defn nf2 [n] (let [a 3
                        b (+ a 4)
                        c (* n b)]
                    c))
    ;; a=3, b=7 folded; c=n*7 runtime.
    (is (= 49 (nf2 7)))))

(deftest let-fold-soundness-on-redef
  ;; The fold-through emits OP_LOAD_K, which the redef-invalidates-bc
  ;; soundness check (compile_ic_gen) is supposed to invalidate when
  ;; `+`/`*` is redefined. After redef, the fn recompiles with the
  ;; new resolution.
  (testing "redef of + after compile triggers recompile and new value"
    (defn rd1 [] (let [x (+ 10 20)] x))
    (is (= 30 (rd1)))
    ;; Shadow + with a local subtraction.
    (let [old-plus +]
      (try
        (def + -)
        (is (= -10 (rd1)))
        (finally (def + old-plus))))
    ;; After restore, original semantics return.
    (is (= 30 (rd1)))))
