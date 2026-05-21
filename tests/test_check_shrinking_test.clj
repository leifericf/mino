(require "tests/test")
(require '[clojure.test.check :as tc])
(require '[clojure.test.check.properties :as prop])
(require '[clojure.test.check.generators :as gen])

;; Rose-tree shrinking shipped in v0.406.0.
;;
;; Properties that fail produce a :shrunk map with :smallest pointing
;; at the minimal counter-example the walker found, plus :depth and
;; :total-nodes-visited so users can see how aggressive the shrink
;; was. Without shrinking, failures would surface as the
;; whatever-was-randomly-generated value, which is rarely the smallest
;; useful repro.

(deftest int-rose-tree-shape
  (let [r (gen/int-rose 5)]
    (is (= 5 (gen/rose-val r)))
    ;; 5's children include 0 (the simplest), 2 (halved), 4 (n-1).
    (is (some #{0} (clojure.core/map gen/rose-val (gen/rose-children r))))))

(deftest int-rose-tree-zero-is-leaf
  (let [r (gen/int-rose 0)]
    (is (= 0 (gen/rose-val r)))
    (is (empty? (gen/rose-children r)))))

(deftest vector-shrinks-by-dropping
  ;; Property fails when any element is negative. The shrinker should
  ;; reduce to the smallest vector with a negative element.
  (let [result (tc/quick-check 100
                 (prop/for-all [v (gen/vector gen/int)]
                   (every? (fn [x] (>= x 0)) v)))]
    (is (= false (:result result)))
    (is (some? (:shrunk result)))
    ;; The shrunk minimum should still violate the property.
    (let [smallest (-> result :shrunk :smallest first)]
      (is (some neg? smallest))
      ;; And it should be smaller than (or equal to) the original.
      (is (<= (count smallest) (count (first (:failing-args result))))))))

(deftest sorted-vec-shrink
  ;; Property fails when v is not in non-decreasing order. Shrink
  ;; should find the smallest counter-example.
  (let [result (tc/quick-check 200
                 (prop/for-all [v (gen/vector gen/int 2)]
                   (apply <= v)))]
    (is (= false (:result result)))
    (is (some? (:shrunk result)))))

(deftest passing-property-no-shrink
  (let [result (tc/quick-check 100
                 (prop/for-all [n gen/nat] (>= n 0)))]
    (is (= true (:result result)))
    (is (= 100 (:num-tests result)))
    (is (nil? (:shrunk result)))))

(deftest seed-makes-runs-reproducible
  (let [r1 (tc/quick-check 50 (prop/for-all [n gen/int] true) :seed 42)
        r2 (tc/quick-check 50 (prop/for-all [n gen/int] true) :seed 42)]
    (is (= (:seed r1) (:seed r2) 42))))
