;; clojure.core.cache — a caching protocol and a family of cache types.
;;
;; A cache is a value carrying a backing association of keys to results
;; plus whatever bookkeeping its eviction policy needs. Every cache type
;; is a field-slot record implementing CacheProtocol inline, so the cache
;; is the protocol's value — not a host map dressed up as a collection.
;; Reads go through the read-through helpers (`through` / `through-cache`):
;; a present key is hit, an absent key is computed once and missed in.
;;
;; SoftCache and soft-cache-factory are intentionally omitted: they lean
;; on a host reference type that clears entries under memory pressure, and
;; mino has no equivalent reference layer, so a faithful port is not
;; possible. Every other upstream cache type is provided.

(ns clojure.core.cache
  "A caching protocol and a family of caches that implement it: a basic
  always-hold cache, plus first-in-first-out, least-recently-used,
  time-to-live, and least-used eviction policies. Caches are values:
  every operation returns a new cache, and the originals are unchanged.")

(defprotocol CacheProtocol
  "The contract every cache type implements. A cache associates keys to
  computed results and decides, per policy, what to drop when it grows
  past its bound."
  (lookup [cache e]
          [cache e not-found]
    "Returns the result associated with e, or not-found (default nil)
    when e is absent. Does not record usage; pair with has? and hit.")
  (has? [cache e]
    "Returns true when e has a live association in the cache.")
  (hit [cache e]
    "Returns the cache with whatever bookkeeping a successful read of e
    implies recorded. Call only when (has? cache e) is true.")
  (miss [cache e ret]
    "Returns the cache with e associated to ret, applying the eviction
    policy if the addition pushes the cache past its bound.")
  (evict [cache e]
    "Returns the cache with any association for e removed.")
  (seed [cache base]
    "Returns the cache reset so its backing association is base, with
    bookkeeping rebuilt for the seeded keys."))

;; ---------------------------------------------------------------------------
;; BasicCache — holds every association, never evicts.
;; ---------------------------------------------------------------------------

(defrecord BasicCache [cache]
  CacheProtocol
  (lookup
    ([_ item] (get cache item))
    ([_ item not-found] (get cache item not-found)))
  (has? [_ item]
    (contains? cache item))
  (hit [this item] this)
  (miss [_ item result]
    (->BasicCache (assoc cache item result)))
  (evict [_ item]
    (->BasicCache (dissoc cache item)))
  (seed [_ base]
    (->BasicCache base)))

;; ---------------------------------------------------------------------------
;; FIFOCache — bounded by threshold, evicts the oldest inserted key.
;; q holds keys in insertion order, oldest at the head.
;; ---------------------------------------------------------------------------

(defn- prune-queue
  "Drops every key from q that is no longer present in the backing map,
  preserving insertion order for the survivors."
  [q cache]
  (reduce (fn [acc k]
            (if (contains? cache k) (conj acc k) acc))
          clojure.lang.PersistentQueue/EMPTY
          q))

(defrecord FIFOCache [cache q threshold]
  CacheProtocol
  (lookup
    ([_ item] (get cache item))
    ([_ item not-found] (get cache item not-found)))
  (has? [_ item]
    (contains? cache item))
  (hit [this item] this)
  (miss [_ item result]
    (let [c (assoc cache item result)
          q* (conj q item)]
      (if (> (count c) threshold)
        (let [eldest (peek q*)]
          (->FIFOCache (dissoc c eldest) (pop q*) threshold))
        (->FIFOCache c q* threshold))))
  (evict [this item]
    (if (contains? cache item)
      (let [c (dissoc cache item)]
        (->FIFOCache c (prune-queue q c) threshold))
      this))
  (seed [_ base]
    (->FIFOCache base
                (into clojure.lang.PersistentQueue/EMPTY (keys base))
                threshold)))

(defn fifo-cache-factory
  "Builds a first-in-first-out cache seeded with base. When the count of
  associations would exceed threshold, the oldest inserted key is
  evicted. threshold must be a positive number and defaults to 32."
  [base & {:keys [threshold] :or {threshold 32}}]
  {:pre [(map? base) (number? threshold) (< 0 threshold)]}
  (seed (->FIFOCache {} clojure.lang.PersistentQueue/EMPTY threshold) base))

;; ---------------------------------------------------------------------------
;; LRUCache — bounded by threshold, evicts the least-recently-used key.
;; lru maps each key to the tick at which it was last touched; tick is a
;; monotonically rising counter.
;; ---------------------------------------------------------------------------

(defn- build-leastness-queue
  "Maps each key of base to a starting usage marker, leaving room above
  the seeded markers for fresh activity to outrank them."
  [base start-at]
  (into {} (for [[i k] (map vector (range) (keys base))]
             [k (+ start-at i)])))

(defn- peek-min-by-val
  "Returns the [key tick] entry with the smallest tick, i.e. the
  least-recently-touched key. m must be non-empty."
  [m]
  (reduce (fn [a b] (if (<= (val a) (val b)) a b)) (seq m)))

(defrecord LRUCache [cache lru tick threshold]
  CacheProtocol
  (lookup
    ([_ item] (get cache item))
    ([_ item not-found] (get cache item not-found)))
  (has? [_ item]
    (contains? cache item))
  (hit [_ item]
    (let [tick* (inc tick)]
      (->LRUCache cache (assoc lru item tick*) tick* threshold)))
  (miss [_ item result]
    (let [tick* (inc tick)]
      (if (>= (count cache) threshold)
        (let [k (if (contains? lru item)
                  item
                  (first (peek-min-by-val lru)))
              c (-> cache (dissoc k) (assoc item result))
              l (-> lru (dissoc k) (assoc item tick*))]
          (->LRUCache c l tick* threshold))
        (->LRUCache (assoc cache item result)
                   (assoc lru item tick*)
                   tick*
                   threshold))))
  (evict [this item]
    (if (contains? cache item)
      (->LRUCache (dissoc cache item) (dissoc lru item) (inc tick) threshold)
      this))
  (seed [_ base]
    (->LRUCache base
               (build-leastness-queue base 0)
               0
               threshold)))

(defn lru-cache-factory
  "Builds a least-recently-used cache seeded with base. When adding an
  association would exceed threshold, the key whose last touch is oldest
  is evicted. A successful lookup must be recorded with hit to refresh a
  key's recency. threshold must be positive and defaults to 32."
  [base & {:keys [threshold] :or {threshold 32}}]
  {:pre [(map? base) (number? threshold) (< 0 threshold)]}
  (seed (->LRUCache {} {} 0 threshold) base))

;; ---------------------------------------------------------------------------
;; TTLCache — entries expire ttl-ms milliseconds after they are written.
;; ttl maps each key to the wall-clock time (epoch ms) it was written.
;; ---------------------------------------------------------------------------

(defn- expired?
  "True when the entry written at write-time is older than ttl-ms as of
  the supplied now."
  [now write-time ttl-ms]
  (> (- now write-time) ttl-ms))

(defn- prune-expired
  "Removes every entry whose age exceeds ttl-ms as of now, from both the
  backing map and the timestamp map. Returns [cache ttl]."
  [cache ttl ttl-ms now]
  (reduce (fn [[c t] k]
            (if (expired? now (get ttl k) ttl-ms)
              [(dissoc c k) (dissoc t k)]
              [c t]))
          [cache ttl]
          (keys ttl)))

(defrecord TTLCache [cache ttl ttl-ms]
  CacheProtocol
  (lookup
    ([this item]
     (lookup this item nil))
    ([_ item not-found]
     (if (and (contains? cache item)
              (not (expired? (time-ms) (get ttl item) ttl-ms)))
       (get cache item)
       not-found)))
  (has? [_ item]
    (and (contains? ttl item)
         (not (expired? (time-ms) (get ttl item) ttl-ms))))
  (hit [this item] this)
  (miss [_ item result]
    (let [now (time-ms)
          [c t] (prune-expired cache ttl ttl-ms now)]
      (->TTLCache (assoc c item result)
                 (assoc t item now)
                 ttl-ms)))
  (evict [_ item]
    (->TTLCache (dissoc cache item) (dissoc ttl item) ttl-ms))
  (seed [_ base]
    (let [now (time-ms)]
      (->TTLCache base
                 (into {} (for [k (keys base)] [k now]))
                 ttl-ms))))

(defn ttl-cache-factory
  "Builds a time-to-live cache seeded with base. An association is live
  for ttl milliseconds after it is written; reads past that age behave as
  if the key were absent. ttl must be a non-negative number."
  [base & {:keys [ttl] :or {ttl 2000}}]
  {:pre [(map? base) (number? ttl) (<= 0 ttl)]}
  (seed (->TTLCache {} {} ttl) base))

;; ---------------------------------------------------------------------------
;; LUCache — bounded by threshold, evicts the least-used key. lu maps each
;; key to the number of times it has been read or written.
;; ---------------------------------------------------------------------------

(defn- peek-min-use
  "Returns the [key uses] entry with the smallest use count. m must be
  non-empty."
  [m]
  (reduce (fn [a b] (if (<= (val a) (val b)) a b)) (seq m)))

(defrecord LUCache [cache lu threshold]
  CacheProtocol
  (lookup
    ([_ item] (get cache item))
    ([_ item not-found] (get cache item not-found)))
  (has? [_ item]
    (contains? cache item))
  (hit [_ item]
    (->LUCache cache
              (assoc lu item (inc (get lu item 0)))
              threshold))
  (miss [_ item result]
    (if (>= (count cache) threshold)
      (let [k (if (contains? lu item)
                item
                (first (peek-min-use lu)))
            c (-> cache (dissoc k) (assoc item result))
            l (-> lu (dissoc k) (assoc item (inc (get lu item 0))))]
        (->LUCache c l threshold))
      (->LUCache (assoc cache item result)
                (assoc lu item (inc (get lu item 0)))
                threshold)))
  (evict [this item]
    (if (contains? cache item)
      (->LUCache (dissoc cache item) (dissoc lu item) threshold)
      this))
  (seed [_ base]
    (->LUCache base
              (into {} (for [k (keys base)] [k 0]))
              threshold)))

(defn lu-cache-factory
  "Builds a least-used cache seeded with base. When adding an association
  would exceed threshold, the key with the fewest reads-plus-writes is
  evicted. A successful lookup must be recorded with hit to raise a key's
  use count. threshold must be positive and defaults to 32."
  [base & {:keys [threshold] :or {threshold 32}}]
  {:pre [(map? base) (number? threshold) (< 0 threshold)]}
  (seed (->LUCache {} {} threshold) base))

;; ---------------------------------------------------------------------------
;; BasicCache factory.
;; ---------------------------------------------------------------------------

(defn basic-cache-factory
  "Builds a basic cache seeded with base. The basic cache holds every
  association and never evicts."
  [base]
  {:pre [(map? base)]}
  (->BasicCache base))

;; ---------------------------------------------------------------------------
;; Read-through helpers.
;; ---------------------------------------------------------------------------

(defn through
  "Reads item through cache, computing on a miss. When the cache already
  holds item, returns the cache with the hit recorded. Otherwise computes
  the value — (value-fn item) by default, or (wrap-fn value-fn item) when
  a wrap-fn is supplied — and returns the cache with the result missed
  in. wrap-fn lets a caller stage the computation (for example, to defer
  it) while still feeding value-fn item underneath."
  ([value-fn cache item]
   (through value-fn #(%1 %2) cache item))
  ([value-fn wrap-fn cache item]
   (if (has? cache item)
     (hit cache item)
     (miss cache item (wrap-fn value-fn item)))))

(defn through-cache
  "Reads item through cache, computing on a miss, with the cache as the
  leading argument. When the cache holds item, returns the cache with the
  hit recorded. Otherwise computes (value-fn item), or (wrap-fn value-fn
  item) when wrap-fn is supplied, and returns the cache with the result
  missed in."
  ([cache item value-fn]
   (through-cache cache item #(%1 %2) value-fn))
  ([cache item wrap-fn value-fn]
   (if (has? cache item)
     (hit cache item)
     (miss cache item (wrap-fn value-fn item)))))
