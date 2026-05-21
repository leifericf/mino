;; Tests for defprotocol, extend-type, extend-protocol, satisfies?.

;; --- defprotocol + extend-type ---

(defprotocol Greetable
  (greet [this])
  (farewell [this other]))

(extend-type :string
  Greetable
  (greet [this] (str "Hello, " this "!"))
  (farewell [this other] (str "Goodbye, " other " from " this)))

(extend-type :int
  Greetable
  (greet [this] (str "Number " this))
  (farewell [this other] (str this " says bye to " other)))

(deftest protocol-basic
  (testing "string dispatch"
    (is (= "Hello, world!" (greet "world")))
    (is (= "Goodbye, Bob from Alice" (farewell "Alice" "Bob"))))
  (testing "integer dispatch"
    (is (= "Number 42" (greet 42)))
    (is (= "42 says bye to you" (farewell 42 "you")))))

;; --- extend-protocol ---

(defprotocol Measurable
  (size [this]))

(extend-protocol Measurable
  :string
  (size [this] (count this))
  :vector
  (size [this] (count this))
  :map
  (size [this] (count this)))

(deftest extend-protocol-test
  (testing "multiple types via extend-protocol"
    (is (= 5 (size "hello")))
    (is (= 3 (size [1 2 3])))
    (is (= 2 (size {:a 1 :b 2})))))

;; --- satisfies? ---

(deftest satisfies-test
  (testing "satisfies? returns true for extended types"
    (is (satisfies? Greetable "x"))
    (is (satisfies? Greetable 42)))
  (testing "satisfies? returns false for non-extended types"
    (is (not (satisfies? Greetable :keyword)))
    (is (not (satisfies? Greetable [1 2])))))

;; --- default fallback ---

(defprotocol Showable
  (show [this]))

(extend-type :default
  Showable
  (show [this] (str "default:" (pr-str this))))

(extend-type :string
  Showable
  (show [this] (str "string:" this)))

(deftest default-dispatch
  (testing "specific type takes precedence"
    (is (= "string:hi" (show "hi"))))
  (testing "default handles other types"
    (is (= "default:42" (show 42)))
    (is (= "default:[1 2]" (show [1 2])))))

;; --- missing implementation error ---

(defprotocol Closeable
  (close-it [this]))

(deftest missing-impl-error
  (testing "throws on missing implementation"
    (try
      (close-it "no-impl")
      (is false "should have thrown")
      (catch e
        (is (= :string (:type (ex-data e))))
        (is (= "close-it" (:method (ex-data e))))))))

;; --- protocol map structure ---

(deftest protocol-map-structure
  (testing "protocol value has name and methods"
    (is (= "Greetable" (:name Greetable)))
    (is (map? (:methods Greetable)))
    (is (contains? (:methods Greetable) :greet))
    (is (contains? (:methods Greetable) :farewell))))

;; --- cross-namespace extension via the protocol's full ns prefix ---

(ns proto-test-source-ns)
(defprotocol Frobbable (frob [x]))

(ns proto-test-extender-ns)
(require '[proto-test-source-ns :as src])
(extend-type :string src/Frobbable
  (frob [x] (str "frob:" x)))

(ns user)

(deftest cross-namespace-extend-type
  (testing "extend-type preserves the protocol's namespace prefix"
    (require '[proto-test-source-ns :as src])
    (is (= "frob:hi" (src/frob "hi")))))

(defprotocol BadShapeProto (bad-shape-m [_]))
(defrecord BadShapeR [])

(deftest extend-type-rejects-method-without-name
  ;; In portable Clojure code, IFn-style "shortcut" specs sometimes
  ;; appear: `(extend-type T IFn ([this kw] body))` -- a method with
  ;; the params vector in place of the method name. This is JVM-only
  ;; sugar (Clojure compiles it through clojure.lang.IFn directly).
  ;; mino has no IFn so the form is malformed; the macro must reject
  ;; it with a clear message that names the offender rather than
  ;; letting the failure cascade into the opaque
  ;; "name: expected a keyword, symbol, or string" from the macro
  ;; internals.
  (let [err (try
              (eval '(extend-type BadShapeR BadShapeProto ([this] :x)))
              nil
              (catch e (if (map? e) (:mino/message e) (str e))))]
    (is (some? err))
    (is (some? (re-find #"extend-type" err)))
    (is (some? (re-find #"method" err)))))

(defprotocol MissingMarkerProto (mmp-fn [_]))

(deftest extend-protocol-rejects-missing-type-marker
  ;; A reader-conditional like #?(:clj Object :cljs default) with no
  ;; :default branch produces nothing on mino, leaving the macro with
  ;; just method specs and no type marker:
  ;;   (extend-protocol P (foo [_x] :a) (bar [this] this))
  ;; mino used to silently accept this, treating the first method spec
  ;; as the type marker, and the failure surfaced far downstream as
  ;; "unbound symbol: this" inside the method body. The macro must
  ;; reject the form up front with a message that names the missing
  ;; type marker so callers can find the offending spec.
  (let [err (try
              (eval '(extend-protocol MissingMarkerProto
                       (mmp-fn [_x] :nope)))
              nil
              (catch e (if (map? e) (:mino/message e) (str e))))]
    (is (some? err))
    (is (some? (re-find #"extend-protocol" err)))
    (is (some? (re-find #"type marker" err)))))
