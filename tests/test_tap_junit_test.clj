(require "tests/test")
(require '[clojure.test.tap :refer [with-tap-output print-tap-plan
                                    print-tap-diagnostic]])
(require '[clojure.test.junit :refer [with-junit-output xml-escape]])
(require '[clojure.string :as str])

;; TAP and JUnit reporter tests.  The reporters are exercised by running
;; throwaway test vars (one passing, one failing) through with-tap-output
;; and with-junit-output and capturing the emitted text with
;; with-out-str.  The probe vars are interned into a scratch namespace and
;; driven through test-var directly, so the failing probe never registers
;; with this file's suite and never reports a failure to the outer runner.
;; Only the is assertions on the captured strings are visible to the
;; runner.

(defn- probe-ns!
  "Recreates a scratch namespace holding one passing and one failing test
  var; returns the two vars as [pass-var fail-var]."
  []
  (when (find-ns 'ttj.probe) (remove-ns 'ttj.probe))
  (create-ns 'ttj.probe)
  (let [pv (intern 'ttj.probe 'probe-pass (fn [] (is (= 1 1))))
        fv (intern 'ttj.probe 'probe-fail (fn [] (is (= 1 2))))]
    (alter-meta! pv assoc :test (var-get pv))
    (alter-meta! fv assoc :test (var-get fv))
    [pv fv]))

(defn- run-probes-tap
  "Captures the TAP output of running both probe vars through their
  begin/end-test-var lifecycle inside with-tap-output."
  []
  (let [[pv fv] (probe-ns!)]
    ;; Bind a fresh counter so the probe failure stays inside this helper
    ;; and never reaches the outer suite's *report-counters*.
    (with-out-str
      (binding [*test-out*        *out*
                *report-counters* (atom *initial-report-counters*)]
        (with-tap-output
          (test-var pv)
          (test-var fv))))))

(defn- run-probes-junit
  "Captures the JUnit XML output of running both probe vars, wrapped in a
  begin/end-test-ns pair so a <testsuite> brackets the cases."
  []
  (let [[pv fv] (probe-ns!)]
    ;; Bind a fresh counter so the probe failure stays inside this helper
    ;; and never reaches the outer suite's *report-counters*.
    (with-out-str
      (binding [*test-out*        *out*
                *report-counters* (atom *initial-report-counters*)]
        (with-junit-output
          (do-report {:type :begin-test-ns :ns 'ttj.probe})
          (test-var pv)
          (test-var fv)
          (do-report {:type :end-test-ns :ns 'ttj.probe}))))))

;; --- TAP ---

(deftest tap-plan-line-renders-1-dot-dot-n
  (is (= "1..3\n"
         (with-out-str
           (binding [*test-out* *out*]
             (print-tap-plan 3))))))

(deftest tap-diagnostic-prefixes-each-line
  (let [out (with-out-str
              (binding [*test-out* *out*]
                (print-tap-diagnostic "first\nsecond")))]
    (is (str/includes? out "# first"))
    (is (str/includes? out "# second"))))

(deftest tap-output-has-ok-not-ok-and-plan
  (let [out (run-probes-tap)]
    (is (str/includes? out "ok "))
    (is (str/includes? out "not ok "))
    (is (str/includes? out "1.."))
    ;; the passing probe emits an ok line that is not a not-ok line
    (is (str/includes? out "ok 1 -"))
    ;; diagnostics for the failure are present
    (is (str/includes? out "# expected:"))))

;; --- JUnit ---

(deftest junit-xml-escape-replaces-metacharacters
  (is (= "a &amp; b &lt;c&gt; &quot;d&quot; &apos;e&apos;"
         (xml-escape "a & b <c> \"d\" 'e'"))))

(deftest junit-output-has-suite-case-and-failure
  (let [out (run-probes-junit)]
    (is (str/includes? out "<testsuite"))
    (is (str/includes? out "<testcase"))
    (is (str/includes? out "<failure"))))

(deftest junit-output-is-balanced
  (let [out (run-probes-junit)]
    ;; every opened container element has its matching close tag
    (is (str/includes? out "<testsuites>"))
    (is (str/includes? out "</testsuites>"))
    (is (str/includes? out "</testsuite>"))
    (is (str/includes? out "</testcase>"))
    (is (str/includes? out "</failure>"))))

(run-tests-and-exit)
