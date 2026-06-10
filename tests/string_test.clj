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

(deftest format-wide-integer-no-stack-bleed
  ;; Width > 64 (the stack-buffer size in the integer branch) triggered an
  ;; snprintf-return-value oob_read: memcpy used the would-be length, not
  ;; the truncated length, reading past the 64-byte stack buffer.
  (let [expected (apply str (concat (repeat 79 \space) [\1]))]
    (is (= expected (format "%80d" 1)))))

(deftest format-wide-float-no-stack-bleed
  ;; Width > 128 (the float branch stack-buffer size) triggered the same
  ;; snprintf-return-value oob_read: memcpy used the would-be length,
  ;; reading past the 128-byte stack buffer.
  (let [expected (apply str (concat (repeat 192 \space) (seq "1.500000")))]
    (is (= expected (format "%200f" 1.5)))))

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

(deftest name-namespace-multi-segment-ns
  ;; When the 2-arg keyword constructor receives an ns containing a
  ;; slash, name/namespace split at the LAST slash so the round-trip
  ;; preserves the constructed parts.
  (let [k (keyword "a/b" "c")]
    (is (= "c"   (name k)))
    (is (= "a/b" (namespace k))))
  (let [k (keyword "my.namespace" "my.key")]
    (is (= "my.key"       (name k)))
    (is (= "my.namespace" (namespace k)))))

(deftest symbol-constructor
  (is (= 'hello (symbol "hello")))
  (is (symbol? (symbol "x")))
  (is (= "abc" (name (symbol "abc")))))

(deftest keyword-constructor
  (is (= :world (keyword "world")))
  (is (keyword? (keyword "foo")))
  (is (= "bar" (name (keyword "bar")))))

(deftest empty-string-namespace-is-preserved
  ;; Two-arg (keyword "" name) and (symbol "" name) construct a value
  ;; whose namespace is the empty string, not nil. This matches
  ;; JVM Clojure, where empty-string is a legal namespace and the
  ;; (str ...) form interleaves the bare separator.
  (let [k (keyword "" "hi")]
    (is (= ""    (namespace k)))
    (is (= "hi"  (name k)))
    (is (= ":/hi" (str k))))
  (let [s (symbol "" "hi")]
    (is (= ""    (namespace s)))
    (is (= "hi"  (name s)))
    (is (= "/hi" (str s))))
  ;; Single-arg constructors with no slash still produce ns=nil.
  (is (nil? (namespace (keyword "hi"))))
  (is (nil? (namespace (symbol "hi")))))

(deftest read-string-fn
  (is (= 42 (read-string "42")))
  (is (= '(+ 1 2) (read-string "(+ 1 2)")))
  (is (= :foo (read-string ":foo")))
  (is (= nil (read-string ""))))

;; Regression: split with a regex separator used to treat the regex
;; source as a literal substring, so `#"\s+"` never matched whitespace
;; in inputs like "x y" and split returned the whole input as a
;; single-element vector. v0.219.0 routes regex separators through
;; the actual regex engine.
(deftest split-with-regex
  (is (= ["a" "b" "c"]      (split "a    b    c"  #"\s+")))
  (is (= ["x" "y"]          (split "x y"          #"\s+")))
  (is (= ["" "ab" "cd"]     (split "  ab cd"      #"\s+")))
  (is (= ["a" "b" "c" "d"]  (split "a,b,c,d"      #",")))
  (is (= ["a" "b,c,d"]      (split "a,b,c,d"      #"," 2)))
  (is (= ["ab" "cd"]        (split "ab cd"        #" +"))))

(deftest split-empty-input
  ;; Regression: (str/split "" re) used to return [] (an empty
  ;; vector). Clojure / JVM String.split returns [""] (a single empty-
  ;; string element) for empty input regardless of the separator.
  ;; Downstream code that destructures [head & tail] on the result
  ;; relies on the [""] shape so head is "" rather than nil.
  (is (= [""] (split "" #",")))
  (is (= [""] (split "" #"\s+")))
  (is (= [""] (split "" ","))))

;; String literal escape repertoire

(deftest string-escapes-control
  (is (= [8] (mapv int "\b")))
  (is (= [12] (mapv int "\f")))
  (is (= [9 10 13] (mapv int "\t\n\r"))))

(deftest string-escapes-unicode
  (is (= "A" (read-string "\"\\u0041\"")))
  (is (= [233] (mapv int (read-string "\"\\u00e9\""))))
  (is (= [9731] (mapv int (read-string "\"\\u2603\""))))
  ;; A surrogate pair combines into one codepoint.
  (is (= [128512] (mapv int (read-string "\"\\ud83d\\ude00\""))))
  ;; Lone surrogates are not representable codepoints.
  (is (thrown? (read-string "\"\\ud800\"")))
  (is (thrown? (read-string "\"\\u00g1\"")))
  (is (thrown? (read-string "\"\\u12\""))))

(deftest string-escapes-octal
  (is (= [65] (mapv int (read-string "\"\\101\""))))
  (is (= [0] (mapv int (read-string "\"\\0\""))))
  (is (= [255] (mapv int (read-string "\"\\377\""))))
  ;; Octal escapes consume at most three digits.
  (is (= [83 52] (mapv int (read-string "\"\\1234\""))))
  (is (thrown? (read-string "\"\\400\""))))

(deftest string-escapes-unknown-rejected
  (is (thrown? (read-string "\"\\q\"")))
  (is (thrown? (read-string "\"\\8\""))))
