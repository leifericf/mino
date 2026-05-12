(require "tests/test")
(require '[clojure.string :as str])
(require '[clojure.string :refer [split join trim upper-case lower-case
                                  starts-with? ends-with? includes?]])
;; replace stays under str/ to avoid clobbering clojure.core/replace.

;; String operations and formatting.

(deftest str-fn
  (is (= "hello world" (str "hello" " " "world")))
  (is (= "n=42" (str "n=" 42)))
  (is (= ":hi 3.14" (str :hi " " 3.14)))
  (is (= "" (str)))
  (is (= "ab" (str "a" nil "b"))))

(deftest subs-fn
  (is (= "el" (subs "hello" 1 3)))
  (is (= "llo" (subs "hello" 2))))

(deftest count-string-codepoints
  ;; count of a string returns the codepoint count (Clojure semantics),
  ;; matching subs / nth / char-at which index in codepoints. For
  ;; ASCII the byte and codepoint counts coincide; for multi-byte
  ;; UTF-8 they diverge.
  (is (= 0 (count "")))
  (is (= 3 (count "abc")))
  ;; em-dash is 3 bytes / 1 codepoint
  (is (= 4 (count "ab—c")))
  (is (= 2 (count "你好")))
  ;; (subs s 0 (count s)) round-trips the whole string after the fix.
  (let [s "ab—c你好"]
    (is (= s (subs s 0 (count s))))))

(deftest split-fn
  (is (= ["a" "b" "c"] (split "a,b,c" ",")))
  (is (= ["a" "b" "c"] (split "abc" ""))))

(deftest join-fn
  (is (= "a-b-c" (join "-" ["a" "b" "c"])))
  (is (= "abc" (join ["a" "b" "c"])))
  (is (= "123" (join nil [1 2 3]))))

(deftest string-predicates
  (is (starts-with? "hello" "he"))
  (is (not (starts-with? "hello" "lo")))
  (is (ends-with? "hello" "lo"))
  (is (not (ends-with? "hello" "he")))
  (is (includes? "hello" "ell"))
  (is (not (includes? "hello" "xyz"))))

(deftest case-fns
  (is (= "HELLO" (upper-case "hello")))
  (is (= "hello" (lower-case "HELLO"))))

(deftest trim-fn
  (is (= "hi" (trim "  hi  ")))
  (is (= "hi" (trim "hi"))))

(deftest char-at-fn
  (is (= "h" (char-at "hello" 0)))
  (is (= "o" (char-at "hello" 4))))

(deftest format-fn
  (is (= "hello world" (format "hello %s" "world")))
  (is (= "n=42" (format "n=%d" 42)))
  (is (= "pi=3.140000" (format "pi=%f" 3.14)))
  (is (= "Bob has 3" (format "%s has %d" "Bob" 3)))
  (is (= "100%" (format "100%%")))
  (is (= "key: :hello" (format "key: %s" :hello))))

(deftest pr-str-fn
  (is (= "42" (pr-str 42)))
  (is (= "\"hi\"" (pr-str "hi")))
  (is (= "1 :a \"b\"" (pr-str 1 :a "b")))
  (is (= "nil" (pr-str nil)))
  (is (= "(1 2)" (pr-str '(1 2)))))

(deftest name-fn
  (is (= "hello" (name :hello)))
  (is (= "world" (name 'world)))
  (is (= "str" (name "str")))
  (is (thrown? (name nil))))

(deftest symbol-constructor
  (is (= 'hello (symbol "hello")))
  (is (symbol? (symbol "x")))
  (is (= "abc" (name (symbol "abc")))))

(deftest keyword-constructor
  (is (= :world (keyword "world")))
  (is (keyword? (keyword "foo")))
  (is (= "bar" (name (keyword "bar")))))

(deftest read-string-fn
  (is (= 42 (read-string "42")))
  (is (= '(+ 1 2) (read-string "(+ 1 2)")))
  (is (= :foo (read-string ":foo")))
  (is (= nil (read-string ""))))
