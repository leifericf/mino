(ns clojure.test.tap
  "Emit clojure.test results as TAP (Test Anything Protocol) output.

  TAP is a line-oriented text format: a plan line of the form 1..N, one
  result line per assertion (ok N - ... for a pass, not ok N - ... for a
  failure or error), and diagnostic lines prefixed with '# '.  Wrap a
  test run in with-tap-output and every assertion is rendered as a TAP
  result line through the with-test-out sink."
  (:require [clojure.test :refer [report with-test-out testing-vars-str
                                  testing-contexts-str inc-report-counter]]
            [clojure.string :refer [split-lines]]))

;; Running count of TAP result lines emitted in the current run.  Bound
;; to a fresh atom by with-tap-output so nested or repeated runs each get
;; their own sequence starting at 1.
(def ^:dynamic *tap-counter* nil)

(defn- next-tap-number
  "Bumps *tap-counter* and returns the new value.  Returns nil when no
  counter is bound, which keeps result lines printing without a number
  rather than throwing."
  []
  (when *tap-counter*
    (swap! *tap-counter* inc)
    @*tap-counter*))

(defn print-tap-plan
  "Prints a TAP plan line, 1..n, declaring that n result lines follow."
  [n]
  (with-test-out
    (println (str "1.." n))))

(defn print-tap-diagnostic
  "Prints data as one or more TAP diagnostic lines.  data is rendered to
  a string, split on newlines, and each line is printed prefixed with
  '# ' so a TAP consumer treats it as commentary rather than a result."
  [data]
  (let [s (if (string? data) data (str data))]
    (with-test-out
      (doseq [line (split-lines s)]
        (println (str "# " line))))))

(defn print-tap-pass
  "Prints a passing TAP result line, ok N - msg."
  [msg]
  (with-test-out
    (let [n (next-tap-number)]
      (println (str "ok " n " - " msg)))))

(defn print-tap-fail
  "Prints a failing TAP result line, not ok N - msg."
  [msg]
  (with-test-out
    (let [n (next-tap-number)]
      (println (str "not ok " n " - " msg)))))

;; --- TAP report fn ---
;;
;; tap-report mirrors the shape of clojure.test/report: it takes a result
;; map and dispatches on its :type.  with-tap-output binds clojure.test's
;; report var to this fn so deftests run through it emit TAP.

(defn- print-tap-failure-detail
  "Prints the shared diagnostic block for a failing or erroring result:
  the test context, an optional message, and the expected/actual values."
  [m]
  (when (seq (testing-contexts-str))
    (print-tap-diagnostic (testing-contexts-str)))
  (when-let [msg (:message m)] (print-tap-diagnostic msg))
  (print-tap-diagnostic (str "expected: " (pr-str (:expected m))))
  (print-tap-diagnostic (str "  actual: " (pr-str (:actual m)))))

(defn tap-report
  "Renders one clojure.test result map as TAP and keeps the report
  counters current.  :pass emits an ok line, :fail and :error emit a not
  ok line followed by diagnostic detail, and the namespace and test-var
  lifecycle events are passed through as counter updates or diagnostics.
  :summary is ignored; the plan line carries the count instead."
  [m]
  (let [t (:type m)]
    (cond
      (= t :pass)
      (do (inc-report-counter :pass)
          (print-tap-pass (testing-vars-str m)))

      (= t :fail)
      (do (inc-report-counter :fail)
          (print-tap-fail (testing-vars-str m))
          (print-tap-failure-detail m))

      (= t :error)
      (do (inc-report-counter :error)
          (print-tap-fail (testing-vars-str m))
          (print-tap-failure-detail m))

      (= t :begin-test-ns)
      (print-tap-diagnostic (str "Testing " (:ns m)))

      (= t :begin-test-var)
      (inc-report-counter :test)

      :else nil)))

;; --- with-tap-output ---

(defmacro with-tap-output
  "Evaluates body with clojure.test/report bound to the TAP reporter and
  a fresh TAP counter.  Every assertion run inside body emits a TAP
  result line; a plan line, 1..N, is printed last reflecting the number
  of result lines emitted.  Returns body's value.

  Uses list construction rather than syntax-quote so the unqualified
  report and *tap-counter* symbols reach mino's dynamic-binding lookup,
  which searches by bare name."
  [& body]
  (list 'binding ['clojure.test/report 'clojure.test.tap/tap-report
                  '*tap-counter* '(atom 0)]
        (list 'let ['result# (cons 'do body)]
              (list 'clojure.test.tap/print-tap-plan '@*tap-counter*)
              'result#)))
