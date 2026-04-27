(require "tests/test")
(require '[clojure.repl :refer [doc-string source-form apropos
                                doc source dir find-doc pst dir-fn]])
(require '[clojure.stacktrace :refer [print-stack-trace print-cause-trace
                                      print-throwable root-cause]])

;; clojure.repl C primitives.

(deftest doc-string-returns-docstring
  (is (string? (doc-string 'map))))

(deftest doc-string-on-undocumented
  (def __undoc 1)
  (is (= nil (doc-string '__undoc))))

(deftest source-form-returns-form
  (def __sq "square" (fn [x] (* x x)))
  (is (= 'def (car (source-form '__sq)))))

(deftest apropos-finds-substring
  (let [r (apropos "cons")]
    (is (cons? r))))

(deftest apropos-empty-on-miss
  (is (= nil (apropos "zzz_no_such_thing_zzz"))))

;; clojure.repl macros emit prints; we just confirm they return nil and don't
;; throw. with-out-str captures the printed text.

(deftest doc-macro-prints
  (is (string? (with-out-str (doc map)))))

(deftest source-macro-prints
  (is (string? (with-out-str (source when)))))

(deftest dir-fn-returns-sorted
  (let [names (dir-fn 'clojure.repl)]
    (is (every? symbol? names))
    (is (some #{'doc-string} names))))

(deftest dir-macro-prints
  (is (string? (with-out-str (dir clojure.repl)))))

(deftest find-doc-prints-matches
  (is (string? (with-out-str (find-doc "lazy sequence")))))

;; clojure.stacktrace formats *e.

(deftest print-throwable-formats
  (let [e {:mino/kind :eval/type :mino/code "MTY001" :mino/message "boom"}]
    (let [out (with-out-str (print-throwable e))]
      (is (clojure.string/includes? out "MTY001"))
      (is (clojure.string/includes? out "boom")))))

(deftest root-cause-walks-chain
  (let [inner {:mino/kind :user :mino/code "X" :mino/message "inner"}
        outer {:mino/kind :user :mino/code "Y" :mino/message "outer"
               :mino/cause inner}]
    (is (= "X" (:mino/code (root-cause outer))))))

(deftest print-stack-trace-prints
  (let [e {:mino/kind :user :mino/code "X" :mino/message "boom"
           :mino/location {:file "f" :line 1 :column 2}
           :mino/trace []}]
    (is (string? (with-out-str (print-stack-trace e))))))

(deftest print-cause-trace-walks
  (let [inner {:mino/kind :user :mino/code "X" :mino/message "inner"}
        outer {:mino/kind :user :mino/code "Y" :mino/message "outer"
               :mino/cause inner}]
    (let [out (with-out-str (print-cause-trace outer))]
      (is (clojure.string/includes? out "Y"))
      (is (clojure.string/includes? out "X")))))
