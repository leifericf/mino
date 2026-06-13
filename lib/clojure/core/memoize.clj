;; clojure.core.memoize — function memoization built on clojure.core.cache.
;;
;; A memoized function pairs an original function with a cache value held in
;; an atom. Each call builds an argument vector, reads it through the cache,
;; and either returns the held result (a hit) or computes the result once,
;; stores it, and returns it (a miss). The eviction policy is whatever cache
;; type backs the memoizer: a basic always-hold cache, or a first-in-first-out,
;; least-recently-used, time-to-live, or least-used cache from
;; clojure.core.cache.
;;
;; Cached results are wrapped so that a miss computes exactly once even if the
;; same key is asked for while the first computation is in flight: the cache
;; holds a deferred holder, and the value is forced on read. mino runs a single
;; interpreter state, so an atom plus swap! is the whole concurrency story — no
;; host concurrency primitives are involved.
;;
;; The memoizer (a PluggableMemoization value paired with the cache atom and
;; the original function) travels on the returned function's metadata, so
;; memoized?, memo-unwrap, snapshot, memo-clear!, and the swap/reset helpers
;; can recover it from any memoized function.

(ns clojure.core.memoize
  "Memoize a function against a pluggable cache. The returned function caches
  its results keyed by the vector of arguments it was called with; the backing
  cache decides what to keep and what to drop. Constructors are provided over
  every clojure.core.cache policy: memo (basic), fifo, lru, ttl, and lu."
  (:require [clojure.core.cache :as cache]))

;; ---------------------------------------------------------------------------
;; Deferred holder — computes its value at most once.
;;
;; Each cache entry is one of these. A miss stores a holder built around the
;; pending computation; the first read forces it. Because the cache value is
;; the holder rather than the bare result, two reads of the same key share one
;; computation.
;; ---------------------------------------------------------------------------

(defprotocol ^:private Derefable
  (derefable-deref [this]
    "Forces and returns the held value, computing it on the first call."))

(defrecord ^:private RetryingDelay [thunk value-box]
  Derefable
  (derefable-deref [_]
    (let [held @value-box]
      (if (= ::unset held)
        (let [computed (thunk)]
          (reset! value-box computed)
          computed)
        held))))

(defn- d-lay
  "Builds a deferred holder around thunk; (derefable-deref it) runs thunk once
  and caches its result."
  [thunk]
  (->RetryingDelay thunk (atom ::unset)))

(defn- derefable-value
  "Forces v when it is a deferred holder, otherwise returns it unchanged. A
  seeded base map may carry plain values, so reads tolerate both shapes."
  [v]
  (if (satisfies? Derefable v)
    (derefable-deref v)
    v))

;; ---------------------------------------------------------------------------
;; PluggableMemoization — the cache wrapped so every entry is a deferred holder.
;;
;; It is itself a CacheProtocol value: it delegates to the backing cache, but
;; on a miss wraps the supplied result thunk in a holder before storing it, so
;; the policy machinery in clojure.core.cache works unchanged.
;; ---------------------------------------------------------------------------

(defrecord PluggableMemoization [cache f]
  cache/CacheProtocol
  (lookup
    ([_ item] (cache/lookup cache item))
    ([_ item not-found] (cache/lookup cache item not-found)))
  (has? [_ item]
    (cache/has? cache item))
  (hit [_ item]
    (->PluggableMemoization (cache/hit cache item) f))
  (miss [_ item result]
    (->PluggableMemoization (cache/miss cache item result) f))
  (evict [_ item]
    (->PluggableMemoization (cache/evict cache item) f))
  (seed [_ base]
    (->PluggableMemoization (cache/seed cache base) f)))

(defn- pluggable-memoization
  "Wraps cache-impl in a PluggableMemoization bound to f."
  [f cache-impl]
  (->PluggableMemoization cache-impl f))

;; ---------------------------------------------------------------------------
;; The memoizer carried on a memoized function's metadata.
;; ---------------------------------------------------------------------------

(defprotocol ^:private MemoizationProtocol
  "What a memoized function's hidden memoizer can do: read a key through its
  cache, swap or unwrap its backing cache, and surface a snapshot."
  (snapshot* [this]
    "Returns the realized argument-vector -> value map of the backing cache.")
  (lazy-snapshot* [this]
    "Returns the argument-vector -> holder map without forcing unrealized
    holders.")
  (memo-cache* [this]
    "Returns the atom holding the backing PluggableMemoization.")
  (original-fn* [this]
    "Returns the un-memoized function."))

(defn- backing-map
  "Returns the raw argument-vector -> holder map inside the backing cache. The
  atom holds a PluggableMemoization whose :cache is the policy cache record,
  whose :cache in turn is the association map."
  [cache-atom]
  (:cache (:cache @cache-atom)))

(defrecord ^:private PluggableMemoizer [cache-atom f]
  MemoizationProtocol
  (snapshot* [_]
    (let [cache-map (backing-map cache-atom)]
      (persistent!
        (reduce-kv (fn [acc k v] (assoc! acc k (derefable-value v)))
                   (transient {})
                   cache-map))))
  (lazy-snapshot* [_]
    (backing-map cache-atom))
  (memo-cache* [_] cache-atom)
  (original-fn* [_] f))

;; ---------------------------------------------------------------------------
;; build-memoizer — the core constructor.
;; ---------------------------------------------------------------------------

(defn build-memoizer
  "Builds a memoized function. cache-factory is a clojure.core.cache factory
  (for example cache/lru-cache-factory); it is called with an empty seed map
  followed by args to produce the backing cache. f is the function to memoize.
  The returned function caches results keyed by the vector of its arguments,
  computing each result once via a deferred holder, and carries its memoizer on
  its metadata so the namespace's helpers can recover it."
  [cache-factory f & args]
  (let [cache-impl (apply cache-factory {} args)
        cache-atom (atom (pluggable-memoization f cache-impl))
        memoizer   (->PluggableMemoizer cache-atom f)
        memoized   (fn [& call-args]
                     (let [k (vec call-args)
                           holder (-> (swap! cache-atom
                                             (fn [c]
                                               (cache/through-cache
                                                 c k
                                                 (fn [_ kk]
                                                   (d-lay #(apply f kk)))
                                                 (fn [_] nil))))
                                      (cache/lookup k))]
                       (derefable-value holder)))]
    (with-meta memoized
               {::memoize memoizer})))

(defn- memoizer-of
  "Returns the hidden memoizer carried by a memoized function f, or nil when f
  carries none."
  [f]
  (::memoize (meta f)))

;; ---------------------------------------------------------------------------
;; Inspection and management.
;; ---------------------------------------------------------------------------

(defn memoized?
  "Returns true when f is a memoized function produced by this namespace."
  [f]
  (boolean (memoizer-of f)))

(defn- check-memoized!
  "Throws a clear error when f is not a memoized function, otherwise returns
  its memoizer."
  [f]
  (or (memoizer-of f)
      (throw (ex-info "Not a memoized function"
                      {:fn f}))))

(defn memo-unwrap
  "Returns the original, un-memoized function wrapped inside the memoized
  function f. Calling the result always recomputes."
  [f]
  (original-fn* (check-memoized! f)))

(defn snapshot
  "Returns a plain map of the argument vectors currently cached in f mapped to
  their realized results."
  [f]
  (snapshot* (check-memoized! f)))

(defn lazy-snapshot
  "Returns a plain map of the argument vectors currently cached in f mapped to
  their results, without forcing entries whose computation has not run. mino
  models entries with deferred holders, so an unrealized holder is left as is."
  [f]
  (lazy-snapshot* (check-memoized! f)))

(defn memo-clear!
  "Empties f's cache when called with one argument. With an argument vector,
  evicts only that entry. Returns f."
  ([f]
   (let [m (check-memoized! f)
         cache-atom (memo-cache* m)]
     (swap! cache-atom cache/seed {})
     f))
  ([f args]
   (let [m (check-memoized! f)
         cache-atom (memo-cache* m)]
     (swap! cache-atom cache/evict (vec args))
     f)))

(defn memo-swap!
  "Replaces the contents of f's cache with base, an argument-vector -> value
  map. Returns f."
  [f base]
  (let [m (check-memoized! f)
        cache-atom (memo-cache* m)]
    (swap! cache-atom (fn [c] (cache/seed c base)))
    f))

(defn memo-reset!
  "Resets f's cache to base, an argument-vector -> value map. Same effect as
  memo-swap!; named for the reset reading of the operation. Returns f."
  [f base]
  (memo-swap! f base))

;; ---------------------------------------------------------------------------
;; Cache-backed constructors.
;; ---------------------------------------------------------------------------

(defn memo
  "Memoizes f against a basic cache that holds every result and never evicts.
  An optional seed map pre-populates the cache."
  ([f] (memo f {}))
  ([f seed]
   (build-memoizer
     (fn [_ & _] (cache/basic-cache-factory seed))
     f)))

(defn- opt
  "Reads option-key out of the option pairs opts (a seq of alternating key and
  value), returning default when the key is absent. Defaults are resolved here
  rather than via destructuring :or, which mino does not yet apply for the
  explicit symbol/namespaced-key map-binding form."
  [opts option-key default]
  (let [m (apply hash-map opts)]
    (get m option-key default)))

(defn fifo
  "Memoizes f against a first-in-first-out cache. When the count of cached
  results exceeds the threshold, the oldest inserted entry is evicted. Options:
  :fifo/threshold (default 32) and :fifo/base (a seed map)."
  [f & opts]
  (let [threshold (opt opts :fifo/threshold 32)
        base      (opt opts :fifo/base {})]
    (build-memoizer
      (fn [_] (cache/fifo-cache-factory base :threshold threshold))
      f)))

(defn lru
  "Memoizes f against a least-recently-used cache. When adding a result would
  exceed the threshold, the entry whose last read is oldest is evicted. Options:
  :lru/threshold (default 32) and :lru/base (a seed map)."
  [f & opts]
  (let [threshold (opt opts :lru/threshold 32)
        base      (opt opts :lru/base {})]
    (build-memoizer
      (fn [_] (cache/lru-cache-factory base :threshold threshold))
      f)))

(defn ttl
  "Memoizes f against a time-to-live cache. A cached result is live for
  :ttl/threshold milliseconds after it is written; a read past that age
  recomputes. Options: :ttl/threshold in milliseconds (default 2000) and
  :ttl/base (a seed map)."
  [f & opts]
  (let [threshold (opt opts :ttl/threshold 2000)
        base      (opt opts :ttl/base {})]
    (build-memoizer
      (fn [_] (cache/ttl-cache-factory base :ttl threshold))
      f)))

(defn lu
  "Memoizes f against a least-used cache. When adding a result would exceed the
  threshold, the entry with the fewest reads-plus-writes is evicted. Options:
  :lu/threshold (default 32) and :lu/base (a seed map)."
  [f & opts]
  (let [threshold (opt opts :lu/threshold 32)
        base      (opt opts :lu/base {})]
    (build-memoizer
      (fn [_] (cache/lu-cache-factory base :threshold threshold))
      f)))
