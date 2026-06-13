(require "tests/test")
(require '[clojure.core.memoize :as memo])

;; Behaviors ported from clojure.core.memoize's test suite, adapted to mino:
;; a memoized function computes a result once per argument vector and serves
;; it from a backing cache; the cache's eviction policy decides what is kept.
;; The call-count atom stands in for "did the original function run?".

(deftest memo-computes-once-per-arg-vector
  (let [calls (atom 0)
        f (memo/memo (fn [x] (swap! calls inc) (* x x)))]
    (is (= 25 (f 5)))
    (is (= 1 @calls))
    ;; Same args: served from cache, no recompute.
    (is (= 25 (f 5)))
    (is (= 1 @calls))
    ;; Different args: a fresh computation.
    (is (= 9 (f 3)))
    (is (= 2 @calls))
    (is (= 9 (f 3)))
    (is (= 2 @calls))))

(deftest memo-handles-multiple-args
  (let [calls (atom 0)
        f (memo/memo (fn [a b] (swap! calls inc) (+ a b)))]
    (is (= 7 (f 3 4)))
    (is (= 7 (f 3 4)))
    (is (= 1 @calls))
    (is (= 10 (f 6 4)))
    (is (= 2 @calls))))

(deftest memo-handles-no-args
  (let [calls (atom 0)
        f (memo/memo (fn [] (swap! calls inc) :result))]
    (is (= :result (f)))
    (is (= :result (f)))
    (is (= 1 @calls))))

(deftest memo-clear-forces-recompute
  (let [calls (atom 0)
        f (memo/memo (fn [x] (swap! calls inc) x))]
    (f 1)
    (f 1)
    (is (= 1 @calls))
    (memo/memo-clear! f)
    ;; Cleared cache: the next call recomputes.
    (f 1)
    (is (= 2 @calls))))

(deftest memo-clear-single-entry
  (let [calls (atom 0)
        f (memo/memo (fn [x] (swap! calls inc) x))]
    (f 1)
    (f 2)
    (is (= 2 @calls))
    ;; Evict only [1]; [2] stays cached.
    (memo/memo-clear! f [1])
    (f 2)
    (is (= 2 @calls))
    (f 1)
    (is (= 3 @calls))))

(deftest memoized-predicate
  (let [f (memo/memo (fn [x] x))]
    (is (memo/memoized? f))
    (is (not (memo/memoized? (fn [x] x))))
    (is (not (memo/memoized? inc)))))

(deftest memo-unwrap-always-recomputes
  (let [calls (atom 0)
        f (memo/memo (fn [x] (swap! calls inc) x))
        raw (memo/memo-unwrap f)]
    (f 1)
    (f 1)
    (is (= 1 @calls))
    ;; The unwrapped function bypasses the cache entirely.
    (raw 1)
    (raw 1)
    (is (= 3 @calls))))

(deftest snapshot-reflects-cached-entries
  (let [f (memo/memo (fn [x] (* x 10)))]
    (is (= {} (memo/snapshot f)))
    (f 1)
    (f 2)
    (is (= {[1] 10 [2] 20} (memo/snapshot f)))
    (memo/memo-clear! f [1])
    (is (= {[2] 20} (memo/snapshot f)))))

(deftest memo-swap-seeds-cache
  (let [calls (atom 0)
        f (memo/memo (fn [x] (swap! calls inc) x))]
    (memo/memo-swap! f {[5] 500})
    ;; The seeded entry is served without computing.
    (is (= 500 (f 5)))
    (is (= 0 @calls))
    (is (= {[5] 500} (memo/snapshot f)))))

(deftest memo-reset-replaces-cache
  (let [f (memo/memo (fn [x] x))]
    (f 1)
    (f 2)
    (memo/memo-reset! f {[9] 90})
    (is (= {[9] 90} (memo/snapshot f)))
    (is (= 90 (f 9)))))

(deftest seeded-memo-prepopulates
  (let [calls (atom 0)
        f (memo/memo (fn [x] (swap! calls inc) x) {[7] 700})]
    (is (= 700 (f 7)))
    (is (= 0 @calls))
    (is (= 8 (f 8)))
    (is (= 1 @calls))))

(deftest non-memoized-fn-errors-on-management
  (is (thrown? Exception (memo/memo-unwrap (fn [x] x))))
  (is (thrown? Exception (memo/snapshot inc)))
  (is (thrown? Exception (memo/memo-clear! (fn [x] x)))))

;; ---------------------------------------------------------------------------
;; Eviction policies.
;; ---------------------------------------------------------------------------

(deftest lru-evicts-least-recently-used
  (let [calls (atom 0)
        f (memo/lru (fn [x] (swap! calls inc) x) :lru/threshold 2)]
    (f 1)                 ; {1}
    (f 2)                 ; {1 2}
    (is (= 2 @calls))
    (f 1)                 ; touch 1, now 2 is least-recently-used
    (is (= 2 @calls))     ; hit, no recompute
    (f 3)                 ; miss; evicts 2 (the LRU entry)
    (is (= 3 @calls))
    (f 1)                 ; 1 was kept, still cached
    (is (= 3 @calls))
    (f 2)                 ; 2 was evicted, recomputes
    (is (= 4 @calls))))

(deftest fifo-evicts-oldest-inserted
  (let [calls (atom 0)
        f (memo/fifo (fn [x] (swap! calls inc) x) :fifo/threshold 2)]
    (f 1)                 ; {1}
    (f 2)                 ; {1 2}
    (f 3)                 ; evicts 1 (oldest inserted)
    (is (= 3 @calls))
    (f 3)                 ; cached
    (is (= 3 @calls))
    (f 1)                 ; evicted, recomputes
    (is (= 4 @calls))))

(deftest lu-evicts-least-used
  (let [calls (atom 0)
        f (memo/lu (fn [x] (swap! calls inc) x) :lu/threshold 2)]
    (f 1)                 ; use 1: miss
    (f 1)                 ; use 1: hit (count rises)
    (f 2)                 ; use 2: miss, once
    (is (= 2 @calls))
    (f 3)                 ; miss; evicts 2 (fewest uses)
    (is (= 3 @calls))
    (f 1)                 ; 1 was most-used, kept
    (is (= 3 @calls))
    (f 2)                 ; 2 was evicted, recomputes
    (is (= 4 @calls))))

(deftest ttl-entry-expires-after-threshold
  (let [calls (atom 0)
        f (memo/ttl (fn [x] (swap! calls inc) x) :ttl/threshold 50)]
    (is (= 10 (f 10)))
    (is (= 10 (f 10)))
    (is (= 1 @calls))     ; live, served from cache
    (Thread/sleep 80)     ; let the entry age past its ttl
    (is (= 10 (f 10)))
    (is (= 2 @calls))))   ; expired, recomputed

(deftest ttl-entry-live-within-threshold
  (let [calls (atom 0)
        f (memo/ttl (fn [x] (swap! calls inc) x) :ttl/threshold 5000)]
    (f 1)
    (f 1)
    ;; Well inside the ttl: still one computation.
    (is (= 1 @calls))))

;; ---------------------------------------------------------------------------
;; build-memoizer directly over a clojure.core.cache factory.
;; ---------------------------------------------------------------------------

(deftest build-memoizer-over-cache-factory
  (let [calls (atom 0)
        f (memo/build-memoizer
            (fn [seed] (clojure.core.cache/basic-cache-factory seed))
            (fn [x] (swap! calls inc) (inc x)))]
    (is (= 2 (f 1)))
    (is (= 2 (f 1)))
    (is (= 1 @calls))
    (is (memo/memoized? f))))

(run-tests-and-exit)
