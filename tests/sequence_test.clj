(require "tests/test")

;; Sequence operations: map, filter, reduce, take, drop, etc.

(deftest map-fn
  (is (= '(2 4 6) (map (fn [x] (* x 2)) (list 1 2 3))))
  (is (= '(11 21 31) (map (fn [x] (+ x 1)) [10 20 30])))
  (is (= '() (map (fn [x] x) nil))))

(deftest map-over-map
  (is (= '(1 2) (map (fn [kv] (nth kv 1)) {:a 1 :b 2}))))

(deftest filter-fn
  (is (= '(3 4 5) (filter (fn [x] (> x 2)) [1 2 3 4 5])))
  (is (= '() (filter (fn [x] false) [1 2 3]))))

(deftest reduce-fn
  (is (= 15 (reduce + 0 [1 2 3 4 5])))
  (is (= 6 (reduce + [1 2 3])))
  (is (= 42 (reduce + [42]))))

(deftest take-fn
  (is (= '(1 2 3) (take 3 [1 2 3 4 5])))
  (is (= '(1 2) (take 10 [1 2])))
  (is (= '() (take 0 [1 2 3]))))

(deftest drop-fn
  (is (= '(3 4 5) (drop 2 [1 2 3 4 5])))
  (is (= '() (drop 10 [1 2]))))

(deftest range-fn
  (is (= '(0 1 2 3 4) (range 5)))
  (is (= '(2 3 4) (range 2 5)))
  (is (= '(0 3 6 9) (range 0 10 3)))
  (is (= '(5 4 3 2 1) (range 5 0 -1))))

(deftest range-numeric-tower
  (is (= '(0.0 0.25 0.5 0.75) (range 0.0 1.0 0.25)))
  (is (= '(0.5 1.5 2.5) (range 0.5 3)))
  (is (= '(2.0 1.0) (range 2.0 0.0 -1.0)))
  (is (= 11 (count (range 0 1 0.1))))
  (is (= 0.9999999999999999 (last (range 0 1 0.1))))
  (is (= 10 (count (range 0.1 1.0 0.1))))
  (is (= '(0 1 2) (range 2.5)))
  (is (= '(1/2 1 3/2) (range 1/2 2 1/2)))
  (is (= '() (range 1.5 1.5)))
  (is (= '() (range 2.0 1.0)))
  (is (= 3N (first (range 3N 5N))))
  (is (= '(0.0 0.5 1.0) (take 3 (iterate (fn [x] (+ x 0.5)) 0.0))))
  (is (thrown? (range :a)))
  (is (thrown? (range 0 :b)))
  (is (thrown? (range 0 5 "s"))))

(deftest repeat-fn
  (is (= '("x" "x" "x") (repeat 3 "x")))
  (is (= '() (repeat 0 "x"))))

(deftest concat-fn
  (is (= '(1 2 3 4) (concat [1 2] [3 4])))
  (is (= '(1 2 3) (concat [1] [2] [3])))
  (is (= '(1 2) (concat [] [1 2]))))

(deftest reverse-fn
  (is (= '(3 2 1) (reverse (list 1 2 3))))
  (is (= '(3 2 1) (reverse [1 2 3])))
  ;; Per Clojure, (reverse nil) returns () not nil.
  (is (= '() (reverse nil))))

(deftest sort-fn
  (is (= '(1 1 3 4 5) (sort [3 1 4 1 5])))
  (is (= '("a" "b" "c") (sort ["c" "a" "b"])))
  ;; Per Clojure, (sort empty-coll) returns () (the empty-list singleton).
  (is (= '() (sort []))))

(deftest sort-with-comparator
  (testing "boolean comparator"
    (is (= '(1 1 3 4 5) (sort < [3 1 4 1 5])))
    (is (= '(5 4 3 1 1) (sort > [3 1 4 1 5]))))
  (testing "compare-style comparator"
    (is (= '(5 4 3 1 1) (sort (fn [a b] (compare b a)) [3 1 4 1 5]))))
  (testing "string sorting with comparator"
    (is (= '("apple" "banana" "cherry") (sort compare ["banana" "apple" "cherry"])))))

(deftest juxt-fn
  (is (= [6 4] ((juxt inc dec) 5))))

(deftest mapcat-fn
  (is (= '(1 10 2 20 3 30) (mapcat (fn [x] [x (* x 10)]) [1 2 3]))))
