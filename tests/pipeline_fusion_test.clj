(require "tests/test")

;; Terminal (non-reduce) seq consumers over a lazy map/filter/take
;; pipeline must produce identical results whether the runtime fuses
;; the pipeline through the shared walker or falls back to the slow
;; per-element lazy-cell path. These tests pin the observable
;; semantics fusion must preserve: value equality, encounter order,
;; short-circuit, empty/nil handling, and correctness across the
;; 32-element chunk boundary of a lazy range.
;;
;; A lazy pipeline head is `(->> (range N) (map f) (filter p) ...)`;
;; the fused fast path recognises the map/filter/take lazy cells by
;; their exact thunk identity. A helper builds fresh pipelines so the
;; tests exercise unrealised heads (a realised head caches its cells).

(defn- pipe [n]
  (->> (range n) (map inc) (filter odd?)))

;; --- count -----------------------------------------------------------

(deftest count-pipeline-equals-realised
  ;; count over a lazy pipeline must equal counting the fully realised
  ;; sequence. Crosses several 32-element chunk boundaries at n=1000.
  (is (= (count (doall (pipe 1000)))
         (count (pipe 1000))))
  (is (= 500 (count (pipe 1000)))))

(deftest count-pipeline-edges
  (is (= 0 (count (pipe 0))))
  (is (= 0 (count (->> (range 10) (filter (fn [_] false))))))
  ;; single surviving element
  (is (= 1 (count (->> (range 1) (map inc) (filter odd?))))))

(deftest count-pipeline-with-take
  ;; take must terminate the walk; count of a take is bounded by it.
  (is (= 7 (count (->> (range 1000) (map inc) (filter odd?) (take 7)))))
  ;; take crossing a chunk boundary (>32)
  (is (= 40 (count (->> (range 1000) (map inc) (filter odd?) (take 40)))))
  ;; take larger than the surviving count clamps to what survives
  (is (= 500 (count (->> (range 1000) (map inc) (filter odd?)
                         (take 100000))))))

;; --- some ------------------------------------------------------------

(deftest some-pipeline-first-truthy
  ;; some returns the first truthy (pred x), short-circuiting.
  (is (= true (some #(> % 5) (pipe 1000))))
  (is (= :hit (some #(when (= % 101) :hit) (pipe 1000)))))

(deftest some-pipeline-never
  (is (= nil (some #(> % 100000) (pipe 1000))))
  (is (= nil (some odd? (->> (range 10) (map inc) (filter even?)))))
  (is (= nil (some (fn [_] true) (pipe 0)))))

(deftest some-pipeline-short-circuits
  ;; Once a truthy is found the pred must not run on later elements.
  ;; A pred that throws on the sentinel proves we stopped before it.
  (let [seen (atom [])]
    (is (= true
           (some (fn [x]
                   (swap! seen conj x)
                   (>= x 4))
                 (pipe 1000))))
    ;; survivors of (map inc)(filter odd?) are 1 3 5 ...; first >=4 is 5.
    (is (= [1 3 5] @seen))))

;; --- every? ----------------------------------------------------------

(deftest every-pipeline-all-true
  (is (= true (every? odd? (pipe 1000))))
  (is (= true (every? #(< % 100000) (pipe 1000))))
  ;; vacuously true on empty
  (is (= true (every? (fn [_] false) (pipe 0)))))

(deftest every-pipeline-early-false
  (is (= false (every? #(< % 5) (pipe 1000))))
  (is (= false (every? even? (pipe 1000)))))

(deftest every-pipeline-short-circuits
  ;; every? must stop at the first falsy element.
  (let [seen (atom [])]
    (is (= false
           (every? (fn [x]
                     (swap! seen conj x)
                     (< x 4))
                   (pipe 1000))))
    ;; survivors 1 3 5 ...; first not <4 is 5.
    (is (= [1 3 5] @seen))))

;; --- frequencies -----------------------------------------------------

(deftest frequencies-pipeline-equals-realised
  (is (= (frequencies (doall (pipe 1000)))
         (frequencies (pipe 1000))))
  ;; every survivor is distinct and odd, so each count is 1
  (is (= 500 (count (frequencies (pipe 1000)))))
  (is (= #{1} (set (vals (frequencies (pipe 1000)))))))

(deftest frequencies-pipeline-repeats
  ;; a stage that collapses values must count multiplicities correctly
  (is (= {0 500, 1 500}
         (frequencies (->> (range 1000) (map #(mod % 2))))))
  (is (= {} (frequencies (pipe 0)))))

;; --- group-by --------------------------------------------------------

(deftest group-by-pipeline-equals-realised
  (is (= (group-by #(mod % 3) (doall (pipe 100)))
         (group-by #(mod % 3) (pipe 100)))))

(deftest group-by-pipeline-encounter-order
  ;; buckets preserve encounter order.
  (is (= {true [1 3 5 7 9]}
         (group-by odd? (->> (range 10) (map inc) (filter odd?)))))
  (is (= {0 [0 3 6 9] 1 [1 4 7] 2 [2 5 8]}
         (group-by #(mod % 3) (->> (range 10)))))
  (is (= {} (group-by odd? (pipe 0)))))

;; --- concrete-source drains for mapv / filterv / into ----------------
;; mapv / filterv / into over a CONCRETE vector or range (not a lazy
;; pipeline head) route through the shared walker's inline
;; vector-drain / int-range fast path instead of seq-iter +
;; per-element apply_callable. The fused result must be
;; indistinguishable from the slow path: same values, same order,
;; same identity for pass-through elements, same empty / single-element
;; behaviour, and the callable must run once per element in order.

(deftest mapv-concrete-vector-equals-slow
  ;; a plain vector source; crosses several 32-element chunk boundaries.
  (let [v (vec (range 1000))]
    (is (= (mapv inc v) (vec (map inc v))))
    (is (= 1000 (count (mapv inc v))))
    (is (= 1 (first (mapv inc v))))
    (is (= 1000 (last (mapv inc v)))))
  ;; empty and single element
  (is (= [] (mapv inc [])))
  (is (= [42] (mapv inc [41])))
  ;; identity function returns the same element objects
  (let [v [{:a 1} {:b 2}]]
    (is (= v (mapv identity v)))
    (is (identical? (first v) (first (mapv identity v))))))

(deftest mapv-concrete-range-equals-slow
  (is (= (mapv inc (range 1000)) (vec (map inc (range 1000)))))
  (is (= [] (mapv inc (range 0))))
  (is (= [1] (mapv inc (range 1))))
  ;; stepped and descending ranges
  (is (= (vec (map inc (range 0 100 3)))
         (mapv inc (range 0 100 3))))
  (is (= (vec (map inc (range 10 0 -1)))
         (mapv inc (range 10 0 -1)))))

(deftest mapv-concrete-visits-once-in-order
  ;; the fast path must apply the fn exactly once per element, in order.
  (let [seen (atom [])
        out  (mapv (fn [x] (swap! seen conj x) (* x 10)) (vec (range 5)))]
    (is (= [0 10 20 30 40] out))
    (is (= [0 1 2 3 4] @seen))))

(deftest filterv-concrete-vector-equals-slow
  (let [v (vec (range 1000))]
    (is (= (filterv even? v) (vec (filter even? v))))
    (is (= 500 (count (filterv even? v)))))
  (is (= [] (filterv even? [])))
  (is (= [] (filterv even? [1 3 5])))
  (is (= [2] (filterv even? [1 2 3])))
  ;; survivors are the identical source objects, not copies
  (let [a {:k 1} b {:k 2}
        v [a b]]
    (is (identical? a (first (filterv (fn [_] true) v))))))

(deftest filterv-concrete-range-equals-slow
  (is (= (filterv even? (range 1000)) (vec (filter even? (range 1000)))))
  (is (= [] (filterv even? (range 0))))
  (is (= [] (filterv (fn [_] false) (range 100))))
  (is (= (vec (range 100)) (filterv (fn [_] true) (range 100)))))

(deftest filterv-concrete-visits-once-in-order
  (let [seen (atom [])
        out  (filterv (fn [x] (swap! seen conj x) (odd? x)) (vec (range 5)))]
    (is (= [1 3] out))
    (is (= [0 1 2 3 4] @seen))))

(deftest into-concrete-vector-equals-slow
  (let [v (vec (range 1000))]
    (is (= (into [] v) v))
    (is (= (into [:x] v) (vec (concat [:x] v))))
    (is (= 1000 (count (into [] v)))))
  (is (= [] (into [] [])))
  (is (= [:a] (into [] [:a])))
  ;; source objects pass through by identity
  (let [a {:k 1}]
    (is (identical? a (first (into [] [a]))))))

(deftest into-concrete-range-equals-slow
  (is (= (into [] (range 1000)) (vec (range 1000))))
  (is (= [] (into [] (range 0))))
  (is (= [:x 0 1 2] (into [:x] (range 3))))
  (is (= (vec (range 10 0 -1)) (into [] (range 10 0 -1)))))
