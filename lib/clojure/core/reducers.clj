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

(defn fold
  "Reduce coll using f. mino has no fork/join, so this is the
  sequential fallback: (fold f coll) and (fold n combinef reducef
  coll) both reduce coll left-to-right through reducef, with combinef
  used to seed the accumulator on a missing init.

  Parallel fork/join semantics are queued for the multi-state OS-
  thread cycle; until then this is a faithful sequential reducer."
  ([reducef coll]
   (clojure.core/reduce reducef (reducef) coll))
  ([combinef reducef coll]
   (clojure.core/reduce reducef (combinef) coll))
  ([_n combinef reducef coll]
   (clojure.core/reduce reducef (combinef) coll)))
