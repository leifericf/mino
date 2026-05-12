(require "tests/test")
(require '[clojure.repl :refer [doc-string source-form apropos]])

;; doc-string, source-form, apropos under clojure.repl.

(deftest doc-with-docstring
  (def inc-doc__rt "increment by one" (fn [x] (+ x 1)))
  (is (= "increment by one" (doc-string 'inc-doc__rt))))

(deftest doc-no-docstring
  (def y__rt 42)
  (is (= nil (doc-string 'y__rt))))

(deftest source-returns-form
  (def sq-src__rt "square" (fn [x] (* x x)))
  (is (= 'def (car (source-form 'sq-src__rt)))))

(deftest defmacro-docstring
  (defmacro my-id__rt "identity macro" (x) x)
  (is (= "identity macro" (doc-string 'my-id__rt))))

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

(deftest name-error-names-the-offender
  ;; (name x) for non-keyword/symbol/string values used to throw the
  ;; bare "name: expected a keyword, symbol, or string" -- which was
  ;; useless several frames downstream of the bad call site. The
  ;; error message now includes `pr-str` of the offending value so
  ;; the caller is greppable from the message alone.
  (let [err (try (name [1 2]) nil
                 (catch e (if (map? e) (:mino/message e) (str e))))]
    (is (some? err))
    (is (some? (re-find #"\[1 2\]" err))))
  (let [err (try (name 42) nil
                 (catch e (if (map? e) (:mino/message e) (str e))))]
    (is (some? err))
    (is (some? (re-find #"42" err)))))

(deftest namespace-error-names-the-offender
  (let [err (try (namespace [1 2]) nil
                 (catch e (if (map? e) (:mino/message e) (str e))))]
    (is (some? err))
    (is (some? (re-find #"\[1 2\]" err)))))
