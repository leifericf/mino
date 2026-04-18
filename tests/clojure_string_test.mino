(require "tests/test")
(require '[clojure.string :as str])

;; --- blank? ---

(deftest str-blank-empty
  (is (true? (str/blank? "")))
  (is (true? (str/blank? nil)))
  (is (true? (str/blank? "  ")))
  (is (true? (str/blank? " \t "))))

(deftest str-blank-non-blank
  (is (false? (str/blank? "hello")))
  (is (false? (str/blank? " x ")))
  (is (false? (str/blank? "nil"))))

(deftest str-blank-throws-non-string
  (is (thrown? (str/blank? 1)))
  (is (thrown? (str/blank? :a)))
  (is (thrown? (str/blank? 'a))))

;; --- capitalize ---

(deftest str-capitalize-basic
  (is (= "Hello world" (str/capitalize "hello WORLD")))
  (is (= "A thing" (str/capitalize "a Thing")))
  (is (= "A thing" (str/capitalize "A THING")))
  (is (= "" (str/capitalize "")))
  (is (= "A" (str/capitalize "a"))))

(deftest str-capitalize-coerces
  (is (= "" (str/capitalize nil)))
  (is (= "1" (str/capitalize 1))))

;; --- starts-with? ---

(deftest str-starts-with-basic
  (is (true? (str/starts-with? "hello" "hel")))
  (is (true? (str/starts-with? "hello" "")))
  (is (false? (str/starts-with? "hello" "world"))))

(deftest str-starts-with-coerces
  (is (false? (str/starts-with? nil "x")))
  (is (false? (str/starts-with? 1 "x"))))

;; --- ends-with? ---

(deftest str-ends-with-basic
  (is (true? (str/ends-with? "hello" "llo")))
  (is (true? (str/ends-with? "hello" "")))
  (is (false? (str/ends-with? "hello" "world"))))

(deftest str-ends-with-coerces
  (is (false? (str/ends-with? nil "x")))
  (is (false? (str/ends-with? 1 "x"))))

;; --- escape ---

(deftest str-escape-basic
  (is (= "" (str/escape "" {})))
  (is (= "A_Abc" (str/escape "abc" {\a "A_A"})))
  (is (= "A_AbC_C" (str/escape "abc" {\a "A_A" \c "C_C"}))))

(deftest str-escape-throws
  (is (thrown? (str/escape nil {\a "A"})))
  (is (thrown? (str/escape 1 {\a "A"}))))

;; --- lower-case ---

(deftest str-lower-case-basic
  (is (= "hello" (str/lower-case "HELLO")))
  (is (= "" (str/lower-case ""))))

(deftest str-lower-case-coerces
  (is (= "" (str/lower-case nil)))
  (is (= "1" (str/lower-case 1))))

;; --- upper-case ---

(deftest str-upper-case-basic
  (is (= "HELLO" (str/upper-case "hello")))
  (is (= "" (str/upper-case ""))))

(deftest str-upper-case-coerces
  (is (= "" (str/upper-case nil)))
  (is (= "1" (str/upper-case 1))))

;; --- reverse ---

(deftest str-reverse-basic
  (is (= "olleh" (str/reverse "hello")))
  (is (= "" (str/reverse "")))
  (is (= "a" (str/reverse "a")))
  (is (= "tset-a" (str/reverse "a-test"))))

(deftest str-reverse-throws
  (is (thrown? (str/reverse nil)))
  (is (thrown? (str/reverse 1)))
  (is (thrown? (str/reverse :a))))
