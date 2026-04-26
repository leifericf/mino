;; GC stress runner: allocation-heavy tests run under MINO_GC_STRESS=1.
;; Uses smaller iteration counts than gc_test.clj since every allocation
;; triggers a collection in stress mode.
;;
;; Usage: MINO_GC_STRESS=1 ./mino tests/gc_stress_runner.clj

(require "tests/test")

(deftest gc-stress-loop
  (is (= 500 (loop [i 0] (if (< i 500) (recur (+ i 1)) i)))))

(deftest gc-stress-vec-churn
  (is (= 200 (count (loop [i 0 acc []]
                      (if (< i 200) (recur (+ i 1) (conj acc i)) acc))))))

(deftest gc-stress-map-churn
  (is (= 150 (get (loop [i 0 m {}]
                    (if (< i 100) (recur (+ i 1) (assoc m i (* i 3))) m))
                  50))))

(deftest gc-stress-closure-churn
  (def make-inc__stress (fn [n] (fn [x] (+ x n))))
  (is (= 4950
         (loop [i 0 acc 0]
           (if (< i 100)
             (recur (+ i 1) ((make-inc__stress i) acc))
             acc)))))

(deftest gc-stress-deep-nest
  (def build__stress (fn [n acc]
    (if (= n 0)
      acc
      (build__stress (- n 1) (list acc)))))
  (is (cons? (build__stress 50 42))))

;; Lazy-seq cache barrier under stress: promote a vector of unrealized
;; lazy seqs, force each into its OLD lazy cache slot, then run more
;; allocation. Every allocation collects in stress mode so a missing
;; barrier on the cached-store would lose the forced YOUNG chains on
;; the very next minor.
(deftest gc-stress-lazy-cache-barrier
  (let [n  12
        mk (fn [k] (map inc (range k)))
        v  (mapv mk (range n))]
    (dotimes [_ 20]
      (doall (take 20 (range 50))))
    (dotimes [i n]
      (doall (nth v i)))
    (dotimes [_ 20]
      (doall (take 20 (range 50))))
    (dotimes [i n]
      (is (= (reduce + 0 (mk i))
             (reduce + 0 (nth v i)))))))

(run-tests)
