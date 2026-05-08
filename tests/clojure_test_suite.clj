;; Pure-mino driver for the external clojure-test-suite.
;; Assumes the suite is at ../clojure-test-suite (sibling of mino).
;;
;; Usage:
;;   ./mino tests/clojure_test_suite.clj
;;       Driver mode. Forks one ./mino sub-process per .cljc file in
;;       the suite (so a segfault in any file doesn't take down the
;;       rest), parses each summary, and prints an aggregate report.
;;
;;   ./mino tests/clojure_test_suite.clj <suite-file>
;;       One-file harness. Loads the file, runs registered tests,
;;       and exits with run-tests-and-exit's status. Used by the
;;       driver above for per-file isolation.
;;
;; Two shims under lib/clojure/core_test/ make the suite loadable
;; against mino: portability.clj (when-var-exists, big-int?, sleep)
;; and number_range.clj (constants the suite reads through reader
;; conditionals as JVM-only Long/MAX_VALUE etc.). Without them the
;; suite cannot even load.

(require '[clojure.string :refer [ends-with? split starts-with? includes?]])

(def suite-dir "../clojure-test-suite/test/clojure/core_test")
(def driver    "tests/clojure_test_suite.clj")

;; -------- one-file harness mode (sub-process) --------

(defn run-one [file-path]
  (require "tests/test")
  (try
    (require file-path)
    (catch e
      (println (str "LOAD-ERR: "
                    (cond
                      (string? e) e
                      (map? e)    (or (:mino/message e) (str e))
                      :else       (str e))))
      (exit 3)))
  (run-tests-and-exit))

;; -------- driver mode (top-level) --------

(defn parse-int-prefix
  "Extract the leading integer from a string, ignoring leading spaces.
  Returns 0 if no digits are found.
  Note: (seq str) yields one-char strings on mino, not characters,
  so we compare via (int c) which works on both types."
  [s]
  (let [zero (int \0)
        nine (int \9)
        space (int \space)
        chars  (seq s)
        digits (->> chars
                    (drop-while (fn [c] (= (int c) space)))
                    (take-while (fn [c] (and (>= (int c) zero)
                                              (<= (int c) nine))))
                    (apply str))]
    (if (= "" digits) 0 (parse-long digits))))

(defn parse-summary
  "Pull the last 'N tests, M assertions: P passed, F failed, E errors'
  line from a stdout blob. Returns map of ints or nil."
  [out]
  (let [lines (split out "\n")
        line  (some (fn [l]
                      (when (and (includes? l "tests, ")
                                 (includes? l "assertions:"))
                        l))
                    (reverse lines))]
    (when line
      (let [colon-split (split line ":")
            head        (first colon-split)
            tail        (if (>= (count colon-split) 2) (nth colon-split 1) "")
            head-words  (split head " ")
            tail-parts  (split tail ",")]
        {:tests   (parse-int-prefix (nth head-words 0 ""))
         :asserts (parse-int-prefix (nth head-words 2 ""))
         :pass    (parse-int-prefix (nth tail-parts 0 ""))
         :fail    (parse-int-prefix (nth tail-parts 1 ""))
         :error   (parse-int-prefix (nth tail-parts 2 ""))}))))

(defn first-error-line [out]
  (some (fn [l]
          (when (or (starts-with? l "LOAD-ERR:")
                    (starts-with? l "error[")
                    (includes?    l "unhandled exception"))
            l))
        (split out "\n")))

(defn classify [summary load-err exit]
  (cond
    (= exit 139)             :crash
    load-err                 :load-error
    (nil? summary)           :no-summary
    (and (= (:fail summary) 0) (= (:error summary) 0))  :ok
    :else                    :test-failures))

(defn run-driver []
  (let [files (sort (filter (fn [f]
                              (and (ends-with? f ".cljc")
                                   (not (ends-with? f "/portability.cljc"))
                                   (not (ends-with? f "/number_range.cljc"))))
                            (file-seq suite-dir)))
        total (count files)
        results (atom [])]

    (println (str "Running " total " suite files (sequential per-process, 30s/file)..."))
    (doseq [f files]
      (let [base (last (split f "/"))
            {:keys [exit out]} (sh "timeout" "30" "./mino" driver f)
            timed-out? (= exit 124)
            err-line (cond
                       timed-out? "TIMEOUT after 30s"
                       :else      (first-error-line out))
            load-err (when (and err-line (starts-with? err-line "LOAD-ERR:")) err-line)
            summary  (when-not timed-out? (parse-summary out))
            status   (cond timed-out? :timeout
                           :else      (classify summary load-err exit))]
        (swap! results conj
               {:file base :exit exit :status status
                :summary summary :err err-line})))

    (let [by-status (group-by :status @results)
          ok        (get by-status :ok [])
          tfails    (get by-status :test-failures [])
          loaderrs  (get by-status :load-error [])
          crashes   (get by-status :crash [])
          timeouts  (get by-status :timeout [])
          nosumms   (get by-status :no-summary [])
          sum-of    (fn [k rs]
                      (reduce + 0 (map (fn [r] (or (get-in r [:summary k]) 0)) rs)))
          completed (concat ok tfails)
          n-tests   (sum-of :tests   completed)
          n-asr     (sum-of :asserts completed)
          n-pass    (sum-of :pass    completed)
          n-fail    (sum-of :fail    completed)
          n-err     (sum-of :error   completed)]

      (println "")
      (println "================ EXTERNAL TEST SUITE REPORT ================")
      (println (str "Suite files processed: " total))
      (println (str "  OK (all assertions pass): " (count ok)))
      (println (str "  Test failures:            " (count tfails)))
      (println (str "  Load errors:              " (count loaderrs)))
      (println (str "  Process crashes (segv):   " (count crashes)))
      (println (str "  Timeouts (>30s):          " (count timeouts)))
      (println (str "  No summary line:          " (count nosumms)))
      (println "")
      (println (str "Aggregate (across files that completed): "
                    n-tests " tests, " n-asr " assertions: "
                    n-pass " passed, " n-fail " failed, " n-err " errors"))

      (println "")
      (println "---- LOAD ERRORS ----")
      (doseq [r (sort-by :file loaderrs)]
        (println (str "  " (:file r) " :: " (:err r))))

      (println "")
      (println "---- CRASHES ----")
      (doseq [r (sort-by :file crashes)]
        (println (str "  " (:file r) " (exit=" (:exit r) ") :: " (:err r))))

      (println "")
      (println "---- TIMEOUTS (>30s, killed) ----")
      (doseq [r (sort-by :file timeouts)]
        (println (str "  " (:file r))))

      (println "")
      (println "---- NO-SUMMARY FILES (loaded but no run-tests output) ----")
      (doseq [r (sort-by :file nosumms)]
        (println (str "  " (:file r) " (exit=" (:exit r) ") :: " (or (:err r) ""))))

      (println "")
      (println "---- FILES WITH ASSERTION FAILURES (sorted by fail+error) ----")
      (let [sorted (sort-by (fn [r] (- (+ (get-in r [:summary :fail] 0)
                                           (get-in r [:summary :error] 0))))
                            tfails)]
        (doseq [r sorted]
          (println (str "  " (:file r)
                        "  fail=" (get-in r [:summary :fail])
                        "  error=" (get-in r [:summary :error])
                        "  pass="  (get-in r [:summary :pass])))))

      (spit "/tmp/external_results.edn" (pr-str @results))
      (println "")
      (println "Per-file raw results saved to /tmp/external_results.edn"))))

;; -------- self-dispatch --------

(if (seq *command-line-args*)
  (run-one (first *command-line-args*))
  (run-driver))
