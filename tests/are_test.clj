(require "tests/test")

;; are macro tests

(deftest are-equality
  (are [x y] (= x y)
    1 1
    2 2
    "a" "a"
    :k :k))

(deftest are-with-computation
  (are [in ex] (= ex (inc in))
    0  1
    1  2
    9  10
    -1 0))

(deftest are-three-bindings
  (are [x y z] (= z (+ x y))
    1  2  3
    10 20 30
    0  0  0))

(deftest are-predicates
  (are [x] (number? x)
    1
    2.5
    -3
    0))

(deftest are-p-thrown
  (are [x] (thrown? (nth [] x))
    0
    1
    99))
