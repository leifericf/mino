(require "tests/test")

;; Namespace and qualified symbol tests

(deftest namespace-qualified-symbol
  (is (= "foo" (namespace 'foo/bar))))

(deftest namespace-unqualified-symbol
  (is (nil? (namespace 'plain))))

(deftest name-qualified-symbol
  (is (= "bar" (name 'foo/bar))))

(deftest name-unqualified-symbol
  (is (= "plain" (name 'plain))))

(deftest namespace-qualified-keyword
  (is (= "ns" (namespace :ns/kw))))

(deftest name-qualified-keyword
  (is (= "kw" (name :ns/kw))))

(deftest name-unqualified-keyword
  (is (= "kw" (name :kw))))

(deftest qualified-symbol?-test
  (is (qualified-symbol? 'foo/bar))
  (is (not (qualified-symbol? 'foo)))
  (is (not (qualified-symbol? :foo/bar)))
  (is (not (qualified-symbol? 42))))

(deftest simple-symbol?-test
  (is (simple-symbol? 'foo))
  (is (not (simple-symbol? 'foo/bar)))
  (is (not (simple-symbol? :foo))))

(deftest qualified-keyword?-test
  (is (qualified-keyword? :foo/bar))
  (is (not (qualified-keyword? :foo)))
  (is (not (qualified-keyword? 'foo/bar))))

(deftest simple-keyword?-test
  (is (simple-keyword? :foo))
  (is (not (simple-keyword? :foo/bar)))
  (is (not (simple-keyword? 'foo))))

(deftest symbol-two-arity
  (is (= 'abc/def (symbol "abc" "def")))
  (is (= 'abc (symbol nil "abc")))
  (is (nil? (namespace (symbol nil "abc")))))
