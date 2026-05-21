(require "tests/test")

;; Regression test for lazy-seq ns scoping. When a defn in namespace A
;; constructs a (lazy-seq ...) whose body references helper symbols in
;; namespace A as unqualified ns-level identifiers, and the lazy is
;; realized from namespace B, the body must resolve the helpers in
;; namespace A (where they were lexically written) rather than in B.

(eval (read-string "
  (do
    (ns lazy-scope.lib)

    (defn helper-fn [n] (* n 10))

    (defn lazy-double-each
      ;; Body references helper-fn as an unqualified ns-level symbol.
      [coll]
      (lazy-seq
        (when (seq coll)
          (cons (helper-fn (first coll))
                (lazy-double-each (rest coll))))))

    (defn lazy-nested-helper
      ;; Recursive case: lazy realization chains through helper-fn and
      ;; back into lazy-nested-helper itself.
      [n]
      (lazy-seq
        (when (pos? n)
          (cons (helper-fn n)
                (lazy-nested-helper (dec n))))))

    (defn anon-style-lazy
      ;; Lazy body containing a fn literal that calls a ns-level helper.
      [coll]
      (lazy-seq
        (when (seq coll)
          (cons ((fn [x] (helper-fn x)) (first coll))
                (anon-style-lazy (rest coll))))))

    (in-ns 'user))"))

(require '[lazy-scope.lib :refer [lazy-double-each
                                    lazy-nested-helper
                                    anon-style-lazy]])

(deftest lazy-seq-ns-resolves-defining-ns
  ;; Force the lazy from the user namespace. The body must resolve
  ;; helper-fn against lazy-scope.lib, not user.
  (is (= '(10 20 30) (lazy-double-each '(1 2 3))))
  (is (= '(40 30 20 10) (lazy-nested-helper 4))))

(deftest lazy-seq-ns-survives-fresh-ns-switch
  ;; Realize the lazy from a third namespace (created inside the eval
  ;; so we leave the user ns in a clean state for later tests).
  (let [result (eval (read-string "
        (do
          (ns lazy-scope.consumer)
          (require '[lazy-scope.lib :refer [lazy-double-each]])
          (let [r (vec (lazy-double-each [5 6 7]))]
            (in-ns 'user)
            r))"))]
    (is (= [50 60 70] result))))

(deftest lazy-seq-ns-anonymous-fn-inside-lazy
  (is (= '(100 200) (anon-style-lazy '(10 20)))))
