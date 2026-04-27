(ns clojure.template
  "Macros that expand to repeated copies of a template expression.

   Mirrors clojure/template.clj from the Clojure repository
   (Stuart Sierra, 2008). Used historically as the substitution
   primitive behind clojure.test/are; mino's are is self-contained
   (lib/clojure/test.clj uses postwalk-replace directly), so this
   namespace exists for user code that references it directly."
  (:require [clojure.walk :refer [postwalk-replace]]))

(defn apply-template
  "Replaces argv symbols in expr with the corresponding values.

   argv is a binding vector of symbols (no destructuring). expr is
   the template form. values is a sequence whose elements are
   substituted positionally.

   (apply-template '[x] '(+ x x) '[2])    ; => (+ 2 2)
   (apply-template '[a b] '(- a b) '[5 3]) ; => (- 5 3)"
  [argv expr values]
  (when-not (vector? argv)
    (throw (ex-info "apply-template: argv must be a vector"
                    {:argv argv})))
  (when-not (every? symbol? argv)
    (throw (ex-info "apply-template: argv must contain only symbols"
                    {:argv argv})))
  (postwalk-replace (zipmap argv values) expr))

(defmacro do-template
  "Repeatedly copies expr (wrapped in a do) for each group of values.

   values are partitioned by (count argv); each group binds argv
   in expr and the resulting form joins the do.

   (do-template [x y] (+ y x) 2 4 3 5)
   ; => (do (+ 4 2) (+ 5 3))"
  [argv expr & values]
  (let [n     (count argv)
        rows  (partition n values)
        forms (map (fn [row]
                     (clojure.template/apply-template argv expr row))
                   rows)]
    (apply list 'do forms)))
