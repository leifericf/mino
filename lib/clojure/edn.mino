(ns clojure.edn)

(defn read-string
  ([s] (clojure.core/read-string s))
  ([opts s]
   (when (seq opts)
     (throw (str "clojure.edn/read-string: reader options not supported in mino")))
   (clojure.core/read-string s)))
