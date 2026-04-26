(require "tests/test")

;; Regression for an intermittent HAMT-under-churn anomaly observed
;; during mino-bench GC timing work: building/rebuilding a string-keyed
;; map of >= 2 000 entries via reduce-over-assoc occasionally produced
;; a map with the wrong count and, more rarely, SIGSEGV. No root cause
;; was isolated at the time because the symptom could not be
;; reproduced after the mark-phase rework. This test locks the
;; behaviour in so any recurrence is caught.

(defn- build-str-map [n]
  (loop [i 0 m {}]
    (if (>= i n) m
        (recur (+ i 1) (assoc m (str "k" i) i)))))

(defn- str-keys [n]
  (loop [i 0 acc []]
    (if (>= i n) acc (recur (+ i 1) (conj acc (str "k" i))))))

(defn- bump-values [m ks]
  (reduce (fn [acc k] (assoc acc k (+ 1 (get acc k)))) m ks))

(deftest hamt-str-build-count
  (is (= 2000 (count (build-str-map 2000))))
  (is (= 2630 (count (build-str-map 2630))))
  (is (= 5000 (count (build-str-map 5000)))))

(deftest hamt-str-bump-preserves-count
  (doseq [n [2000 2630 5000]]
    (let [m  (build-str-map n)
          ks (str-keys n)]
      (is (= n (count (bump-values m ks)))))))

(deftest hamt-str-repeated-bump
  (let [n  2000
        ks (str-keys n)
        m0 (build-str-map n)]
    (is (= n (count (loop [i 0 m m0]
                      (if (>= i 5) m
                          (recur (+ i 1) (bump-values m ks)))))))))
