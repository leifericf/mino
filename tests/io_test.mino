(require "tests/test")

;; I/O operations: spit, slurp, println, prn.

(def ^:private tmpdir
  (or (getenv "TMPDIR") (getenv "TEMP") (getenv "TMP") "/tmp"))

(deftest spit-and-slurp
  (let [f (str tmpdir "/mino_test_spit.txt")]
    (spit f "hello")
    (is (= "hello" (slurp f)))))

(deftest spit-non-string
  (let [f (str tmpdir "/mino_test_spit2.txt")]
    (spit f 42)
    (is (= "42" (slurp f)))))

(deftest println-returns-nil
  (is (= nil (println "io-test-println"))))

(deftest prn-returns-nil
  (is (= nil (prn 1 2 3))))

(deftest print-returns-nil
  (is (= nil (print "io-test-print"))))

(deftest pr-returns-nil
  (is (= nil (pr 1 2 3))))

(deftest newline-returns-nil
  (is (= nil (newline))))

(deftest newline-takes-no-args
  (is (thrown? (newline 1))))
