(require "tests/test")

;; Bindings: def, let, lexical scope, redefinition.

(deftest def-then-read
  (def x__bt 41)
  (is (= 42 (+ x__bt 1))))

(deftest def-redefine
  (def r__bt 1)
  (is (= 1 r__bt))
  (def r__bt 2)
  (is (= 2 r__bt)))

(deftest let-binding
  (testing "single"
    (is (= 5 (let [x 5] x))))
  (testing "multi"
    (is (= 3 (let [x 1 y 2] (+ x y)))))
  (testing "sequential"
    (is (= 11 (let [x 1 y (+ x 10)] y))))
  (testing "shadow"
    (def a__bt 1)
    (is (= 99 (let [a__bt 99] a__bt)))
    (is (= 1 a__bt))))

(deftest var-redef-closure
  (def vrc-f (fn [] vrc-x))
  (def vrc-x 10)
  (is (= 10 (vrc-f)))
  (def vrc-x 20)
  (is (= 20 (vrc-f))))
