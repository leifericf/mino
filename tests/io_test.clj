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

;; *out* / with-out-str: rebind *out* to a string-collecting atom and
;; return the captured text.

(deftest out-default-is-stdout-keyword
  (is (= :mino/stdout *out*)))

(deftest err-default-is-stderr-keyword
  (is (= :mino/stderr *err*)))

(deftest with-out-str-captures-println
  (is (= "hi\n" (with-out-str (println "hi")))))

(deftest with-out-str-captures-prn
  (is (= "[1 2 3]\n" (with-out-str (prn [1 2 3])))))

(deftest with-out-str-captures-print
  (is (= "1 2 3" (with-out-str (print 1 2 3)))))

(deftest with-out-str-captures-pr-builtin
  (is (= "{:a 1}" (with-out-str (pr-builtin {:a 1})))))

(deftest with-out-str-captures-newline
  (is (= "\n" (with-out-str (newline)))))

(deftest with-out-str-captures-multiple-prints
  (is (= "ab\n" (with-out-str (print "a") (println "b")))))

(deftest with-out-str-empty-body-returns-empty
  (is (= "" (with-out-str))))

(deftest print-str-no-newline
  (is (= "1 2 3" (print-str 1 2 3))))

(deftest println-str-trailing-newline
  (is (= "a b\n" (println-str "a" "b"))))

(deftest prn-str-readable-with-newline
  (is (= "[1 2 3]\n" (prn-str [1 2 3])))
  (is (= "\"hi\"\n" (prn-str "hi"))))

(deftest printf-formats-and-prints
  (is (= "x=42 y=ok" (with-out-str (printf "x=%d y=%s" 42 "ok")))))

(deftest flush-returns-nil
  (is (= nil (flush))))

(deftest flush-takes-no-args
  (is (thrown? (flush 1))))

;; *in* / with-in-str / read / read-line: bind *in* to a string-cursor
;; atom and consume forms or lines from it.

(deftest in-default-is-stdin-keyword
  (is (= :mino/stdin *in*)))

(deftest with-in-str-read-line-single
  (is (= "hello" (with-in-str "hello\nworld" (read-line)))))

(deftest with-in-str-read-line-multiple
  (is (= ["a" "b" "c" nil]
        (with-in-str "a\nb\nc"
          [(read-line) (read-line) (read-line) (read-line)]))))

(deftest with-in-str-read-line-no-trailing-newline
  (is (= "no-newline" (with-in-str "no-newline" (read-line)))))

(deftest with-in-str-read-line-empty-string-is-nil
  (is (= nil (with-in-str "" (read-line)))))

(deftest with-in-str-read-form
  (is (= '(+ 1 2) (with-in-str "(+ 1 2)" (read)))))

(deftest with-in-str-read-multiple-forms
  (is (= ['(+ 1 2) [3 4]]
        (with-in-str "  (+ 1 2)  [3 4]  "
          [(read) (read)]))))

(deftest with-in-str-read-feeds-eval
  (is (= 30 (with-in-str "(+ 10 20)" (eval (read))))))

(deftest read-from-stdin-is-unsupported
  (is (thrown? (read))))

(deftest read-string-still-works
  (is (= '(* 6 7) (read "(* 6 7)"))))
