(require "tests/test")

;; Strict-arity coverage: lock in that mino throws on mismatched arity
;; across every fn-call shape (interpreter and BC VM), matching JVM
;; Clojure's behavior. A silent bind-to-nil or drop-extras would let
;; typos and refactors propagate without a stack trace, so we treat
;; this as a guarantee, not an implementation detail.

(deftest fixed-arity-strict
  (is (thrown? ((fn [x y] [x y]) 1)))
  (is (thrown? ((fn [x y] [x y]) 1 2 3)))
  (is (thrown? ((fn [] :ok) :extra)))
  (is (thrown? ((fn [x] x))))
  (is (= :ok ((fn [] :ok))))
  (is (= [1 2] ((fn [x y] [x y]) 1 2))))

(deftest variadic-rest-arity
  (is (thrown? ((fn [x y & rest] [x y rest]) 1)))
  (is (= [1 2 nil] ((fn [x y & rest] [x y rest]) 1 2)))
  (is (= [1 2 '(3)] ((fn [x y & rest] [x y rest]) 1 2 3)))
  ;; Pure variadic fn accepts zero args
  (is (nil? ((fn [& rest] rest))))
  (is (= '(1 2) ((fn [& rest] rest) 1 2))))

(deftest multi-arity-fn
  (let [f (fn ([] :zero)
              ([a] [:one a])
              ([a b] [:two a b]))]
    (is (= :zero (f)))
    (is (= [:one 1] (f 1)))
    (is (= [:two 1 2] (f 1 2)))
    (is (thrown? (f 1 2 3))))
  ;; Mix of fixed clauses + variadic catch-all
  (let [g (fn ([] :zero)
              ([a] [:one a])
              ([a b & rest] [:rest a b (if rest (count rest) 0)]))]
    (is (= :zero (g)))
    (is (= [:one 1] (g 1)))
    (is (= [:rest 1 2 0] (g 1 2)))
    (is (= [:rest 1 2 3] (g 1 2 3 4 5)))))

(deftest defn-arity-strict
  (defn _strict-defn [a b] (+ a b))
  (is (thrown? (_strict-defn 1)))
  (is (thrown? (_strict-defn 1 2 3)))
  (is (= 3 (_strict-defn 1 2)))
  (let [msg (try (_strict-defn 1) (catch e (ex-message e)))]
    (is (some? (re-find #"_strict-defn" msg)))
    (is (some? (re-find #"arity" msg)))))

(deftest apply-arity-strict
  (is (thrown? (apply (fn [x y] [x y]) [1])))
  (is (thrown? (apply (fn [x y] [x y]) [1 2 3])))
  (is (= [1 2] (apply (fn [x y] [x y]) [1 2])))
  ;; apply with rest-arg fn -- not an arity error
  (is (= [1 2 3 4 5]
         (apply (fn [a b & rest] (vec (cons a (cons b rest)))) [1 2 3 4 5]))))

(deftest macro-arity-strict
  (defmacro _strict-mac [a b] `(+ ~a ~b))
  (is (thrown? (eval '(_strict-mac 1))))
  (is (thrown? (eval '(_strict-mac 1 2 3))))
  (is (= 3 (_strict-mac 1 2))))

(deftest map-destructure-arity-strict
  ;; Map-destructured params are still fixed-arity from the dispatcher
  ;; standpoint; calling with the wrong number of arg slots throws.
  (is (thrown? ((fn [{a :a b :b}] [a b]))))
  (is (thrown? ((fn [{a :a b :b}] [a b]) {:a 1 :b 2} :extra)))
  (is (= [1 2] ((fn [{a :a b :b}] [a b]) {:a 1 :b 2})))
  (is (= [1 2] ((fn [{:keys [a b]}] [a b]) {:a 1 :b 2}))))

(deftest vec-destructure-arity-strict
  (is (thrown? ((fn [[a b]] [a b]))))
  (is (thrown? ((fn [[a b]] [a b]) [1 2] :extra)))
  (is (= [1 2] ((fn [[a b]] [a b]) [1 2])))
  ;; A vector destructure with rest binding is permissive on its INNER
  ;; coll (because `&` collects), but the OUTER arity is still strict.
  (is (= [1 [2 3]] ((fn [[a & rest]] [a (vec rest)]) [1 2 3])))
  (is (thrown? ((fn [[a & rest]] [a rest]) [1] :extra))))

(deftest bc-hot-loop-arity-strict
  ;; Run a callee many times to force any JIT/BC fast lane. A regression
  ;; that bypassed arity in the hot path would surface here.
  (defn _hot-fn [x y] (+ x y))
  (dotimes [_ 5000]
    (is (= 3 (_hot-fn 1 2))))
  (is (thrown? (_hot-fn 1)))
  (is (thrown? (_hot-fn 1 2 3))))

(deftest thrown-with-msg-arity
  ;; The thrown-with-msg? assertion (shipped in v0.402.0) also lets
  ;; us probe arity errors by message regex without depending on the
  ;; structured map shape.
  (is (thrown-with-msg? #"arity" ((fn [x] x) 1 2)))
  (is (thrown-with-msg? Exception #"arity" ((fn [x y] x) 1))))

(deftest arity-error-classifies-as-eval-arity
  ;; The thrown value IS the structured diagnostic map (mino's
  ;; `catch e` binds the raw value, not a wrapped ex-info). Lock in
  ;; the :mino/kind and :mino/code keys so port-from-JVM code can
  ;; dispatch on the structured form.
  (let [e (try ((fn [x] x) 1 2)
               (catch e e))]
    (is (map? e))
    (is (= :eval/arity (:mino/kind e)))
    (is (#{"MAR001" "MAR002"} (:mino/code e)))
    (is (string? (:mino/message e)))))
