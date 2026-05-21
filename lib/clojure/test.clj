(ns clojure.test
  (:require [clojure.string :refer [join]]))

;; --- State ---

;; Registry of every (deftest)-registered test in this image, populated
;; at registration time. A non-dynamic atom on purpose: the registry is
;; shared across files and across run-tests invocations.
(def tests-registry (atom []))

;; Per-run pass/fail counters. run-tests binds this to a fresh atom so
;; concurrent or nested runs do not stomp each other.
(def ^:dynamic *report-counters* nil)

;; Stack of (testing ...) descriptions, outer-most last (so cons is push).
;; Pure dynamic var — no atom — pushed by the testing macro via binding.
(def ^:dynamic *testing-contexts* ())

;; Name of the test currently executing; bound per test by run-tests.
(def ^:dynamic *current-test* nil)

;; Suite drivers (tests/run.clj and friends) flip suite-mode to true
;; before requiring test files and back to false before their final
;; run-tests-and-exit. Inside the require chain, each test file's
;; bottom call to (run-tests-and-exit) is a no-op so the driver
;; controls the single end-to-end run.
;;
;; Plain atom rather than a dynamic var: mino's require evaluates a
;; loaded file outside the caller's dynamic scope, so a binding would
;; not reach the loaded file's top-level forms.
(def suite-mode (atom false))

;; Registered fixtures keyed by namespace name (string). Each entry is
;; a map {:once [fn ...] :each [fn ...]}. use-fixtures appends; the
;; runner composes them around test execution.
(def fixtures-registry (atom {}))

;; --- Internal helpers ---

(defn assert-pass! []
  (swap! *report-counters* update :pass inc))

(defn assert-fail! [form msg detail]
  (let [ctx   (reverse *testing-contexts*)
        tname *current-test*
        entry {:test tname
               :form form
               :message msg
               :detail detail
               :context ctx}]
    (swap! *report-counters* update :fail inc)
    (swap! *report-counters* update :failures conj entry)))

(defn assert-error! [form msg thrown]
  (let [ctx   (reverse *testing-contexts*)
        tname *current-test*
        entry {:test tname
               :form form
               :message msg
               :error (str thrown)
               :context ctx}]
    (swap! *report-counters* update :error inc)
    (swap! *report-counters* update :failures conj entry)))

(defn- thrown?-form? [sym]
  (and (symbol? sym) (= "thrown?" (name sym))))

(defn- thrown-with-msg?-form? [sym]
  (and (symbol? sym) (= "thrown-with-msg?" (name sym))))

(defmacro is-thrown [expr msg]
  ;; expr matches either of two shapes:
  ;;   (thrown? <type> <body>...)  -- JVM-clojure compatible
  ;;   (thrown? <body>)            -- mino single-arg shorthand
  ;; mino has no exception class hierarchy; the type symbol is
  ;; documentation-only, so any throw from the body counts as a
  ;; pass. We detect the JVM shape by arity: 2+ elements after
  ;; `thrown?` means the first is the type (skip it); 1 element
  ;; is the body itself.
  (let [args        (rest expr)
        body-forms  (if (= 1 (count args)) args (rest args))]
    `(try
       (do ~@body-forms
           (assert-fail! (pr-str '~expr) ~msg "expected exception but none thrown"))
       (catch __e (assert-pass!)))))

(defn exception-message-for-match [e]
  ;; Best-effort: pull a string out of the thrown value for regex match.
  ;; Order matches the way mino values surface from `catch`:
  ;;   raw string  -> use it directly
  ;;   raw map     -> :mino/message (mino diagnostic) or :message
  ;;   ex-info     -> ex-message walks the same path
  ;;   anything    -> pr-str fallback so the regex still has *something*
  (cond
    (string? e)            e
    (and (map? e)
         (contains? e :mino/message)) (:mino/message e)
    (and (map? e)
         (contains? e :message))      (:message e)
    :else                  (or (try (ex-message e) (catch _ nil))
                               (pr-str e))))

(defmacro is-thrown-with-msg [expr msg]
  ;; expr matches either of two shapes:
  ;;   (thrown-with-msg? <type> <re> <body>...)  -- JVM-clojure compatible
  ;;   (thrown-with-msg? <re> <body>...)         -- mino shorthand
  ;; Type symbol (if present) is documentation-only; mino has no class
  ;; hierarchy. The regex is matched against the thrown value's message
  ;; via exception-message-for-match.
  (let [args (rest expr)
        ;; Detect JVM shape: first arg is a Class-like type *symbol* and
        ;; the second is the regex; mino's shorthand puts the regex first.
        ;; A regex literal carries the regex type; a type symbol won't.
        first-arg  (first args)
        regex-form (if (and (symbol? first-arg)
                            (not (re-find #"^#" (pr-str first-arg))))
                     (second args)
                     first-arg)
        body-forms (if (and (symbol? first-arg)
                            (not (re-find #"^#" (pr-str first-arg))))
                     (rest (rest args))
                     (rest args))]
    `(try
       (do ~@body-forms
           (assert-fail! (pr-str '~expr) ~msg
                         "expected exception but none thrown"))
       (catch __e
         (let [m# (exception-message-for-match __e)]
           (if (and m# (re-find ~regex-form m#))
             (assert-pass!)
             (assert-fail! (pr-str '~expr) ~msg
                           (str "thrown message did not match " (pr-str ~regex-form)
                                "; got: " (pr-str m#)))))))))

(defmacro is-eq [expr msg]
  (let [gs-exp (gensym) gs-act (gensym)
        a (first (rest expr))
        b (first (rest (rest expr)))]
    `(try
       (let [~gs-exp ~a
             ~gs-act ~b]
         (if (= ~gs-exp ~gs-act)
           (assert-pass!)
           (assert-fail! (pr-str '~expr) ~msg
             (str "expected: " (pr-str ~gs-exp) "\n    actual: " (pr-str ~gs-act)))))
       (catch __e (assert-error! (pr-str '~expr) ~msg __e)))))

(defmacro is-truthy [expr msg]
  (let [gs (gensym)]
    `(try
       (let [~gs ~expr]
         (if ~gs
           (assert-pass!)
           (assert-fail! (pr-str '~expr) ~msg
             (str "expected truthy, got: " (pr-str ~gs)))))
       (catch __e (assert-error! (pr-str '~expr) ~msg __e)))))

;; --- Public API ---

(defmacro is [& args]
  (let [expr (first args)
        msg  (second args)]
    (cond
      (and (cons? expr) (thrown?-form? (first expr)))
      `(is-thrown ~expr ~msg)
      (and (cons? expr) (thrown-with-msg?-form? (first expr)))
      `(is-thrown-with-msg ~expr ~msg)
      (and (cons? expr) (= (first expr) '=))
      `(is-eq ~expr ~msg)
      :else
      `(is-truthy ~expr ~msg))))

(defmacro deftest [tname & body]
  `(swap! tests-registry conj
     {:name (name '~tname)
      :ns   (str *ns*)
      :fn   (fn [] ~@body)}))

(defmacro use-fixtures
  "Register fixture functions for the current namespace. Each fixture
  is a fn-of-one-arg whose argument is a no-arg thunk that runs the
  wrapped body; the fixture is responsible for any setup/teardown
  around invoking the thunk.

  kind is :once (run around the entire batch of tests in this ns) or
  :each (run around each individual test). Multiple fixtures of the
  same kind compose left-to-right (the first fixture is outermost).

  Implemented as a macro so the calling namespace is captured at
  expansion time -- mino's `*ns*` does not propagate dynamically into
  function bodies, so a function-based use-fixtures would always
  register under `clojure.test` instead of the caller's ns.

  Mirrors clojure.test/use-fixtures."
  [kind & fixtures]
  (let [ns-name (str *ns*)]
    `(do
       (when-not (or (= ~kind :once) (= ~kind :each))
         (throw (ex-info "use-fixtures: kind must be :once or :each"
                         {:kind ~kind})))
       (swap! fixtures-registry update ~ns-name
              (fn [m#] (update (or m# {}) ~kind
                               (fn [existing#]
                                 (into (or existing# []) [~@fixtures]))))))))

(defmacro testing [desc & body]
  `(binding [*testing-contexts* (cons ~desc *testing-contexts*)]
     ~@body))

(defmacro are [bindings expr & args]
  (let [n     (count bindings)
        rows  (partition n args)
        tests (apply list
                (map (fn [row]
                       (let [smap (zipmap bindings row)]
                         `(is ~(postwalk-replace smap expr))))
                     rows))]
    `(do ~@tests)))

;; --- Runner ---

(defn- print-failures [failures]
  (when (seq failures)
    (println "")
    (println "Failures:")
    (loop [fs failures]
      (when (seq fs)
        (let [f (first fs)]
          (when (get f :test)
            (println (str "  in " (get f :test))))
          (when (seq (get f :context))
            (println (str "    " (join " > " (get f :context)))))
          (when (get f :form)
            (println (str "    " (get f :form))))
          (when (get f :message)
            (println (str "    " (get f :message))))
          (when (get f :detail)
            (println (str "    " (get f :detail))))
          (when (get f :error)
            (println (str "    ERROR: " (get f :error))))
          (println ""))
        (recur (rest fs))))))

(defn- compose-fixtures
  "Compose a sequence of fixture fns (each taking a thunk that runs
  the wrapped body) into a single fn-of-thunk. The result calls the
  outermost fixture first; its body invokes the next inner fixture,
  and so on, with the supplied thunk at the centre."
  [fixtures]
  (reduce (fn [inner fixture]
            (fn [thunk] (fixture (fn [] (inner thunk)))))
          (fn [thunk] (thunk))
          (reverse fixtures)))

(defn- run-tests-impl [tests]
  (binding [*report-counters* (atom {:pass 0 :fail 0 :error 0 :failures []})]
    (let [n (count tests)
          ;; Group tests by ns so :once fixtures wrap the per-ns run
          ;; once. Within a ns, :each fixtures wrap each test body.
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
                          each-wrap (compose-fixtures (get fxs :each))]
                      (when trace?
                        (binding [*out* *err*]
                          (println (str "[trace] " tns "/" tname))
                          (flush)))
                      (binding [*current-test*     tname
                                *testing-contexts* ()]
                        (try
                          (each-wrap tfn)
                          (catch e
                            (swap! *report-counters* update :error inc)
                            (swap! *report-counters* update :failures conj
                              {:test tname :error (str e)}))))))]
      (doseq [ns-name ns-order]
        (let [ns-tests  (get by-ns ns-name)
              fxs       (get @fixtures-registry ns-name)
              once-wrap (compose-fixtures (get fxs :once))]
          (once-wrap (fn [] (doseq [t ns-tests] (run-one t))))))
      (let [state    @*report-counters*
            passes   (get state :pass)
            fails    (get state :fail)
            errors   (get state :error)
            total    (+ passes fails errors)
            failures (get state :failures)]
        (print-failures failures)
        (println (str n " tests, " total " assertions: "
                      passes " passed, " fails " failed, " errors " errors"))
        {:test n :pass passes :fail fails :error errors :failures failures}))))

(defn run-tests
  "Run registered tests and return a summary map
  {:test n :pass n :fail n :error n :failures [...]}.

  With no arguments, runs every registered test. Given namespace symbols
  (or namespace objects), runs only tests registered in those namespaces.

  Prints a summary line and any failure details to stdout."
  ([] (run-tests-impl @tests-registry))
  ([& nss]
   (let [allowed (set (map str nss))]
     (run-tests-impl (filter (fn [t] (allowed (get t :ns))) @tests-registry)))))

(defn run-tests-and-exit
  "CLI wrapper around run-tests: runs the tests and exits the process
  with code 0 when there are no failures or errors, 1 otherwise. Used
  by tests/run.clj and other CLI driver files. A no-op while
  suite-mode is true so that per-file (run-tests-and-exit) calls do
  not short-circuit a multi-file driver."
  [& nss]
  (when-not @suite-mode
    (let [s (apply run-tests nss)]
      (if (and (= 0 (get s :fail)) (= 0 (get s :error)))
        (exit 0)
        (exit 1)))))
