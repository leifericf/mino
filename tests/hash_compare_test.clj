(require "tests/test")

;; hash and compare primitives.

(deftest hash-consistency
  (testing "same value produces same hash"
    (is (= (hash 42) (hash 42)))
    (is (= (hash "hello") (hash "hello")))
    (is (= (hash :foo) (hash :foo)))
    (is (= (hash [1 2 3]) (hash [1 2 3]))))
  (testing "different values usually differ"
    (is (not= (hash 1) (hash 2)))
    (is (not= (hash "a") (hash "b")))))

(deftest hash-types
  (is (number? (hash 42)))
  (is (number? (hash nil)))
  (is (number? (hash "str")))
  (is (number? (hash :kw)))
  (is (number? (hash [1 2])))
  (is (number? (hash {:a 1}))))

(deftest compare-numbers
  (is (= -1 (compare 1 2)))
  (is (= 0 (compare 5 5)))
  (is (= 1 (compare 10 3)))
  (is (= -1 (compare 1.5 2.5)))
  (is (= 0 (compare 1 1.0))))

(deftest compare-strings
  (is (= -1 (compare "a" "b")))
  (is (= 0 (compare "hello" "hello")))
  (is (= 1 (compare "z" "a"))))

(deftest compare-keywords
  (is (= -1 (compare :a :b)))
  (is (= 0 (compare :foo :foo)))
  (is (= 1 (compare :z :a))))

(deftest compare-nil
  (is (= 0 (compare nil nil)))
  (is (= -1 (compare nil 1)))
  (is (= 1 (compare 1 nil))))

;; --- Forcing equality on lazy contents ---

(deftest map-eq-forces-lazy-value
  (is (= {:tail (rest [1 2 3])} {:tail '(2 3)}))
  (is (= {:a 1 :b (rest [10 20 30])} {:a 1 :b '(20 30)}))
  (is (not= {:tail (rest [1 2 3])} {:tail '(2 4)})))

(deftest nested-map-eq-forces-deep
  (is (= {:outer {:inner (rest [9 8 7])}}
         {:outer {:inner '(8 7)}})))

;; --- Cached-hash invariants on immutable collections ---

(deftest equal-implies-equal-hash-vec
  (let [a [1 2 3 4 5]
        b [1 2 3 4 5]]
    (is (= a b))
    (is (= (hash a) (hash b)))))

(deftest equal-implies-equal-hash-map
  (let [a {:k1 1 :k2 2 :k3 3}
        b (-> {} (assoc :k3 3) (assoc :k1 1) (assoc :k2 2))]
    (is (= a b))
    (is (= (hash a) (hash b)))))

(deftest equal-implies-equal-hash-set
  (let [a #{:x :y :z}
        b (-> #{} (conj :z) (conj :x) (conj :y))]
    (is (= a b))
    (is (= (hash a) (hash b)))))

(deftest hash-is-deterministic
  (let [v [10 20 30 40 50 60]
        h1 (hash v)
        h2 (hash v)
        h3 (hash v)]
    (is (= h1 h2))
    (is (= h2 h3))))

(deftest eq-pointer-fast-path
  (let [a {:a 1 :b 2}]
    (is (= a a)))
  (let [v [1 2 3 4]]
    (is (= v v)))
  (let [s #{1 2 3}]
    (is (= s s))))

(deftest eq-differing-content-after-hash
  ;; Build two maps that differ at one deep key. Force both to
  ;; populate their cached hashes via (hash ...), then verify
  ;; structural compare still returns false. Guards against a
  ;; bogus short-circuit on populated mismatched hashes.
  (let [a {:k1 1 :k2 {:inner 42}}
        b {:k1 1 :k2 {:inner 43}}]
    (hash a)
    (hash b)
    (is (not= a b))))

(deftest eq-large-equal-vectors
  (let [build (fn [] (loop [i 0 v []]
                       (if (< i 100) (recur (+ i 1) (conj v i)) v)))
        a (build)
        b (build)]
    (is (= a b))
    (is (= (hash a) (hash b)))))
