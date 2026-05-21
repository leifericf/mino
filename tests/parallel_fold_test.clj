(require "tests/test")
(require '[clojure.core.reducers :as r])

;; Parallel r/fold shipped in v0.407.0. The 3-arity (fold combinef
;; reducef coll) and 4-arity (fold n combinef reducef coll) forms
;; partition vectors > n elements into chunks of size n, reduce each
;; chunk in parallel via futures, then combine the partial results
;; with combinef. Smaller vectors and non-vector collections reduce
;; sequentially.

(deftest fold-arity-1
  ;; (fold reducef coll) -- combinef defaults to reducef. + acts as
  ;; its own identity-zero combinef.
  (is (= 10 (r/fold + [1 2 3 4]))))

(deftest fold-arity-2-equals-reduce
  (is (= 499500 (r/fold + + (vec (range 1000)))))
  ;; vector smaller than default partition (512) → sequential branch.
  (is (= 15 (r/fold + + [1 2 3 4 5]))))

(deftest fold-arity-3-with-partition
  ;; Explicit partition size still produces the same total (sum is
  ;; associative).
  (is (= 499500 (r/fold 100 + + (vec (range 1000)))))
  (is (= 499500 (r/fold 250 + + (vec (range 1000)))))
  (is (= 499500 (r/fold 1 + + (vec (range 1000))))))

(deftest fold-non-vector-falls-back-sequential
  ;; Lists / lazy seqs don't get partitioned (no O(1) random access);
  ;; they reduce sequentially through (reduce reducef (combinef) coll).
  (is (= 499500 (r/fold + + (range 1000))))
  (is (= 499500 (r/fold + + (list* (range 1000))))))

(deftest fold-parallel-and-seq-agree
  ;; Across a range of partition sizes, the parallel branch must
  ;; produce the same result as the sequential branch when the
  ;; combinef is associative.
  (let [v (vec (range 5000))]
    (doseq [n [1 10 100 500 1000 4999 5000 5001]]
      (is (= 12497500 (r/fold n + + v))))))

(deftest fold-min-max-combinef
  ;; min and max are associative; verify a non-additive combinef.
  ;; (mino doesn't have Long/MAX_VALUE; use the literal.)
  (let [v (vec (range 1 1001))]
    (is (= 1    (r/fold 100 (fn ([] 9223372036854775807)  ([a b] (min a b))) min v)))
    (is (= 1000 (r/fold 100 (fn ([] -9223372036854775808) ([a b] (max a b))) max v)))))
