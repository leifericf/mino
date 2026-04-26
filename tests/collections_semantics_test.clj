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
