(ns clojure.edn)

;; mino's reader has no #= eval tag, so the only behavioural
;; differentiator from clojure.core/read is preserving reader
;; conditionals as data instead of evaluating them. Both arities pass
;; {:read-cond :preserve} into clojure.core/read-string.

(defn read-string
  ([s]
   (clojure.core/read-string {:read-cond :preserve} s))
  ([opts s]
   (clojure.core/read-string (merge {:read-cond :preserve} opts) s)))

(defn read
  ([s]      (read-string s))
  ([opts s] (read-string opts s)))
