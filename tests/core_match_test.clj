(require "tests/test")
(require '[clojure.core.match :refer [match matchv match-let]])

;; clojure.core.match -- pattern matching for mino.
;; The public surface (match / matchv / match-let, and the pattern
;; grammar below) is identical to upstream; only the compiler internals
;; differ.  These tests are the affordance-fidelity gate: every pattern
;; form, the no-match throw, binding semantics, and a wide-dispatch
;; regression that asserts no exponential code blow-up.

;; --------------------------------------------------------------------
;; Literal patterns
;; --------------------------------------------------------------------

(deftest match-int-literals
  (is (= :one  (match [1] [1] :one [2] :two :else :other)))
  (is (= :two  (match [2] [1] :one [2] :two :else :other)))
  (is (= :other (match [3] [1] :one [2] :two :else :other))))

(deftest match-mixed-literals
  (is (= :kw    (match [:a] [:a] :kw :else :no)))
  (is (= :str   (match ["x"] ["x"] :str :else :no)))
  (is (= :true  (match [true] [true] :true :else :no)))
  (is (= :false (match [false] [false] :false :else :no)))
  (is (= :nil   (match [nil] [nil] :nil :else :no))))

(deftest match-multiple-occurrences
  (is (= :both (match [1 2] [1 2] :both [_ _] :other)))
  (is (= :other (match [1 3] [1 2] :both [_ _] :other))))

;; --------------------------------------------------------------------
;; Wildcards and bindings
;; --------------------------------------------------------------------

(deftest match-wildcard
  (is (= :yes (match [42] [_] :yes))))

(deftest match-binding-used-in-rhs
  (is (= 43 (match [42] [n] (inc n))))
  (is (= [2 1] (match [1 2] [a b] [b a]))))

(deftest match-first-row-wins
  ;; Clause order matters: first matching row is taken.
  (is (= :first (match [1] [_] :first [1] :second))))

;; --------------------------------------------------------------------
;; Quoted-symbol literals
;; --------------------------------------------------------------------

(deftest match-quoted-symbol-is-literal
  (is (= :sym (match ['foo] ['foo] :sym :else :no)))
  (is (= :no  (match ['bar] ['foo] :sym :else :no))))

;; --------------------------------------------------------------------
;; Vector patterns
;; --------------------------------------------------------------------

(deftest match-vector-literal
  (is (= :v (match [[1 2 3]] [[1 2 3]] :v :else :no)))
  (is (= :no (match [[1 2 4]] [[1 2 3]] :v :else :no))))

(deftest match-vector-binds
  (is (= [1 3] (match [[1 2 3]] [[a _ c]] [a c]))))

(deftest match-vector-rest
  (is (= [1 [2 3 4]] (match [[1 2 3 4]] [[a & r]] [a (vec r)])))
  (is (= [1 2 [3 4]] (match [[1 2 3 4]] [[a b & r]] [a b (vec r)]))))

(deftest match-vector-nested
  (is (= 2 (match [[1 [2 3]]] [[_ [x _]]] x))))

(deftest match-vector-length-mismatch
  (is (= :no (match [[1 2]] [[_ _ _]] :yes :else :no))))

;; --------------------------------------------------------------------
;; Seq patterns
;; --------------------------------------------------------------------

(deftest match-seq-pattern
  (is (= :s (match [(list 1 2 3)] [([1 2 3] :seq)] :s :else :no)))
  (is (= [1 [2 3]] (match [(list 1 2 3)] [([a & r] :seq)] [a (vec r)]))))

(deftest match-seq-binds
  (is (= [1 2] (match [(list 1 2)] [([a b] :seq)] [a b]))))

;; --------------------------------------------------------------------
;; Map patterns
;; --------------------------------------------------------------------

(deftest match-map-pattern
  (is (= 1 (match [{:a 1 :b 2}] [{:a a}] a)))
  (is (= [1 2] (match [{:a 1 :b 2}] [{:a a :b b}] [a b]))))

(deftest match-map-literal-value
  (is (= :hit (match [{:a 1}] [{:a 1}] :hit :else :no)))
  (is (= :no  (match [{:a 2}] [{:a 1}] :hit :else :no))))

(deftest match-map-missing-key
  (is (= :no (match [{:a 1}] [{:b b}] :yes :else :no))))

(deftest match-map-only
  (is (= :ok (match [{:a 1 :b 2}] [({:a _ :b _} :only [:a :b])] :ok :else :no)))
  ;; :only fails when the map carries keys outside the allowed set.
  (is (= :no (match [{:a 1 :b 2 :c 3}] [({:a _} :only [:a])] :ok :else :no))))

;; --------------------------------------------------------------------
;; Or patterns
;; --------------------------------------------------------------------

(deftest match-or-pattern
  (is (= :hit (match [2] [(:or 1 2 3)] :hit :else :no)))
  (is (= :no  (match [4] [(:or 1 2 3)] :hit :else :no))))

(deftest match-or-nested-in-vector
  (is (= :hit (match [[1 5]] [[1 (:or 4 5 6)]] :hit :else :no))))

;; --------------------------------------------------------------------
;; Guard patterns
;; --------------------------------------------------------------------

(deftest match-guard
  (is (= :even (match [4] [(n :guard even?)] :even :else :odd)))
  (is (= :odd  (match [5] [(n :guard even?)] :even :else :odd))))

(deftest match-guard-vector-of-preds
  (is (= :ok (match [6] [(n :guard [even? pos?])] :ok :else :no)))
  (is (= :no (match [-6] [(n :guard [even? pos?])] :ok :else :no))))

;; --------------------------------------------------------------------
;; As patterns
;; --------------------------------------------------------------------

(deftest match-as-pattern
  (is (= [[1 2 3] 1] (match [[1 2 3]] [([a & _] :as whole)] [(vec whole) a]))))

;; --------------------------------------------------------------------
;; No-match behavior
;; --------------------------------------------------------------------

(deftest match-no-match-throws
  (is (thrown? Exception (match [99] [1] :one [2] :two))))

;; --------------------------------------------------------------------
;; matchv (vector-type tagged match) -- surface-compatible with match
;; --------------------------------------------------------------------

(deftest matchv-basic
  (is (= :two (matchv :clj [2] [1] :one [2] :two :else :no)))
  (is (= [1 2] (matchv :clj [[1 2]] [[a b]] [a b]))))

;; --------------------------------------------------------------------
;; match-let
;; --------------------------------------------------------------------

(deftest match-let-binds
  (is (= [1 2 3]
         (match-let [[a b c] [1 2 3]]
           [a b c]))))

(deftest match-let-nested
  (is (= 2 (match-let [[_ [x _]] [1 [2 3]]]
             x))))

;; --------------------------------------------------------------------
;; Wide-dispatch regression: no exponential blow-up.
;; A wide row set with shared column prefixes must compile and run; if
;; the compiler duplicated downstream code per row the expansion would
;; explode.  We assert correct dispatch across many rows as a behavioral
;; proxy.
;; --------------------------------------------------------------------

(deftest match-wide-dispatch
  (let [classify (fn [a b c]
                   (match [a b c]
                     [1 _ _] :a
                     [_ 2 _] :b
                     [_ _ 3] :c
                     [4 5 6] :d
                     [_ _ _] :none))]
    (is (= :a (classify 1 9 9)))
    (is (= :b (classify 9 2 9)))
    (is (= :c (classify 9 9 3)))
    (is (= :none (classify 9 9 9)))
    (is (= :a (classify 1 2 3)))))   ;; first matching row wins

(deftest match-many-literal-rows
  ;; 20 literal rows over one column: the compiler must dispatch each to
  ;; its own result without mis-binding across the shared occurrence.
  (let [f (fn [x] (match [x]
                    [0] :z0 [1] :z1 [2] :z2 [3] :z3 [4] :z4
                    [5] :z5 [6] :z6 [7] :z7 [8] :z8 [9] :z9
                    [10] :z10 [11] :z11 [12] :z12 [13] :z13 [14] :z14
                    [15] :z15 [16] :z16 [17] :z17 [18] :z18 [19] :z19
                    :else :none))]
    (is (= :z0 (f 0)))
    (is (= :z9 (f 9)))
    (is (= :z19 (f 19)))
    (is (= :none (f 20)))))

(run-tests-and-exit)
