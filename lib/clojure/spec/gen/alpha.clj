(ns clojure.spec.gen.alpha
  (:refer-clojure :exclude [boolean char double int keyword list map
                            set string symbol vector hash-map])
  (:require [clojure.test.check.generators :as tcgen]))

;; clojure.spec.gen.alpha -- the generator wrapper that spec uses to
;; build sample data. On the reference platform this namespace lazily
;; loads test.check so that spec stays usable without it; mino bundles
;; test.check directly, so this port requires
;; clojure.test.check.generators eagerly and delegates straight to it.
;; Every public name below is a thin pass-through to the corresponding
;; generator combinator, with the same calling shape callers expect.
;;
;; A few combinators on the reference surface (frequency, choose,
;; large-integer*, double*) have no bundled equivalent in mino's
;; test.check, so they are built here from the available primitives.
;; Names with no faithful equivalent are omitted rather than stubbed;
;; each omission is noted in a comment.

;; --- generation and sampling ------------------------------------------------

(def generate
  "Generate one value from the generator g (default size 30)."
  tcgen/generate)

(def sample
  "Generate n values from the generator g (default 10) at growing size."
  tcgen/sample)

;; --- combinators ------------------------------------------------------------

(defn fmap
  "Generator that applies f to each value produced by g."
  [f g]
  (tcgen/fmap f g))

(defn such-that
  "Generator that keeps only values from g where (pred v) is truthy,
  retrying up to max-tries times (default 10)."
  ([pred g] (tcgen/such-that pred g))
  ([pred g max-tries] (tcgen/such-that pred g max-tries)))

(defn one-of
  "Generator that picks one of the supplied generators uniformly, then
  draws a value from it. gens is a collection of generators."
  [gens]
  (tcgen/one-of gens))

(defn tuple
  "Generator that yields a vector with one value per supplied
  generator, in order."
  [& gens]
  (apply tcgen/tuple gens))

(defn vector
  "Generator that yields vectors of values from g. With no bound the
  length grows with size; with one bound the length is fixed; with two
  bounds the length falls in [min, max]."
  ([g] (tcgen/vector g))
  ([g num-elements] (tcgen/vector g num-elements))
  ([g min-elements max-elements] (tcgen/vector g min-elements max-elements)))

(defn list
  "Generator that yields lists of values from g."
  [g]
  (tcgen/list g))

(defn set
  "Generator that yields sets of values from g."
  [g]
  (tcgen/set g))

(defn map
  "Generator that yields maps with keys from kgen and values from vgen."
  [kgen vgen]
  (tcgen/map kgen vgen))

(defn hash-map
  "Generator that yields maps with fixed keys and a generator per key.
  Arguments alternate key, generator, key, generator, and so on."
  [& kvs]
  (let [pairs (partition 2 kvs)
        ks    (mapv first pairs)
        gens  (mapv second pairs)]
    (fmap (fn [vals] (zipmap ks vals))
          (apply tcgen/tuple gens))))

(defn elements
  "Generator that picks uniformly from a non-empty collection."
  [coll]
  (tcgen/elements coll))

(defn bind
  "Generator that draws a value from gen, calls k on it to obtain a
  second generator, and draws from that one."
  [gen k]
  (tcgen/bind gen k))

(defn return
  "Generator that always yields v."
  [v]
  (tcgen/return v))

;; --- numeric generators -----------------------------------------------------

(defn choose
  "Generator that yields integers uniformly in the inclusive range
  [lower, upper], shrinking toward the in-range value closest to zero."
  [lower upper]
  (tcgen/choose lower upper))

(defn large-integer*
  "Generator for integers, optionally bounded by :min and :max in the
  options map. Unbounded sides default to a wide range. Shrinks toward
  zero (or the nearest in-range bound)."
  [{:keys [min max]}]
  (let [lo (or min -1000000)
        hi (or max 1000000)]
    (tcgen/choose lo hi)))

(def large-integer
  "Generator for integers across a wide range."
  (large-integer* {}))

;; double*: built over an integer generator scaled into the requested
;; range, so the result carries a shrink tree (toward the low bound) the
;; way a plain (rand)-based value would not. The :infinite? and :NaN?
;; options are accepted for surface compatibility but the produced range
;; is always finite in this port.
(def ^:private double-resolution 1000000)

(defn double*
  "Generator for doubles, optionally bounded by :min and :max in the
  options map. Shrinks toward the low bound."
  [{:keys [min max]}]
  (let [lo   (clojure.core/double (or min -1000000.0))
        hi   (clojure.core/double (or max 1000000.0))
        span (- hi lo)]
    (fmap (fn [n] (+ lo (* span (/ (clojure.core/double n) double-resolution))))
          (tcgen/choose 0 double-resolution))))

(defn frequency
  "Generator that picks among the supplied generators weighted by their
  likelihoods. pairs is a collection of [weight generator] entries; a
  generator is chosen with probability proportional to its weight."
  [pairs]
  (let [pairs (vec pairs)
        total (reduce + (clojure.core/map first pairs))]
    (when (or (empty? pairs) (not (pos? total)))
      (throw (ex-info "frequency: needs at least one positive weight"
                      {:pairs pairs})))
    (bind (choose 0 (dec total))
          (fn [n]
            (loop [acc 0
                   [[w g] & more] pairs]
              (let [acc' (+ acc w)]
                (if (< n acc')
                  g
                  (recur acc' more))))))))

;; --- scalar generators ------------------------------------------------------

(def boolean
  "Generator for booleans."
  tcgen/boolean)

(def char
  "Generator for single-character strings drawn from the printable set."
  tcgen/char)

(def char-alpha
  "Generator for single alphabetic characters."
  tcgen/char-alpha)

(def char-alphanumeric
  "Generator for single alphanumeric characters."
  tcgen/char-alphanumeric)

(def char-ascii
  "Generator for single printable ASCII characters."
  tcgen/char-ascii)

(def double
  "Generator for doubles."
  tcgen/double)

(def int
  "Generator for integers."
  tcgen/int)

(def nat
  "Generator for non-negative integers."
  tcgen/nat)

(def keyword
  "Generator for unqualified keywords."
  tcgen/keyword)

;; keyword-ns: a qualified keyword built from two alphabetic names.
(def keyword-ns
  "Generator for namespace-qualified keywords."
  (fmap (fn [[ns nm]] (clojure.core/keyword (name ns) (name nm)))
        (tcgen/tuple tcgen/keyword tcgen/keyword)))

(def string
  "Generator for strings."
  tcgen/string)

(def string-alphanumeric
  "Generator for alphanumeric strings."
  tcgen/string-alphanumeric)

(def string-ascii
  "Generator for printable-ASCII strings."
  tcgen/string-ascii)

(def symbol
  "Generator for unqualified symbols."
  tcgen/symbol)

;; symbol-ns: a qualified symbol built from two alphabetic names.
(def symbol-ns
  "Generator for namespace-qualified symbols."
  (fmap (fn [[ns nm]] (clojure.core/symbol (name ns) (name nm)))
        (tcgen/tuple tcgen/symbol tcgen/symbol)))

(def uuid
  "Generator for random UUIDs. A UUID has no meaningful smaller form, so
  the values do not shrink (matching the reference generator)."
  (fmap (fn [_] (random-uuid)) (return nil)))

;; --- mixed-type generators --------------------------------------------------

(def simple-type
  "Generator for a value of one of the basic scalar types."
  (one-of [int boolean string keyword symbol double]))

(def simple-type-printable
  "Generator for a value of one of the basic, printable scalar types."
  (one-of [int boolean string keyword symbol]))

(def any
  "Generator for an arbitrary value drawn from the basic scalar types."
  (one-of [int boolean string keyword symbol double]))

(def any-printable
  "Generator for an arbitrary printable value from the basic scalar
  types."
  (one-of [int boolean string keyword symbol]))

;; Intentionally omitted from this port (no faithful bundled equivalent
;; and not reached by spec's core generator paths in mino):
;;   vector-distinct, delay-impl, gen-for-name, gen-for-pred, lazy-prim,
;;   for-all*, and the lazy-combinator / lazy-combinators / lazy-prims
;;   macros. These exist on the reference platform to support the JVM
;;   dynamic-load shim, which this port does not need because test.check
;;   is bundled.
