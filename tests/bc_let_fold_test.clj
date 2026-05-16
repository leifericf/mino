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

(deftest let-dead-binding-elim
  ;; Phase E2: unused side-effect-free bindings are dropped at
  ;; compile time -- no register, no value emission. Verified
  ;; behaviorally; the contract is "same result as if the binding
  ;; were present, but the binding's expression is never evaluated".
  (testing "unused literal binding is fine"
    (defn de1 [] (let [_unused 7] 42))
    (is (= 42 (de1))))
  (testing "unused pure-prim binding is fine"
    (defn de2 [] (let [_unused (+ 1 2)] 99))
    (is (= 99 (de2))))
  (testing "side-effecting unused binding still runs"
    (let [hits (atom 0)
          run! (fn [] (swap! hits inc) :ok)]
      (defn de3 [] (let [_unused (run!)] :done))
      (de3)
      (is (= 1 @hits))
      (de3) (de3)
      (is (= 3 @hits))))
  (testing "unused symbol binding (pure) is dropped"
    (let [x 7]
      (defn de4 [] (let [_unused x] :result))
      (is (= :result (de4)))))
  (testing "mix of used and unused"
    (defn de5 [] (let [a 10
                       _b (+ 1 2)
                       c 20]
                   (+ a c)))
    (is (= 30 (de5)))))

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

(deftest if-else-fold-error-stays-unreachable
  ;; When an if-form's cond is statically truthy (or a compile-time
  ;; resolvable shape) and the else-branch contains a call that would
  ;; fold to a runtime error (division by zero, shift out of range,
  ;; LLONG_MIN/-1 overflow), the compiler's speculative fold attempt
  ;; on the else expression must NOT escape into the surrounding
  ;; eval context. The unreachable else stays unreachable; the fn
  ;; returns the then-branch value normally.
  (testing "(zero? 0) cond + (quot 1 0) unreachable else"
    (defn iea1 [] (if (zero? 0) 43 (quot 1 0)))
    (is (= 43 (iea1))))
  (testing "let-bound zero divisor under (zero? d) cond"
    (defn iea2 [] (let [d 0] (if (zero? d) 43 (mod 43 d))))
    (is (= 43 (iea2))))
  (testing "rem under (zero? d) cond"
    (defn iea3 [] (let [d 0] (if (zero? d) 99 (rem 100 d))))
    (is (= 99 (iea3))))
  (testing "when guarding an unreachable quot"
    (defn iea4 [] (when (zero? 0) (quot 100 5)))
    (is (= 20 (iea4))))
  (testing "param-based cond doesn't pre-eval else"
    (defn iea5 [c] (if c 43 (quot 1 0)))
    (is (= 43 (iea5 true))))
  (testing "param-false cond hits the runtime quot error"
    (defn iea6 [c] (if c 43 (quot 1 0)))
    (is (thrown? (iea6 false)))))
