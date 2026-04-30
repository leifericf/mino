(ns clojure.test.check.properties)

(require '[clojure.test.check.generators :as gen])

;; Minimal clojure.test.check.properties port for mino. A property
;; is a map {:test.check/property true :gen-vec args-gen :pred pred}
;; where args-gen produces a vector of generated args and pred is
;; called with those args (apply pred args).

(defn property?
  [x]
  (and (map? x) (true? (:test.check/property x))))

(defn make-property
  "Build a property from an args-vec generator and a predicate fn."
  [gen-args pred]
  {:test.check/property true :gen-args gen-args :pred pred})

(defmacro for-all
  "Like clojure.test.check.properties/for-all. Binding form is a
   vector of [name gen ...]; body is evaluated with names bound to
   one generated value each. Returns truthy on success."
  [bindings & body]
  (let [pairs (partition 2 bindings)
        names (mapv first pairs)
        gens  (mapv second pairs)]
    `(make-property
       (apply gen/tuple ~gens)
       (fn ~names ~@body))))

(defn run-property
  "Generate one args vector and run the predicate. Returns
   {:result true} on pass, {:result false :args args} on fail."
  [prop size]
  (let [args (gen/call-gen (:gen-args prop) size)
        pred (:pred prop)
        ok   (try (boolean (apply pred args))
                  (catch __e false))]
    (if ok
      {:result true :args args}
      {:result false :args args})))
