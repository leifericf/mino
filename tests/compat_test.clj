(require "tests/test")

;; --- Character literals for terminator characters ---

(deftest char-literal-terminators
  (testing "single terminator characters as literals"
    (is (char? \{))
    (is (= "{" (str \{)))
    (is (= "}" (str \})))
    (is (= ";" (str \;)))
    (is (= "\"" (str \")))
    (is (= "^" (str \^)))
    (is (= "~" (str \~)))
    (is (= "@" (str \@)))
    (is (= "`" (str \`)))
    (is (= "(" (str \()))
    (is (= ")" (str \))))
    (is (= "[" (str \[)))
    (is (= "]" (str \]))))

  (testing "named character literals still work"
    (is (char? \space))
    (is (= " " (str \space)))
    (is (= "\n" (str \newline)))
    (is (= "\t" (str \tab))))

  (testing "unicode escapes (char literals)"
    (is (= \A \u0041))
    (is (= \a \u0061))
    (is (= \0 \u0030))
    (is (= 65 (int \u0041))))

  (testing "whitespace characters as literals"
    (is (= "," (str \,)))))

;; --- Map literal reader conditional key elimination ---

(deftest map-reader-conditional-key-elim
  ;; Mino matches both :mino and :clj, so we use :cljs to drive
  ;; elimination behavior — :cljs is the always-skipped tag.
  (testing "eliminated key skips paired value"
    (is (= {:c 2} {#?(:cljs :b) 1 :c 2})))

  (testing "both branches eliminated"
    (is (= {:ok 42} {#?(:cljs :y) #?(:cljs 2) :ok 42})))

  (testing "empty map after elimination"
    (is (= {} {#?(:cljs :b) 1})))

  (testing "mino branch matches"
    (is (= {:a 1} {#?(:mino :a :clj :b) 1})))

  (testing "clj branch also matches under reader-conditional"
    (is (= {:a 1} {#?(:clj :a :cljs :b) 1}))))

;; --- Unquote-splicing in vectors ---

(deftest vector-unquote-splicing
  (testing "basic splice"
    (let [xs [2 3]]
      (is (= [1 2 3 4] `[1 ~@xs 4]))))

  (testing "splice from list"
    (let [xs (list 1 2)]
      (is (= [1 2] `[~@xs]))))

  (testing "empty splice"
    (let [xs []]
      (is (= [] `[~@xs]))))

  (testing "multiple splices"
    (let [a [1 2] b [3 4]]
      (is (= [1 2 3 4] `[~@a ~@b]))))

  (testing "mix of unquote and splice"
    (let [x 42 xs [1 2]]
      (is (= [42 1 2] `[~x ~@xs]))))

  (testing "nested vector splice"
    (let [xs [2 3]]
      (is (= [1 [2 3] 4] `[1 [~@xs] 4])))))

;; --- defn- ---

(defn- private-fn [x] (* x x))

(deftest defn-private
  (testing "defn- defines a working function"
    (is (= 25 (private-fn 5)))
    (is (= 0 (private-fn 0)))))

;; --- defprotocol with docstring ---

(defprotocol Describable
  "A protocol for things that describe themselves."
  (describe [this]))

(extend-type :string
  Describable
  (describe [this] (str "string: " this)))

(extend-type :int
  Describable
  (describe [this] (str "number: " this)))

(deftest defprotocol-docstring
  (testing "protocol with docstring works"
    (is (= "string: hello" (describe "hello")))
    (is (= "number: 42" (describe 42)))))

;; --- Multi-arity defmacro ---

(defmacro my-add
  ([x] x)
  ([x y] (list '+ x y))
  ([x y & more] (list '+ x (apply list 'my-add y more))))

(deftest multi-arity-defmacro
  (testing "single arity"
    (is (= 5 (my-add 5))))
  (testing "two arities"
    (is (= 7 (my-add 3 4))))
  (testing "variadic arity"
    (is (= 10 (my-add 1 2 3 4)))))

(defmacro my-str-macro
  "A multi-arity macro with docstring."
  ([] "")
  ([x] (list 'str x))
  ([x & more] (list 'str x (apply list 'my-str-macro more))))

(deftest multi-arity-defmacro-with-doc
  (testing "zero arity"
    (is (= "" (my-str-macro))))
  (testing "one arg"
    (is (= "hello" (my-str-macro "hello"))))
  (testing "multiple args"
    (is (= "abc" (my-str-macro "a" "b" "c")))))

;; --- Multimethods ---

(defmulti area-mm :shape)
(defmethod area-mm :circle [{:keys [r]}] (* 3.14159 r r))
(defmethod area-mm :rect [{:keys [w h]}] (* w h))
(defmethod area-mm :default [s] :unknown)

(deftest multimethod-basic
  (testing "dispatch on keyword"
    (is (= 12 (area-mm {:shape :rect :w 3 :h 4}))))
  (testing "default method"
    (is (= :unknown (area-mm {:shape :hexagon})))))

(defmulti greet-mm (fn [x] (:lang x)))
(defmethod greet-mm :en [_] "Hello!")
(defmethod greet-mm :fr [_] "Bonjour!")

(deftest multimethod-custom-dispatch
  (testing "custom dispatch function"
    (is (= "Hello!" (greet-mm {:lang :en})))
    (is (= "Bonjour!" (greet-mm {:lang :fr})))))

(deftest multimethod-methods-api
  (testing "methods returns method table"
    (is (= 3 (count (methods area-mm)))))
  (testing "get-method returns specific method"
    (is (fn? (get-method area-mm :circle)))
    (is (nil? (get-method area-mm :nonexistent)))))

(deftest multimethod-remove
  (let [mm-test (do
                  (defmulti temp-mm identity)
                  (defmethod temp-mm :a [_] 1)
                  (defmethod temp-mm :b [_] 2)
                  temp-mm)]
    (is (= 2 (count (methods mm-test))))
    (remove-method mm-test :b)
    (is (= 1 (count (methods mm-test))))))

(derive :compat-circle :compat-shape)
(derive :compat-rect :compat-shape)
(derive :compat-square :compat-rect)

(defmulti draw-mm identity)
(defmethod draw-mm :compat-shape [s] :generic)
(defmethod draw-mm :compat-circle [s] :circle)

(deftest multimethod-hierarchy
  (testing "exact match takes priority"
    (is (= :circle (draw-mm :compat-circle))))
  (testing "hierarchy-aware fallback"
    (is (= :generic (draw-mm :compat-rect))))
  (testing "transitive hierarchy"
    (is (= :generic (draw-mm :compat-square)))))

(defmulti process-mm "Processes items by type." :type)
(defmethod process-mm :text [{:keys [content]}] (str "text: " content))

(deftest multimethod-with-docstring
  (testing "defmulti with docstring"
    (is (= "text: hello" (process-mm {:type :text :content "hello"})))))

;; --- Regex literals ---

(deftest regex-literal-reader
  (testing "basic regex literal produces a string"
    (is (= "hello" #"hello")))

  (testing "regex literal works with re-find"
    (is (= "123" (re-find #"[0-9]+" "abc123def"))))

  (testing "regex literal in a vector"
    (is (= ["a" "b"] [#"a" #"b"])))

  (testing "regex literal in a map"
    (is (= {:pat "foo"} {:pat #"foo"})))

  (testing "regex literal with quantifiers"
    (is (= "hell" (re-find #"hel+" "hello")))))

;; --- letfn ---

(deftest letfn-basic
  (testing "basic local function binding"
    (is (= 10 (letfn [(double [x] (* 2 x))]
                (double 5)))))

  (testing "multiple bindings"
    (is (= 11 (letfn [(double [x] (* 2 x))
                       (add1 [x] (+ x 1))]
                (add1 (double 5))))))

  (testing "self-recursive function"
    (is (= 120 (letfn [(factorial [n]
                          (if (<= n 1) 1 (* n (factorial (dec n)))))]
                 (factorial 5)))))

  (testing "multi-arity local function"
    (is (= 6 (letfn [(add
                        ([x] x)
                        ([x y] (+ x y))
                        ([x y z] (+ x y z)))]
               (add 1 2 3))))))

;; --- defonce ---

(defonce compat-defonce-val 42)
(defonce compat-defonce-val 99)

(deftest defonce-basic
  (testing "first defonce binds the value"
    (is (= 42 compat-defonce-val)))

  (testing "second defonce does not overwrite"
    (is (= 42 compat-defonce-val))))

;; --- set! no-op ---

(deftest set-bang-noop
  (testing "set! is a no-op that returns nil"
    (is (nil? (set! *warn-on-reflection* true)))))

;; --- random-uuid ---

(deftest random-uuid-basic
  (testing "returns a 36-character string"
    (is (= 36 (count (random-uuid)))))

  (testing "version 4 nibble"
    (is (= "4" (subs (random-uuid) 14 15))))

  (testing "unique on each call"
    (is (not= (random-uuid) (random-uuid)))))

;; --- Platform-specific forms throw ---

(deftest jvm-forms-throw-with-clear-message
  (testing "defrecord throws because mino has no JVM classes"
    (is (thrown? (defrecord Foo [x y])))
    (is (= :defrecord
           (:mino/unsupported
            (try (defrecord Foo [x y]) nil
                 (catch e (ex-data e)))))))

  (testing "deftype throws"
    (is (thrown? (deftype Bar [x]))))

  (testing "reify throws"
    (is (thrown? (reify Object (toString [this] "hi")))))

  (testing "import throws as a top-level form"
    (is (thrown? (import java.util.Date))))

  (testing "instance? throws"
    (is (thrown? (instance? String "abc")))))

;; --- #?@ splice in maps ---

(deftest reader-cond-splice-in-map
  (testing "unmatched splice is silently skipped"
    (is (= {:a 1} {:a 1 #?@(:cljs [:b 2])})))

  (testing "matched :mino splice adds pairs"
    (is (= {:a 1 :b 2 :c 3} {:a 1 #?@(:mino [:b 2 :c 3])})))

  (testing "matched :default splice"
    (is (= {:x 10 :y 20} {:x 10 #?@(:default [:y 20])})))

  (testing "multiple splices"
    (is (= {:a 1 :b 2} {#?@(:mino [:a 1]) #?@(:mino [:b 2])}))))

;; --- &env in macros ---

(defmacro env-nil? [] (list 'quote (nil? &env)))
(defmacro env-contains? [sym] (list 'quote (and &env (contains? &env sym))))

(deftest ampersand-env-in-macros
  (testing "&env is nil in macro bodies"
    (is (true? (env-nil?))))

  (testing "contains? on nil &env returns falsy"
    (is (not (env-contains? x)))))

;; --- Pure-Clojure surface ---

(deftest ident-predicates
  (testing "ident? covers symbols and keywords"
    (is (true? (ident? 'foo)))
    (is (true? (ident? :foo)))
    (is (false? (ident? "foo")))
    (is (false? (ident? 42))))

  (testing "simple-ident? excludes namespaced forms"
    (is (true? (simple-ident? 'foo)))
    (is (true? (simple-ident? :foo)))
    (is (false? (simple-ident? 'a/b)))
    (is (false? (simple-ident? :a/b))))

  (testing "qualified-ident? selects only namespaced forms"
    (is (true? (qualified-ident? 'a/b)))
    (is (true? (qualified-ident? :a/b)))
    (is (false? (qualified-ident? 'foo)))
    (is (false? (qualified-ident? :foo)))))

(deftest special-symbol-membership
  (testing "common specials"
    (is (true? (special-symbol? 'if)))
    (is (true? (special-symbol? 'fn)))
    (is (true? (special-symbol? 'quote)))
    (is (true? (special-symbol? 'recur))))

  (testing "non-specials"
    (is (false? (special-symbol? 'inc)))
    (is (false? (special-symbol? :if)))
    (is (false? (special-symbol? "if")))))

(deftest map-entry-predicate
  (testing "two-vector is a map entry"
    (is (true? (map-entry? [:a 1])))
    (is (true? (map-entry? ["k" "v"]))))

  (testing "non-entries"
    (is (false? (map-entry? [1])))
    (is (false? (map-entry? [1 2 3])))
    (is (false? (map-entry? '(1 2))))
    (is (false? (map-entry? {:a 1})))))

(deftest unsupported-predicates-return-false
  (testing "bytes?, inst?, uri? all return false on mino"
    (is (false? (bytes? "anything")))
    (is (false? (bytes? [1 2 3])))
    (is (false? (inst? :now)))
    (is (false? (uri? "https://example.com")))))

(deftest uuid-predicate
  (testing "valid uuid strings"
    (is (true? (uuid? "550e8400-e29b-41d4-a716-446655440000")))
    (is (true? (uuid? "00000000-0000-0000-0000-000000000000"))))

  (testing "invalid uuid strings"
    (is (false? (uuid? "550e8400-e29b-41d4-a716-44665544000")))
    (is (false? (uuid? "not-a-uuid")))
    (is (false? (uuid? "")))
    (is (false? (uuid? nil)))
    (is (false? (uuid? 42)))))

(deftest find-keyword-equivalence
  (testing "find-keyword returns interned keyword for strings"
    (is (= :a (find-keyword "a")))
    (is (= :ns/a (find-keyword "ns" "a"))))

  (testing "find-keyword returns nil for non-strings"
    (is (nil? (find-keyword 42)))
    (is (nil? (find-keyword nil)))))

(deftest parse-boolean-cases
  (testing "valid booleans"
    (is (true?  (parse-boolean "true")))
    (is (false? (parse-boolean "false"))))

  (testing "case-sensitive — 'True' is not parsed"
    (is (nil? (parse-boolean "True")))
    (is (nil? (parse-boolean "FALSE"))))

  (testing "non-boolean inputs"
    (is (nil? (parse-boolean "")))
    (is (nil? (parse-boolean "yes")))
    (is (nil? (parse-boolean nil)))
    (is (nil? (parse-boolean 42)))))

(deftest parse-uuid-canonicalization
  (testing "canonical lowercase round-trip"
    (is (= "550e8400-e29b-41d4-a716-446655440000"
           (parse-uuid "550E8400-E29B-41D4-A716-446655440000")))
    (is (= "550e8400-e29b-41d4-a716-446655440000"
           (parse-uuid "550e8400-e29b-41d4-a716-446655440000"))))

  (testing "rejected inputs"
    (is (nil? (parse-uuid "junk")))
    (is (nil? (parse-uuid nil)))))

(deftest partitionv-returns-vectors
  (testing "exact partitions"
    (is (= [[1 2] [3 4]] (vec (partitionv 2 [1 2 3 4]))))
    (is (every? vector? (partitionv 2 [1 2 3 4]))))

  (testing "step form"
    (is (= [[1 2] [2 3] [3 4]] (vec (partitionv 2 1 [1 2 3 4])))))

  (testing "step + pad"
    (is (= [[1 2] [3 4] [5 :p]] (vec (partitionv 2 2 [:p :p] [1 2 3 4 5]))))))

(deftest partitionv-all-returns-vectors
  (testing "all-partition keeps trailing remainder as a vector"
    (is (= [[1 2] [3 4] [5]] (vec (partitionv-all 2 [1 2 3 4 5]))))
    (is (every? vector? (partitionv-all 2 [1 2 3 4 5])))))

(deftest splitv-at-tuple
  (testing "basic split returns two vectors"
    (is (= [[1 2] [3 4]] (splitv-at 2 [1 2 3 4]))))

  (testing "n exceeding length yields empty second element"
    (is (= [[1 2 3] []] (splitv-at 5 [1 2 3]))))

  (testing "n=0 yields empty first element"
    (is (= [[] [1 2 3]] (splitv-at 0 [1 2 3])))))

(deftest replicate-deprecated-alias
  (testing "n copies"
    (is (= [:x :x :x] (vec (replicate 3 :x))))
    (is (= [] (vec (replicate 0 :x))))))

(deftest hash-collection-helpers
  (testing "hash-ordered-coll is order-sensitive"
    (is (not= (hash-ordered-coll [1 2 3])
              (hash-ordered-coll [3 2 1]))))

  (testing "hash-unordered-coll is order-insensitive"
    (is (= (hash-unordered-coll #{1 2 3})
           (hash-unordered-coll #{3 2 1}))))

  (testing "mix-collection-hash returns a number"
    (is (number? (mix-collection-hash 12345 3)))))

(deftest ex-cause-from-data-or-meta
  (testing "ex-data :cause"
    (is (= :down (ex-cause (ex-info "boom" {:cause :down})))))

  (testing "no cause yields nil"
    (is (nil? (ex-cause (ex-info "boom" {})))))

  (testing "non-exception input is nil-safe"
    (is (nil? (ex-cause nil)))))

(deftest inst-ms-throws
  (testing "inst-ms is unsupported"
    (is (thrown? (inst-ms 0)))
    (let [d (try (inst-ms 0) nil
                 (catch e (ex-data e)))]
      (is (= :inst-ms (:mino/unsupported d))))))

(deftest tap-mechanism
  (testing "add-tap and tap> deliver values"
    (let [received (atom [])
          f        (fn [v] (swap! received conj v))]
      (add-tap f)
      (tap> :hi)
      (tap> :ho)
      (remove-tap f)
      (is (= [:hi :ho] @received))))

  (testing "remove-tap stops further deliveries"
    (let [received (atom [])
          f        (fn [v] (swap! received conj v))]
      (add-tap f)
      (tap> :first)
      (remove-tap f)
      (tap> :second)
      (is (= [:first] @received))))

  (testing "tap> returns true and survives misbehaving taps"
    (let [received (atom 0)
          ok       (fn [_] (swap! received inc))
          bad      (fn [_] (throw (ex-info "boom" {})))]
      (add-tap ok)
      (add-tap bad)
      (is (true? (tap> :v)))
      (remove-tap ok)
      (remove-tap bad)
      (is (= 1 @received)))))

(deftest tagged-literal-and-reader-conditional-records
  (testing "tagged-literal stores tag and form, predicate detects"
    (let [t (tagged-literal :foo 42)]
      (is (true? (tagged-literal? t)))
      (is (= :foo (:tag t)))
      (is (= 42 (:form t)))
      (is (false? (tagged-literal? {:tag :foo :form 42})))))

  (testing "reader-conditional stores form and splicing? flag"
    (let [r (reader-conditional [:a 1] true)]
      (is (true? (reader-conditional? r)))
      (is (= [:a 1] (:form r)))
      (is (true?    (:splicing? r))))))

(deftest with-redefs-fn-restores-roots
  (testing "rebinds during thunk and restores after"
    (def my-fn-w-redefs-fn_ (fn [] :original))
    (let [observed (atom nil)]
      (with-redefs-fn {#'my-fn-w-redefs-fn_ (fn [] :redef)}
                      (fn [] (reset! observed (my-fn-w-redefs-fn_))))
      (is (= :redef    @observed))
      (is (= :original (my-fn-w-redefs-fn_))))))

(def ^:dynamic *bf-x* 1)

(deftest get-thread-bindings-is-nil-without-binding
  (is (nil? (get-thread-bindings))))

(deftest get-thread-bindings-snapshots-active-frame
  (binding [*bf-x* 42]
    (let [m (get-thread-bindings)]
      (is (= 42 (get m '*bf-x*))))))

(deftest bound-fn-star-replays-captured-bindings
  (binding [*bf-x* 5]
    (let [g (bound-fn* (fn [] *bf-x*))]
      (is (= 5 (g)))
      (binding [*bf-x* 99]
        (is (= 5 (g)))))))

(deftest bound-fn-macro-captures-binding-context
  (binding [*bf-x* 7]
    (let [g (bound-fn [] *bf-x*)]
      (is (= 7 (g)))
      (binding [*bf-x* 99]
        (is (= 7 (g)))))))

(deftest read-default-mirrors-read-string
  (is (= '(+ 1 2) (read "(+ 1 2)")))
  (is (= 42       (read "42"))))

(deftest read-cond-allow-evaluates-conditional
  (is (= 1 (read "#?(:clj 1 :cljs 2)")))
  (is (= 1 (read {:read-cond :allow} "#?(:clj 1 :cljs 2)"))))

(deftest read-cond-preserve-builds-record
  (let [r (read {:read-cond :preserve} "#?(:clj 1 :cljs 2)")]
    (is (reader-conditional? r))
    (is (= '(:clj 1 :cljs 2) (:form r)))
    (is (false? (:splicing? r)))))

(deftest read-cond-preserve-vector-element
  (let [v (read {:read-cond :preserve} "[#?(:clj 1) :tail]")
        r (first v)]
    (is (reader-conditional? r))
    (is (= :tail (second v)))))

(deftest read-cond-disallow-throws
  (is (thrown? (read {:read-cond :disallow} "#?(:clj 1)"))))

(deftest edn-read-preserves-conditionals
  (require 'clojure.edn)
  (let [r (clojure.edn/read "#?(:clj 1 :mino 2)")]
    (is (reader-conditional? r))
    (is (= '(:clj 1 :mino 2) (:form r)))))

(defn destructure-eval_ [pairs body]
  ;; Build a let form whose binding vector is the destructured pairs and
  ;; whose body is the supplied form. eval'd to verify the bindings work.
  (eval (list 'let (destructure pairs) body)))

(deftest destructure-symbol-passthrough
  (let [bs (destructure '[a 1])]
    (is (= '[a 1] bs))
    (is (= 1 (destructure-eval_ '[a 1] 'a)))))

(deftest destructure-vector-positional
  (is (= [1 2 3] (destructure-eval_ '[[a b c] [1 2 3]]
                                    '[a b c]))))

(deftest destructure-vector-rest-and-as
  (is (= [1 '(2 3) [1 2 3]]
         (destructure-eval_ '[[a & rest :as v] [1 2 3]]
                            '[a rest v]))))

(deftest destructure-map-keys-and-as
  (is (= [1 2 {:x 1 :y 2}]
         (destructure-eval_ '[{:keys [x y] :as m} {:x 1 :y 2}]
                            '[x y m]))))

(deftest destructure-map-or-defaults
  (is (= [1 99]
         (destructure-eval_ '[{:keys [x y] :or {y 99}} {:x 1}]
                            '[x y]))))
