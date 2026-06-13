(ns clojure.test.junit
  "Emit clojure.test results as JUnit XML output.

  A test run is rendered as a tree of elements built by plain string
  construction: one <testsuite> per namespace wraps one <testcase> per
  test var, and a failing assertion adds a nested <failure> element while
  an uncaught error adds a nested <error> element.  Wrap a test run in
  with-junit-output and the whole run prints as a single well-formed XML
  document through the with-test-out sink."
  (:require [clojure.test :refer [report with-test-out testing-vars-str
                                  testing-contexts-str inc-report-counter]]
            [clojure.string :refer [join]]))

;; Per-run accumulator holding the open testsuite/testcase state and the
;; emitted lines.  Bound to a fresh atom by with-junit-output.  Shape:
;;   {:lines     [str ...]   ;; emitted XML lines, in order
;;    :suite     str|nil     ;; name of the open <testsuite>, or nil
;;    :case-name str|nil     ;; name of the open <testcase>, or nil
;;    :case-body [str ...]}  ;; nested <failure>/<error> lines for the
;;                           ;; currently open testcase
(def ^:dynamic *junit-state* nil)

(defn xml-escape
  "Returns s with the five XML metacharacters replaced by their entity
  references so the result is safe to place in element text or an
  attribute value."
  [s]
  (-> (str s)
      (clojure.string/replace "&" "&amp;")
      (clojure.string/replace "<" "&lt;")
      (clojure.string/replace ">" "&gt;")
      (clojure.string/replace "\"" "&quot;")
      (clojure.string/replace "'" "&apos;")))

(defn- emit-line!
  "Appends one already-rendered XML line to the run accumulator."
  [line]
  (when *junit-state*
    (swap! *junit-state* update :lines (fnil conj []) line)))

(defn- emit-case-body!
  "Appends one nested line (a <failure> or <error> element) to the open
  testcase's body."
  [line]
  (when *junit-state*
    (swap! *junit-state* update :case-body (fnil conj []) line)))

(defn- failure-detail
  "Builds the human-readable text body shared by <failure> and <error>
  elements: the test location, the test context, an optional message,
  and the expected/actual values, each on its own line and XML-escaped."
  [m]
  (let [parts (concat
               [(str "in " (testing-vars-str m))]
               (when (seq (testing-contexts-str))
                 [(testing-contexts-str)])
               (when-let [msg (:message m)] [msg])
               [(str "expected: " (pr-str (:expected m)))
                (str "  actual: " (pr-str (:actual m)))])]
    (xml-escape (join "\n" parts))))

;; --- JUnit report fn ---

(defn junit-report
  "Renders one clojure.test result map into the JUnit XML accumulator and
  keeps the report counters current.

  Lifecycle events drive the element structure: :begin-test-ns opens a
  <testsuite>, :end-test-ns closes it, :begin-test-var opens a <testcase>
  and :end-test-var closes it (emitting any nested <failure>/<error>
  collected for that case).  :pass, :fail and :error update counters and,
  for the latter two, add a nested element to the open testcase.
  :summary is ignored."
  [m]
  (let [t (:type m)]
    (cond
      (= t :begin-test-ns)
      (do (swap! *junit-state* assoc :suite (str (:ns m)))
          (emit-line! (str "<testsuite name=\"" (xml-escape (:ns m)) "\">")))

      (= t :end-test-ns)
      (do (emit-line! "</testsuite>")
          (swap! *junit-state* assoc :suite nil))

      (= t :begin-test-var)
      (let [vname (str (:name (meta (:var m))))]
        (inc-report-counter :test)
        (swap! *junit-state* assoc :case-name vname :case-body []))

      (= t :end-test-var)
      (let [st   @*junit-state*
            name (:case-name st)
            body (:case-body st)]
        (if (seq body)
          (do (emit-line! (str "  <testcase name=\"" (xml-escape name) "\">"))
              (doseq [line body] (emit-line! line))
              (emit-line! "  </testcase>"))
          (emit-line! (str "  <testcase name=\"" (xml-escape name) "\"/>")))
        (swap! *junit-state* assoc :case-name nil :case-body []))

      (= t :pass)
      (inc-report-counter :pass)

      (= t :fail)
      (do (inc-report-counter :fail)
          (emit-case-body!
           (str "    <failure message=\""
                (xml-escape (or (:message m) "assertion failed"))
                "\">" (failure-detail m) "</failure>")))

      (= t :error)
      (do (inc-report-counter :error)
          (emit-case-body!
           (str "    <error message=\""
                (xml-escape (or (:message m) "uncaught exception"))
                "\">" (failure-detail m) "</error>")))

      :else nil)))

(defn print-junit-document
  "Prints the accumulated lines as one XML document: the XML declaration,
  a <testsuites> root, and every emitted line in order, through the
  with-test-out sink."
  []
  (with-test-out
    (println "<?xml version=\"1.0\" encoding=\"UTF-8\"?>")
    (println "<testsuites>")
    (doseq [line (:lines @*junit-state*)]
      (println line))
    (println "</testsuites>")))

;; --- with-junit-output ---

(defmacro with-junit-output
  "Evaluates body with clojure.test/report bound to the JUnit reporter
  and a fresh accumulator, then prints the collected results as one
  well-formed JUnit XML document.  Returns body's value.

  Uses list construction rather than syntax-quote so the unqualified
  report and *junit-state* symbols reach mino's dynamic-binding lookup,
  which searches by bare name."
  [& body]
  (list 'binding ['clojure.test/report 'clojure.test.junit/junit-report
                  '*junit-state* '(atom {:lines [] :suite nil
                                         :case-name nil :case-body []})]
        (list 'let ['result# (cons 'do body)]
              '(clojure.test.junit/print-junit-document)
              'result#)))
