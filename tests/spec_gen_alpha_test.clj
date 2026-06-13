(require "tests/test")
(require '[clojure.spec.gen.alpha :as gen])

;; clojure.spec.gen.alpha is the generator wrapper spec uses to build
;; sample data; it delegates to the bundled test.check generators. The
;; tests below favor deterministic generators (return, elements, fmap)
;; and assert membership or shape for the random ones.

(deftest return-yields-its-value
  (is (= 5 (gen/generate (gen/return 5))))
  (is (= :x (gen/generate (gen/return :x)))))

(deftest sample-count-and-membership
  (let [vs (gen/sample (gen/elements [1 2 3]) 10)]
    (is (= 10 (count vs)))
    (is (every? #{1 2 3} vs))))

(deftest fmap-transforms-values
  (is (= 2 (gen/generate (gen/fmap inc (gen/return 1)))))
  (is (every? even?
              (gen/sample (gen/fmap #(* 2 %) (gen/elements [1 2 3])) 20))))

(deftest tuple-pairs-positions
  (let [vs (gen/sample (gen/tuple (gen/return :a) (gen/return :b)))]
    (is (every? #(= [:a :b] %) vs))))

(deftest such-that-constrains-values
  (let [vs (gen/sample (gen/such-that even? (gen/elements [2 4 6 8])) 15)]
    (is (every? even? vs))))

(deftest one-of-draws-from-its-generators
  (let [vs (gen/sample (gen/one-of [(gen/return :a) (gen/return :b)]) 20)]
    (is (every? #{:a :b} vs))))

(deftest vector-produces-vectors
  (let [vs (gen/sample (gen/vector (gen/return 0) 3) 5)]
    (is (every? vector? vs))
    (is (every? #(= [0 0 0] %) vs))))

(deftest choose-stays-in-range
  (let [vs (gen/sample (gen/choose 5 10) 30)]
    (is (every? #(and (<= 5 %) (<= % 10)) vs))
    (is (every? integer? vs))))

(deftest hash-map-fixed-keys
  (let [vs (gen/sample (gen/hash-map :a (gen/return 1) :b (gen/return 2)) 5)]
    (is (every? #(= {:a 1 :b 2} %) vs))))

(deftest bind-chains-generators
  (let [g (gen/bind (gen/return 3)
                    (fn [n] (gen/return (* n n))))]
    (is (= 9 (gen/generate g)))))

(deftest frequency-picks-from-pairs
  (let [vs (gen/sample
             (gen/frequency [[1 (gen/return :rare)] [3 (gen/return :common)]])
             40)]
    (is (every? #{:rare :common} vs))))

(deftest large-integer-yields-integers
  (is (every? integer? (gen/sample (gen/large-integer* {:min 0 :max 9}) 20)))
  (is (every? #(and (<= 0 %) (<= % 9))
              (gen/sample (gen/large-integer* {:min 0 :max 9}) 20))))

(deftest scalar-generators-have-expected-shape
  (is (every? boolean? (gen/sample gen/boolean 10)))
  (is (every? integer? (gen/sample gen/int 10)))
  (is (every? string? (gen/sample gen/string 10)))
  (is (every? keyword? (gen/sample gen/keyword 10)))
  (is (every? symbol? (gen/sample gen/symbol 10)))
  (is (every? uuid? (gen/sample gen/uuid 5))))

(deftest qualified-name-generators
  (is (every? #(and (keyword? %) (namespace %))
              (gen/sample gen/keyword-ns 10)))
  (is (every? #(and (symbol? %) (namespace %))
              (gen/sample gen/symbol-ns 10))))

(run-tests-and-exit)
