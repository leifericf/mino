;; Portability helpers for the external test suite.
;; Matches the :default branches in clojure.core-test.portability.

(ns clojure.core-test.portability)

(defmacro when-var-exists [var-sym & body]
  (if (resolve var-sym)
    `(do ~@body)
    `(println "SKIP -" '~var-sym)))

(defn big-int? [n] (bigint? n))

(defn sleep [ms] nil)
