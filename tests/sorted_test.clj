(require "tests/test")

;; Sorted map and sorted set.

;; --- sorted-map ---

(deftest sorted-map-basic
  (is (= 1 (get (sorted-map :a 1 :b 2) :a)))
  (is (= 2 (count (sorted-map :a 1 :b 2))))
  (is (nil? (get (sorted-map :a 1) :z))))

(deftest sorted-map-ordering
  (let [m (sorted-map :c 3 :a 1 :b 2)]
    (is (= '([:a 1] [:b 2] [:c 3]) (seq m)))
    (is (= [:a 1] (first m)))))

(deftest sorted-map-assoc
  (let [m (assoc (sorted-map :a 1) :c 3 :b 2)]
    (is (= 3 (count m)))
    (is (= '([:a 1] [:b 2] [:c 3]) (seq m)))))

(deftest sorted-map-dissoc
  (let [m (dissoc (sorted-map :a 1 :b 2 :c 3) :b)]
    (is (= 2 (count m)))
    (is (= '([:a 1] [:c 3]) (seq m)))))

(deftest sorted-map-conj
  (let [m (conj (sorted-map :a 1) [:c 3] [:b 2])]
    (is (= 3 (count m)))
    (is (= '([:a 1] [:b 2] [:c 3]) (seq m)))))

(deftest sorted-map-contains
  (is (contains? (sorted-map :a 1 :b 2) :a))
  (is (not (contains? (sorted-map :a 1) :z))))

(deftest sorted-map-rest
  (is (= '([:b 2] [:c 3]) (rest (sorted-map :a 1 :b 2 :c 3)))))

(deftest sorted-map-find
  (is (= [:a 1] (find (sorted-map :a 1 :b 2) :a)))
  (is (nil? (find (sorted-map :a 1) :z))))

(deftest sorted-map-into
  (is (= (sorted-map :a 1 :b 2) (into (sorted-map) [[:a 1] [:b 2]]))))

(deftest sorted-map-equality
  (is (= (sorted-map :a 1 :b 2) (sorted-map :b 2 :a 1)))
  (is (not= (sorted-map :a 1) (sorted-map :a 2))))

(deftest sorted-map-numeric-keys
  (let [m (sorted-map 3 "c" 1 "a" 2 "b")]
    (is (= '([1 "a"] [2 "b"] [3 "c"]) (seq m)))))

;; --- sorted-set ---

(deftest sorted-set-basic
  (is (= 3 (count (sorted-set 3 1 2))))
  (is (contains? (sorted-set 1 2 3) 2))
  (is (not (contains? (sorted-set 1 2) 9))))

(deftest sorted-set-ordering
  (is (= '(1 2 3) (seq (sorted-set 3 1 2)))))

(deftest sorted-set-dedup
  (is (= 3 (count (sorted-set 1 2 3 2 1)))))

(deftest sorted-set-conj
  (let [s (conj (sorted-set 3 1) 2 0)]
    (is (= '(0 1 2 3) (seq s)))))

(deftest sorted-set-get
  (is (= 2 (get (sorted-set 1 2 3) 2)))
  (is (nil? (get (sorted-set 1 2 3) 9))))

(deftest sorted-set-first-rest
  (is (= 1 (first (sorted-set 3 1 2))))
  (is (= '(2 3) (rest (sorted-set 3 1 2)))))

(deftest sorted-set-empty
  (is (= 0 (count (sorted-set))))
  (is (nil? (seq (sorted-set)))))

(deftest sorted-set-equality
  (is (= (sorted-set 1 2 3) (sorted-set 3 2 1)))
  (is (not= (sorted-set 1 2) (sorted-set 1 3))))

(deftest sorted-set-type
  (is (= :sorted-set (type (sorted-set))))
  (is (= :sorted-map (type (sorted-map)))))

;; --- sorted-map-by / sorted-set-by ---

(deftest sorted-map-by-custom-order
  (let [rev (fn [a b] (compare b a))
        m   (sorted-map-by rev :a 1 :b 2 :c 3)]
    (is (= '([:c 3] [:b 2] [:a 1]) (seq m)))
    (is (= 3 (count m)))))

(deftest sorted-map-by-assoc-preserves-comparator
  (let [rev (fn [a b] (compare b a))
        m   (assoc (sorted-map-by rev :b 2) :a 1 :c 3)]
    (is (= '([:c 3] [:b 2] [:a 1]) (seq m)))))

(deftest sorted-set-by-custom-order
  (let [rev (fn [a b] (compare b a))
        s   (sorted-set-by rev 1 2 3)]
    (is (= '(3 2 1) (seq s)))))

(deftest sorted-by-rejects-non-fn
  (is (thrown? (sorted-map-by 42 :a 1)))
  (is (thrown? (sorted-set-by :not-a-fn 1 2))))

(deftest sorted-map-natural-still-works
  ;; Anti-pattern check: verifying sorted-map path still works after the
  ;; constructor flip.
  (let [m (sorted-map :c 3 :a 1 :b 2)]
    (is (= '([:a 1] [:b 2] [:c 3]) (seq m)))))

;; --- subseq / rsubseq ---

(deftest subseq-map-lower-bound
  (let [m (apply sorted-map (mapcat (fn [i] [i (* i i)]) (range 6)))]
    (is (= '([3 9] [4 16] [5 25]) (subseq m >= 3)))
    (is (= '([4 16] [5 25])       (subseq m > 3)))))

(deftest subseq-map-upper-bound
  (let [m (apply sorted-map (mapcat (fn [i] [i (* i i)]) (range 6)))]
    (is (= '([0 0] [1 1] [2 4])         (subseq m <= 2)))
    (is (= '([0 0] [1 1])               (subseq m < 2)))))

(deftest subseq-map-range
  (let [m (apply sorted-map (mapcat (fn [i] [i (* i i)]) (range 10)))]
    (is (= '([3 9] [4 16] [5 25] [6 36]) (subseq m >= 3 < 7)))
    (is (= '([3 9] [4 16] [5 25] [6 36] [7 49]) (subseq m >= 3 <= 7)))
    (is (= '([4 16] [5 25] [6 36] [7 49]) (subseq m > 3 <= 7)))))

(deftest subseq-set
  (let [s (sorted-set 1 2 3 4 5)]
    (is (= '(3 4 5) (subseq s >= 3)))
    (is (= '(2 3 4) (subseq s > 1 < 5)))))

(deftest rsubseq-map-lower-bound
  (let [m (apply sorted-map (mapcat (fn [i] [i (* i i)]) (range 6)))]
    (is (= '([5 25] [4 16] [3 9]) (rsubseq m >= 3)))))

(deftest rsubseq-map-range
  (let [m (apply sorted-map (mapcat (fn [i] [i (* i i)]) (range 10)))]
    (is (= '([6 36] [5 25] [4 16] [3 9]) (rsubseq m >= 3 < 7)))))

(deftest rsubseq-set
  (let [s (sorted-set 1 2 3 4 5)]
    (is (= '(4 3 2) (rsubseq s > 1 < 5)))))

(deftest subseq-empty-range
  (let [m (sorted-map 1 :a 5 :b 10 :c)]
    (is (nil? (subseq m > 5 < 5)))
    (is (nil? (subseq m > 100)))))

(deftest subseq-empty-coll
  (is (nil? (subseq (sorted-map) >= 1)))
  (is (nil? (subseq (sorted-set) >= 1))))

(deftest subseq-rejects-bad-args
  (is (thrown? (subseq [1 2 3] >= 1)))       ; not sorted
  (is (thrown? (subseq (sorted-map) map 3))) ; test not comparison
  (is (thrown? (subseq (sorted-map) < 1 > 5)))  ; start-test must be > family
  (is (thrown? (subseq (sorted-map) >= 1 > 5)))) ; end-test must be < family

;; (run-tests) -- called by tests/run.mino
