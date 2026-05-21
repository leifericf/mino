;; clojure.core.reducers — sequential transducer-layer wrapper.
;;
;; Each transformer here builds a transducer when called with a single
;; collection argument, in the canonical Clojure shape. Reduce / fold
;; are sequential — mino does not yet ship the JVM fork/join machinery
;; that backs parallel `r/fold`, so the body is reduced left-to-right
;; through the standard `reduce` path.
;;
;; The transducer-shaped wrappers (r/map / r/filter / r/mapcat / etc.)
;; produce a "reducer" — an object that can be passed to `reduce` and
;; whose elements are the result of applying the named transducer to
;; the source collection. mino represents reducers as anonymous fns
;; that satisfy `clojure.core.protocols/CollReduce` via the existing
;; `reduce-fn` chain — wrap the source coll in an eduction over the
;; same transducer.

(ns clojure.core.reducers
  (:refer-clojure :exclude [reduce map mapcat filter remove take take-while
                            drop drop-while flatten cat]))

(defn reduce
  "Like clojure.core/reduce except sequential by definition (mino has
  no fork/join). Identical contract: with an init, calls (f init x0),
  (f acc x1), ...; without an init, uses (f x0) for arity-0 of f."
  ([f coll]      (clojure.core/reduce f coll))
  ([f init coll] (clojure.core/reduce f init coll)))

(defn map
  "Reducer that applies f to each element."
  [f coll]
  (eduction (clojure.core/map f) coll))

(defn filter
  "Reducer that keeps elements for which pred is truthy."
  [pred coll]
  (eduction (clojure.core/filter pred) coll))

(defn remove
  "Reducer that drops elements for which pred is truthy."
  [pred coll]
  (eduction (clojure.core/remove pred) coll))

(defn mapcat
  "Reducer that maps f over coll and concatenates the results."
  [f coll]
  (eduction (clojure.core/mapcat f) coll))

(defn take
  "Reducer that yields at most n elements."
  [n coll]
  (eduction (clojure.core/take n) coll))

(defn take-while
  "Reducer that yields elements while pred is truthy."
  [pred coll]
  (eduction (clojure.core/take-while pred) coll))

(defn drop
  "Reducer that drops the first n elements."
  [n coll]
  (eduction (clojure.core/drop n) coll))

(defn drop-while
  "Reducer that drops elements while pred is truthy."
  [pred coll]
  (eduction (clojure.core/drop-while pred) coll))

(defn flatten
  "Reducer that flattens one level of nested collections."
  [coll]
  (eduction clojure.core/cat coll))

(defn cat
  "A reducer that concatenates the output of x-coll and y-coll. Calls
  reduce on x-coll first then y-coll. Returns a fn so it composes the
  standard reducer protocol."
  [x-coll y-coll]
  (eduction clojure.core/cat (list x-coll y-coll)))

(defn monoid
  "Helper for fold: return f if init is provided, else a fn that calls
  ctor for an init value."
  [op ctor]
  (fn
    ([]    (ctor))
    ([a b] (op a b))))

(defn foldcat
  "Equivalent to (fold cat append! coll), but reducer-shaped: returns a
  vector of all elements in coll."
  [coll]
  (clojure.core/reduce conj [] coll))

(defn- fold-partition-vec
  "Partition a vector into subvecs of size n (last may be smaller).
  Uses subvec so the result shares structure with the source vector."
  [v n]
  (let [c (count v)]
    (loop [start 0
           acc   []]
      (if (>= start c)
        acc
        (recur (+ start n)
               (conj acc (subvec v start (min (+ start n) c))))))))

(defn- fold-sequential
  "The sequential fallback shape used by fold when the host has no
  thread budget or coll isn't shape-amenable to parallel reduction."
  [combinef reducef coll]
  (clojure.core/reduce reducef (combinef) coll))

(defn fold
  "Reduces coll using reducef. The 3-arity form takes a partition
  size n (default 512) and a combinef. When the host has granted
  thread budget (mino-thread-limit > 1) AND coll is a vector larger
  than n, fold partitions coll into chunks of size n, runs reducef
  in parallel over each chunk via futures, and combines the partial
  results with combinef. Smaller vectors and non-vector collections
  reduce sequentially through (clojure.core/reduce reducef
  (combinef) coll).

  - (fold reducef coll) -- combinef defaults to reducef (so the
    no-arg branch must return the reducer's identity).
  - (fold combinef reducef coll) -- partition size defaults to 512.
  - (fold n combinef reducef coll) -- explicit partition size.

  Pure semantics: reducef and combinef must be associative; combinef
  with no args must return the identity element. Without those,
  parallel and sequential results may differ."
  ([reducef coll]
   (fold reducef reducef coll))
  ([combinef reducef coll]
   (fold 512 combinef reducef coll))
  ([n combinef reducef coll]
   (let [thr   (mino-thread-limit)
         c     (when (vector? coll) (count coll))]
     (if (or (<= thr 1)
             (not (vector? coll))
             (<= c n))
       (fold-sequential combinef reducef coll)
       ;; Cap chunk count at (thread-limit - 1) so we don't exceed the
       ;; host's thread budget. The user-supplied n is a MINIMUM chunk
       ;; size; if c/n would exceed the budget, grow the chunk size so
       ;; the count fits in the budget. This matches the spirit of
       ;; JVM fork/join's "n is the leaf-size hint" without needing a
       ;; recursive split.
       (let [max-chunks   (max 1 (dec thr))
             ;; Integer ceiling of (c / max-chunks) without floats.
             chunk-size   (max n (quot (+ c (dec max-chunks)) max-chunks))
             parts        (fold-partition-vec coll chunk-size)
             futures-seq  (mapv
                            (fn [chunk]
                              (future
                                (clojure.core/reduce reducef
                                  (combinef) chunk)))
                            parts)
             partial-vals (mapv clojure.core/deref futures-seq)]
         (clojure.core/reduce combinef (combinef) partial-vals))))))
