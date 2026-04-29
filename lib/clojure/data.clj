(ns clojure.data
  (:require [clojure.set :as set]))

(declare diff)

(defn- diff-map [a b]
  (reduce
    (fn [[only-a only-b both] k]
      (let [has-a (contains? a k)
            has-b (contains? b k)]
        (cond
          (and has-a (not has-b))
          [(assoc only-a k (get a k)) only-b both]

          (and (not has-a) has-b)
          [only-a (assoc only-b k (get b k)) both]

          :else
          (let [[da db db2] (diff (get a k) (get b k))]
            [(if (some? da) (assoc only-a k da) only-a)
             (if (some? db) (assoc only-b k db) only-b)
             (if (some? db2) (assoc both k db2) both)]))))
    [nil nil nil]
    (distinct (concat (keys a) (keys b)))))

(defn- diff-sequential [a b]
  (let [va (vec a)
        vb (vec b)
        na (count va)
        nb (count vb)
        mx (max na nb)
        [ra rb rc]
        (reduce
          (fn [[only-a only-b both] i]
            (let [has-a (< i na)
                  has-b (< i nb)]
              (cond
                (and has-a (not has-b))
                [(conj only-a (nth va i)) (conj only-b nil) (conj both nil)]

                (and (not has-a) has-b)
                [(conj only-a nil) (conj only-b (nth vb i)) (conj both nil)]

                :else
                (let [[da db db2] (diff (nth va i) (nth vb i))]
                  [(conj only-a da) (conj only-b db) (conj both db2)]))))
          [[] [] []]
          (range mx))]
    [(when (some some? ra) ra)
     (when (some some? rb) rb)
     (when (some some? rc) rc)]))

(defn- diff-set [a b]
  (let [only-a (set/difference a b)
        only-b (set/difference b a)
        both   (set/intersection a b)]
    [(when (seq only-a) only-a)
     (when (seq only-b) only-b)
     (when (seq both) both)]))

(defn diff [a b]
  (cond
    (= a b) [nil nil a]
    (and (map? a) (map? b)) (diff-map a b)
    (and (set? a) (set? b)) (diff-set a b)
    (and (sequential? a) (sequential? b)) (diff-sequential a b)
    :else [a b nil]))
