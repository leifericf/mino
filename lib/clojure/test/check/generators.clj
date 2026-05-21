(ns clojure.test.check.generators
  (:refer-clojure :exclude [boolean char double int keyword list map
                            set string symbol vector]))

;; clojure.test.check.generators port for mino with real rose-tree
;; shrinking. A generator is wrapped in
;;   {:test.check/generator true :gen-fn (fn [size] -> rose-tree)}
;; where a rose-tree is [value lazy-seq-of-child-rose-trees]. The
;; lazy-seq-of-child-rose-trees produces smaller variants of value;
;; quick-check walks this tree on failure to find a minimal counter-
;; example. Generators that don't shrink return [value ()].

;; --- rose-tree primitives ---------------------------------------------------

(defn rose-pure
  "Rose tree with no shrinks: [v ()]."
  [v]
  [v ()])

(defn rose-val
  "Value at the rose tree's root."
  [r]
  (first r))

(defn rose-children
  "Lazy seq of child rose trees (smaller values)."
  [r]
  (second r))

(defn rose-fmap
  "Apply f to every value in a rose tree, preserving structure."
  [f r]
  [(f (rose-val r))
   (lazy-seq
     (clojure.core/map (fn [c] (rose-fmap f c)) (rose-children r)))])

(defn rose-filter
  "Keep only sub-rose-trees whose value satisfies pred."
  [pred r]
  (when (pred (rose-val r))
    [(rose-val r)
     (lazy-seq
       (clojure.core/filter identity
         (clojure.core/map (fn [c] (rose-filter pred c)) (rose-children r))))]))

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
  "Generates one rose tree from `g` at the given size."
  [g size]
  ((:gen-fn (ensure-gen g)) size))

;; --- shrink-tree builders for scalars ---------------------------------------

(declare int-rose)

(defn int-rose
  "Rose tree for an integer, shrinking toward 0. Children: zero, then
  halved values."
  [n]
  [n (lazy-seq
       (when-not (zero? n)
         (let [halved (quot n 2)]
           (concat
             (when-not (= n 0) [(int-rose 0)])
             (when-not (or (zero? halved) (= halved n))
               [(int-rose halved)])
             (let [closer (- n (if (pos? n) 1 -1))]
               (when (and (not (zero? closer)) (not= closer halved))
                 [(int-rose closer)]))))))])

(defn- bool-rose
  "Rose tree for a boolean. true shrinks to false; false has no shrinks
  (matches the JVM clojure.test.check convention)."
  [b]
  (if b [true [[false ()]]] [false ()]))

;; vec-rose-tree builds the rose tree for a vector. The shrink children
;; are wrapped in a lazy-seq so they only materialize when the walker
;; descends.

(declare vec-rose-tree)

(defn vec-drop-each-rose
  "Build rose trees by removing each rose at index i in turn."
  [roses]
  (clojure.core/map
    (fn [i]
      (let [smaller (vec (concat (subvec roses 0 i) (subvec roses (inc i))))]
        (vec-rose-tree smaller)))
    (range (count roses))))

(defn vec-shrink-each-rose
  "Build rose trees by shrinking the rose at position i (using that
  position's own children) and recombining."
  [roses]
  (mapcat
    (fn [i]
      (let [elem-rose (nth roses i)]
        (clojure.core/map
          (fn [child]
            (vec-rose-tree (assoc roses i child)))
          (rose-children elem-rose))))
    (range (count roses))))

(defn vec-rose-tree
  "Rose tree for a vector of element-rose-trees. The root value is
  the vector of element root values; shrink children are produced
  by dropping each element, then by shrinking each element."
  [roses]
  [(mapv rose-val roses)
   (lazy-seq
     (when (seq roses)
       (concat (vec-drop-each-rose roses)
               (vec-shrink-each-rose roses))))])

(defn- str-rose
  "Rose tree for a string. Shrinks by progressively dropping characters
  from the end (the simplest shrinker that still finds short repros)."
  [s]
  [s (lazy-seq
       (when (pos? (count s))
         (clojure.core/map str-rose
                           (for [i (range (count s))]
                             (str (subs s 0 i) (subs s (inc i)))))))])

;; --- core combinators -------------------------------------------------------

(defn return
  "Generator that always yields v. The value has no shrinks."
  [v]
  (gen-record (fn [_size] (rose-pure v))))

(defn fmap
  "Applies f to each value produced by g, preserving the shrink tree."
  [f g]
  (gen-record (fn [size] (rose-fmap f (call-gen g size)))))

(defn bind
  "Calls k on each value produced by g; k must return a generator. The
  returned shrink tree comes from the generator k produces -- we don't
  re-thread the source generator's shrinks (matches the simplest
  semantics of clojure.test.check/bind without the second-pass shrink)."
  [g k]
  (gen-record
    (fn [size]
      (let [r  (call-gen g size)
            g2 (k (rose-val r))]
        (call-gen g2 size)))))

(defn such-that
  "Filters g to values where (pred v) is truthy. Retries up to
   max-tries (default 10) times. The shrink tree is filtered to keep
   only smaller values that still satisfy pred."
  ([pred g] (such-that pred g 10))
  ([pred g max-tries]
   (gen-record
     (fn [size]
       (loop [tries-left max-tries]
         (if (zero? tries-left)
           (throw (ex-info "such-that: gave up after max-tries"
                           {:max-tries max-tries}))
           (let [r (call-gen g size)]
             (if (pred (rose-val r))
               (or (rose-filter pred r) [(rose-val r) ()])
               (recur (dec tries-left))))))))))

(defn elements
  "Generator that picks uniformly from a non-empty collection. Shrinks
  toward earlier elements in the collection (the first element is
  treated as the simplest)."
  [coll]
  (let [v (vec coll)
        n (count v)]
    (when (zero? n)
      (throw (ex-info "elements: collection must be non-empty" {})))
    (gen-record
      (fn [_size]
        (let [i  (rand-int n)
              vv (nth v i)]
          [vv (lazy-seq
                (when (pos? i)
                  ;; Try the first element as the simplest shrink.
                  [(rose-pure (nth v 0))]))])))))

(defn one-of
  "Picks uniformly from a non-empty collection of generators, then
   draws one value from the chosen generator. Shrinking propagates
   from the chosen generator."
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
  (gen-record (fn [_size] (bool-rose (< (rand) 0.5)))))

(def int
  ;; Range scales with size; size 0 yields {0} only.
  (gen-record (fn [size] (int-rose (rand-int-in (- size) size)))))

(def nat
  (gen-record (fn [size] (int-rose (rand-int-in 0 size)))))

(def s-pos-int
  (gen-record (fn [size] (int-rose (rand-int-in 1 (max 1 size))))))

(def neg-int
  (gen-record (fn [size] (int-rose (- (rand-int-in 1 (max 1 size)))))))

(def double
  (gen-record (fn [size]
                (let [span (* 2.0 (max 1.0 (clojure.core/double size)))
                      v    (- (* span (rand)) (clojure.core/double size))]
                  ;; Doubles shrink toward zero by halving.
                  [v (lazy-seq
                       (when-not (zero? v)
                         (let [h (* v 0.5)]
                           [[0.0 ()] [h ()]])))]))))

;; mino strings don't have a separate char type; "char" generators
;; yield single-character strings instead.
(def ^:private printable-chars
  " !\"#$%&'()*+,-./0123456789:;<=>?@ABCDEFGHIJKLMNOPQRSTUVWXYZ[\\]^_`abcdefghijklmnopqrstuvwxyz{|}~")

(def ^:private alpha-chars
  "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz")

(def ^:private alphanumeric-chars
  "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz")

(def char
  (gen-record (fn [_size]
                (let [i (rand-int (count printable-chars))]
                  (rose-pure (subs printable-chars i (inc i)))))))

(def char-ascii char)

(def char-alpha
  (gen-record (fn [_size]
                (let [i (rand-int (count alpha-chars))]
                  (rose-pure (subs alpha-chars i (inc i)))))))

(def char-alphanumeric
  (gen-record (fn [_size]
                (let [i (rand-int (count alphanumeric-chars))]
                  (rose-pure (subs alphanumeric-chars i (inc i)))))))

;; --- compound generators ----------------------------------------------------

(defn- elem-shrink-fn
  "Build a per-element shrink fn for vec-rose-tree from a generator.
  Used when we need a shrink-tree for an already-produced element."
  [_g]
  ;; Without access to the original size, we treat the element as a
  ;; leaf for shrinking. Future enhancement: store the rose tree of
  ;; each element produced and reuse here.
  (fn [v] (rose-pure v)))

(defn vector
  "Vector of values from g. Size grows with the test size. Each
  generated element keeps its own shrink tree, so the outer vector
  shrinks by removing elements AND by shrinking each element via
  the source generator's shrinkers."
  ([g] (gen-record
         (fn [size]
           (let [n      (rand-int-in 0 size)
                 roses  (vec (for [_ (range n)] (call-gen g size)))]
             (vec-rose-tree roses)))))
  ([g num-elements]
   (gen-record
     (fn [size]
       (let [roses (vec (for [_ (range num-elements)] (call-gen g size)))]
         ;; Fixed-length vectors don't drop elements; only shrink each.
         [(mapv rose-val roses)
          (lazy-seq (vec-shrink-each-rose roses))]))))
  ([g min-elements max-elements]
   (gen-record
     (fn [size]
       (let [n     (rand-int-in min-elements max-elements)
             roses (vec (for [_ (range n)] (call-gen g size)))]
         ;; min-bounded: drop elements only down to the min.
         [(mapv rose-val roses)
          (lazy-seq
            (concat
              (when (> n min-elements) (vec-drop-each-rose roses))
              (vec-shrink-each-rose roses)))])))))

(defn list
  "List of values from g. Shrinks the same way vectors do (drop and
  shrink-each), with the result wrapped in clojure.core/list at each
  rose node."
  [g]
  (gen-record
    (fn [size]
      (let [n     (rand-int-in 0 size)
            roses (vec (for [_ (range n)] (call-gen g size)))]
        (rose-fmap (fn [v] (apply clojure.core/list v))
                   (vec-rose-tree roses))))))

(defn set
  "Set of values from g; size is an upper bound."
  [g]
  (gen-record
    (fn [size]
      (let [n     (rand-int-in 0 size)
            roses (vec (for [_ (range n)] (call-gen g size)))]
        (rose-fmap clojure.core/set (vec-rose-tree roses))))))

(defn map
  "Map with keys from kg and vals from vg. Shrinks by dropping pairs
  (and shrinking individual key/value pairs through the element's own
  rose tree). The pairs are kept as paired rose-trees so the shrinker
  can swap one pair for a smaller version."
  [kg vg]
  (gen-record
    (fn [size]
      (let [n      (rand-int-in 0 size)
            pair-roses (vec
                         (for [_ (range n)]
                           ;; Each pair is itself a rose-pure of [k v]; we
                           ;; don't shrink within a pair (would require
                           ;; cross-product of k/v shrink trees).
                           (rose-pure [(rose-val (call-gen kg size))
                                       (rose-val (call-gen vg size))])))]
        (rose-fmap (fn [pv] (into {} pv))
                   (vec-rose-tree pair-roses))))))

(defn tuple
  "Tuple (vector) of one value per supplied generator, in order. The
  tuple itself doesn't shrink in length; element shrinks propagate
  per-position."
  [& gens]
  (gen-record
    (fn [size]
      (let [roses  (mapv (fn [g] (call-gen g size)) gens)
            values (mapv rose-val roses)]
        ;; Build a rose tree where each child shrinks ONE position
        ;; using that position's element-rose children.
        [values
         (lazy-seq
           (mapcat
             (fn [i]
               (let [r (nth roses i)]
                 (clojure.core/map
                   (fn [child]
                     [(assoc values i (rose-val child)) ()])
                   (rose-children r))))
             (range (count roses))))]))))

(def string
  (gen-record
    (fn [size]
      (let [n (rand-int-in 0 size)
            s (apply str (for [_ (range n)] (rose-val (call-gen char size))))]
        (str-rose s)))))

(def string-ascii
  (gen-record
    (fn [size]
      (let [n (rand-int-in 0 size)
            s (apply str (for [_ (range n)] (rose-val (call-gen char-ascii size))))]
        (str-rose s)))))

(def string-alphanumeric
  (gen-record
    (fn [size]
      (let [n (rand-int-in 0 size)
            s (apply str (for [_ (range n)] (rose-val (call-gen char-alphanumeric size))))]
        (str-rose s)))))

(def keyword
  (gen-record
    (fn [size]
      (let [n (max 1 (rand-int-in 1 (max 1 size)))
            s (apply str (for [_ (range n)] (rose-val (call-gen char-alpha size))))]
        ;; Keywords don't shrink (you rarely want a shorter keyword name).
        (rose-pure (clojure.core/keyword s))))))

(def symbol
  (gen-record
    (fn [size]
      (let [n (max 1 (rand-int-in 1 (max 1 size)))
            s (apply str (for [_ (range n)] (rose-val (call-gen char-alpha size))))]
        (rose-pure (clojure.core/symbol s))))))

(def any
  ;; A simple "any" that mixes scalar generators. Recursive containers
  ;; are deferred to keep the MVP small.
  (one-of [int boolean string keyword]))

;; --- sampling ---------------------------------------------------------------

(defn sample
  "Generate n samples from g (default 10) at growing size."
  ([g] (sample g 10))
  ([g n]
   (vec (for [size (range n)] (rose-val (call-gen g size))))))

(defn generate
  "Generate one value from g (default size 30)."
  ([g] (generate g 30))
  ([g size] (rose-val (call-gen g size))))
