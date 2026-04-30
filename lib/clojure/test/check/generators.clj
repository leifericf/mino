(ns clojure.test.check.generators
  (:refer-clojure :exclude [boolean char double int keyword list map
                            set string symbol vector]))

;; Minimal clojure.test.check.generators port for mino.
;;
;; A generator is wrapped in a record-like map {:gen-fn (fn [size]
;; -> value)}. The generator function consumes the per-state PRNG
;; (clojure.core/rand) so that (random-seed! seed) makes a sample
;; sequence reproducible. Shrinking is deferred -- a failing
;; property reports the sample as-found.

;; --- internals --------------------------------------------------------------

(defn- gen-record [f]
  {:test.check/generator true :gen-fn f})

(defn generator?
  "Returns true if x is a generator built by this namespace."
  [x]
  (and (map? x) (true? (:test.check/generator x))))

(defn- ensure-gen [g]
  (when-not (generator? g)
    (throw (ex-info "expected a test.check generator" {:got g})))
  g)

(defn- rand-int-in
  "Random integer in [lo, hi] inclusive."
  [lo hi]
  (let [span (- hi lo)]
    (if (<= span 0)
      lo
      (+ lo (clojure.core/int (* (rand) (inc span)))))))

(defn call-gen
  "Generates one value from `g` at the given size."
  [g size]
  ((:gen-fn (ensure-gen g)) size))

;; --- core combinators -------------------------------------------------------

(defn return
  "Generator that always yields v."
  [v]
  (gen-record (fn [_size] v)))

(defn fmap
  "Applies f to each value produced by g."
  [f g]
  (gen-record (fn [size] (f (call-gen g size)))))

(defn bind
  "Calls k on each value produced by g; k must return a generator."
  [g k]
  (gen-record
    (fn [size]
      (let [v  (call-gen g size)
            g2 (k v)]
        (call-gen g2 size)))))

(defn such-that
  "Filters g to values where (pred v) is truthy. Retries up to
   max-tries (default 10) times before giving up with ex-info."
  ([pred g] (such-that pred g 10))
  ([pred g max-tries]
   (gen-record
     (fn [size]
       (loop [tries-left max-tries]
         (if (zero? tries-left)
           (throw (ex-info "such-that: gave up after max-tries"
                           {:max-tries max-tries}))
           (let [v (call-gen g size)]
             (if (pred v) v (recur (dec tries-left))))))))))

(defn elements
  "Generator that picks uniformly from a non-empty collection."
  [coll]
  (let [v (vec coll)
        n (count v)]
    (when (zero? n)
      (throw (ex-info "elements: collection must be non-empty" {})))
    (gen-record (fn [_size] (nth v (rand-int n))))))

(defn one-of
  "Picks uniformly from a non-empty collection of generators, then
   draws one value from the chosen generator."
  [gens]
  (let [v (vec gens)
        n (count v)]
    (when (zero? n)
      (throw (ex-info "one-of: must have at least one generator" {})))
    (doseq [g v] (ensure-gen g))
    (gen-record
      (fn [size]
        (call-gen (nth v (rand-int n)) size)))))

;; --- scalar generators ------------------------------------------------------

(def boolean
  (gen-record (fn [_size] (< (rand) 0.5))))

(def int
  ;; Range scales with size; size 0 yields {0} only.
  (gen-record (fn [size] (rand-int-in (- size) size))))

(def nat
  (gen-record (fn [size] (rand-int-in 0 size))))

(def s-pos-int
  (gen-record (fn [size] (rand-int-in 1 (max 1 size)))))

(def neg-int
  (gen-record (fn [size] (- (rand-int-in 1 (max 1 size))))))

(def double
  (gen-record (fn [size]
                (let [span (* 2.0 (max 1.0 (clojure.core/double size)))]
                  (- (* span (rand)) (clojure.core/double size))))))

;; mino strings don't have a separate char type; "char" generators
;; yield single-character strings instead. Pre-build the pool strings
;; once and index into them per generation.
(def ^:private printable-chars
  " !\"#$%&'()*+,-./0123456789:;<=>?@ABCDEFGHIJKLMNOPQRSTUVWXYZ[\\]^_`abcdefghijklmnopqrstuvwxyz{|}~")

(def ^:private alpha-chars
  "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz")

(def ^:private alphanumeric-chars
  "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz")

(def char
  (gen-record (fn [_size]
                (let [i (rand-int (count printable-chars))]
                  (subs printable-chars i (inc i))))))

(def char-ascii char)

(def char-alpha
  (gen-record (fn [_size]
                (let [i (rand-int (count alpha-chars))]
                  (subs alpha-chars i (inc i))))))

(def char-alphanumeric
  (gen-record (fn [_size]
                (let [i (rand-int (count alphanumeric-chars))]
                  (subs alphanumeric-chars i (inc i))))))

;; --- compound generators ----------------------------------------------------

(defn vector
  "Vector of values from g. Size grows with the test size."
  ([g] (gen-record
         (fn [size]
           (let [n (rand-int-in 0 size)]
             (vec (for [_ (range n)] (call-gen g size)))))))
  ([g num-elements]
   (gen-record
     (fn [size]
       (vec (for [_ (range num-elements)] (call-gen g size))))))
  ([g min-elements max-elements]
   (gen-record
     (fn [size]
       (let [n (rand-int-in min-elements max-elements)]
         (vec (for [_ (range n)] (call-gen g size))))))))

(defn list
  "List of values from g."
  [g]
  (gen-record
    (fn [size]
      (let [n (rand-int-in 0 size)]
        (apply clojure.core/list (for [_ (range n)] (call-gen g size)))))))

(defn set
  "Set of values from g; size is an upper bound."
  [g]
  (gen-record
    (fn [size]
      (let [n (rand-int-in 0 size)]
        (clojure.core/set (for [_ (range n)] (call-gen g size)))))))

(defn map
  "Map with keys from kg and vals from vg."
  [kg vg]
  (gen-record
    (fn [size]
      (let [n (rand-int-in 0 size)]
        (into {} (for [_ (range n)]
                   [(call-gen kg size) (call-gen vg size)]))))))

(defn tuple
  "Tuple (vector) of one value per supplied generator, in order."
  [& gens]
  (gen-record
    (fn [size]
      (mapv (fn [g] (call-gen g size)) gens))))

(def string
  (gen-record
    (fn [size]
      (let [n (rand-int-in 0 size)]
        (apply str (for [_ (range n)] (call-gen char size)))))))

(def string-ascii
  (gen-record
    (fn [size]
      (let [n (rand-int-in 0 size)]
        (apply str (for [_ (range n)] (call-gen char-ascii size)))))))

(def string-alphanumeric
  (gen-record
    (fn [size]
      (let [n (rand-int-in 0 size)]
        (apply str (for [_ (range n)] (call-gen char-alphanumeric size)))))))

(def keyword
  (gen-record
    (fn [size]
      (let [n (max 1 (rand-int-in 1 (max 1 size)))]
        (clojure.core/keyword
          (apply str (for [_ (range n)] (call-gen char-alpha size))))))))

(def symbol
  (gen-record
    (fn [size]
      (let [n (max 1 (rand-int-in 1 (max 1 size)))]
        (clojure.core/symbol
          (apply str (for [_ (range n)] (call-gen char-alpha size))))))))

(def any
  ;; A simple "any" that mixes scalar generators. Recursive containers
  ;; are deferred to keep the MVP small.
  (one-of [int boolean string keyword]))

;; --- sampling ---------------------------------------------------------------

(defn sample
  "Generate n samples from g (default 10) at growing size."
  ([g] (sample g 10))
  ([g n]
   (vec (for [size (range n)] (call-gen g size)))))

(defn generate
  "Generate one value from g (default size 30)."
  ([g] (generate g 30))
  ([g size] (call-gen g size)))
