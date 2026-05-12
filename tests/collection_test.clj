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
  (is (= #{1 2 3} #{1 2 2 3}))
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
