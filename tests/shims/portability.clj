;; Portability helpers for the external test suite.
;; Provides when-var-exists, big-int?, and sleep.

(require "tests/test")

(defmacro when-var-exists [var-sym & body]
  (if (resolve var-sym)
    `(do ~@body)
    `(println "SKIP -" '~var-sym)))

(defn big-int? [n] (integer? n))

(defn sleep [ms] nil)
