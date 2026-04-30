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

(defn- thrown?-form? [sym]
  (and (symbol? sym) (= "thrown?" (name sym))))

(defmacro is-thrown [expr msg]
  `(try
     (do ~@(rest expr)
         (assert-fail! (pr-str '~expr) ~msg "expected exception but none thrown"))
     (catch __e (assert-pass!))))

(defmacro is-eq [expr msg]
  (let [gs-exp (gensym) gs-act (gensym)
        a (first (rest expr))
        b (first (rest (rest expr)))]
    `(let [~gs-exp ~a
           ~gs-act ~b]
       (if (= ~gs-exp ~gs-act)
         (assert-pass!)
         (assert-fail! (pr-str '~expr) ~msg
           (str "expected: " (pr-str ~gs-exp) "\n    actual: " (pr-str ~gs-act)))))))

(defmacro is-truthy [expr msg]
  (let [gs (gensym)]
    `(let [~gs ~expr]
       (if ~gs
         (assert-pass!)
         (assert-fail! (pr-str '~expr) ~msg
           (str "expected truthy, got: " (pr-str ~gs)))))))

;; --- Public API ---

(defmacro is [& args]
  (let [expr (first args)
        msg  (second args)]
    (cond
      (and (cons? expr) (thrown?-form? (first expr)))
      `(is-thrown ~expr ~msg)
      (and (cons? expr) (= (first expr) '=))
      `(is-eq ~expr ~msg)
      :else
      `(is-truthy ~expr ~msg))))

(defmacro deftest [tname & body]
  `(swap! tests-registry conj
     {:name (name '~tname)
      :ns   (str *ns*)
      :fn   (fn [] ~@body)}))

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

(defn- run-tests-impl [tests]
  (binding [*report-counters* (atom {:pass 0 :fail 0 :error 0 :failures []})]
    (let [n (count tests)]
      (loop [i 0]
        (when (< i n)
          (let [t     (nth tests i)
                tname (get t :name)
                tfn   (get t :fn)]
            (binding [*current-test*     tname
                      *testing-contexts* ()]
              (try
                (tfn)
                (catch e
                  (do
                    (swap! *report-counters* update :error inc)
                    (swap! *report-counters* update :failures conj
                      {:test tname :error (str e)}))))))
          (recur (+ i 1))))
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
