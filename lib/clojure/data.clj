(ns clojure.data
  (:require [clojure.set :as set]))

(declare diff)

(defn- atom-diff [a b]
  (if (= a b) [nil nil a] [a b nil]))

(defn- sparse->vec
  "Builds a vector from an index->value map, nil-filling gaps up to
   the highest index. Returns nil for an empty map so an absent diff
   slot stays nil."
  [m]
  (when (seq m)
    (let [n (inc (apply max (keys m)))]
      (mapv (fn [i] (get m i)) (range n)))))

(defn- diff-at-key
  "Diffs a and b at the single key k, returning a triple of one-entry
   maps [only-a only-b both] with nil for absent slots. The pair is
   shared at k when the sub-diff found common structure, or when both
   sides hold nil at k."
  [a b k]
  (let [va (get a k)
        vb (get b k)
        [oa ob ab] (diff va vb)
        in-a (contains? a k)
        in-b (contains? b k)
        shared (and in-a in-b
                    (or (some? ab)
                        (and (nil? va) (nil? vb))))]
    [(when (and in-a (or (some? oa) (not shared))) {k oa})
     (when (and in-b (or (some? ob) (not shared))) {k ob})
     (when shared {k ab})]))

(defn- diff-associative
  "Diffs associative things a and b over the keys in ks, merging the
   per-key triples into [only-a only-b both] maps (nil when empty)."
  [a b ks]
  (reduce
    (fn [acc k] (mapv merge acc (diff-at-key a b k)))
    [nil nil nil]
    ks))

(defn- diff-sequential [a b]
  (let [va (vec a)
        vb (vec b)
        [oa ob ab] (diff-associative va vb
                                     (range (max (count va) (count vb))))]
    [(sparse->vec oa) (sparse->vec ob) (sparse->vec ab)]))

(defn- diff-set [a b]
  [(not-empty (set/difference a b))
   (not-empty (set/difference b a))
   (not-empty (set/intersection a b))])

(defprotocol EqualityPartition
  "Internal dispatch hook for diff; not a stable interface."
  (equality-partition [x]
    "Returns the partition x diffs within: :atom, :set,
     :sequential, or :map."))

(defprotocol Diff
  "Internal dispatch hook for diff; not a stable interface."
  (diff-similar [a b]
    "Diffs a and b, which share an equality partition."))

;; Built-in types grouped by equality partition; everything else
;; (including strings) compares as an atom via the :default entries.
(def ^:private partition-types
  {:sequential [:vector :list :lazy-seq :map-entry]
   :set        [:set :sorted-set]
   :map        [:map :sorted-map]})

(def ^:private diff-similar-fns
  {:sequential diff-sequential
   :set        diff-set
   :map        (fn [a b]
                 (diff-associative a b
                                   (distinct (concat (keys a) (keys b)))))})

(doseq [[part types] partition-types
        t types]
  (extend t
    EqualityPartition {:equality-partition (fn [_] part)}
    Diff              {:diff-similar (get diff-similar-fns part)}))

(extend :default
  EqualityPartition {:equality-partition (fn [_] :atom)}
  Diff              {:diff-similar atom-diff})

(defn diff
  "Recursively compares a and b, returning the triple
  [only-in-a only-in-b in-both]. Equal values yield [nil nil a].
  Maps recurse into values under shared keys; sequential things
  diff position by position and report vectors; sets split into
  difference/difference/intersection without recursing. Anything
  else, strings included, compares whole as an atom."
  [a b]
  (if (= a b)
    [nil nil a]
    (if (= (equality-partition a) (equality-partition b))
      (diff-similar a b)
      (atom-diff a b))))
