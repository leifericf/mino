(require "tests/test")

;; Spec-first tests for three clojure.test canon-alignment items.
;; These tests describe the INTENDED behavior; some assertions FAIL
;; today and are expected to start passing once the implementation lands.
;;
;; Item 1: test-var must be a ^:dynamic var (today: plain defn).
;;   Failure form: (:dynamic (meta ...)) returns nil instead of true.
;;
;; Item 2: run-tests must be a function (today: macro).
;;   Failure form: (:macro (meta ...)) returns true instead of nil/false.
;;
;; Item 3: use-fixtures must be a function (today: macro).
;;   Failure form: (:macro (meta ...)) returns true instead of nil/false.
;;
;; Behavior tests (fixture wrapping, *ns* at call time) that do NOT
;; depend on the flag changes are written to PASS today and must
;; continue passing after the implementation.

;; --- Isolation helpers ---
;;
;; Rule: do not corrupt the global tests-registry or fixtures-registry.
;; Every test that touches these globals saves and restores them, or
;; uses a scratch namespace cleaned up in a finally block.

(defn- tlf-save-restore
  "Calls thunk with tests-registry and fixtures-registry snapshotted
  before the call; restores both registries after, regardless of
  exceptions.  Returns thunk's return value."
  [thunk]
  (let [saved-tests    @clojure.test/tests-registry
        saved-fixtures @clojure.test/fixtures-registry]
    (try
      (thunk)
      (finally
        (reset! clojure.test/tests-registry saved-tests)
        (reset! clojure.test/fixtures-registry saved-fixtures)))))

(defn- tlf-quietly
  "Calls thunk with test output suppressed.  Returns thunk's return value."
  [thunk]
  (let [result (atom nil)]
    (with-out-str
      (binding [*test-out* *out*]
        (reset! result (thunk))))
    @result))

(defn- tlf-fresh-ns
  "Removes any stale namespace named ns-sym, creates it fresh, returns it."
  [ns-sym]
  (when (find-ns ns-sym) (remove-ns ns-sym))
  (create-ns ns-sym)
  ns-sym)

(defn- tlf-test-var!
  "Interns vname in ns-sym with f as value and :test metadata."
  [ns-sym vname f]
  (let [v (intern ns-sym vname f)]
    (alter-meta! v assoc :test f)
    v))

;; =========================================================================
;; Item 1: test-var must be ^:dynamic
;; =========================================================================

;; FAILS today: (:dynamic (meta ...)) is nil; must become true.
(deftest tlf-test-var-is-dynamic
  (is (true? (:dynamic (meta (resolve 'clojure.test/test-var))))
      "test-var must carry ^:dynamic metadata so binding can intercept it"))

;; FAILS today (the wrapper calls the static fn, not the dynamic var).
;; After implementation: binding clojure.test/test-var should intercept
;; calls routed through the deftest-generated wrapper fn.
(deftest tlf-test-var-binding-intercepts-wrapper
  ;; The deftest-generated wrapper for a test-var calls (test-var (var self)).
  ;; When test-var is dynamic, a binding must replace that call.
  (let [intercepted (atom false)
        result      (tlf-save-restore
                     (fn []
                       (tlf-fresh-ns 'tlf.tvbi)
                       (try
                         ;; Create a test var in the scratch namespace.
                         (let [v (tlf-test-var! 'tlf.tvbi 'tvbi-sample
                                                (fn [] (is (= 1 1))))]
                           ;; Build a wrapper fn mimicking deftest's generated form:
                           ;; (fn [] (test-var (var self))). We simulate this by
                           ;; calling test-var on v directly, since that IS the
                           ;; call site we want to intercept.
                           (binding [clojure.test/test-var
                                     (fn [_v] (reset! intercepted true) :intercepted)]
                             ;; A plain (test-var v) call must use the rebound fn.
                             (clojure.test/test-var v)))
                         (finally (remove-ns 'tlf.tvbi)))))]
    (is (true? @intercepted)
        "binding clojure.test/test-var must intercept calls at the dynamic var site")))

;; PASSES today and must keep passing: default (unbound) test-var behavior.
(deftest tlf-test-var-unbound-behavior-unchanged
  ;; When test-var is not rebound, it must still count, bind *testing-vars*,
  ;; and run the :test fn.
  (let [seen (atom :unset)
        c    (tlf-save-restore
              (fn []
                (tlf-fresh-ns 'tlf.tvub)
                (try
                  (let [v (tlf-test-var! 'tlf.tvub 'tvub-probe
                                         (fn []
                                           (reset! seen
                                                   (mapv (fn [tv] (:name (meta tv)))
                                                         *testing-vars*))))]
                    (tlf-quietly
                     (fn []
                       (binding [*report-counters*
                                 (atom *initial-report-counters*)]
                         (clojure.test/test-var v)
                         @*report-counters*))))
                  (finally (remove-ns 'tlf.tvub)))))]
    (is (= ['tvub-probe] @seen)
        "test-var must bind *testing-vars* to the var being tested")
    (is (= 1 (:test c))
        "test-var must increment the :test counter")))

;; =========================================================================
;; Item 2: run-tests must be a function (not a macro)
;; =========================================================================

;; FAILS today: (:macro (meta ...)) is true; must become nil/false.
(deftest tlf-run-tests-is-not-a-macro
  (let [m (meta (resolve 'clojure.test/run-tests))]
    (is (not (:macro m))
        "run-tests must not carry :macro metadata after the implementation")
    (is (fn? (var-get (resolve 'clojure.test/run-tests)))
        "run-tests must be a function value")))

;; FAILS today: as a macro, *ns* is captured at expansion time.
;; After implementation: the no-arg arity must read *ns* DYNAMICALLY
;; (at call time), so binding *ns* before the call changes which
;; namespace's tests are run.
(deftest tlf-run-tests-reads-ns-at-call-time
  (tlf-save-restore
   (fn []
     (tlf-fresh-ns 'tlf.rtnc)
     (try
       ;; Register a test in the scratch namespace.
       (let [v (tlf-test-var! 'tlf.rtnc 'rtnc-pass (fn [] (is (= 1 1))))]
         (swap! clojure.test/tests-registry conj
                {:name "rtnc-pass" :ns "tlf.rtnc" :fn (:test (meta v))})
         ;; Call (run-tests) with *ns* dynamically rebound to the scratch ns.
         ;; If run-tests is a function it reads *ns* at call time;
         ;; if it is a macro it captures the calling namespace at compile time.
         (let [summary (tlf-quietly
                        (fn []
                          (binding [*ns* (find-ns 'tlf.rtnc)]
                            (clojure.test/run-tests))))]
           (is (= 1 (:test summary))
               "run-tests with *ns* rebound must run tests from the rebound namespace")))
       (finally (remove-ns 'tlf.rtnc))))))

;; =========================================================================
;; Item 3: use-fixtures must be a function (not a macro)
;; =========================================================================

;; FAILS today: (:macro (meta ...)) is true; must become nil/false.
(deftest tlf-use-fixtures-is-not-a-macro
  (let [m (meta (resolve 'clojure.test/use-fixtures))]
    (is (not (:macro m))
        "use-fixtures must not carry :macro metadata after the implementation")
    (is (fn? (var-get (resolve 'clojure.test/use-fixtures)))
        "use-fixtures must be a function value")))

;; FAILS today: as a macro, *ns* is captured at expansion time.
;; After implementation: use-fixtures must read *ns* at call time so
;; that calling it from a different dynamic namespace registers the
;; fixture there, not at the def site.
(deftest tlf-use-fixtures-reads-ns-at-call-time
  (tlf-save-restore
   (fn []
     (tlf-fresh-ns 'tlf.ufnc)
     (try
       ;; Call use-fixtures with *ns* rebound to the scratch namespace.
       ;; As a function reading *ns* at call time, the fixture must be
       ;; registered under "tlf.ufnc", not the current test file's ns.
       (let [fx (fn [f] (f))]
         (binding [*ns* (find-ns 'tlf.ufnc)]
           (clojure.test/use-fixtures :each fx))
         ;; The fixture must appear under the scratch ns, not this ns.
         (let [reg @clojure.test/fixtures-registry]
           (is (= [fx] (:each (get reg "tlf.ufnc")))
               "use-fixtures must register fixture under *ns* at call time")
           (is (nil? (:each (get reg (str *ns*))))
               "use-fixtures must not pollute the calling file's fixture slot")))
       (finally (remove-ns 'tlf.ufnc))))))

;; PASSES today and must keep passing after implementation:
;; :each fixture wrapping actually wraps each test invocation.
(deftest tlf-use-fixtures-each-wraps-test-runs
  ;; Strategy: register an :each fixture in a scratch namespace,
  ;; populate it with a test, drive run-namespace-tests, and verify
  ;; the fixture ran around the test body.
  (tlf-save-restore
   (fn []
     (tlf-fresh-ns 'tlf.ufex)
     (try
       (let [log (atom [])]
         ;; Register an :each fixture for the scratch namespace.
         (swap! clojure.test/fixtures-registry update "tlf.ufex"
                (fn [m]
                  (update (or m {}) :each
                          (fn [xs]
                            (conj (or xs [])
                                  (fn [f]
                                    (swap! log conj :before)
                                    (f)
                                    (swap! log conj :after)))))))
         ;; Register a test in the scratch namespace.
         (let [v (tlf-test-var! 'tlf.ufex 'ufex-target
                                (fn [] (swap! log conj :body)))]
           (swap! clojure.test/tests-registry conj
                  {:name "ufex-target" :ns "tlf.ufex"
                   :fn   (:test (meta v))}))
         ;; Drive the run.
         (tlf-quietly
          (fn [] (clojure.test/run-namespace-tests ["tlf.ufex"])))
         ;; The fixture must have wrapped the test body.
         (is (= [:before :body :after] @log)
             ":each fixture must execute before and after each test body"))
       (finally (remove-ns 'tlf.ufex))))))

;; PASSES today and must keep passing:
;; use-fixtures rejects invalid kind values.
(deftest tlf-use-fixtures-rejects-invalid-kind
  (tlf-save-restore
   (fn []
     (is (thrown? Exception
                  (clojure.test/use-fixtures :wrong (fn [f] (f))))
         "use-fixtures must throw on a kind that is not :once or :each"))))

(run-tests-and-exit)
