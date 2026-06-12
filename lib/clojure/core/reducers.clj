;; clojure.core.reducers — reducers and sequential folding.
;;
;; Each transformer here builds a transducer when called with a single
;; collection argument, in the canonical shape. The transformer
;; wrappers (map / filter / mapcat / etc.) produce a "reducer" — a
;; value that can be handed to `reduce` and whose elements are the
;; result of applying the named transducer to the source collection.
;; mino represents these as eductions over the source coll; eductions
;; reduce through the standard `reduce` path and fold through the seq
;; strategies registered on CollFold at the bottom of this file.
;;
;; The `reducer` / `folder` constructors wrap a collection with a
;; user-supplied reducing-fn transformer instead: Reducer is reducible
;; only, Folder is reducible and foldable. Both are field-slot record
;; types so each wrapper carries its own coll and xf.

(ns clojure.core.reducers
  "A library for reduction and folding. The fold contract holds in full:
  (combinef) seeds the reduction, reducef accumulates, and folding a map
  feeds reducef the key and value separately as (reducef ret k v). fold
  reduces the source in one left-to-right pass, so code written against
  associative combinef and reducef produces the same result whether or
  not the reduction is partitioned."
  (:refer-clojure :exclude [reduce map mapcat filter remove take take-while
                            drop drop-while flatten cat]))

(defn reduce
  "Like clojure.core/reduce, with two reducer-layer differences: the
  no-init arity seeds the reduction from (f) rather than the first
  element, and maps reduce through reduce-kv so f receives the
  accumulator, key, and value."
  ([f coll] (reduce f (f) coll))
  ([f init coll]
   (if (map? coll)
     (reduce-kv f init coll)
     (clojure.core/reduce f init coll))))

(defprotocol CollFold
  "Per-type folding strategy. fold dispatches through coll-fold with
  the group-size hint n, the combining fn, and the reducing fn. Every
  mino strategy reduces sequentially (see the ns docstring), so n is
  accepted and unused."
  (coll-fold [coll n combinef reducef]))

(defn fold
  "Reduces coll through a seed-and-combine contract: reducef
  accumulates over the elements starting from (combinef), and combinef
  (default reducef) must be associative with (combinef) producing its
  identity element. n is the canonical group-size hint (default 512).
  mino executes the whole fold as one sequential pass, which yields
  the same result a grouped execution of associative fns would."
  ([reducef coll] (fold reducef reducef coll))
  ([combinef reducef coll] (fold 512 combinef reducef coll))
  ([n combinef reducef coll] (coll-fold coll n combinef reducef)))

(defrecord Reducer [coll xf]
  CollReduce
  (coll-reduce [r f1 init]
    (clojure.core/reduce (xf f1) init coll)))

(defn reducer
  "Wraps a reducible coll so any reducing fn supplied to reduce is
  first transformed by xf — a fn from reducing fn to reducing fn. The
  wrapper is reducible only; folding it falls back to the sequential
  reduce strategy."
  [coll xf]
  (->Reducer coll xf))

(defrecord Folder [coll xf]
  CollReduce
  (coll-reduce [fld f1 init]
    (clojure.core/reduce (xf f1) init coll))
  CollFold
  (coll-fold [fld n combinef reducef]
    (coll-fold coll n combinef (xf reducef))))

(defn folder
  "Wraps a foldable coll so any reducing fn supplied to reduce or fold
  is first transformed by xf — a fn from reducing fn to reducing fn.
  Unlike reducer, the wrapper participates in fold: coll-fold hands
  (xf reducef) down to the wrapped collection's own strategy."
  [coll xf]
  (->Folder coll xf))

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

(defn monoid
  "Helper for fold: return f if init is provided, else a fn that calls
  ctor for an init value."
  [op ctor]
  (fn
    ([]    (ctor))
    ([a b] (op a b))))

;; Catenation values. The canonical Cat is a deferred binary node;
;; mino realizes the catenation into a vector tagged with the Cat type
;; (via :type metadata), so count, seq, and reduce operate on the
;; native value while fold dispatches through CollFold. Construct
;; through `cat`, not ->Cat.

(def Cat ::Cat)

(defn ->Cat
  "Positional constructor for a catenation value: a count and the left
  and right halves. mino realizes the halves eagerly, so the resulting
  count always comes from the data itself; cnt is accepted for
  signature parity with the canonical constructor."
  [cnt left right]
  (with-meta (into (vec left) right) {:type Cat}))

(defn cat
  "Combining fn yielding the catenation of its two reduced arguments.
  With no arguments, returns the empty accumulator (a vector). With
  one argument ctor, returns a combining fn whose zero arity calls
  ctor instead. With two, a side whose count is zero answers the other
  side unchanged; otherwise the halves become a Cat value — counted,
  seqable, reducible, and foldable. See also foldcat."
  ([] [])
  ([ctor]
   (fn
     ([] (ctor))
     ([left right] (cat left right))))
  ([left right]
   (cond
     (zero? (count left))  right
     (zero? (count right)) left
     :else (->Cat (+ (count left) (count right)) left right))))

(defn append!
  "Accumulates x onto acc and returns the updated accumulator. Named
  with a bang for contract parity: the canonical accumulator mutates
  in place, while mino's is a persistent vector and a fresh value
  comes back — fold threads the return value either way."
  [acc x]
  (conj acc x))

(defn foldcat
  "Pours coll into a counted, seqable, reducible accumulation of its
  elements: (fold cat append! coll)."
  [coll]
  (fold cat append! coll))

;; Fold strategies. One sequential shape serves every type: seed with
;; (combinef), accumulate with reducef. Maps get their own clause so
;; reducef sees each key and value separately ((reducef ret k v) via
;; reduce-kv) instead of whole map entries; nil folds straight to the
;; combinef identity. The :default clause covers everything else —
;; including Reducer wrappers, whose CollReduce impl applies their xf
;; inside the reduce.

(extend-protocol CollFold
  nil
  (coll-fold [_coll _n combinef _reducef]
    (combinef))

  :default
  (coll-fold [coll _n combinef reducef]
    (reduce reducef (combinef) coll))

  :vector
  (coll-fold [v _n combinef reducef]
    (reduce reducef (combinef) v))

  :list
  (coll-fold [s _n combinef reducef]
    (reduce reducef (combinef) s))

  :lazy-seq
  (coll-fold [s _n combinef reducef]
    (reduce reducef (combinef) s))

  :map
  (coll-fold [m _n combinef reducef]
    (reduce-kv reducef (combinef) m))

  :sorted-map
  (coll-fold [m _n combinef reducef]
    (reduce-kv reducef (combinef) m))

  Cat
  (coll-fold [c _n combinef reducef]
    (reduce reducef (combinef) c)))
