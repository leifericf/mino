(require "tests/test")
(require '[clojure.core.logic :as l
          :refer [run run* fresh != conde conda condu
                  succeed fail membero appendo conso firsto resto
                  emptyo distincto everyg project all
                  matche matcha matchu defne fne
                  defrel fact facts retract tabled
                  lvar lvar?]])

;; clojure.core.logic -- relational (logic) programming.  The user-facing
;; surface (run / run* / fresh / conde / / != / the relation library)
;; is identical to upstream core.logic; the substitution, stream, and
;; constraint engine underneath is built natively for mino.  Upstream's
;; own test suite is the behavioral spec.

;; --------------------------------------------------------------------
;; Unification and the query variable
;; --------------------------------------------------------------------

(deftest run-binds-query
  (is (= '(1) (run* [q] (l/== q 1))))
  (is (= '(:a) (run* [q] (l/== :a q)))))

(deftest run-fail-yields-nothing
  (is (= '() (run* [q] (l/== 1 2))))
  (is (= '() (run* [q] fail))))

(deftest run-succeed-leaves-fresh
  ;; An unconstrained query var reifies to _0.
  (is (= '(_0) (run* [q] succeed))))

(deftest unify-vectors
  (is (= '([1 2]) (run* [q] (fresh [x y] (l/== x 1) (l/== y 2) (l/== q [x y])))))
  (is (= '(2) (run* [q] (fresh [x] (l/== [1 q] [1 2]) (l/== x q))))))

(deftest unify-maps
  (is (= '(1) (run* [q] (l/== {:a q} {:a 1})))))

;; --------------------------------------------------------------------
;; conde: disjunction
;; --------------------------------------------------------------------

(deftest conde-branches
  (is (= '(1 2 3)
         (run* [q] (conde
                     [(l/== q 1)]
                     [(l/== q 2)]
                     [(l/== q 3)])))))

(deftest conde-conjunction-in-branch
  (is (= '([1 2])
         (run* [q] (fresh [x y]
                     (conde [(l/== x 1) (l/== y 2)])
                     (l/== q [x y]))))))

;; --------------------------------------------------------------------
;; The relation library
;; --------------------------------------------------------------------

(deftest membero-enumerates
  (is (= '(1 2 3) (run* [q] (membero q [1 2 3]))))
  (is (= '(:x) (run* [q] (l/== q :x) (membero q [:a :x :z])))))

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
                     (l/== q [x y]))))))

;; --------------------------------------------------------------------
;; Disequality
;; --------------------------------------------------------------------

(deftest disequality-blocks-unify
  (is (= '() (run* [q] (!= q 1) (l/== q 1))))
  (is (= '(2) (run* [q] (!= q 1) (l/== q 2) (l/== q 2) succeed)))
  (is (= '(2) (run* [q] (!= q 1) (l/== q 2)))))

(deftest disequality-compound
  ;; q must not be [1 2]; binding it to [1 3] is fine.
  (is (= '([1 3]) (run* [q] (!= q [1 2]) (l/== q [1 3]))))
  (is (= '() (run* [q] (!= q [1 2]) (l/== q [1 2])))))

;; --------------------------------------------------------------------
;; Committed choice
;; --------------------------------------------------------------------

(deftest conda-commits-to-first-success
  (is (= '(1) (run* [q] (conda [(l/== q 1)] [(l/== q 2)]))))
  (is (= '(2) (run* [q] (conda [fail (l/== q 1)] [(l/== q 2)])))))

(deftest condu-head-once
  ;; condu commits to the first clause and its head yields a single answer
  ;; (q bound to 1), rather than enumerating every member.
  (is (= '(1) (run* [q] (condu [(membero q [1 2 3])] [succeed])))))

;; --------------------------------------------------------------------
;; project: reach into bound values
;; --------------------------------------------------------------------

(deftest project-uses-value
  (is (= '(2) (run* [q] (fresh [x] (l/== x 1) (project [x] (l/== q (inc x))))))))

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
                     [(l/== x :found)])))]
    (is (= '(:found) (run 1 [q] (foundo q))))))

(deftest distincto-and-everyg
  (is (= '(_0) (run* [q] (distincto [1 2 3]))))
  (is (= '() (run* [q] (distincto [1 2 2]))))
  (is (= '(_0) (run* [q] (everyg #(membero % [1 2 3]) [1 2])))))

;; --------------------------------------------------------------------
;; matche / defne pattern-matching relations
;; --------------------------------------------------------------------

(deftest matche-basic
  ;; A matche literal-head clause binds the query var to the literal.
  (is (= '(:a :b) (run* [q] (matche [q] ([:a]) ([:b])))))
  ;; Classify a fixed input by structure; the first matching clause wins
  ;; per branch but conde explores both, so a vector matches the pair
  ;; clause and the wildcard clause.
  (is (= '(:pair :other)
         (run* [out]
           (fresh [x]
             (l/== x [1 2])
             (matche [x]
               ([[_ _]] (l/== out :pair))
               ([_] (l/== out :other))))))))

(defne my-membero [x l]
  ([_ [x . _]])
  ([_ [_ . tail]] (my-membero x tail)))

(deftest defne-membero
  (is (= '(1 2 3) (run* [q] (my-membero q [1 2 3]))))
  (is (= '(_0) (run* [q] (my-membero 2 [1 2 3]))))
  (is (= '() (run* [q] (my-membero 9 [1 2 3])))))

(defne my-appendo [l1 l2 o]
  ([() _ l2])
  ([[a . d] _ [a . r]] (my-appendo d l2 r)))

(deftest defne-appendo
  (is (= '((1 2 3 4)) (run* [q] (my-appendo [1 2] [3 4] q)))))

(deftest fne-anonymous
  (let [heado (fne [l x] ([[x . _] _]))]
    (is (= '(1) (run* [q] (heado [1 2 3] q))))))

;; --------------------------------------------------------------------
;; Facts database: defrel / fact / facts / retract
;; --------------------------------------------------------------------

(defrel parent p c)
(fact parent :gomez :pugsley)
(fact parent :gomez :wednesday)
(fact parent :morticia :pugsley)

(deftest defrel-query
  (is (= '(:pugsley :wednesday)
         (sort (run* [q] (parent :gomez q)))))
  (is (= '(:gomez :morticia)
         (sort (run* [q] (parent q :pugsley))))))

(deftest defrel-retract
  (defrel edge a b)
  (fact edge 1 2)
  (fact edge 2 3)
  (is (= '(2) (run* [q] (edge 1 q))))
  (retract edge 1 2)
  (is (= '() (run* [q] (edge 1 q)))))

(deftest facts-bulk
  (defrel likes a b)
  (facts likes [[:a :b] [:b :c]])
  (is (= '(:b) (run* [q] (likes :a q)))))

;; --------------------------------------------------------------------
;; Tabling: recursion over a cyclic relation must terminate and yield the
;; full (finite) answer set, which plain depth-first search cannot.
;; --------------------------------------------------------------------

(defrel arco x y)
(fact arco :a :b)
(fact arco :b :c)
(fact arco :c :a)   ;; cycle back to :a
(fact arco :c :d)

(def patho
  (tabled [x y]
    (conde
      [(arco x y)]
      [(fresh [z] (arco x z) (patho z y))])))

(deftest tabled-reachability
  (is (= '(:a :b :c :d) (sort (run* [q] (patho :a q)))))
  ;; From :b the cycle reaches everything too.
  (is (= '(:a :b :c :d) (sort (run* [q] (patho :b q))))))

(run-tests-and-exit)
