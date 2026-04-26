(require "tests/test")

;; Targeted tests for generational collector invariants. The full test
;; suite already exercises the collector in aggregate; these tests pin
;; specific behaviours so regressions surface loudly.

(defn- young-bytes []   (:bytes-young   (gc-stats)))
(defn- old-bytes []     (:bytes-old     (gc-stats)))
(defn- minor-count []   (:collections-minor (gc-stats)))
(defn- major-count []   (:collections-major (gc-stats)))

;; Default nursery is 1 MiB; allocating a few hundred thousand short-
;; lived cons cells forces many minor cycles. A few majors may fire
;; along the way as some survivors promote and old-gen crosses the
;; growth threshold, but minor must dominate the collection work.
(deftest minor-fires-under-nursery-pressure
  (let [m0 (minor-count)
        M0 (major-count)]
    (dotimes [_ 2000]
      (doall (take 100 (range 1000))))
    (is (> (minor-count) m0))
    (is (> (- (minor-count) m0) (* 10 (- (major-count) M0))))))

;; After enough minor survivors the old-gen baseline grows; a repeated
;; build-use-discard cycle should show bytes-old nonzero without
;; triggering a major per minor (young-only collection does the bulk
;; of the work).
(deftest promotion-grows-old-gen
  (let [m0 (minor-count)
        M0 (major-count)]
    (dotimes [_ 500]
      (doall (take 200 (range 10000))))
    (is (> (old-bytes) 0))
    ;; Majors should be much rarer than minors.
    (is (< (- (major-count) M0) (- (minor-count) m0)))))

;; Atom val barrier: an OLD-promoted atom pointing at a fresh YOUNG
;; value must survive across a minor. Force promotion by looping past
;; the nursery budget, then repeatedly refresh the atom and read it.
(deftest atom-val-survives-remset
  (let [a (atom nil)]
    (dotimes [i 2000]
      (reset! a (vec (range (rem i 50))))
      (when (zero? (rem i 100))
        (is (= (rem i 50) (count @a)))))
    (is (vector? @a))))

;; Lazy-seq cache barrier: an OLD-promoted lazy seq, reachable only
;; through a persistent structure, must survive forcing and subsequent
;; minor pressure. The lazy_force path writes three slots on realisation
;; (cached, body, env); each store is routed through gc_write_barrier,
;; so a freshly-realised YOUNG chain installed into an OLD lazy must be
;; tracked in the remset. Build a vector of unrealized lazy seqs, promote
;; it via minor pressure, force each lazy (populating its cache with
;; fresh YOUNG cons cells), run more minor pressure, then re-read every
;; element. Without the barrier the cached chain is swept on the next
;; minor and the element values drift from the expected sums.
(deftest lazy-cache-barrier-across-minors
  (let [n    64
        mk   (fn [k] (map inc (range k)))
        v    (mapv mk (range n))]
    ;; Pressure: push v into OLD and each lazy element with it.
    (dotimes [_ 2000]
      (doall (take 100 (range 1000))))
    ;; Force every lazy seq inside v. Each force realises the lazy and
    ;; writes a fresh YOUNG cons chain into the OLD lazy's cache slot.
    (dotimes [i n]
      (doall (nth v i)))
    ;; More minor pressure. Barrier-tracked caches must survive.
    (dotimes [_ 2000]
      (doall (take 100 (range 1000))))
    ;; Every element must still decode to the expected sum. If the
    ;; barrier were missing, cached chains would be swept and element
    ;; reads would return corrupt data or crash.
    (dotimes [i n]
      (is (= (reduce + 0 (mk i))
             (reduce + 0 (nth v i)))))))

;; Var rebind barrier: redefining a namespace-level fn repeatedly
;; across minors must keep the most recent value reachable.
(def ^:private gc-test-var nil)
(deftest var-rebind-survives-remset
  (dotimes [i 2000]
    (def ^:private gc-test-var (vec (range (rem i 64)))))
  (is (vector? gc-test-var)))

;; Conservative stack scan: a locally-rooted value must not be swept.
;; The `kept` binding stays on the C stack while inner loop allocates
;; enough to trigger minors.
(deftest conservative-scan-protects-locals
  (let [kept (vec (range 123))]
    (dotimes [_ 2000]
      (doall (take 100 (range 1000))))
    (is (= 123 (count kept)))))

;; List-tail append barrier: building a long list via seq primitives
;; that allocate fresh cons cells across minors produces the expected
;; length.
(deftest list-tail-append-survives-minor
  (let [n 20000
        s (seq (vec (range n)))]
    (is (= n (count s)))))

;; Env binding barrier: repeatedly rebinding a let-scoped local in a
;; deeply nested scope exercises env_bind_impl with an OLD env.
(deftest env-rebind-survives-remset
  (let [acc (loop [i 0 acc 0]
              (if (>= i 5000)
                acc
                (recur (+ i 1) (+ acc i))))]
    (is (= 12497500 acc))))

;; Age-based promotion sanity: with promotion_age=1, bytes-young should
;; never climb far past the configured nursery size -- the minor trigger
;; fires on overflow and drains the nursery every cycle.
(deftest nursery-bounded
  (dotimes [_ 200]
    (doall (take 1000 (range 10000))))
  (is (< (young-bytes)
         (* 4 (:nursery-bytes (gc-stats))))))

;; Literal-builder barrier: vector/map/set literals whose elements are
;; non-constant calls allocate a VALARR scratch buffer and fill it slot
;; by slot. Each eval_value in the loop can trigger a minor that
;; promotes the scratch buffer to OLD; subsequent slot writes of fresh
;; YOUNG values must go through the write barrier or the next minor
;; loses those values. The loop length and per-iteration allocation are
;; sized to force many minors and promotion of the scratch array.
(deftest literal-builder-barrier
  (dotimes [i 2500]
    (let [v [(conj (vec (range 200)) i)
             (conj (vec (range 200)) (+ i 1))
             (conj (vec (range 200)) (+ i 2))]]
      (is (= (+ i 2) (last (nth v 2))))))
  (is true))

;; Observability: remset and mark-stack expose their configured capacity
;; and peak usage. Capacity must be >= the current size; high-water must
;; be >= the current size and non-decreasing across time. These are
;; read-only introspection fields for embedders tuning workload-sensitive
;; parameters.
(deftest gc-stats-observability-fields
  (let [s (gc-stats)]
    (is (>= (:remset-cap s) (:remset-entries s)))
    (is (>= (:remset-high-water s) (:remset-entries s)))
    (is (>= (:mark-stack-cap s) 0))
    (is (>= (:mark-stack-high-water s) 0))))
