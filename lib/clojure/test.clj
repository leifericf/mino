(ns clojure.test
  (:require [clojure.string :refer [join]]))

;; --- User-modifiable globals ---

(def ^:dynamic *load-tests* true)

(def ^:dynamic *stack-trace-depth* nil)

;; --- Reporting globals ---

;; Template for a fresh per-run counter map.
(def ^:dynamic *initial-report-counters*
  {:test 0 :pass 0 :fail 0 :error 0})

;; Per-run pass/fail counters. Bound to a fresh atom by run-tests and
;; test-ns so concurrent or nested runs do not stomp each other.
;; test-var and inc-report-counter update this atom.
(def ^:dynamic *report-counters* nil)

;; Stack of (testing ...) descriptions, outer-most last (cons is push).
(def ^:dynamic *testing-contexts* ())

;; Name of the test var currently executing; bound per test by test-var.
(def ^:dynamic *testing-vars* (list))

;; Output sink for test reporting.  Use with-test-out to route report
;; output through it.  Initialized to :mino/stdout so the default
;; behaviour is identical to printing to *out* without redirecting.
(def ^:dynamic *test-out* :mino/stdout)

;; Suite drivers (tests/run.clj and friends) flip suite-mode to true
;; before requiring test files and back to false before their final
;; run-tests-and-exit.  Each test file's bottom call is a no-op while
;; this is true so the driver controls the single end-to-end run.
;;
;; Plain atom rather than a dynamic var: mino's require evaluates a
;; loaded file outside the caller's dynamic scope, so a binding would
;; not reach the loaded file's top-level forms.
(def suite-mode (atom false))

;; Registered fixtures keyed by namespace name (string).  Each entry
;; is a map {:once [fn ...] :each [fn ...]}.  use-fixtures appends; the
;; runner composes them around test execution.
(def fixtures-registry (atom {}))

;; Registry of every (deftest)-registered test in this image.  Used by
;; the run-tests-and-exit / run-tests flow (tests/run.clj).
(def tests-registry (atom []))

;; Name of the test currently executing (legacy; also set by test-var).
(def ^:dynamic *current-test* nil)

;; --- with-test-out ---

;; Routes body output through *test-out*.
;;
;; Implementation note: mino's *out* at the Clojure level always
;; evaluates to :mino/stdout (the root keyword) regardless of what
;; with-out-str captures at the C level.  When *test-out* is
;; :mino/stdout we skip the rebind so the C-level capture from an
;; enclosing with-out-str remains intact.  When *test-out* is a
;; different value (e.g. an atom or :mino/stderr) we rebind *out* to
;; route through it.  Uses list construction (not syntax-quote) so the
;; unqualified *out* symbol is stored in the binding; the qualified form
;; that syntax-quote would produce is not found by mino's C-level symbol
;; lookup for dynamic bindings.
(defmacro with-test-out [& body]
  (list 'if '(= *test-out* :mino/stdout)
        (cons 'do body)
        (list 'binding ['*out* '*test-out*] (cons 'do body))))

;; --- inc-report-counter ---

(defn inc-report-counter
  "Increments the named counter in *report-counters* (an atom).
  Does nothing when *report-counters* is nil."
  [name]
  (when *report-counters*
    (swap! *report-counters* update name (fnil inc 0))))

;; --- Utilities ---

(defn testing-vars-str
  "Returns a string representation of the current test.  Renders names
  in *testing-vars* as a list, then the source file and line of the
  current assertion."
  [m]
  (let [{:keys [file line]} m]
    (str (reverse (map (fn [v] (:name (meta v))) *testing-vars*))
         " (" file ":" line ")")))

(defn testing-contexts-str
  "Returns a string representation of the current test context.  Joins
  strings in *testing-contexts* with spaces."
  []
  (join " " (reverse *testing-contexts*)))

(defn get-possibly-unbound-var
  "Like var-get but returns nil when the var is unbound."
  [v]
  (try (var-get v)
       (catch _ nil)))

(defn function?
  "Returns true if argument is a function or a symbol that resolves to
  a function (not a macro)."
  [x]
  (if (symbol? x)
    (when-let [v (resolve x)]
      (when-let [value (get-possibly-unbound-var v)]
        (and (fn? value)
             (not (:macro (meta v))))))
    (fn? x)))

(defn file-position
  "Returns a vector [filename line-number] for the nth call up the stack.
  Deprecated in Clojure 1.2; present for compatibility."
  [n]
  [nil nil])

;; --- report dispatch ---

;; report receives a result map with at least a :type key and dispatches
;; on that key.  Extend it method-by-method with (defmethod report :type
;; ...) to add a new event handler alongside the built-in set, or rebind
;; the whole var with binding to swap in a different dispatcher.  The var
;; is both a multimethod (so defmethod extension takes effect) and
;; ^:dynamic (so binding-based replacement still works).  Built-in
;; methods handle :pass, :fail, :error and :summary; every other :type
;; falls through the :default method and is silently ignored.

(def ^:dynamic report
  (create-multimethod :type :default))

(defmethod report :pass [m]
  (with-test-out (inc-report-counter :pass)))

(defmethod report :fail [m]
  (with-test-out
    (inc-report-counter :fail)
    ;; Also push into :failures for the run-tests-impl flow.
    (when *report-counters*
      (swap! *report-counters* update :failures
             (fnil conj []) m))
    (println "\nFAIL in" (testing-vars-str m))
    (when (seq *testing-contexts*) (println (testing-contexts-str)))
    (when-let [msg (:message m)] (println msg))
    (println "expected:" (pr-str (:expected m)))
    (println "  actual:" (pr-str (:actual m)))))

(defmethod report :error [m]
  (with-test-out
    (inc-report-counter :error)
    (when *report-counters*
      (swap! *report-counters* update :failures
             (fnil conj []) m))
    (println "\nERROR in" (testing-vars-str m))
    (when (seq *testing-contexts*) (println (testing-contexts-str)))
    (when-let [msg (:message m)] (println msg))
    (println "expected:" (pr-str (:expected m)))
    (println "  actual:" (pr-str (:actual m)))))

(defmethod report :summary [m]
  (with-test-out
    (println "\nRan" (:test m) "tests containing"
             (+ (or (:pass m) 0) (or (:fail m) 0) (or (:error m) 0)) "assertions.")
    (println (:fail m) "failures," (:error m) "errors.")))

(defmethod report :default [m])  ; silently ignore begin-test-ns, end-test-ns, etc.

;; --- do-report ---

(defn do-report
  "Add file and line information to a test result map and call report.
  When writing a custom assert-expr method, call this to pass results
  to report."
  [m]
  (report m))

;; --- assert-any and assert-predicate ---

(defn assert-predicate
  "Returns generic assertion code for any functional predicate.
  The :expected key will contain the original form; :actual will
  contain the evaluated form.  On failure :actual is wrapped in
  (not ...)."
  [msg form]
  (let [args (rest form)
        pred (first form)]
    `(let [values# (list ~@args)
           result# (apply ~pred values#)]
       (if result#
         (do-report {:type :pass :message ~msg
                     :expected '~form :actual (cons '~pred values#)})
         (do-report {:type :fail :message ~msg
                     :expected '~form :actual (list '~'not (cons '~pred values#))}))
       result#)))

(defn assert-any
  "Returns generic assertion code for any test, including macros and
  isolated symbols."
  [msg form]
  `(let [value# ~form]
     (if value#
       (do-report {:type :pass :message ~msg
                   :expected '~form :actual value#})
       (do-report {:type :fail :message ~msg
                   :expected '~form :actual value#}))
     value#))

;; --- Internal report helpers shared by the built-in assert-expr methods ---

(defn assert-pass! []
  (do-report {:type :pass}))

(defn assert-fail! [form msg detail]
  (do-report {:type :fail
              :message msg
              :expected form
              :actual detail}))

(defn assert-error! [form msg thrown]
  (do-report {:type :error
              :message msg
              :expected form
              :actual thrown}))

(defn exception-message-for-match [e]
  ;; Best-effort: pull a string out of the thrown value for regex match.
  (cond
    (string? e)            e
    (and (map? e)
         (contains? e :mino/message)) (:mino/message e)
    (and (map? e)
         (contains? e :message))      (:message e)
    :else                  (or (try (ex-message e) (catch _ nil))
                               (pr-str e))))

;; --- assert-expr multimethod ---

;; assert-expr is the single per-form expansion point behind is: for
;; every test form, (is form msg) expands to (try-expr msg form), which
;; calls (assert-expr msg form).  assert-expr dispatches on the first
;; element of the test form and returns a quoted form that, when
;; evaluated, calls do-report with :pass, :fail or :error.  Extend it
;; with (defmethod assert-expr 'my-op ...) and the new shape fires for
;; every is that uses it -- no editing of is itself.

(defmulti assert-expr
  (fn [msg form]
    (cond
      (nil? form)  :always-fail
      (seq? form)  (first form)
      :else        :default)))

(defmethod assert-expr :always-fail [msg form]
  `(do-report {:type :fail :message ~msg}))

(defmethod assert-expr :default [msg form]
  (if (and (sequential? form) (function? (first form)))
    (assert-predicate msg form)
    (assert-any msg form)))

;; (is (= a b)) binds both arguments, compares, and reports the evaluated
;; values on failure.  = is an ordinary function, so :default would route
;; it through assert-predicate; the explicit method keeps the canonical
;; per-operator extension point visible and overridable.
(defmethod assert-expr '= [msg form]
  (let [gs-exp (gensym) gs-act (gensym) gs-res (gensym)
        a (first (rest form))
        b (first (rest (rest form)))]
    `(let [~gs-exp ~a
           ~gs-act ~b
           ~gs-res (= ~gs-exp ~gs-act)]
       (if ~gs-res
         (assert-pass!)
         (assert-fail! '~form ~msg
                       (str "expected: " (pr-str ~gs-exp)
                            "\n    actual: " (pr-str ~gs-act))))
       ~gs-res)))

;; (is (thrown? type body...)) passes when body throws.  The form also
;; accepts the single-argument shorthand (thrown? body): mino has no
;; exception class hierarchy, so the type symbol is documentation-only
;; and any throw counts as a pass.
(defmethod assert-expr 'thrown? [msg form]
  (let [args       (rest form)
        body-forms (if (= 1 (count args)) args (rest args))]
    `(try
       (do ~@body-forms
           (assert-fail! '~form ~msg "expected exception but none thrown"))
       (catch __e (assert-pass!)))))

;; (is (thrown-with-msg? regex body...)) passes when body throws and the
;; thrown value's message matches regex.  Accepts an optional leading
;; type symbol before the regex for canon compatibility.
(defmethod assert-expr 'thrown-with-msg? [msg form]
  (let [args      (rest form)
        first-arg (first args)
        typed?    (and (symbol? first-arg)
                       (not (re-find #"^#" (pr-str first-arg))))
        regex-form (if typed? (second args) first-arg)
        body-forms (if typed? (rest (rest args)) (rest args))]
    `(try
       (do ~@body-forms
           (assert-fail! '~form ~msg "expected exception but none thrown"))
       (catch __e
         (let [m# (exception-message-for-match __e)]
           (if (and m# (re-find ~regex-form m#))
             (assert-pass!)
             (assert-fail! '~form ~msg
                           (str "thrown message did not match " (pr-str ~regex-form)
                                "; got: " (pr-str m#)))))))))

;; --- try-expr ---

(defmacro try-expr
  "Used by the is macro to catch unexpected exceptions."
  [msg form]
  `(try ~(assert-expr msg form)
        (catch __e
          (assert-error! '~form ~msg __e))))

;; --- Public API macros ---

;; Every form routes through try-expr/assert-expr, so user extensions to
;; assert-expr take effect for all is forms.
(defmacro is [& args]
  (let [expr (first args)
        msg  (second args)]
    `(try-expr ~msg ~expr)))

;; Uses list construction (not syntax-quote) so *testing-contexts* stays
;; as a bare unqualified symbol in the binding form.  Syntax-quote would
;; qualify it to clojure.test/*testing-contexts*, but mino's dyn_lookup
;; searches by bare name and would miss the qualified binding.
(defmacro testing [desc & body]
  (list 'binding ['*testing-contexts* (list 'cons desc '*testing-contexts*)]
        (cons 'do body)))

(defmacro are [bindings expr & args]
  (let [n     (count bindings)
        rows  (partition n args)
        tests (apply list
                (map (fn [row]
                       (let [smap (zipmap bindings row)]
                         `(is ~(postwalk-replace smap expr))))
                     rows))]
    `(do ~@tests)))

;; --- Definition forms ---

;; deftest creates a public var whose value is a fn that calls test-var
;; on itself, and whose :test metadata holds the test body fn.
;; It also registers the test in tests-registry for run-tests-and-exit.
(defmacro deftest [tname & body]
  (when *load-tests*
    `(do
       (def ~tname (fn [] (test-var (var ~tname))))
       (alter-meta! (var ~tname) assoc :test (fn [] ~@body))
       (swap! tests-registry conj
              {:name (name '~tname)
               :ns   (str *ns*)
               :fn   (:test (meta (var ~tname)))}))))

;; deftest- is like deftest but marks the var private.
(defmacro deftest- [tname & body]
  (when *load-tests*
    `(do
       (def ~tname (fn [] (test-var (var ~tname))))
       (alter-meta! (var ~tname) assoc
                    :test (fn [] ~@body)
                    :private true)
       (swap! tests-registry conj
              {:name (name '~tname)
               :ns   (str *ns*)
               :fn   (:test (meta (var ~tname)))}))))

;; with-test evaluates the definition form and attaches a :test fn.
;; When *load-tests* is false, only the definition is evaluated.
(defmacro with-test [definition & body]
  (if *load-tests*
    `(doto ~definition (alter-meta! assoc :test (fn [] ~@body)))
    definition))

;; set-test replaces the :test metadata on an already-defined var.
;; When *load-tests* is false, this is a no-op.
(defmacro set-test [name & body]
  (when *load-tests*
    `(alter-meta! (var ~name) assoc :test (fn [] ~@body))))

;; --- use-fixtures ---

(defn use-fixtures
  "Register fixture functions for the current namespace.

  kind is :once (run around the entire batch of tests in this ns) or
  :each (run around each individual test).  Multiple fixtures of the
  same kind compose left-to-right (the first fixture is outermost).

  Reads *ns* at call time so the fixture is registered under the
  namespace that is current when use-fixtures is called (JVM canon
  parity)."
  [kind & fixtures]
  (when-not (or (= kind :once) (= kind :each))
    (throw (ex-info "use-fixtures: kind must be :once or :each"
                    {:kind kind})))
  ;; Read *ns* through the var binding stack so dynamic rebinding of *ns*
  ;; is visible even when the function body falls back to the tree-walker
  ;; interpreter (which happens when macro calls force bc-compile to decline).
  ;; (var clojure.core/*ns*) is resolved at read time; var-get consults the
  ;; thread-binding stack, matching JVM Clojure behavior.
  (let [ns-name (str (var-get (var clojure.core/*ns*)))]
    (swap! fixtures-registry update ns-name
           (fn [m]
             (update (or m {}) kind
                     (fn [existing]
                       (into (or existing []) fixtures)))))))

;; --- Fixture composition ---

(defn compose-fixtures
  "Composes two fixture functions into a single fixture function that
  calls f1 as the outer fixture and f2 as the inner fixture."
  [f1 f2]
  (fn [g] (f1 (fn [] (f2 g)))))

(defn- default-fixture [f] (f))

(defn join-fixtures
  "Composes a collection of fixtures in order, returning a single
  fixture function.  Always returns a valid fixture function, even for
  an empty collection."
  [fixtures]
  (reduce compose-fixtures default-fixture fixtures))

;; --- Low-level runners ---

(defn ^:dynamic test-var
  "If v has a function in its :test metadata, calls that function with
  *testing-vars* bound to (conj *testing-vars* v).  Increments the
  :test counter in *report-counters*.  Dynamic so that binding can
  replace the implementation for custom runners (JVM canon parity)."
  [v]
  (when-let [t (:test (meta v))]
    (binding [*testing-vars*     (conj *testing-vars* v)
              *current-test*     (:name (meta v))
              *testing-contexts* ()]
      (do-report {:type :begin-test-var :var v})
      (inc-report-counter :test)
      (try
        (t)
        (catch e
          (do-report {:type :error
                      :message "Uncaught exception, not in assertion."
                      :expected nil
                      :actual   e})))
      (do-report {:type :end-test-var :var v}))))

(defn test-vars
  "Runs test-var on each var in vars that has :test metadata."
  [vars]
  (doseq [v vars]
    (when (:test (meta v))
      (test-var v))))

(defn test-all-vars
  "Calls test-vars on every var interned in the namespace."
  [ns]
  (test-vars (vals (ns-interns ns))))

(defn test-ns
  "Runs tests in the given namespace (symbol or ns object).  Binds
  *report-counters* to a fresh atom, calls test-all-vars, and returns
  the final counter map."
  [ns]
  (binding [*report-counters* (atom *initial-report-counters*)]
    (let [ns-sym (if (symbol? ns) ns (ns-name ns))]
      (do-report {:type :begin-test-ns :ns ns-sym})
      (test-all-vars ns-sym)
      (do-report {:type :end-test-ns :ns ns-sym}))
    @*report-counters*))

;; --- High-level runners ---

(defn successful?
  "Returns true when the given test summary has no failures or errors."
  [summary]
  (and (zero? (or (:fail summary) 0))
       (zero? (or (:error summary) 0))))

(defn run-test-var
  "Runs the tests for a single var with a fresh counter, and returns a
  summary map with :type :summary."
  [v]
  (binding [*report-counters* (atom *initial-report-counters*)]
    (test-var v)
    (let [summary (assoc @*report-counters* :type :summary)]
      (do-report summary)
      summary)))

(defmacro run-test
  "Runs a single test identified by its symbol in the current namespace.
  Returns a summary map."
  [test-symbol]
  (let [tv (resolve test-symbol)]
    (cond
      (nil? tv)
      `(binding [*out* *err*]
         (println "Unable to resolve" '~test-symbol "to a test function."))

      (not (:test (meta tv)))
      `(binding [*out* *err*]
         (println '~test-symbol "is not a test."))

      :else
      `(run-test-var ~tv))))

(defn run-all-tests
  "Runs all tests in every namespace whose name matches the optional
  regex re.  With no argument, runs all tests in all namespaces.
  Returns a summary map."
  ([]
   (let [ns-list (vec (all-ns))
         summaries (mapv (fn [ns]
                           (binding [*report-counters* (atom *initial-report-counters*)]
                             (test-all-vars (ns-name ns))
                             @*report-counters*))
                         ns-list)
         merged (reduce (fn [acc m]
                          (-> acc
                              (update :test + (or (:test m) 0))
                              (update :pass + (or (:pass m) 0))
                              (update :fail + (or (:fail m) 0))
                              (update :error + (or (:error m) 0))))
                        {:test 0 :pass 0 :fail 0 :error 0}
                        summaries)
         summary (assoc merged :type :summary)]
     (do-report summary)
     summary))
  ([re]
   (let [matching (filter (fn [ns]
                            (re-matches re (str (ns-name ns))))
                          (all-ns))
         summaries (mapv (fn [ns]
                           (binding [*report-counters* (atom *initial-report-counters*)]
                             (test-all-vars (ns-name ns))
                             @*report-counters*))
                         matching)
         merged (reduce (fn [acc m]
                          (-> acc
                              (update :test + (or (:test m) 0))
                              (update :pass + (or (:pass m) 0))
                              (update :fail + (or (:fail m) 0))
                              (update :error + (or (:error m) 0))))
                        {:test 0 :pass 0 :fail 0 :error 0}
                        summaries)
         summary (assoc merged :type :summary)]
     (do-report summary)
     summary)))

;; --- Legacy runner (tests/run.clj path) ---

(defn- print-failures [failures]
  (when (seq failures)
    (println "")
    (println "Failures:")
    (loop [fs failures]
      (when (seq fs)
        (let [f (first fs)]
          ;; Support both legacy format (:form/:detail/:error) and
          ;; canonical format (:expected/:actual).
          (when (get f :test)
            (println (str "  in " (get f :test))))
          (when (seq (get f :context))
            (println (str "    " (join " > " (get f :context)))))
          (when (get f :form)
            (println (str "    " (get f :form))))
          (when (get f :expected)
            (println (str "    expected: " (pr-str (get f :expected)))))
          (when (get f :message)
            (println (str "    " (get f :message))))
          (when (get f :detail)
            (println (str "    " (get f :detail))))
          (when (get f :actual)
            (println (str "    actual: " (pr-str (get f :actual)))))
          (when (get f :error)
            (println (str "    ERROR: " (get f :error))))
          (println ""))
        (recur (rest fs))))))

(defn- run-tests-impl [tests]
  (binding [*report-counters* (atom (assoc *initial-report-counters* :failures []))]
    (let [n (count tests)
          by-ns (reduce (fn [acc t]
                          (update acc (get t :ns) (fnil conj []) t))
                        {} tests)
          ns-list (mapv (fn [t] (get t :ns)) tests)
          ns-order (vec (distinct ns-list))
          trace? (some? (getenv "MINO_TEST_TRACE"))
          run-one (fn [t]
                    (let [tname (get t :name)
                          tfn   (get t :fn)
                          tns   (get t :ns)
                          fxs   (get @fixtures-registry tns)
                          each-wrap (join-fixtures (get fxs :each))]
                      (when trace?
                        (binding [*out* *err*]
                          (println (str "[trace] " tns "/" tname))
                          (flush)))
                      (binding [*current-test*     tname
                                *testing-contexts* ()]
                        (try
                          (each-wrap tfn)
                          (catch e
                            (inc-report-counter :error)
                            (when *report-counters*
                              (swap! *report-counters* update :failures
                                     (fnil conj [])
                                     {:test tname :error (str e)})))))))]
      (doseq [ns-name ns-order]
        (let [ns-tests  (get by-ns ns-name)
              fxs       (get @fixtures-registry ns-name)
              once-wrap (join-fixtures (get fxs :once))]
          (once-wrap (fn [] (doseq [t ns-tests] (run-one t))))))
      (let [state    @*report-counters*
            passes   (or (get state :pass) 0)
            fails    (or (get state :fail) 0)
            errors   (or (get state :error) 0)
            total    (+ passes fails errors)
            failures (get state :failures)]
        (print-failures failures)
        (println (str n " tests, " total " assertions: "
                      passes " passed, " fails " failed, " errors " errors"))
        {:test n :pass passes :fail fails :error errors :failures failures}))))

(defn run-namespace-tests
  "Runs the registered tests for the given namespace names (strings) and
  returns a summary map.  An empty collection runs nothing."
  [ns-names]
  (let [allowed (set ns-names)]
    (run-tests-impl (filter (fn [t] (allowed (get t :ns))) @tests-registry))))

(defn run-all-registered-tests
  "Runs every test in the registry, regardless of namespace, and returns
  a summary map.  Used by the suite driver (tests/run.clj) and by
  run-tests-and-exit."
  []
  (run-tests-impl @tests-registry))

(defn run-tests
  "Run registered tests and return a summary map.

  With no arguments, runs the tests registered in the namespace that is
  current when run-tests is called (reads *ns* at call time, per JVM
  canon).  Given namespace symbols or objects, runs tests registered in
  those namespaces.

  Prints a summary line and any failure details to stdout."
  ([] (run-namespace-tests [(str *ns*)]))
  ([& namespaces] (run-namespace-tests (map str namespaces))))

(defn run-tests-and-exit
  "Runs the whole registry and exits the process with code 0 on no
  failures or errors, 1 otherwise.  A no-op while suite-mode is true.
  Used as the bottom-of-file entry point in test files and by the suite
  driver; it always runs every registered test, independent of the
  namespace-scoped run-tests."
  [& _nss]
  (when-not @suite-mode
    (let [s (run-all-registered-tests)]
      (if (and (= 0 (get s :fail)) (= 0 (get s :error)))
        (exit 0)
        (exit 1)))))
