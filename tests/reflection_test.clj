(require "tests/test")

;; doc, source, apropos.

(deftest doc-with-docstring
  (def inc-doc__rt "increment by one" (fn [x] (+ x 1)))
  (is (= "increment by one" (doc 'inc-doc__rt))))

(deftest doc-no-docstring
  (def y__rt 42)
  (is (= nil (doc 'y__rt))))

(deftest source-returns-form
  (def sq-src__rt "square" (fn [x] (* x x)))
  (is (= 'def (car (source 'sq-src__rt)))))

(deftest defmacro-docstring
  (defmacro my-id__rt "identity macro" (x) x)
  (is (= "identity macro" (doc 'my-id__rt))))

(deftest apropos-finds
  (let [results (apropos "cons")]
    (is results)
    (is (cons? results))))

(deftest apropos-empty
  (is (= nil (apropos "zzzznotfound"))))

;; load-string and load-file: read+eval all forms; return last value.

(deftest load-string-single-form
  (is (= 2 (load-string "(+ 1 1)"))))

(deftest load-string-multiple-forms
  (is (= 84 (load-string "(def __ls_x 42) (* __ls_x 2)"))))

(deftest load-string-define-then-call
  (is (= 25 (load-string "(defn __ls_sq [x] (* x x)) (__ls_sq 5)"))))

(deftest load-string-empty-returns-nil
  (is (= nil (load-string ""))))

(deftest load-string-rejects-non-string
  (is (thrown? (load-string 42))))
