(require "tests/test")
(require '[clojure.core.logic :as l :refer [run run* fresh ==]])
(require '[clojure.core.logic.fd :as fd])

;; clojure.core.logic.fd -- finite-domain constraints.  The surface (in,
;; interval, domain, the arithmetic and relational constraints, distinct,
;; and the eq sugar) matches upstream; the engine narrows enumerated
;; integer domains to a fixpoint and the run machinery labels the
;; remaining variables to enumerate solutions.

(deftest fd-in-and-label
  (is (= '(1 2 3) (run* [q] (fd/in q (fd/interval 1 3)))))
  (is (= '(2 4 6) (run* [q] (fd/in q (fd/domain 2 4 6))))))

(deftest fd-membership-fail
  ;; 7 is outside the declared domain.
  (is (= '() (run* [q] (fresh [] (fd/in q (fd/interval 1 5)) (== q 7))))))

(deftest fd-arithmetic
  (is (= '([1 4] [2 3] [3 2] [4 1])
         (run* [q] (fresh [x y]
                     (fd/in x y (fd/interval 1 4))
                     (fd/+ x y 5)
                     (== q [x y])))))
  (is (= '([2 6] [3 4])
         (run* [q] (fresh [x y]
                     (fd/in x y (fd/interval 1 6))
                     (fd/* x y 12)
                     (fd/< x y)
                     (== q [x y]))))))

(deftest fd-comparisons
  (is (= '([1 4] [2 3])
         (run* [q] (fresh [x y]
                     (fd/in x y (fd/interval 1 4))
                     (fd/+ x y 5)
                     (fd/< x y)
                     (== q [x y])))))
  (is (= '([1 2] [1 3] [2 3] [1 4] [2 4] [3 4])
         (sort-by (fn [[a b]] [b a])
                  (run* [q] (fresh [x y]
                              (fd/in x y (fd/interval 1 4))
                              (fd/< x y)
                              (== q [x y])))))))

(deftest fd-eq-sugar
  (is (= '([1 5] [2 4])
         (run* [q] (fresh [x y]
                     (fd/in x y (fd/interval 1 5))
                     (fd/eq (= (+ x y) 6) (< x y))
                     (== q [x y]))))))

(deftest fd-distinct
  (is (= '(_0) (run* [q] (fresh [x y z]
                           (fd/in x y z (fd/domain 1 2 3))
                           (== x 1) (== y 2) (== z 3)
                           (fd/distinct [x y z])))))
  (is (= '() (run* [q] (fresh [x y]
                         (fd/in x y (fd/domain 5))
                         (fd/distinct [x y]))))))

;; A small all-different placement puzzle: three distinct values in 1..3
;; with a < b < c forces the unique ordering [1 2 3].
(deftest fd-ordered-distinct
  (is (= '([1 2 3])
         (run* [q] (fresh [a b c]
                     (fd/in a b c (fd/interval 1 3))
                     (fd/distinct [a b c])
                     (fd/< a b) (fd/< b c)
                     (== q [a b c]))))))

;; A compact arithmetic puzzle: a+b=c, all distinct, each in 1..5.
(deftest fd-sum-puzzle
  (is (= '([1 2 3] [1 3 4] [1 4 5] [2 1 3] [2 3 5] [3 1 4] [3 2 5] [4 1 5])
         (sort (run* [q] (fresh [a b c]
                           (fd/in a b c (fd/interval 1 5))
                           (fd/+ a b c)
                           (fd/distinct [a b c])
                           (== q [a b c])))))))

(run-tests-and-exit)
