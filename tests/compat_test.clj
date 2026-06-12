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
  ;; Mino is its own dialect (matches :mino, then :default), so any
  ;; non-:mino non-:default tag (:cljs, :clj, :jank, …) drives
  ;; elimination behavior in mino.
  (testing "eliminated key skips paired value"
    (is (= {:c 2} {#?(:cljs :b) 1 :c 2})))

  (testing "both branches eliminated"
    (is (= {:ok 42} {#?(:cljs :y) #?(:cljs 2) :ok 42})))

  (testing "empty map after elimination"
    (is (= {} {#?(:cljs :b) 1})))

  (testing "mino branch matches"
    (is (= {:a 1} {#?(:mino :a :clj :b) 1})))

  (testing "clj branch is skipped on mino"
    (is (= {} {#?(:clj :a :cljs :b) 1}))))

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
  (testing "basic regex literal produces a regex value"
    (is (regex? #"hello"))
    (is (= :regex (type #"hello"))))

  (testing "regex literal works with re-find"
    (is (= "123" (re-find #"[0-9]+" "abc123def"))))

  (testing "regex literal in a vector"
    ;; The literals construct distinct regex values; identity equality
    ;; on MINO_REGEX makes (= #\"a\" #\"a\") false, mirroring Clojure
    ;; JVM's Pattern.equals.
    (let [v [#"a" #"b"]]
      (is (regex? (nth v 0)))
      (is (regex? (nth v 1)))))

  (testing "regex literal in a map"
    (let [m {:pat #"foo"}]
      (is (regex? (:pat m)))))

  (testing "regex literal with quantifiers"
    (is (= "hell" (re-find #"hel+" "hello"))))

  (testing "two distinct #\"x\" literals are not = (identity equality)"
    (is (not (= #"x" #"x")))
    (let [r #"x"]
      (is (= r r)))))

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
               (add 1 2 3)))))

  (testing "mutual recursion"
    ;; Each fn's closure captures the same env, so forward references
    ;; resolve at call time.
    (is (= :odd (letfn [(my-even? [n] (if (zero? n) :even (my-odd?  (dec n))))
                        (my-odd?  [n] (if (zero? n) :odd  (my-even? (dec n))))]
                  (my-even? 7))))
    (is (= :even (letfn [(my-even? [n] (if (zero? n) :even (my-odd?  (dec n))))
                         (my-odd?  [n] (if (zero? n) :odd  (my-even? (dec n))))]
                   (my-even? 6))))))

;; --- defonce ---

(defonce compat-defonce-val 42)
(defonce compat-defonce-val 99)

(deftest defonce-basic
  (testing "first defonce binds the value"
    (is (= 42 compat-defonce-val)))

  (testing "second defonce does not overwrite"
    (is (= 42 compat-defonce-val))))

;; --- set! on dynamic vars ---
;;
;; mino used to make set! a no-op so JVM-Clojure code that pokes
;; *warn-on-reflection* etc. would load without erroring. That hid
;; the legitimate set!-on-bound-dynamic-var contract: real Clojure
;; mutates the thread-local binding frame, and code that needs to
;; bump a counter or stash a value inside binding relies on it.
;; Now set! mutates the topmost dyn frame when one exists, and
;; throws "Can't change/establish root binding" when no binding
;; frame is active -- matching JVM Clojure's runtime contract.

(deftest set-bang-mutates-dynamic-binding
  (testing "set! mutates the topmost binding frame"
    (def ^:dynamic *set-bang-test* 0)
    (binding [*set-bang-test* 0]
      (is (= 1 (set! *set-bang-test* 1)))
      (is (= 1 *set-bang-test*))
      (set! *set-bang-test* 99)
      (is (= 99 *set-bang-test*))))
  (testing "set! on dynamic var without binding form throws"
    (def ^:dynamic *set-bang-unbound* 0)
    (is (thrown? (set! *set-bang-unbound* 5))))
  (testing "set!-style increment inside binding"
    (def ^:dynamic *counter* 0)
    (binding [*counter* 0]
      (dotimes [_ 5] (set! *counter* (inc *counter*)))
      (is (= 5 *counter*)))))

;; --- random-uuid ---

(deftest random-uuid-basic
  (testing "returns a UUID value"
    (is (uuid? (random-uuid))))

  (testing "version 4 nibble (JVM-canon str form: bare 36-char layout)"
    ;; (str u) matches JVM UUID.toString: 8-4-4-4-12 hex. Position 14
    ;; is the version digit, always '4' for v4 UUIDs.
    (is (= "4" (subs (str (random-uuid)) 14 15))))

  (testing "unique on each call"
    (is (not= (random-uuid) (random-uuid)))))

;; --- Platform-specific forms throw ---

(deftest jvm-forms-throw-with-clear-message
  (testing "import throws as a top-level form"
    (is (thrown? (import java.util.Date)))))

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

(defmacro env-map? [] (list 'quote (map? &env)))
(defmacro env-contains? [sym] (list 'quote (contains? &env sym)))
(defmacro env-keys [] (list 'quote (sort (map name (keys &env)))))

(deftest ampersand-env-in-macros
  (testing "&env is a map in macro bodies"
    (is (true? (env-map?))))

  (testing "contains? sees lexical locals"
    (is (not (env-contains? x)))
    (let [x 1]
      (is (env-contains? x))))

  (testing "keys lists every lexical local in scope"
    (let [a 1 b 2 c 3
          ks (set (env-keys))]
      (is (contains? ks "a"))
      (is (contains? ks "b"))
      (is (contains? ks "c")))))

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
  (testing "uuid? recognises UUID values"
    ;; uuid? matches Clojure JVM: only true for actual java.util.UUID
    ;; values, never for the canonical string form.
    (is (true? (uuid? (parse-uuid "550e8400-e29b-41d4-a716-446655440000"))))
    (is (true? (uuid? #uuid "00000000-0000-0000-0000-000000000000")))
    (is (true? (uuid? (random-uuid)))))

  (testing "non-uuids"
    (is (false? (uuid? "550e8400-e29b-41d4-a716-446655440000")))
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

  (testing "non-matching strings return nil"
    (is (nil? (parse-boolean "")))
    (is (nil? (parse-boolean "yes"))))

  (testing "non-string inputs throw"
    ;; Per Clojure 1.11+, parse-boolean throws on non-string args
    ;; (NullPointerException / ClassCastException on the JVM); mino
    ;; mirrors that contract with an explicit ex-info.
    (is (thrown? (parse-boolean nil)))
    (is (thrown? (parse-boolean 42)))))

(deftest parse-uuid-canonicalization
  (testing "case-insensitive parse produces same UUID value"
    ;; parse-uuid returns a UUID value (mino has a real UUID type as
    ;; of v0.100.7); two case variants of the canonical hex form parse
    ;; to bytewise-equal UUIDs.
    (is (= (parse-uuid "550E8400-E29B-41D4-A716-446655440000")
           (parse-uuid "550e8400-e29b-41d4-a716-446655440000")))
    (is (uuid? (parse-uuid "550e8400-e29b-41d4-a716-446655440000")))
    (is (= #uuid "550e8400-e29b-41d4-a716-446655440000"
           (parse-uuid "550E8400-E29B-41D4-A716-446655440000"))))

  (testing "rejected inputs"
    (is (nil? (parse-uuid "junk")))
    ;; Non-string input throws (matches Clojure JVM, which dispatches
    ;; UUID.fromString and rejects nil with NPE / non-string with
    ;; ClassCastException).
    (is (thrown? (parse-uuid nil)))))

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

(deftest inst-ms-rejects-non-inst
  ;; inst-ms now works on real inst values (see inst_test.clj). Passing
  ;; a non-inst value (number / string / arbitrary map) still throws
  ;; with the value in :got data.
  (testing "inst-ms on a non-inst throws"
    (is (thrown? (inst-ms 0)))
    (let [d (try (inst-ms 0) nil
                 (catch e (ex-data e)))]
      (is (= 0 (:got d))))))

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

(deftest get-thread-bindings-omits-vars-without-active-binding
  (let [m (get-thread-bindings)]
    (is (not (contains? m '*bf-x*)))))

(deftest get-thread-bindings-snapshots-active-frame
  ;; JVM Clojure keys this map by vars; mino keys it by symbols. The
  ;; symbol is fully qualified so the entry names the exact var --
  ;; required for replay via with-bindings* and cross-thread
  ;; conveyance to install the binding on the same var regardless of
  ;; the namespace the replay runs in.
  (binding [*bf-x* 42]
    (let [m (get-thread-bindings)]
      (is (= 42 (get m 'user/*bf-x*))))))

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
  ;; mino is not a JVM dialect, so :clj branches don't fire; with no
  ;; :default key the reader returns nil for the conditional.
  (is (nil? (read "#?(:clj 1 :cljs 2)")))
  (is (nil? (read {:read-cond :allow} "#?(:clj 1 :cljs 2)")))
  (is (= 3 (read "#?(:mino 3 :clj 1 :cljs 2)")))
  (is (= 9 (read "#?(:clj 1 :default 9)"))))

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
