(require "tests/test")
(require '[clojure.spec.gen.alpha :as sgen])

;; clojure.spec.gen.alpha is the generator wrapper spec uses to build
;; sample data; it delegates to the bundled test.check generators. The
;; tests below favor deterministic generators (return, elements, fmap)
;; and assert membership or shape for the random ones.

(deftest return-yields-its-value
  (is (= 5 (sgen/generate (sgen/return 5))))
  (is (= :x (sgen/generate (sgen/return :x)))))

(deftest sample-count-and-membership
  (let [vs (sgen/sample (sgen/elements [1 2 3]) 10)]
    (is (= 10 (count vs)))
    (is (every? #{1 2 3} vs))))

(deftest fmap-transforms-values
  (is (= 2 (sgen/generate (sgen/fmap inc (sgen/return 1)))))
  (is (every? even?
              (sgen/sample (sgen/fmap #(* 2 %) (sgen/elements [1 2 3])) 20))))

(deftest tuple-pairs-positions
  (let [vs (sgen/sample (sgen/tuple (sgen/return :a) (sgen/return :b)))]
    (is (every? #(= [:a :b] %) vs))))

(deftest such-that-constrains-values
  (let [vs (sgen/sample (sgen/such-that even? (sgen/elements [2 4 6 8])) 15)]
    (is (every? even? vs))))

(deftest one-of-draws-from-its-generators
  (let [vs (sgen/sample (sgen/one-of [(sgen/return :a) (sgen/return :b)]) 20)]
    (is (every? #{:a :b} vs))))

(deftest vector-produces-vectors
  (let [vs (sgen/sample (sgen/vector (sgen/return 0) 3) 5)]
    (is (every? vector? vs))
    (is (every? #(= [0 0 0] %) vs))))

(deftest choose-stays-in-range
  (let [vs (sgen/sample (sgen/choose 5 10) 30)]
    (is (every? #(and (<= 5 %) (<= % 10)) vs))
    (is (every? integer? vs))))

(deftest hash-map-fixed-keys
  (let [vs (sgen/sample (sgen/hash-map :a (sgen/return 1) :b (sgen/return 2)) 5)]
    (is (every? #(= {:a 1 :b 2} %) vs))))

(deftest bind-chains-generators
  (let [g (sgen/bind (sgen/return 3)
                    (fn [n] (sgen/return (* n n))))]
    (is (= 9 (sgen/generate g)))))

(deftest frequency-picks-from-pairs
  (let [vs (sgen/sample
             (sgen/frequency [[1 (sgen/return :rare)] [3 (sgen/return :common)]])
             40)]
    (is (every? #{:rare :common} vs))))

(deftest large-integer-yields-integers
  (is (every? integer? (sgen/sample (sgen/large-integer* {:min 0 :max 9}) 20)))
  (is (every? #(and (<= 0 %) (<= % 9))
              (sgen/sample (sgen/large-integer* {:min 0 :max 9}) 20))))

(deftest scalar-generators-have-expected-shape
  (is (every? boolean? (sgen/sample sgen/boolean 10)))
  (is (every? integer? (sgen/sample sgen/int 10)))
  (is (every? string? (sgen/sample sgen/string 10)))
  (is (every? keyword? (sgen/sample sgen/keyword 10)))
  (is (every? symbol? (sgen/sample sgen/symbol 10)))
  (is (every? uuid? (sgen/sample sgen/uuid 5))))

(deftest qualified-name-generators
  (is (every? #(and (keyword? %) (namespace %))
              (sgen/sample sgen/keyword-ns 10)))
  (is (every? #(and (symbol? %) (namespace %))
              (sgen/sample sgen/symbol-ns 10))))

(run-tests-and-exit)
