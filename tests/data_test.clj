(require "tests/test")

(require '[clojure.data :as data :refer [diff]])

;; --- equal arguments ---

(deftest data-diff-equal
  (is (= [nil nil 1]      (diff 1 1)))
  (is (= [nil nil "x"]    (diff "x" "x")))
  (is (= [nil nil nil]    (diff nil nil)))
  (is (= [nil nil [1 2]]  (diff [1 2] [1 2])))
  (is (= [nil nil {:a 1}] (diff {:a 1} {:a 1}))))

;; --- non-collection difference ---

(deftest data-diff-scalars
  (is (= [1 2 nil]   (diff 1 2)))
  (is (= ["a" "b" nil] (diff "a" "b"))))

;; --- different types fall through ---

(deftest data-diff-different-types
  (is (= [{:a 1} [1] nil] (diff {:a 1} [1])))
  (is (= [#{1} [1] nil]   (diff #{1} [1]))))

;; --- map diffs ---

(deftest data-diff-map-disjoint
  (is (= [{:a 1} {:b 2} nil]
         (diff {:a 1} {:b 2}))))

(deftest data-diff-map-overlap
  (is (= [{:a 1} {:c 3} {:b 2}]
         (diff {:a 1 :b 2} {:b 2 :c 3}))))

(deftest data-diff-map-nested
  (is (= [{:a {:y 2}} {:a {:y 3}} {:a {:x 1}}]
         (diff {:a {:x 1 :y 2}} {:a {:x 1 :y 3}}))))

(deftest data-diff-map-empty
  (is (= [nil nil {}] (diff {} {}))))

;; --- sequential diffs ---

(deftest data-diff-seq-equal-length
  (is (= [[nil nil 3] [nil nil 4] [1 2]]
         (diff [1 2 3] [1 2 4]))))

(deftest data-diff-seq-different-length
  (is (= [[nil nil 3] nil [1 2]]
         (diff [1 2 3] [1 2])))
  (is (= [nil [nil nil 4] [1 2]]
         (diff [1 2] [1 2 4]))))

(deftest data-diff-seq-empty
  (is (= [nil nil []] (diff [] []))))

(deftest data-diff-seq-list-and-vec
  (is (= [nil nil [1 2 3]] (diff '(1 2 3) [1 2 3]))))

;; --- set diffs ---

(deftest data-diff-set-disjoint
  (is (= [#{1 2} #{3 4} nil]
         (diff #{1 2} #{3 4}))))

(deftest data-diff-set-overlap
  (is (= [#{1} #{4} #{2 3}]
         (diff #{1 2 3} #{2 3 4}))))

(deftest data-diff-set-empty
  (is (= [nil nil #{}] (diff #{} #{}))))

(deftest data-diff-set-shared-element
  (is (= [#{1} #{3} #{2}] (diff #{1 2} #{2 3}))))

;; --- canon pinning: keys replaced across maps ---

(deftest data-diff-map-key-replaced
  (is (= [{:b 2} {:c 3} {:a 1}]
         (diff {:a 1 :b 2} {:a 1 :c 3}))))

(deftest data-diff-map-nested-key-replaced
  (is (= [{:a {:c 2}} {:a {:d 3}} {:a {:b 1}}]
         (diff {:a {:b 1 :c 2}} {:a {:b 1 :d 3}}))))

(deftest data-diff-map-shared-nil-value
  (is (= [{:b 1} nil {:a nil}]
         (diff {:a nil :b 1} {:a nil}))))

;; --- protocols ---

(deftest data-protocol-vars-exist
  (is (var? (resolve 'clojure.data/EqualityPartition)))
  (is (var? (resolve 'clojure.data/Diff)))
  (is (var? (resolve 'clojure.data/equality-partition)))
  (is (var? (resolve 'clojure.data/diff-similar))))

(deftest data-equality-partition
  (is (= :atom (data/equality-partition 1)))
  (is (= :atom (data/equality-partition "x")))
  (is (= :atom (data/equality-partition nil)))
  (is (= :sequential (data/equality-partition [1])))
  (is (= :sequential (data/equality-partition '(1))))
  (is (= :set (data/equality-partition #{1})))
  (is (= :map (data/equality-partition {:a 1}))))

(deftest data-diff-similar-sequential
  (is (= [[nil 2] [nil 3] [1]] (data/diff-similar [1 2] [1 3])))
  (is (= (diff [1 2] [1 3]) (data/diff-similar [1 2] [1 3]))))

(run-tests-and-exit)
