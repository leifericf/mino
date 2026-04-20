(ns clojure.pprint)

(defn pprint
  ([x] (println (pr-str x))))

(defn cl-format [writer fmt & args]
  (throw (str "cl-format is not implemented in mino")))

(defn print-table
  ([rows] (print-table (keys (first rows)) rows))
  ([ks rows]
    (doseq [k ks] (print (str k "\t")))
    (println)
    (doseq [row rows]
      (doseq [k ks] (print (str (get row k) "\t")))
      (println))))
