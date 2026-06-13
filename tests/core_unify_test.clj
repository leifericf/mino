(require "tests/test")
(require '[clojure.core.unify :as u])

;; First-order syntactic unification. unify returns a binding map that
;; makes two terms equal (or nil); unifier returns the unified term;
;; subst applies a binding map. Logic variables are ?-prefixed symbols.

(deftest unify-var-to-constant
  (is (= '{?x 1} (u/unify '?x 1)))
  (is (= '{?x 1} (u/unify 1 '?x))))

(deftest unify-equal-constants
  (is (= {} (u/unify 1 1)))
  (is (= {} (u/unify :a :a)))
  (is (= {} (u/unify 'foo 'foo))))

(deftest unify-clashing-constants
  (is (nil? (u/unify 1 2)))
  (is (nil? (u/unify :a :b)))
  (is (nil? (u/unify 'foo 'bar))))

(deftest unify-seqs-pairwise
  (is (= '{?x 1 ?y 2} (u/unify '(?x ?y) '(1 2))))
  (is (nil? (u/unify '(?x ?y) '(1 2 3))))
  (is (nil? (u/unify '(?x ?y ?z) '(1 2)))))

(deftest unify-consistency
  (is (= '{?x 1} (u/unify '(?x ?x) '(1 1))))
  (is (nil? (u/unify '(?x ?x) '(1 2)))))

(deftest unify-vectors
  (is (= '{?x 1 ?y 2} (u/unify '[?x ?y ?x] '[1 2 1])))
  (is (nil? (u/unify '[?x ?y ?x] '[1 2 3])))
  (is (= '{?x 1 ?y 2} (u/unify '[?x ?y] '[1 2]))))

(deftest unify-nested
  (is (= '{?x 3} (u/unify '(a (b ?x)) '(a (b 3)))))
  (is (= '{?x 1 ?y 2} (u/unify '(p ?x (q ?y)) '(p 1 (q 2)))))
  (is (nil? (u/unify '(a (b ?x)) '(a (c 3))))))

(deftest unify-occurs-check
  (is (nil? (u/unify '?x '(f ?x))))
  (is (nil? (u/unify '?x '[?x])))
  (is (nil? (u/unify '(g ?x) '(g (f ?x))))))

(deftest unify-var-to-var
  ;; ?x unifies with ?y, then ?y is pinned to 1; both resolve to 1.
  (is (= '{?x 1 ?y 1} (u/unify '(?x ?y 1) '(?y 1 ?x)))))

(deftest unify-wildcard
  (is (= {} (u/unify '_ 1)))
  (is (= {} (u/unify '(_ _) '(1 2))))
  (is (= '{?x 2} (u/unify '(_ ?x) '(1 2)))))

(deftest unify-with-initial-bindings
  (is (= '{?x 1 ?y 2} (u/unify '?y 2 '{?x 1})))
  (is (nil? (u/unify '?x 2 '{?x 1}))))

(deftest unify-maps
  (is (= '{?x 1} (u/unify '{:a ?x} '{:a 1})))
  (is (= '{?x 1 ?y 2} (u/unify '{:a ?x :b ?y} '{:a 1 :b 2})))
  (is (nil? (u/unify '{:a ?x} '{:b 1})))
  (is (nil? (u/unify '{:a 1} '{:a 2}))))

(deftest subst-applies-bindings
  (is (= '(1 2) (u/subst '(?x ?y) '{?x 1 ?y 2})))
  (is (= '[1 2 1] (u/subst '[?x ?y ?x] '{?x 1 ?y 2})))
  (is (= '(a (b 3)) (u/subst '(a (b ?x)) '{?x 3})))
  (is (= '{:a 1} (u/subst '{:a ?x} '{?x 1})))
  ;; Unbound variables pass through unchanged.
  (is (= '(?z 2) (u/subst '(?z ?y) '{?y 2}))))

(deftest unifier-returns-unified-term
  (is (= '(1 2) (u/unifier '(?x ?y) '(1 2))))
  ;; Unifying (?x ?y) with (1 ?z): ?x=1, ?y=?z; substitute into the
  ;; first term, so ?y becomes ?z.
  (is (= '(1 ?z) (u/unifier '(?x ?y) '(1 ?z))))
  (is (= '(a (b 3)) (u/unifier '(a (b ?x)) '(a (b 3)))))
  (is (nil? (u/unifier 1 2)))
  (is (nil? (u/unifier '?x '(f ?x)))))

(deftest flatten-bindings-chases-chains
  ;; ?x -> ?y -> 3 resolves both keys to 3.
  (is (= '{?x 3 ?y 3} (u/flatten-bindings '{?x ?y ?y 3})))
  (is (= '{?x 1} (u/flatten-bindings '{?x 1}))))

(deftest garner-unifiers-accumulates
  (is (= '{?x 1 ?y 2}
         (u/flatten-bindings (u/garner-unifiers '(?x ?y) '(1 2) {}))))
  (is (nil? (u/garner-unifiers 1 2 {}))))

(deftest factory-fns-build-defaults
  ;; A unifier built from the factory over the default predicate behaves
  ;; like the bundled default.
  (let [my-unify (u/make-occurs-unify-fn u/lvar?)]
    (is (= '{?x 1} (my-unify '?x 1)))
    (is (nil? (my-unify '?x '(f ?x)))))
  (let [no-occurs (u/make-unify-fn u/lvar?)]
    ;; With the occurs check off, ?x against (?x) binds rather than fails.
    (is (some? (no-occurs '?x '(?x))))))

(run-tests-and-exit)
