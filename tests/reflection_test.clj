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

;; class: concrete type tag, ignoring :type metadata, nil->nil.

;; Spec-first: class is unimplemented; tests fail with "unbound symbol:
;; class" until the C primitive lands.

(deftest class-nil-returns-nil
  ;; (class nil) => nil, not :nil.
  ;; Contrast: (type nil) => :nil.
  (is (= nil (class nil)))
  (is (= :nil (type nil))))

(deftest class-scalars
  ;; class agrees with type for plain scalar values.
  (is (= :int     (class 42)))
  (is (= :float   (class 3.14)))
  (is (= :string  (class "hello")))
  (is (= :bool    (class true)))
  (is (= :bool    (class false)))
  (is (= :char    (class \a)))
  (is (= :keyword (class :foo)))
  (is (= :symbol  (class 'bar)))
  (is (= :fn      (class inc)))
  (is (= :fn      (class (fn [x] x)))))

(deftest class-collections
  ;; class agrees with type for collection values.
  (is (= :list    (class '(1 2 3))))
  (is (= :list    (class '())))
  (is (= :vector  (class [1 2])))
  (is (= :vector  (class [])))
  (is (= :map     (class {:a 1})))
  (is (= :map     (class {})))
  (is (= :set     (class #{1 2})))
  (is (= :set     (class #{}))))

(deftest class-ignores-type-metadata
  ;; The defining difference from type: :type metadata is invisible to class.
  (is (= :map     (class (with-meta {} {:type :foo}))))
  (is (= :vector  (class (with-meta [] {:type :bar}))))
  (is (= :list    (class (with-meta '(1 2) {:type :custom}))))
  (is (= :set     (class (with-meta #{} {:type :other}))))
  ;; type sees the metadata override; class does not.
  (is (= :foo     (type  (with-meta {} {:type :foo}))))
  (is (= :map     (class (with-meta {} {:type :foo})))))

(defrecord ClassPt__rt [x y])

(deftest class-record-matches-type
  ;; For records, class and type agree: both return the record's type symbol.
  (let [r (->ClassPt__rt 1 2)]
    (is (= (type r) (class r)))
    (is (= ClassPt__rt (class r)))))

(deftest class-same-type-comparison
  ;; (= (class a) (class b)) is a valid same-type predicate.
  (is (= (class 1)   (class 2)))
  (is (= (class "x") (class "y")))
  (is (not= (class 1) (class "x")))
  (is (= (class [])  (class [1 2])))
  (is (not= (class []) (class {}))))

(deftest class-resolve-and-doc
  ;; class must be a named var with a docstring.
  (is (var? (resolve 'class)))
  (is (some? (doc-string 'class))))

(deftest class-arity
  ;; class requires exactly one argument.
  (is (thrown? (class)))
  (is (thrown? (class 1 2))))

(run-tests-and-exit)
