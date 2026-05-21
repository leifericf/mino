(ns clojure.test.check.properties)

(require '[clojure.test.check.generators :as gen])

;; clojure.test.check.properties port for mino. A property is a map
;;   {:test.check/property true :gen-args args-gen :pred pred}
;; where args-gen is a generator producing a vector of args (with rose
;; tree shrinking) and pred is called via (apply pred args). The
;; predicate's result is wrapped as {:result <truthy?>}.

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

(defn pred-passes?
  "Apply the property's predicate to args; return true on truthy
   result, false on falsy or thrown."
  [pred args]
  (try (boolean (apply pred args))
       (catch __e false)))

(defn run-property
  "Generate one args rose tree at the given size and run the
   predicate against the root. Returns
   {:result true} or {:result false :args args :rose rose}.
  The rose tree is included on failure so quick-check can shrink."
  [prop size]
  (let [rose (gen/call-gen (:gen-args prop) size)
        args (gen/rose-val rose)
        pred (:pred prop)]
    (if (pred-passes? pred args)
      {:result true :args args}
      {:result false :args args :rose rose})))
