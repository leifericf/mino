(require "tests/test")
(require '[clojure.data.json :as json])

;;;; JSON reader

(deftest read-null
  (is (= nil (json/read-str "null"))))

(deftest read-true
  (is (= true (json/read-str "true"))))

(deftest read-false
  (is (= false (json/read-str "false"))))

;;;; Numbers

(deftest read-integer
  (is (= 42 (json/read-str "42"))))

(deftest read-negative-integer
  (is (= -7 (json/read-str "-7"))))

(deftest read-zero
  (is (= 0 (json/read-str "0"))))

(deftest read-float
  (is (= 3.14 (json/read-str "3.14"))))

(deftest read-negative-float
  (is (= -0.5 (json/read-str "-0.5"))))

(deftest read-exponent
  (is (= 100.0 (json/read-str "1e2"))))

(deftest read-capital-exponent
  (is (= 100.0 (json/read-str "1E2"))))

(deftest read-negative-exponent
  (is (= 0.01 (json/read-str "1e-2"))))

;;;; Strings

(deftest read-simple-string
  (is (= "hello" (json/read-str "\"hello\""))))

(deftest read-empty-string
  (is (= "" (json/read-str "\"\""))))

(deftest read-string-escapes
  (is (= "\"" (json/read-str "\"\\\"\"")))
  (is (= "\\" (json/read-str "\"\\\\\"")))
  (is (= "/" (json/read-str "\"\\/\"")))
  (is (= "\b" (json/read-str "\"\\b\"")))
  (is (= "\f" (json/read-str "\"\\f\"")))
  (is (= "\n" (json/read-str "\"\\n\"")))
  (is (= "\r" (json/read-str "\"\\r\"")))
  (is (= "\t" (json/read-str "\"\\t\""))))

(deftest read-unicode-escape
  (is (= "A" (json/read-str "\"\\u0041\"")))
  (is (= "\u00e9" (json/read-str "\"\\u00e9\""))))

(deftest read-string-with-spaces
  (is (= "hello world" (json/read-str "\"hello world\""))))

;;;; Arrays

(deftest read-empty-array
  (is (= [] (json/read-str "[]"))))

(deftest read-simple-array
  (is (= [1 2 3] (json/read-str "[1,2,3]"))))

(deftest read-mixed-array
  (is (= [1 "two" true nil] (json/read-str "[1,\"two\",true,null]"))))

(deftest read-nested-arrays
  (is (= [[1 2] [3 4]] (json/read-str "[[1,2],[3,4]]"))))

(deftest read-array-with-whitespace
  (is (= [1 2 3] (json/read-str "[ 1 , 2 , 3 ]"))))

;;;; Objects

(deftest read-empty-object
  (is (= {} (json/read-str "{}"))))

(deftest read-simple-object
  (is (= {"a" 1 "b" 2} (json/read-str "{\"a\":1,\"b\":2}"))))

(deftest read-object-with-string-values
  (is (= {"name" "mino"} (json/read-str "{\"name\":\"mino\"}"))))

(deftest read-nested-object
  (is (= {"outer" {"inner" 42}} (json/read-str "{\"outer\":{\"inner\":42}}"))))

(deftest read-object-with-array-value
  (is (= {"items" [1 2 3]} (json/read-str "{\"items\":[1,2,3]}"))))

(deftest read-array-of-objects
  (is (= [{"a" 1} {"b" 2}] (json/read-str "[{\"a\":1},{\"b\":2}]"))))

;;;; key-fn option

(deftest read-with-keyword-keys
  (is (= {:a 1 :b 2} (json/read-str "{\"a\":1,\"b\":2}" :key-fn keyword))))

(deftest read-default-keys-are-strings
  (let [result (json/read-str "{\"a\":1}")]
    (is (string? (first (keys result))))))

;;;; Whitespace

(deftest read-leading-whitespace
  (is (= 42 (json/read-str "  42"))))

(deftest read-trailing-whitespace
  (is (= 42 (json/read-str "42  "))))

(deftest read-newlines-and-tabs
  (is (= [1 2] (json/read-str "[\n\t1,\n\t2\n]"))))

;;;; Error cases

(deftest read-bare-string-throws
  (is (thrown? (json/read-str "hello"))))

(deftest read-trailing-comma-throws
  (is (thrown? (json/read-str "[1,2,]"))))

(deftest read-single-quotes-throws
  (is (thrown? (json/read-str "{'a':1}"))))

(run-tests-and-exit)
