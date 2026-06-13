(require "tests/test")
(require '[clojure.core.logic :as l
          :refer [run run* fresh == != conde conda condu
                  succeed fail membero appendo conso firsto resto
                  emptyo distincto everyg project all
                  lvar lvar?]])

;; clojure.core.logic -- relational (logic) programming.  The user-facing
;; surface (run / run* / fresh / conde / == / != / the relation library)
;; is identical to upstream core.logic; the substitution, stream, and
;; constraint engine underneath is built natively for mino.  Upstream's
;; own test suite is the behavioral spec.

;; --------------------------------------------------------------------
;; Unification and the query variable
;; --------------------------------------------------------------------

(deftest run-binds-query
  (is (= '(1) (run* [q] (== q 1))))
  (is (= '(:a) (run* [q] (== :a q)))))

(deftest run-fail-yields-nothing
  (is (= '() (run* [q] (== 1 2))))
  (is (= '() (run* [q] fail))))

(deftest run-succeed-leaves-fresh
  ;; An unconstrained query var reifies to _0.
  (is (= '(_0) (run* [q] succeed))))

(deftest unify-vectors
  (is (= '([1 2]) (run* [q] (fresh [x y] (== x 1) (== y 2) (== q [x y])))))
  (is (= '(2) (run* [q] (fresh [x] (== [1 q] [1 2]) (== x q))))))

(deftest unify-maps
  (is (= '(1) (run* [q] (== {:a q} {:a 1})))))

;; --------------------------------------------------------------------
;; conde: disjunction
;; --------------------------------------------------------------------

(deftest conde-branches
  (is (= '(1 2 3)
         (run* [q] (conde
                     [(== q 1)]
                     [(== q 2)]
                     [(== q 3)])))))

(deftest conde-conjunction-in-branch
  (is (= '([1 2])
         (run* [q] (fresh [x y]
                     (conde [(== x 1) (== y 2)])
                     (== q [x y]))))))

;; --------------------------------------------------------------------
;; The relation library
;; --------------------------------------------------------------------

(deftest membero-enumerates
  (is (= '(1 2 3) (run* [q] (membero q [1 2 3]))))
  (is (= '(:x) (run* [q] (== q :x) (membero q [:a :x :z])))))

(deftest membero-as-test
  (is (= '(_0) (run* [q] (membero 2 [1 2 3]))))
  (is (= '() (run* [q] (membero 9 [1 2 3])))))

(deftest conso-firsto-resto
  (is (= '(1) (run* [q] (firsto [1 2 3] q))))
  (is (= '((2 3)) (run* [q] (resto [1 2 3] q))))
  (is (= '((1 2 3)) (run* [q] (conso 1 [2 3] q)))))

(deftest appendo-forward
  (is (= '((1 2 3 4)) (run* [q] (appendo [1 2] [3 4] q)))))

(deftest appendo-backward
  ;; All ways to split a 3-element list.
  (is (= '([() (1 2 3)]
           [(1) (2 3)]
           [(1 2) (3)]
           [(1 2 3) ()])
         (run* [q] (fresh [x y]
                     (appendo x y [1 2 3])
                     (== q [x y]))))))

;; --------------------------------------------------------------------
;; Disequality
;; --------------------------------------------------------------------

(deftest disequality-blocks-unify
  (is (= '() (run* [q] (!= q 1) (== q 1))))
  (is (= '(2) (run* [q] (!= q 1) (== q 2) (== q 2) succeed)))
  (is (= '(2) (run* [q] (!= q 1) (== q 2)))))

(deftest disequality-compound
  ;; q must not be [1 2]; binding it to [1 3] is fine.
  (is (= '([1 3]) (run* [q] (!= q [1 2]) (== q [1 3]))))
  (is (= '() (run* [q] (!= q [1 2]) (== q [1 2])))))

;; --------------------------------------------------------------------
;; Committed choice
;; --------------------------------------------------------------------

(deftest conda-commits-to-first-success
  (is (= '(1) (run* [q] (conda [(== q 1)] [(== q 2)]))))
  (is (= '(2) (run* [q] (conda [fail (== q 1)] [(== q 2)])))))

(deftest condu-head-once
  ;; condu commits to the first clause and its head yields a single answer
  ;; (q bound to 1), rather than enumerating every member.
  (is (= '(1) (run* [q] (condu [(membero q [1 2 3])] [succeed])))))

;; --------------------------------------------------------------------
;; project: reach into bound values
;; --------------------------------------------------------------------

(deftest project-uses-value
  (is (= '(2) (run* [q] (fresh [x] (== x 1) (project [x] (== q (inc x))))))))

;; --------------------------------------------------------------------
;; Completeness: interleaving search must reach answers on the right of a
;; divergent (infinite) branch.  Under naive depth-first concatenation
;; the left branch would loop forever and the answer would never appear.
;; --------------------------------------------------------------------

(deftest interleaving-completeness
  (let [foundo (fn foundo [x]
                 (fresh []
                   (conde
                     ;; A branch that recurses forever without producing x.
                     [(fresh [y] (foundo y) fail)]
                     ;; The productive branch.
                     [(== x :found)])))]
    (is (= '(:found) (run 1 [q] (foundo q))))))

(deftest distincto-and-everyg
  (is (= '(_0) (run* [q] (distincto [1 2 3]))))
  (is (= '() (run* [q] (distincto [1 2 2]))))
  (is (= '(_0) (run* [q] (everyg #(membero % [1 2 3]) [1 2])))))

(run-tests-and-exit)
