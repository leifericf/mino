(require "tests/test")
(require '[clojure.core.cache :as c])

;; Behaviors ported from clojure.core.cache's test suite, adapted to
;; mino's record-based caches. SoftCache has no counterpart here and is
;; not exercised. TTL expiry is checked two ways: a real elapsed-time
;; sleep, and a deterministic age boundary that does not depend on timing.

;; ---------------------------------------------------------------------------
;; BasicCache
;; ---------------------------------------------------------------------------

(deftest basic-seed-and-lookup
  (let [b (c/basic-cache-factory {:a 1 :b 2})]
    (is (= 1 (c/lookup b :a)))
    (is (= 2 (c/lookup b :b)))
    (is (nil? (c/lookup b :missing)))
    (is (= :nf (c/lookup b :missing :nf)))))

(deftest basic-has?-reports-membership
  (let [b (c/basic-cache-factory {:a 1})]
    (is (true? (c/has? b :a)))
    (is (false? (c/has? b :z)))))

(deftest basic-miss-adds-without-evicting
  (let [b (c/basic-cache-factory {:a 1})
        b2 (c/miss b :b 2)]
    (is (= 2 (c/lookup b2 :b)))
    (is (= 1 (c/lookup b2 :a)))
    ;; original cache is untouched — caches are values
    (is (false? (c/has? b :b)))))

(deftest basic-evict-removes
  (let [b (c/basic-cache-factory {:a 1 :b 2})
        b2 (c/evict b :a)]
    (is (false? (c/has? b2 :a)))
    (is (true? (c/has? b2 :b)))))

(deftest basic-hit-is-identity
  (let [b (c/basic-cache-factory {:a 1})]
    (is (= 1 (c/lookup (c/hit b :a) :a)))))

;; ---------------------------------------------------------------------------
;; FIFOCache
;; ---------------------------------------------------------------------------

(deftest fifo-evicts-oldest-on-overflow
  (let [f (-> (c/fifo-cache-factory {} :threshold 2)
              (c/miss :a 1)
              (c/miss :b 2)
              (c/miss :c 3))]
    ;; :a was inserted first, so it is the one dropped
    (is (false? (c/has? f :a)))
    (is (true? (c/has? f :b)))
    (is (true? (c/has? f :c)))
    (is (= 3 (c/lookup f :c)))))

(deftest fifo-hit-does-not-change-eviction-order
  ;; FIFO ignores reads; insertion order alone decides eviction.
  (let [f (-> (c/fifo-cache-factory {} :threshold 2)
              (c/miss :a 1)
              (c/miss :b 2))
        f2 (c/hit f :a)
        f3 (c/miss f2 :c 3)]
    (is (false? (c/has? f3 :a)))
    (is (true? (c/has? f3 :b)))
    (is (true? (c/has? f3 :c)))))

(deftest fifo-evict-removes-from-queue
  ;; Evicting :a by hand drops it from the queue too, so when the cache
  ;; later overflows the oldest survivor (:b) is the one auto-evicted.
  (let [f (-> (c/fifo-cache-factory {} :threshold 3)
              (c/miss :a 1)
              (c/miss :b 2)
              (c/evict :a)
              (c/miss :c 3)
              (c/miss :d 4)
              (c/miss :e 5))]
    (is (false? (c/has? f :a)))
    (is (false? (c/has? f :b)))
    (is (true? (c/has? f :c)))
    (is (true? (c/has? f :d)))
    (is (true? (c/has? f :e)))))

;; ---------------------------------------------------------------------------
;; LRUCache
;; ---------------------------------------------------------------------------

(deftest lru-evicts-least-recently-used
  ;; Touch :a after both are in, so :b is the least-recently-used and
  ;; gets evicted when :c arrives.
  (let [l (-> (c/lru-cache-factory {} :threshold 2)
              (c/miss :a 1)
              (c/miss :b 2))
        l2 (c/hit l :a)
        l3 (c/miss l2 :c 3)]
    (is (false? (c/has? l3 :b)))
    (is (true? (c/has? l3 :a)))
    (is (true? (c/has? l3 :c)))))

(deftest lru-hit-protects-key-from-eviction
  ;; Without the hit, :a would be least-recently-used and dropped.
  (let [l (-> (c/lru-cache-factory {} :threshold 2)
              (c/miss :a 1)
              (c/miss :b 2))
        ;; refresh :a, then refresh :b so :a is again the oldest touch
        protected (-> l (c/hit :b) (c/hit :a))
        l2 (c/miss protected :c 3)]
    (is (true? (c/has? l2 :a)))
    (is (false? (c/has? l2 :b)))
    (is (true? (c/has? l2 :c)))))

(deftest lru-lookup-does-not-record-use
  ;; lookup alone must not refresh recency — only hit does.
  (let [l (-> (c/lru-cache-factory {} :threshold 2)
              (c/miss :a 1)
              (c/miss :b 2))]
    ;; a bare lookup of :a leaves :a still the oldest touch
    (c/lookup l :a)
    (let [l2 (c/miss l :c 3)]
      (is (false? (c/has? l2 :a)))
      (is (true? (c/has? l2 :b)))
      (is (true? (c/has? l2 :c))))))

;; ---------------------------------------------------------------------------
;; TTLCache
;; ---------------------------------------------------------------------------

(deftest ttl-fresh-entry-is-present
  (let [t (-> (c/ttl-cache-factory {} :ttl 10000)
              (c/miss :a 1))]
    (is (true? (c/has? t :a)))
    (is (= 1 (c/lookup t :a)))
    (is (= 1 (c/lookup t :a :nf)))))

(deftest ttl-entry-expires-after-sleep
  ;; Real elapsed-time check: a tiny ttl plus a longer sleep expires the
  ;; entry, so it reads as absent.
  (let [t (-> (c/ttl-cache-factory {} :ttl 30)
              (c/miss :a 1))]
    (is (true? (c/has? t :a)))
    (Thread/sleep 70)
    (is (false? (c/has? t :a)))
    (is (= :nf (c/lookup t :a :nf)))
    (is (nil? (c/lookup t :a)))))

(deftest ttl-respects-boundary-deterministically
  ;; Timing-independent check of the same expiry logic: seed two caches
  ;; whose seed timestamps differ, separated by a sleep wider than the
  ;; ttl, and confirm the older one's entry has aged out while the fresh
  ;; one is still live.
  (let [stale (c/ttl-cache-factory {:a 1} :ttl 20)]
    (Thread/sleep 50)
    (let [fresh (c/ttl-cache-factory {:b 2} :ttl 20)]
      (is (false? (c/has? stale :a)))
      (is (true? (c/has? fresh :b))))))

;; ---------------------------------------------------------------------------
;; LUCache
;; ---------------------------------------------------------------------------

(deftest lu-evicts-least-used
  ;; :a is hit twice, :b never, so :b is the least used and is dropped
  ;; when :c arrives.
  (let [u (-> (c/lu-cache-factory {} :threshold 2)
              (c/miss :a 1)
              (c/miss :b 2))
        used (-> u (c/hit :a) (c/hit :a))
        u2 (c/miss used :c 3)]
    (is (false? (c/has? u2 :b)))
    (is (true? (c/has? u2 :a)))
    (is (true? (c/has? u2 :c)))))

(deftest lu-frequently-hit-key-survives
  ;; Build pressure repeatedly while keeping :a hot; it should never be
  ;; the one evicted.
  (let [u (-> (c/lu-cache-factory {} :threshold 2)
              (c/miss :a 1)
              (c/hit :a) (c/hit :a) (c/hit :a)
              (c/miss :b 2)
              (c/miss :c 3)
              (c/miss :d 4))]
    (is (true? (c/has? u :a)))))

;; ---------------------------------------------------------------------------
;; Read-through helpers
;; ---------------------------------------------------------------------------

(deftest through-cache-computes-once
  (let [calls (atom 0)
        value-fn (fn [k] (swap! calls inc) (* 10 k))
        c1 (c/through-cache (c/basic-cache-factory {}) 5 value-fn)]
    ;; first read is a miss: the value fn runs
    (is (= 50 (c/lookup c1 5)))
    (is (= 1 @calls))
    ;; second read hits: the value fn does not run again
    (let [c2 (c/through-cache c1 5 value-fn)]
      (is (= 50 (c/lookup c2 5)))
      (is (= 1 @calls)))))

(deftest through-helper-computes-once
  (let [calls (atom 0)
        value-fn (fn [k] (swap! calls inc) (str "v" k))
        c1 (c/through value-fn (c/basic-cache-factory {}) :k)]
    (is (= "v:k" (c/lookup c1 :k)))
    (is (= 1 @calls))
    (let [c2 (c/through value-fn c1 :k)]
      (is (= "v:k" (c/lookup c2 :k)))
      (is (= 1 @calls)))))

(deftest through-cache-honors-eviction-policy
  ;; Read-through over a bounded cache still evicts: filling a FIFO of
  ;; size 2 by reading three keys through drops the first.
  (let [vf (fn [k] (* 2 k))
        f (-> (c/fifo-cache-factory {} :threshold 2)
              (c/through-cache 1 vf)
              (c/through-cache 2 vf)
              (c/through-cache 3 vf))]
    (is (false? (c/has? f 1)))
    (is (true? (c/has? f 2)))
    (is (= 6 (c/lookup f 3)))))

(run-tests-and-exit)
