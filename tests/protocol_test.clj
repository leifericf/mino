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
