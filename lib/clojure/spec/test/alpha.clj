;; clojure.spec.test.alpha -- the instrument / check tooling that sits on
;; top of clojure.spec.alpha.
;;
;; instrument wraps an fdef'd var so every call validates its arguments
;; against the registered :args spec; unstrument restores the original.
;; check drives an fdef'd fn with generated arguments (drawn from the
;; :args spec through the spec generator path) and reports, per symbol,
;; whether the returned value conformed to :ret (and, when present, the
;; :fn relation between args and ret) across many samples.
;;
;; This namespace reaches into clojure.spec.alpha only through its public
;; surface (get-spec, registry, conform, valid?, explain-data, gen) so the
;; coupling stays a clean library boundary. Argument generation runs
;; through the bundled clojure.test.check/quick-check property runner,
;; which carries the rose-tree shrinking used to report a minimal
;; counterexample on failure.

(ns clojure.spec.test.alpha
  (:require [clojure.spec.alpha :as s]
            [clojure.test.check :as tc]
            [clojure.test.check.generators :as gen]
            [clojure.test.check.properties :as prop]))

;; ---------------------------------------------------------------------------
;; Instrument flag.  Instrumented wrappers consult this dynamic var so a
;; body wrapped in with-instrument-disabled skips argument checking.
;; ---------------------------------------------------------------------------

(def ^:dynamic *instrument-enabled*
  "When false, instrumented fns call through without validating their
  arguments.  Bound to false inside with-instrument-disabled."
  true)

(defmacro with-instrument-disabled
  "Evaluate body with instrumented-fn argument checking suspended.
  Returns the value of the last body form."
  [& body]
  `(binding [*instrument-enabled* false]
     ~@body))

;; ---------------------------------------------------------------------------
;; Registry helpers.  An fdef'd symbol carries an fspec in the spec
;; registry; the fspec exposes its :args / :ret / :fn sub-specs under the
;; plain keys.
;; ---------------------------------------------------------------------------

(defn- fdef-syms
  "Return a seq of the symbols in the spec registry that name an fdef'd
  fn (a registered fspec keyed by symbol)."
  []
  (for [[k v] (s/registry)
        :when (and (symbol? k)
                   (map? v)
                   (= :clojure.spec.alpha/fspec (:clojure.spec.alpha/kind v)))]
    k))

(defn- args-spec
  "Return the :args sub-spec of the fspec registered for sym, or nil."
  [sym]
  (when-let [fs (s/get-spec sym)]
    (:args fs)))

(defn instrumentable-syms
  "Return the set of symbols that name an fdef'd fn carrying an :args
  spec, and so can be instrumented.  An opts map is accepted for
  signature compatibility; it does not change the result."
  ([] (instrumentable-syms nil))
  ([_opts]
   (set (filter args-spec (fdef-syms)))))

(defn checkable-syms
  "Return the set of symbols that name an fdef'd fn carrying an :args
  spec, and so can be checked.  An opts map is accepted for signature
  compatibility; it does not change the result."
  ([] (checkable-syms nil))
  ([_opts]
   (set (filter args-spec (fdef-syms)))))

(defn enumerate-namespace
  "Given a namespace symbol, or a set of them, return the set of fdef'd
  symbols whose namespace is among them."
  [ns-sym]
  (let [wanted (if (set? ns-sym)
                 (set (map name ns-sym))
                 #{(name ns-sym)})]
    (set (filter (fn [sym] (contains? wanted (namespace sym)))
                 (fdef-syms)))))

;; ---------------------------------------------------------------------------
;; instrument / unstrument.  Moved here from clojure.spec.alpha: the
;; reference clojure.spec.alpha never carried instrument, so keeping the
;; machinery in this namespace makes spec.alpha's surface canonical.
;; ---------------------------------------------------------------------------

(def ^:private instrumented-vars (atom {}))

(defn- check-call-args
  "Throw when args do not conform to the :args spec registered for sym.
  Does nothing when no fdef or no :args spec is registered, or when
  instrumentation is disabled in the current dynamic scope."
  [sym args]
  (when *instrument-enabled*
    (when-let [aspec (args-spec sym)]
      (when-not (s/valid? aspec args)
        (throw (ex-info (str "Call to " sym " did not conform to spec.")
                        {:sym sym
                         :args args
                         :clojure.spec.alpha/failure :instrument
                         :problems (:clojure.spec.alpha/problems
                                    (s/explain-data aspec args))}))))))

(defn instrument
  "Wrap the var named by sym so its arguments are validated against the
  registered fdef :args spec on every call.  Returns sym if the var was
  instrumented, nil when no fdef is registered for sym."
  [sym]
  (let [v (resolve sym)]
    (when (and v (s/get-spec sym))
      (when-not (contains? @instrumented-vars sym)
        (let [orig (deref v)]
          (swap! instrumented-vars assoc sym orig)
          (alter-var-root v
                          (fn [_]
                            (fn [& args]
                              (check-call-args sym args)
                              (apply orig args))))))
      sym)))

(defn unstrument
  "Restore an instrumented var named by sym to its original value.
  Returns sym if it was instrumented, nil otherwise."
  [sym]
  (when-let [orig (get @instrumented-vars sym)]
    (let [v (resolve sym)]
      (when v
        (alter-var-root v (fn [_] orig))
        (swap! instrumented-vars dissoc sym)
        sym))))

;; ---------------------------------------------------------------------------
;; check.  For each checkable symbol, build an argument generator from
;; the fspec's :args spec and drive the fn through quick-check, validating
;; :ret (and :fn) for every generated argument list.
;; ---------------------------------------------------------------------------

(def ^:private default-num-tests 100)

(defn- check-one
  "Run a generative check of the fn named by sym.  Returns a result map
  with :sym and :clojure.spec.test.check/ret (the quick-check result).
  Returns nil when sym has no :args spec to generate from."
  [sym num-tests]
  (when-let [aspec (args-spec sym)]
    (let [fs       (s/get-spec sym)
          ret-spec (:ret fs)
          fn-spec  (:fn fs)
          f        (resolve sym)
          arg-gen  (s/gen aspec)
          ;; quick-check applies the predicate to a generated arg
          ;; vector; the property's generator yields a one-element
          ;; vector holding the generated argument list, so the
          ;; predicate sees exactly that list.
          one-arg  (gen/fmap vector arg-gen)
          pred     (fn [args]
                     (let [ret (apply f args)]
                       (and (or (nil? ret-spec) (s/valid? ret-spec ret))
                            (or (nil? fn-spec)
                                (s/valid? fn-spec {:args args :ret ret})))))
          property (prop/make-property one-arg pred)
          qc       (tc/quick-check num-tests property)]
      {:sym sym
       :clojure.spec.test.check/ret qc})))

(defn- syms->check
  "Resolve a check target into a seq of symbols to check.  nil means
  every checkable symbol; a single symbol means itself; a collection
  means each of its members."
  [sym-or-syms]
  (cond
    (nil? sym-or-syms) (seq (checkable-syms))
    (symbol? sym-or-syms) (list sym-or-syms)
    :else (seq sym-or-syms)))

(defn check
  "Run generative checks.  With no argument, check every checkable
  symbol.  With a symbol, check that one; with a collection of symbols,
  check each.  opts may carry :clojure.spec.test.check/opts {:num-tests
  n}.  Returns a seq of result maps, one per checked symbol, each with
  :sym and :clojure.spec.test.check/ret (the quick-check result, whose
  :result is true / false / a thrown value and whose :shrunk holds a
  minimal counterexample on failure)."
  ([] (check nil nil))
  ([sym-or-syms] (check sym-or-syms nil))
  ([sym-or-syms opts]
   (let [num-tests (or (get-in opts [:clojure.spec.test.check/opts :num-tests])
                       default-num-tests)]
     (->> (syms->check sym-or-syms)
          (keep (fn [sym] (check-one sym num-tests)))
          (doall)))))

;; ---------------------------------------------------------------------------
;; Result summarization.
;; ---------------------------------------------------------------------------

(defn- failure-type
  "Classify a check result.  Returns :check-passed when the quick-check
  run passed, :check-failed when it found a counterexample, or :no-gen
  when the result carried no quick-check run."
  [result]
  (let [qc (:clojure.spec.test.check/ret result)]
    (cond
      (nil? qc) :no-gen
      (true? (:result qc)) :check-passed
      :else :check-failed)))

(defn abbrev-result
  "Condense a check result for display: keep :sym, replace the full
  quick-check result with a compact :failure (the shrunk counterexample
  and num-tests on failure) and drop the heavy spec objects.  A passing
  result is returned with just its :sym and a :result of true."
  [result]
  (let [qc (:clojure.spec.test.check/ret result)]
    (if (true? (:result qc))
      {:sym (:sym result) :result true}
      {:sym (:sym result)
       :failure (select-keys qc [:result :shrunk :num-tests
                                 :failing-args :seed])})))

(defn summarize-results
  "Aggregate a seq of check results, printing one abbreviated form per
  result, and return a summary map with :total and the count under each
  outcome key (:check-passed / :check-failed / :no-gen).  An opts map is
  accepted for signature compatibility."
  ([check-results] (summarize-results check-results nil))
  ([check-results _opts]
   (reduce
     (fn [summary result]
       (let [kind (failure-type result)]
         (println (pr-str (abbrev-result result)))
         (-> summary
             (update :total inc)
             (update kind (fnil inc 0)))))
     {:total 0}
     check-results)))
