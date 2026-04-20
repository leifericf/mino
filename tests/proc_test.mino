(require "tests/test")

;; Process execution primitives: sh, sh!.

(deftest sh-echo
  (let [result (sh "echo" "hello")]
    (is (= 0 (:exit result)))
    (is (= "hello\n" (:out result)))))

(deftest sh-multiple-args
  (let [result (sh "echo" "a" "b" "c")]
    (is (= 0 (:exit result)))
    (is (= "a b c\n" (:out result)))))

(deftest sh-nonzero-exit
  (let [result (sh "false")]
    (is (not= 0 (:exit result)))))

(deftest sh!-returns-stdout
  (is (= "hello\n" (sh! "echo" "hello"))))

(deftest sh!-throws-on-failure
  (is (thrown? (sh! "false"))))

(deftest sh-special-chars
  ;; Verify shell escaping handles special characters.
  (let [result (sh "echo" "it's a test")]
    (is (= "it's a test\n" (:out result)))))

(deftest sh-type-errors
  (is (thrown? (sh 42)))
  (is (thrown? (sh "echo" 42))))

(run-tests)
