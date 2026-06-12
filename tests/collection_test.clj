(require "tests/test")

;; Collections: lists, vectors, maps, sets, and their operations.

;; --- Lists ---

(deftest list-ops
  (is (= '(1 2 3) (cons 1 '(2 3))))
  (is (= '(1 2 a) (list 1 2 'a)))
  (is (= 1 (car '(1 2 3))))
  (is (= '(2 3) (cdr '(1 2 3))))
  (is (= '(true false) (list true false))))

;; --- Keywords ---

(deftest keywords
  (is (= :foo :foo))
  (is (= :a :a))
  (is (not= :a :b))
  (is (= '(:a :b :c) (list :a :b :c))))

;; --- Vectors ---

(deftest vector-literals
  (is (= [] []))
  (is (= [1 2 3] [1 2 3]))
  (is (= [[1 2] [3 4]] [[1 2] [3 4]])))

(deftest vector-equality
  (is (= [1 2 3] [1 2 3]))
  (is (not= [1 2] [1 2 3])))

(deftest vector-evals
  (def vx__ct 7)
  (is (= [7 8 49] [vx__ct (+ vx__ct 1) (* vx__ct vx__ct)])))

(deftest vector-scale
  (testing "2000 build + nth"
    (let [big (loop [i 0 v []]
               (if (< i 2000) (recur (+ i 1) (conj v i)) v))]
      (is (= 2000 (count big)))
      (is (= 4109 (+ (nth big 0) (nth big 31) (nth big 32) (nth big 1023)
                      (nth big 1024) (nth big 1999))))))
  (testing "assoc shares source"
    (let [big (loop [i 0 v []]
               (if (< i 2000) (recur (+ i 1) (conj v i)) v))
          big2 (assoc big 500 :x)]
      (is (= 500 (nth big 500)))
      (is (= :x (nth big2 500)))
      (is (= (count big) (count big2))))))

;; --- Maps ---

(deftest map-literals
  (is (= {} {}))
  (is (= {:a 1 :b 2} {:a 1 :b 2}))
  (is (= {:a 1 :b 2} {:b 2 :a 1}))
  (is (not= {:a 1} {:a 2})))

(deftest map-evals
  (def mx__ct 7)
  (is (= {:n 7 :sq 49} {:n mx__ct :sq (* mx__ct mx__ct)})))

(deftest map-order
  (is (= '(:z :a :m :b) (keys {:z 1 :a 2 :m 3 :b 4})))
  (is (= '(:z :a :m) (keys (assoc {:z 1 :a 2 :m 3} :a 99))))
  (is (= '(:z :a :b :c) (keys (assoc {:z 1 :a 2} :b 3 :c 4)))))

(deftest map-200-entries
  (let [m (loop [i 0 m {}]
           (if (< i 200) (recur (+ i 1) (assoc m i (* i 10))) m))]
    (is (= 200 (count m)))
    (is (= 0 (get m 0)))
    (is (= 1000 (get m 100)))
    (is (= 1990 (get m 199)))
    (is (= :miss (get m 300 :miss)))))

;; --- Sets ---

(deftest set-literals
  (is (= #{1 2 3} #{1 2 3}))
  (is (thrown? (read-string "#{1 2 2 3}")))
  (is (= 3 (count #{1 2 3})))
  (is (= #{1 2 3} #{3 2 1}))
  (is (= :set (type #{1}))))

(deftest set-ops
  (is (= #{1 2 3} (conj #{1 2} 3)))
  (is (= #{1 2 3} (conj #{1 2} 2 3)))
  (is (= #{1 2 3} (hash-set 1 2 3)))
  (is (set? #{1 2}))
  (is (not (set? [1 2])))
  (is (contains? #{1 2 3} 2))
  (is (not (contains? #{1 2 3} 4)))
  (is (= #{1 3} (disj #{1 2 3} 2)))
  (is (= #{1 3} (disj #{1 2 3 4} 2 4)))
  (is (= :a (get #{:a :b} :a)))
  (is (= nil (get #{:a :b} :c)))
  (is (empty? #{})))

;; --- Collection primitives ---

(deftest count-fn
  (is (= 3 (count [1 2 3])))
  (is (= 2 (count {:a 1 :b 2})))
  (is (= 3 (count (list 1 2 3))))
  (is (= 0 (count nil)))
  (is (= 5 (count "hello"))))

(deftest first-rest
  (is (= 10 (first [10 20 30])))
  (is (= '(20 30) (rest [10 20 30])))
  (is (= nil (first nil)))
  (is (= '() (rest nil))))

(deftest nth-fn
  (is (= 20 (nth [10 20 30] 1)))
  (is (= :none (nth [10 20] 99 :none))))

(deftest constructors
  (is (= [1 2 3] (vector 1 2 3)))
  (is (= {:a 1 :b 2} (hash-map :a 1 :b 2))))

(deftest assoc-fn
  (is (= {:a 1 :b 2} (assoc {:a 1} :b 2)))
  (is (= {:a 99} (assoc {:a 1} :a 99)))
  (is (= [10 99 30] (assoc [10 20 30] 1 99)))
  (is (= [10 20 30] (assoc [10 20] 2 30))))

(deftest dissoc-fn
  (is (= {:a 1 :c 3} (dissoc {:a 1 :b 2 :c 3} :b)))
  (is (= {:b 2} (dissoc {:a 1 :b 2 :c 3} :a :c)))
  (is (= {:a 1} (dissoc {:a 1} :z)))
  (is (= nil (dissoc nil :a))))

(deftest get-fn
  (is (= 1 (get {:a 1 :b 2} :a)))
  (is (= :def (get {:a 1} :no :def)))
  (is (= 20 (get [10 20 30] 1)))
  ;; Per Clojure, indexing a string returns a `\char`.
  (is (= \h (get "hello" 0)))
  (is (= \o (get "hello" 4)))
  (is (= nil (get "hello" 99)))
  (is (= :miss (get "hello" 99 :miss))))

(deftest conj-fn
  (is (= '(4 3 1 2) (conj '(1 2) 3 4)))
  (is (= [1 2 3 4] (conj [1 2] 3 4)))
  (is (= {:a 1 :b 2} (conj {:a 1} [:b 2]))))

(deftest update-fn
  (is (= {:a 50} (update {:a 5} :a (fn [n] (* n 10))))))

(deftest keys-vals
  (is (= '(:a :b) (keys {:a 1 :b 2})))
  (is (= '(1 2) (vals {:a 1 :b 2}))))

(deftest keys-vals-on-seq-of-entries
  ;; JVM Clojure's keys/vals accept any seq whose elements are
  ;; MapEntry or [k v] vectors, so chained map ops like
  ;; (keys (remove pred (frequencies xs))) work without re-into.
  (let [m {:a 1 :b 2 :c 3}
        entries (filter (fn [[_ v]] (odd? v)) m)]
    (is (= #{:a :c} (set (keys entries))))
    (is (= #{1 3}   (set (vals entries)))))
  (is (= '(:a :b) (keys [[:a 1] [:b 2]])))
  (is (= '(1 2)   (vals [[:a 1] [:b 2]])))
  (is (= '(:a :b) (keys (list [:a 1] [:b 2])))))

(deftest contains-fn
  (is (contains? {:a 1} :a))
  (is (contains? [10 20 30] 1)))

(deftest collection-compose
  (def ccm__ct (hash-map :x 1 :y 2))
  (is (= 101 (get (update ccm__ct :x (fn [n] (+ n 100))) :x))))

;; --- Collection utilities ---

(deftest merge-fn
  (is (= {:a 3 :b 2} (merge {:a 1} {:b 2} {:a 3}))))

(deftest select-keys-fn
  (is (= {:a 1 :c 3} (select-keys {:a 1 :b 2 :c 3} [:a :c]))))

(deftest find-fn
  (is (= [:a 1] (find {:a 1 :b 2} :a)))
  (is (= nil (find {:a 1} :z))))

(deftest zipmap-fn
  (is (= {:a 1 :b 2} (zipmap [:a :b] [1 2]))))

(deftest frequencies-fn
  (is (= {1 3, 2 2, 3 1} (frequencies [1 2 1 3 2 1]))))

(deftest group-by-fn
  (is (= {false [1 3], true [2 4]} (group-by even? [1 2 3 4]))))

(deftest into-fn
  (is (= [0 1 2] (into [] (range 3))))
  (is (= #{1 2 3} (into #{} [1 2 2 3])))
  (is (= {:a 1 :b 2} (into {} [[:a 1] [:b 2]])))
  (is (= '(3 2 1) (into (list) [1 2 3]))))

;; --- Small persistent maps ---

(deftest small-map-basic-ops
  (let [m {:a 1 :b 2 :c 3 :d 4}]
    (is (= 4 (count m)))
    (is (= 1 (get m :a)))
    (is (= 4 (get m :d)))
    (is (= nil (get m :z)))
    (is (= :miss (get m :z :miss)))
    (is (contains? m :b))
    (is (not (contains? m :z)))))

(deftest small-map-assoc-then-dissoc
  (let [m0 {:a 1 :b 2 :c 3}
        m1 (assoc m0 :d 4)
        m2 (dissoc m1 :b)]
    (is (= 4 (count m1)))
    (is (= 4 (get m1 :d)))
    (is (= 3 (count m2)))
    (is (= nil (get m2 :b)))
    (is (= 1 (get m2 :a)))
    (is (= 3 (count m0)))))

(deftest small-map-mixed-key-types
  (let [m (assoc {} :kw 1 "str" 2 'sym 3 42 4)]
    (is (= 4 (count m)))
    (is (= 1 (get m :kw)))
    (is (= 2 (get m "str")))
    (is (= 3 (get m 'sym)))
    (is (= 4 (get m 42)))))

(deftest map-cross-threshold-growth
  (let [m (loop [i 0 m {}]
            (if (< i 12) (recur (+ i 1) (assoc m i (* i 10))) m))]
    (is (= 12 (count m)))
    (is (= 0 (get m 0)))
    (is (= 70 (get m 7)))
    (is (= 110 (get m 11)))
    (is (= nil (get m 99)))))

(deftest map-shrink-from-large-keeps-equality
  (let [big    (loop [i 0 m {}]
                 (if (< i 12) (recur (+ i 1) (assoc m i i)) m))
        shrunk (reduce dissoc big [5 6 7 8 9 10 11])
        flat   (zipmap [0 1 2 3 4] [0 1 2 3 4])]
    (is (= 5 (count shrunk)))
    (is (= 0 (get shrunk 0)))
    (is (= 4 (get shrunk 4)))
    (is (= nil (get shrunk 5)))
    (is (= shrunk flat))))

(deftest map-equality-flat-vs-large
  (let [flat  {:a 1 :b 2 :c 3}
        large (loop [i 0 m flat]
                (if (< i 12) (recur (+ i 1) (assoc m (str "k" i) i)) m))
        large2 (reduce dissoc large (map (fn [i] (str "k" i)) (range 12)))]
    (is (= flat large2))
    (is (= (hash flat) (hash large2)))))

(deftest empty-map-ops
  (is (= 0 (count {})))
  (is (= nil (get {} :anything)))
  (is (= {:a 1} (assoc {} :a 1)))
  (is (= {} (dissoc {} :nope)))
  (is (= nil (keys {})))
  (is (= nil (vals {}))))

(deftest small-map-insertion-order
  (let [m {:z 1 :a 2 :m 3 :b 4}]
    (is (= '(:z :a :m :b) (keys m)))
    (is (= '(1 2 3 4) (vals m)))
    (is (= '([:z 1] [:a 2] [:m 3] [:b 4])
           (map (fn [e] [(key e) (val e)]) (seq m))))))

(deftest small-map-meta-preservation
  (let [m (with-meta {:a 1 :b 2} {:tag :small})]
    (is (= {:tag :small} (meta m)))
    (is (= {:tag :small} (meta (assoc m :c 3))))
    (is (= {:tag :small} (meta (dissoc m :a))))))

(deftest large-map-meta-preservation
  (let [big (loop [i 0 m {}]
              (if (< i 12) (recur (+ i 1) (assoc m i i)) m))
        tagged (with-meta big {:tag :large})]
    (is (= {:tag :large} (meta tagged)))
    (is (= {:tag :large} (meta (assoc tagged 100 100))))
    (is (= {:tag :large} (meta (dissoc tagged 5))))))

(deftest assoc-noop-short-circuit
  ;; (assoc m k existing-v) returns m unchanged (`identical?`-eq).
  ;; Flatmap and HAMT layouts both honor the short-circuit.
  (testing "flatmap: replace with same value is identical?"
    (let [m {:a 1 :b 2}]
      (is (identical? m (assoc m :a 1)))
      (is (identical? m (assoc m :a (get m :a))))))
  (testing "HAMT: replace with same value is identical?"
    (let [big (loop [i 0 m {}]
                (if (< i 50) (recur (inc i) (assoc m i i)) m))]
      (is (identical? big (assoc big 5 5)))
      (is (identical? big (assoc big 5 (get big 5))))))
  (testing "replace with different value returns new map"
    (let [m {:a 1}]
      (is (not (identical? m (assoc m :a 2))))
      (is (= {:a 2} (assoc m :a 2)))))
  (testing "absent key returns new map"
    (let [m {:a 1}]
      (is (not (identical? m (assoc m :b 2))))
      (is (= {:a 1 :b 2} (assoc m :b 2)))))
  (testing "= uses Clojure's tier-strict semantics"
    ;; (= 1 1.0) is false in Clojure, so 1 and 1.0 are NOT treated
    ;; as the same value here; the assoc returns a new map. The
    ;; no-op short-circuit triggers on mino_eq equality, which is
    ;; the same predicate `=` exposes to script-level callers.
    (let [m {:a 1}]
      (is (not (identical? m (assoc m :a 1.0)))))))

(deftest conj-set-noop-short-circuit
  ;; (conj s x) where x is already in s returns s unchanged.
  (testing "already-present: identical?"
    (let [s #{:a :b :c}]
      (is (identical? s (conj s :a)))
      (is (identical? s (conj s :c)))))
  (testing "absent element: new set"
    (let [s #{:a}]
      (is (not (identical? s (conj s :b))))
      (is (= #{:a :b} (conj s :b)))))
  (testing "size up to HAMT boundary"
    (let [s (loop [i 0 s #{}]
              (if (< i 50) (recur (inc i) (conj s i)) s))]
      (is (identical? s (conj s 25))))))

(deftest disj-set-noop-short-circuit
  ;; (disj s x) where x is absent returns s unchanged. The existing
  ;; prim_disj implementation already inherits this from the
  ;; "only rebuild on hit" structure; the test pins that contract.
  (testing "absent element: identical?"
    (let [s #{:a :b :c}]
      (is (identical? s (disj s :z)))))
  (testing "present element: new set"
    (let [s #{:a :b}]
      (is (not (identical? s (disj s :a))))
      (is (= #{:b} (disj s :a))))))

(deftest sorted-map-dissoc-missing-noop
  (testing "absent key: count and content preserved"
    (let [m (sorted-map :a 1 :b 2)
          m2 (dissoc m :z)]
      (is (= 2 (count m2)))
      (is (= m m2))
      (is (identical? m m2))))
  (testing "absent key: hash agrees with equality"
    (let [m (sorted-map :a 1 :b 2)]
      (is (= (hash m) (hash (dissoc m :z))))))
  (testing "repeated absent dissocs do not underflow count"
    (let [m (sorted-map :a 1 :b 2 :c 3)
          m2 (reduce dissoc m (range 100))]
      (is (= 3 (count m2)))
      (is (= m m2))))
  (testing "single-element sorted-map, dissoc missing"
    (let [m (sorted-map :only 1)]
      (is (= 1 (count (dissoc m :missing))))
      (is (identical? m (dissoc m :missing)))))
  (testing "empty sorted-map, dissoc missing"
    (let [m (sorted-map)]
      (is (= 0 (count (dissoc m :missing))))
      (is (identical? m (dissoc m :missing))))))

(deftest sorted-set-disj-missing-noop
  (testing "absent element: count and content preserved"
    (let [s (sorted-set 1 2 3)
          s2 (disj s 99)]
      (is (= 3 (count s2)))
      (is (= s s2))
      (is (identical? s s2))))
  (testing "repeated absent disj does not underflow count"
    (let [s (sorted-set 10 20 30)
          s2 (reduce disj s (range 100 200))]
      (is (= 3 (count s2)))
      (is (= s s2))))
  (testing "empty sorted-set, disj missing"
    (let [s (sorted-set)]
      (is (= 0 (count (disj s :missing))))
      (is (identical? s (disj s :missing))))))

(deftest sorted-map-hash-honors-equality
  (testing "two equal sorted-maps hash equal"
    (let [sm1 (sorted-map :a 1 :b 2)
          sm2 (sorted-map :a 1 :b 2)]
      (is (= sm1 sm2))
      (is (= (hash sm1) (hash sm2)))))
  (testing "sorted-map and hash-map with same content hash equal"
    (let [hm {:a 1 :b 2}
          sm (sorted-map :a 1 :b 2)]
      (is (= hm sm))
      (is (= (hash hm) (hash sm)))))
  (testing "sorted-map keys round-trip through HAMT-sized hash-map"
    (let [sm-a   (sorted-map :a 1)
          filler (zipmap (range 100) (range 100))
          m      (assoc filler sm-a :A-val)]
      (is (= :A-val (get m (sorted-map :a 1))))))
  (testing "empty sorted-map hashes equal to empty hash-map"
    (is (= (hash {}) (hash (sorted-map))))))

(deftest sorted-set-hash-honors-equality
  (testing "two equal sorted-sets hash equal"
    (let [ss1 (sorted-set 1 2 3)
          ss2 (sorted-set 1 2 3)]
      (is (= ss1 ss2))
      (is (= (hash ss1) (hash ss2)))))
  (testing "sorted-set and hash-set with same content hash equal"
    (let [hs #{1 2 3}
          ss (sorted-set 1 2 3)]
      (is (= hs ss))
      (is (= (hash hs) (hash ss)))))
  (testing "sorted-set element round-trips through HAMT-sized hash-map"
    (let [ss-a   (sorted-set :a :b)
          filler (zipmap (range 100) (range 100))
          m      (assoc filler ss-a :found)]
      (is (= :found (get m (sorted-set :a :b))))))
  (testing "empty sorted-set hashes equal to empty hash-set"
    (is (= (hash #{}) (hash (sorted-set))))))

(deftest sequential-hash-honors-equality
  (testing "empty sequentials hash equal"
    (is (= (hash []) (hash ())))
    (is (= (hash []) (hash (list))))
    ;; Forcing a lazy-seq via `seq` realizes it; the hash then sees an
    ;; empty sequence rather than the unrealized thunk.
    (let [l (lazy-seq nil)]
      (seq l)
      (is (= (hash []) (hash l)))))
  (testing "non-empty: vector, list, seq-of-vector, cons-chain hash equal"
    (let [v  [0 1 2]
          l  (list 0 1 2)
          sv (seq [0 1 2])
          cc (cons 0 (cons 1 (cons 2 nil)))]
      (is (= v l sv cc))
      (is (= (hash v) (hash l)))
      (is (= (hash v) (hash sv)))
      (is (= (hash v) (hash cc)))))
  (testing "forced lazy seq hashes equal to vector with same content"
    (let [r (range 3)
          _ (doall r)
          v [0 1 2]]
      (is (= v r))
      (is (= (hash v) (hash r)))))
  (testing "sequential as HAMT-sized hash-map key, looked up by equal but distinct sequential"
    (let [v      [0 1 2]
          filler (zipmap (range 100) (range 100))
          m      (assoc filler v :found)]
      (is (= :found (get m [0 1 2])))
      (is (= :found (get m (list 0 1 2))))
      (is (= :found (get m (seq [0 1 2]))))
      (is (= :found (get m (cons 0 (cons 1 (cons 2 nil))))))))
  (testing "hash is independent of sequential representation for nested content"
    (let [v  [[1 2] [3 4]]
          l  (list (list 1 2) (list 3 4))]
      (is (= v l))
      (is (= (hash v) (hash l))))))

(deftest map-seq-paths-yield-map-entries
  ;; Every seq view of a map yields map entries, regardless of the
  ;; map flavor or the path that built the seq.
  (is (= [:map-entry :map-entry] (mapv type (seq (sorted-map 1 :a 2 :b)))))
  (is (= '(1 2) (map key (sorted-map 1 :a 2 :b))))
  (is (= '(:a :b) (map val (sorted-map 1 :a 2 :b))))
  (is (= :map-entry (type (first (rseq (sorted-map 1 :a))))))
  (is (= '(2) (map key (subseq (sorted-map 1 :a 2 :b) > 1))))
  (is (= '(1) (map key (rsubseq (sorted-map 1 :a 2 :b) < 2))))
  (is (= :map-entry (type (second (cons 0 {:a 1}))))))

;; --- Queues ---

(deftest queue-collection-contract
  (let [q (conj clojure.lang.PersistentQueue/EMPTY 1 2)]
    (is (coll? q))
    (is (counted? q))
    (is (sequential? q))
    (is (not (seq? q)))
    ;; Sequential equality and the matching hash.
    (is (= q [1 2]))
    (is (= q (list 1 2)))
    (is (= (hash q) (hash [1 2])))
    (is (not= q [1 3]))
    ;; into via conj.
    (is (= [1 2 3 4] (vec (into q [3 4]))))
    ;; Popping an empty queue yields an empty queue.
    (is (= 0 (count (pop clojure.lang.PersistentQueue/EMPTY))))
    (is (= clojure.lang.PersistentQueue/EMPTY
           (pop clojure.lang.PersistentQueue/EMPTY)))))

(deftest conj-merges-maps-into-any-map-flavor
  (is (= {1 :a 3 :c 5 :e} (merge (sorted-map 1 :a) {5 :e 3 :c})))
  (is (= {1 :a 2 :b} (conj (sorted-map 1 :a) {2 :b})))
  (is (= {1 :a 2 :b} (conj {1 :a} (sorted-map 2 :b))))
  (is (= '(2 1) (keys (conj (sorted-map-by > 1 :a) (sorted-map 2 :b)))))
  (is (= '(1 2) (keys (conj (sorted-map 2 :b) {1 :a})))))

(deftest sorted-by-collections-equal-plain-ones
  ;; Equality ignores the comparator; only content matters.
  (is (= {1 :a 2 :b} (sorted-map-by > 1 :a 2 :b)))
  (is (= (sorted-map-by > 1 :a 2 :b) {1 :a 2 :b}))
  (is (= #{1 2} (sorted-set-by > 1 2)))
  (is (= (sorted-set-by > 1 2) #{1 2}))
  (is (not= {1 :a 2 :b} (sorted-map-by > 1 :a 2 :c)))
  (is (= (hash {1 :a}) (hash (sorted-map-by > 1 :a)))))

(deftest uuid-compare-orders-as-signed-longs
  ;; UUIDs compare like the canonical compareTo: most-significant and
  ;; least-significant 64-bit halves as signed longs. A high-bit-set
  ;; half is negative, so it sorts before zero.
  (is (= 0 (compare #uuid "11111111-1111-1111-1111-111111111111"
                    #uuid "11111111-1111-1111-1111-111111111111")))
  (is (= -1 (compare #uuid "00000000-0000-0000-0000-000000000001"
                     #uuid "00000000-0000-0000-0000-000000000002")))
  (is (= 1 (compare #uuid "00000000-0000-0000-0000-000000000000"
                    #uuid "ffffffff-ffff-ffff-ffff-ffffffffffff")))
  (is (= -1 (compare #uuid "80000000-0000-0000-0000-000000000000"
                     #uuid "00000000-0000-0000-0000-000000000000"))))

(deftest map-entry-supports-assoc
  ;; A map entry behaves as a 2-element vector under assoc, like it
  ;; already does under conj and nth.
  (let [e (first {:a 1})]
    (is (= [:a 1 :x] (assoc e 2 :x)))
    (is (= [:b 1] (assoc e 0 :b)))
    (is (= [:a 2] (assoc e 1 2)))
    (is (thrown? (assoc e 3 :y)))))

(deftest sorted-set-conj-stress
  ;; Regression for memory-collections-r2-001: sorted_set_conj1 must
  ;; gc_pin nv across rb_assoc so that a GC triggered inside rb_assoc
  ;; cannot collect the freshly-allocated result.  Build a large set
  ;; by repeated conj to provoke allocation pressure.
  (let [s (reduce conj (sorted-set) (range 2000))]
    (is (= 2000 (count s)))
    (is (= 0 (first s)))
    (is (= 1999 (last s)))))

(deftest val-compare-cross-type-is-total
  ;; Deviation from JVM Clojure documented in conformance-collections-r2-001:
  ;; mino's compare is a total order across all types; incompatible-type
  ;; pairs order by type-enum rather than throwing.  Verify that a
  ;; sorted-set accepts heterogeneous keys without error (user contract
  ;; -- JVM would throw ClassCastException).
  (let [s (sorted-set-by compare 1 "a" :k)]
    (is (= 3 (count s)))))
