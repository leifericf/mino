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
