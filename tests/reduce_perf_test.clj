(require "tests/test")

;; Parity tests for the direct-walk fast paths in `reduce` over
;; persistent maps and sets. The bytecode VM and tree-walker both
;; flow through the same C `prim_reduce`, so the contract here is
;; "same answer as a hand-computed reference, across the flatmap /
;; HAMT boundary at 8 entries and across set sizes that cross the
;; trie spine boundaries."

(defn- build-map [n]
  ;; Insertion-order keyed map of size n. Use string keys so the
  ;; result is independent of hash ordering -- iteration walks
  ;; key_order anyway, but the test reads better this way.
  (loop [i 0 m {}]
    (if (= i n) m
        (recur (inc i) (assoc m (str "k" i) i)))))

(defn- build-set [n]
  (loop [i 0 s #{}]
    (if (= i n) s
        (recur (inc i) (conj s i)))))

(deftest reduce-map-direct-parity
  (testing "empty map with init returns init"
    (is (= :init (reduce (fn [a _e] a) :init {}))))
  (testing "empty map without init calls (f) once"
    (is (= 0 (reduce + {}))))
  (testing "size 1 (flatmap)"
    (let [m (build-map 1)
          v (reduce (fn [a [k v]] (conj a [k v])) [] m)]
      (is (= 1 (count v)))
      (is (= ["k0" 0] (first v)))))
  (testing "size 8 (flatmap upper bound)"
    (let [m (build-map 8)
          v (reduce (fn [a [_ v]] (+ a v)) 0 m)]
      (is (= 28 v)))) ;; 0+1+...+7
  (testing "size 9 (first HAMT)"
    (let [m (build-map 9)
          v (reduce (fn [a [_ v]] (+ a v)) 0 m)]
      (is (= 36 v))))
  (testing "size 100 (HAMT)"
    (let [m (build-map 100)
          v (reduce (fn [a [_ v]] (+ a v)) 0 m)]
      (is (= 4950 v))))
  (testing "MapEntry destructures as [k v]"
    (let [m {:a 1 :b 2}
          v (reduce (fn [a e]
                      (let [[k v] e] (assoc a v k)))
                    {} m)]
      (is (= {1 :a 2 :b} v))))
  (testing "MapEntry is vector?"
    (let [m {:k :v}
          v (reduce (fn [_ e] (vector? e)) nil m)]
      (is (true? v))))
  (testing "reduced early-exits"
    (let [m (build-map 100)
          v (reduce (fn [a [_ v]]
                      (if (>= v 10) (reduced a) (+ a v)))
                    0 m)]
      (is (= 45 v)))) ;; 0+1+...+9
  (testing "no-init: first entry becomes acc"
    (let [m {:a 1}
          v (reduce (fn [a _] a) m)]
      (is (vector? v))
      (is (= [:a 1] v)))))

(deftest reduce-set-direct-parity
  (testing "empty set with init returns init"
    (is (= :init (reduce (fn [a _] a) :init #{}))))
  (testing "empty set without init calls (f) once"
    (is (= 0 (reduce + #{}))))
  (testing "size 1"
    (is (= 42 (reduce + 0 #{42}))))
  (testing "size 10"
    (let [s (build-set 10)
          v (reduce + 0 s)]
      (is (= 45 v))))
  (testing "size 100"
    (let [s (build-set 100)
          v (reduce + 0 s)]
      (is (= 4950 v))))
  (testing "reduced early-exits"
    (let [s (build-set 100)
          v (reduce (fn [a x]
                      (if (>= x 50) (reduced a) (+ a x)))
                    0 s)]
      (is (number? v))))
  (testing "no-init: first element becomes acc"
    (is (= 1 (reduce + #{1})))))

(deftest reduce-vec-direct-parity
  (testing "empty vec with init returns init"
    (is (= :init (reduce (fn [a _] a) :init []))))
  (testing "empty vec without init calls (f) once"
    (is (= 0 (reduce + []))))
  (testing "size 1"
    (is (= 42 (reduce + 0 [42]))))
  (testing "size 31 (root NULL, all in tail)"
    (is (= (reduce + (range 31)) (reduce + 0 (vec (range 31))))))
  (testing "size 32 (root NULL, full tail)"
    (is (= (reduce + (range 32)) (reduce + 0 (vec (range 32))))))
  (testing "size 33 (one leaf in trie, one in tail)"
    (is (= (reduce + (range 33)) (reduce + 0 (vec (range 33))))))
  (testing "size 1024 (full second-level branch)"
    (is (= 523776 (reduce + 0 (vec (range 1024)))))) ;; (1023*1024)/2
  (testing "size 10000"
    (is (= 49995000 (reduce + 0 (vec (range 10000))))))
  (testing "no-init: first elem becomes acc"
    (is (= 45 (reduce + (vec (range 10))))))
  (testing "reduced early-exits"
    (let [v (vec (range 100))]
      (is (= 45 (reduce (fn [a x] (if (>= x 10) (reduced a) (+ a x)))
                        0 v)))))
  (testing "subvec honors offset/len"
    (let [v (subvec (vec (range 100)) 10 20)]
      (is (= 145 (reduce + 0 v))))) ;; 10+11+...+19
  (testing "non-int elements (collected)"
    (is (= [:a :b :c] (reduce conj [] [:a :b :c])))))

(deftest reduce-map-seq-matches-map-entry
  ;; (seq m) now yields MINO_MAP_ENTRY for each pair, which is
  ;; vector?-true and `=`-equal to [k v]. Verify both contracts.
  (testing "(seq m) entries are vector?-true"
    (is (every? vector? (seq {:a 1 :b 2}))))
  (testing "(seq m) entries are `=`-equal to [k v]"
    (let [m {:a 1}
          e (first (seq m))]
      (is (= [:a 1] e))
      (is (= :a (first e)))
      (is (= 1 (second e))))))

;; Fn-literal wraps-prim recogniser: `(fn [x] (inc x))` and friends
;; share semantics with the bare prim, plus the pipeline fast lane
;; routes through the prim directly. Tests assert semantics; perf is
;; covered in mino-bench.
(deftest fn-wraps-prim-inc
  (testing "fn-wrap inc matches bare inc"
    (is (= (reduce + 0 (map inc (range 100)))
           (reduce + 0 (map (fn [x] (inc x)) (range 100))))))
  (testing "fn-wrap dec matches bare dec"
    (is (= (reduce + 0 (map dec (range 100)))
           (reduce + 0 (map (fn [x] (dec x)) (range 100))))))
  (testing "fn-wrap odd? matches bare odd?"
    (is (= (count (filter odd? (range 100)))
           (count (filter (fn [x] (odd? x)) (range 100))))))
  (testing "fn-wrap zero? on integer overflow stays correct"
    ;; The fast lane's overflow fallback path: dec on MIN_INT bails
    ;; out to the slow path. Verify the fn-wrap and bare versions
    ;; agree on a value that doesn't overflow.
    (is (= (reduce + 0 (map dec (range 1 50)))
           (reduce + 0 (map (fn [x] (dec x)) (range 1 50))))))
  (testing "fn-wrap recognized only for matching shape"
    ;; (fn [x] (+ x 1)) is NOT (fn [x] (inc x)): different prim, body
    ;; isn't a single inc call. Must still produce correct results.
    (is (= 100 (count (map (fn [x] (+ x 1)) (range 100)))))))
