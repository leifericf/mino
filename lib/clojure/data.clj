(ns clojure.data
  (:require [clojure.set :as set]))

(declare diff)

(defn- diff-map [a b]
  (let [all-keys (distinct (concat (keys a) (keys b)))
        only-a   (atom {})
        only-b   (atom {})
        both     (atom {})]
    (doseq [k all-keys]
      (let [has-a (contains? a k)
            has-b (contains? b k)]
        (cond
          (and has-a (not has-b))
          (swap! only-a assoc k (get a k))

          (and (not has-a) has-b)
          (swap! only-b assoc k (get b k))

          :else
          (let [[da db db2] (diff (get a k) (get b k))]
            (when (some? da) (swap! only-a assoc k da))
            (when (some? db) (swap! only-b assoc k db))
            (when (some? db2) (swap! both assoc k db2))))))
    [(when (seq @only-a) @only-a)
     (when (seq @only-b) @only-b)
     (when (seq @both) @both)]))

(defn- diff-sequential [a b]
  (let [va (vec a)
        vb (vec b)
        na (count va)
        nb (count vb)
        mx (max na nb)
        only-a (atom [])
        only-b (atom [])
        both   (atom [])]
    (loop [i 0]
      (when (< i mx)
        (let [has-a (< i na)
              has-b (< i nb)]
          (cond
            (and has-a (not has-b))
            (do (swap! only-a conj (nth va i))
                (swap! only-b conj nil)
                (swap! both conj nil))

            (and (not has-a) has-b)
            (do (swap! only-a conj nil)
                (swap! only-b conj (nth vb i))
                (swap! both conj nil))

            :else
            (let [[da db db2] (diff (nth va i) (nth vb i))]
              (swap! only-a conj da)
              (swap! only-b conj db)
              (swap! both conj db2))))
        (recur (+ i 1))))
    (let [ra @only-a rb @only-b rc @both]
      [(when (some some? ra) ra)
       (when (some some? rb) rb)
       (when (some some? rc) rc)])))

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
