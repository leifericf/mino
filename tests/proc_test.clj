(require "tests/test")
(require '[clojure.string :as pstr])

;; Process execution primitives: sh, sh!.
;;
;; echo's exact stdout is shell-specific: cmd.exe on Windows emits a
;; trailing space before the newline (and double-quotes an argument
;; containing an apostrophe) where POSIX sh does not. These tests
;; exercise mino's capture / exit / throw mechanism, not the shell's
;; byte-exact formatting, so they strip the trailing-space difference
;; before comparing and assert substring content for the quoted case.

(defn- norm-eol
  "Strip trailing spaces before each newline so cmd.exe's trailing-space
   echo compares equal to POSIX echo. A no-op on POSIX output."
  [s]
  (pstr/replace s #" +\n" "\n"))

(deftest sh-echo
  (let [result (sh "echo" "hello")]
    (is (= 0 (:exit result)))
    (is (= "hello\n" (norm-eol (:out result))))))

(deftest sh-multiple-args
  (let [result (sh "echo" "a" "b" "c")]
    (is (= 0 (:exit result)))
    (is (= "a b c\n" (norm-eol (:out result))))))

(deftest sh-nonzero-exit
  (let [result (sh "false")]
    (is (not= 0 (:exit result)))))

(deftest sh!-returns-stdout
  (is (= "hello\n" (norm-eol (sh! "echo" "hello")))))

(deftest sh!-throws-on-failure
  (is (thrown? (sh! "false"))))

(deftest sh-special-chars
  ;; The apostrophe must survive argument passing + capture. cmd.exe
  ;; wraps the arg in double quotes where POSIX sh does not, so assert
  ;; the content is present rather than byte-exact.
  (let [result (sh "echo" "it's a test")]
    (is (= 0 (:exit result)))
    (is (pstr/includes? (norm-eol (:out result)) "it's a test"))))

(deftest sh-type-errors
  (is (thrown? (sh 42)))
  (is (thrown? (sh "echo" 42))))

(run-tests-and-exit)
