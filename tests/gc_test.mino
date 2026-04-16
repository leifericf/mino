(require "tests/test")

;; GC stability: heavy allocation tests.
;; Also run under `make test-gc-stress` which collects on every allocation.

(deftest gc-long-tail
  (is (= 50000 (loop [i 0] (if (< i 50000) (recur (+ i 1)) i)))))

(deftest gc-vec-churn
  (is (= 2000 (count (loop [i 0 acc []]
                       (if (< i 2000) (recur (+ i 1) (conj acc i)) acc))))))

(deftest gc-map-churn
  (is (= 450 (get (loop [i 0 m {}]
                    (if (< i 300) (recur (+ i 1) (assoc m i (* i 3))) m))
                  150))))

(deftest gc-closure-churn
  (def make-inc__gc (fn [n] (fn [x] (+ x n))))
  (is (= 499500
         (loop [i 0 acc 0]
           (if (< i 1000)
             (recur (+ i 1) ((make-inc__gc i) acc))
             acc)))))

(deftest deep-nest-safe
  (def build__gc (fn [n acc]
    (if (= n 0)
      acc
      (build__gc (- n 1) (list acc)))))
  (is (cons? (build__gc 200 42))))
