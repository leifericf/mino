(require "tests/test")

;; --- subvec ---

(deftest subvec-basic
  (is (= [2 3] (subvec [1 2 3 4] 1 3)))
  (is (= [3 4] (subvec [1 2 3 4] 2)))
  (is (= [] (subvec [1 2 3] 1 1)))
  (is (= [1 2 3] (subvec [1 2 3] 0)))
  (is (= [1 2 3] (subvec [1 2 3] 0 3)))
  (is (= [] (subvec [] 0 0)))
  (is (= [] (subvec [1] 0 0)))
  (is (= [1] (subvec [1] 0 1)))
  (is (= [] (subvec [1 2 3] 3))))

(deftest subvec-operations
  (let [sv (subvec [1 2 3 4 5] 1 4)]
    (is (= 3 (count sv)))
    (is (= 2 (nth sv 0)))
    (is (= 4 (nth sv 2)))
    (is (= 3 (get sv 1)))
    (is (= [2 3 4 99] (conj sv 99)))
    (is (= [:x 3 4] (assoc sv 0 :x)))
    (is (= [2 3] (pop sv)))))

(deftest subvec-nested
  (let [sv (subvec (subvec [1 2 3 4 5] 1 4) 1 2)]
    (is (= [3] sv))
    (is (= 3 (first sv)))))

(deftest subvec-equality
  (is (= (subvec [1 2 3] 1) [2 3]))
  (is (= [2 3] (subvec [1 2 3] 1)))
  (is (vector? (subvec [1 2 3] 1))))

(deftest subvec-seq
  (is (= '(2 3) (seq (subvec [1 2 3 4] 1 3))))
  (is (= 2 (first (subvec [1 2 3 4] 1))))
  (is (= '(3 4) (rest (subvec [1 2 3 4] 1)))))

(deftest subvec-large
  (let [big (vec (range 100))
        sv  (subvec big 50 60)]
    (is (= 10 (count sv)))
    (is (= 50 (nth sv 0)))
    (is (= 55 (nth sv 5)))
    (is (= 59 (nth sv 9)))))

(deftest subvec-bounds-error
  (is (thrown? (subvec [1 2 3] -1)))
  (is (thrown? (subvec [1 2 3] 4)))
  (is (thrown? (subvec [1 2 3] 2 1)))
  (is (thrown? (subvec [1 2 3] 0 4))))

;; --- ifn? ---

(deftest ifn-predicate
  (is (ifn? (fn [x] x)))
  (is (ifn? +))
  (is (ifn? :keyword))
  (is (ifn? {:a 1}))
  (is (ifn? [1 2 3]))
  (is (ifn? #{1 2}))
  (is (ifn? (sorted-map :a 1)))
  (is (ifn? (sorted-set 1)))
  (is (not (ifn? 42)))
  (is (not (ifn? "hello")))
  (is (not (ifn? nil)))
  (is (not (ifn? true))))

;; --- seqable? ---

(deftest seqable-predicate
  (is (seqable? nil))
  (is (seqable? [1 2]))
  (is (seqable? '(1 2)))
  (is (seqable? {:a 1}))
  (is (seqable? #{1}))
  (is (seqable? "abc"))
  (is (seqable? (sorted-map :a 1)))
  (is (seqable? (sorted-set 1)))
  (is (not (seqable? 42)))
  (is (not (seqable? :kw)))
  (is (not (seqable? true))))

;; --- indexed? ---

(deftest indexed-predicate
  (is (indexed? [1 2 3]))
  (is (indexed? []))
  (is (not (indexed? '(1 2))))
  (is (not (indexed? {:a 1})))
  (is (not (indexed? nil)))
  (is (not (indexed? "abc"))))

;; --- empty metadata preservation ---

(deftest empty-preserves-meta
  (is (= {:a 1} (meta (empty (with-meta [1 2] {:a 1})))))
  (is (= {:b 2} (meta (empty (with-meta {:x 1} {:b 2})))))
  (is (= {:c 3} (meta (empty (with-meta #{1 2} {:c 3}))))))

;; --- empty on strings returns nil (matches JVM behavior) ---

(deftest empty-string-returns-nil
  (is (nil? (empty "hello")))
  (is (nil? (empty ""))))

(deftest deeply-nested-structural-equality
  ;; Element-position nesting must compare to arbitrary depth, bounded
  ;; by the heap rather than the C stack.
  (testing "deep car-nested lists"
    (let [build (fn [n] (reduce (fn [acc _] (list acc)) :bottom (range n)))
          d1 (build 100000)
          d2 (build 100000)]
      (is (true? (= d1 d2)))
      (is (false? (= d1 (build 99999))))))
  (testing "deep vector towers"
    (let [build (fn [n leaf] (reduce (fn [acc _] (vector acc)) leaf (range n)))
          v1 (build 100000 :x)
          v2 (build 100000 :x)]
      (is (true? (= v1 v2)))
      (is (false? (= v1 (build 100000 :y))))))
  (testing "deep mixed list-vector nesting"
    (let [build (fn [n] (reduce (fn [acc i] (if (even? i) (list acc) (vector acc)))
                                :z (range n)))
          m1 (build 60000)
          m2 (build 60000)]
      (is (true? (= m1 m2))))))

(deftest deeply-nested-compare
  ;; Lexicographic vector compare must handle element nesting bounded
  ;; by the heap rather than the C stack, for both the compare fn and
  ;; the sorted-collection comparator path.
  (let [build (fn [n leaf] (reduce (fn [acc _] (vector acc)) leaf (range n)))
        v1 (build 100000 1)
        v2 (build 100000 1)
        v3 (build 100000 2)]
    (testing "equal towers compare zero"
      (is (zero? (compare v1 v2))))
    (testing "leaf difference decides deep compare"
      (is (neg? (compare v1 v3)))
      (is (pos? (compare v3 v1))))
    (testing "sorted collections accept deeply nested keys"
      (is (= 2 (count (sorted-set v1 v3)))))))
