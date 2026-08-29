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

(deftest read-error-is-classified-json-parse
  ;; Parse failures classify as :json/parse and carry a 1-based
  ;; :location {:line :col} in ex-data, matching the toml/yaml readers.
  (is (= :json/parse
         (try (json/read-str "hello") (catch e (:mino/kind e)))))
  ;; classed catch dispatches on the promoted kind
  (is (= :caught
         (try (json/read-str "[1,2,]") (catch :json/parse _ :caught))))
  (let [loc (try (json/read-str "{'a':1}")
                 (catch e (:location (ex-data e))))]
    (is (= 1 (:line loc)))
    (is (= 2 (:col loc))))
  ;; multi-line input reports the failing line
  (let [loc (try (json/read-str "[\n  1,\n  bad\n]")
                 (catch e (:location (ex-data e))))]
    (is (= 3 (:line loc)))))

;;;; read-str option policy

(deftest read-str-key-fn-supported
  ;; :key-fn is the one supported reader option: a per-key transform
  ;; passed straight to the json-parse prim.
  (is (= {:a 1} (json/read-str "{\"a\":1}" :key-fn keyword))))

(deftest read-str-rejects-unknown-option
  ;; Unknown reader options throw rather than silently ignoring them,
  ;; mirroring write-str's works-or-throws policy.
  (is (thrown? (json/read-str "1" :bogus 2)))
  (is (= :json/opts
         (try (json/read-str "1" :bogus 2) (catch e (:mino/kind e))))))

(deftest read-str-rejects-value-fn
  ;; :value-fn has no native support; reject it with a clear diagnostic
  ;; rather than silently dropping the requested transform.
  (is (thrown? (json/read-str "1" :value-fn (fn [_ v] v))))
  (is (= :json/opts
         (try (json/read-str "1" :value-fn (fn [_ v] v))
              (catch e (:mino/kind e))))))

;;;; JSON writer

(deftest write-nil
  (is (= "null" (json/write-str nil))))

(deftest write-true
  (is (= "true" (json/write-str true))))

(deftest write-false
  (is (= "false" (json/write-str false))))

(deftest write-integer
  (is (= "42" (json/write-str 42))))

(deftest write-negative-integer
  (is (= "-7" (json/write-str -7))))

(deftest write-zero
  (is (= "0" (json/write-str 0))))

(deftest write-float
  (is (= "3.14" (json/write-str 3.14))))

(deftest write-simple-string
  (is (= "\"hello\"" (json/write-str "hello"))))

(deftest write-empty-string
  (is (= "\"\"" (json/write-str ""))))

(deftest write-string-escapes
  (is (= "\"\\\"\"" (json/write-str "\"")))
  (is (= "\"\\\\\"" (json/write-str "\\")))
  (is (= "\"\\n\"" (json/write-str "\n")))
  (is (= "\"\\t\"" (json/write-str "\t")))
  (is (= "\"\\r\"" (json/write-str "\r")))
  (is (= "\"\\b\"" (json/write-str "\b")))
  (is (= "\"\\f\"" (json/write-str "\f"))))

(deftest write-ratio-throws
  ;; A ratio has no JSON representation; emitting "22/7" would
  ;; produce invalid JSON that no reader accepts.
  (is (thrown? (json/write-str 22/7)))
  (is (thrown? (json/write-str {"a" [1/3]}))))

(deftest write-non-finite-double-throws
  (is (thrown? (json/write-str ##Inf)))
  (is (thrown? (json/write-str ##NaN)))
  (is (thrown? (json/write-str [1 ##-Inf]))))

(deftest write-empty-array
  (is (= "[]" (json/write-str []))))

(deftest write-simple-array
  (is (= "[1,2,3]" (json/write-str [1 2 3]))))

(deftest write-mixed-array
  (is (= "[1,\"two\",true,null]" (json/write-str [1 "two" true nil]))))

(deftest write-nested-arrays
  (is (= "[[1,2],[3,4]]" (json/write-str [[1 2] [3 4]]))))

(deftest write-empty-object
  (is (= "{}" (json/write-str {}))))

(deftest write-simple-object
  (is (= "{\"a\":1,\"b\":2}" (json/write-str {"a" 1 "b" 2}))))

(deftest write-object-with-keyword-keys
  (is (= "{\"a\":1}" (json/write-str {:a 1}))))

(deftest write-nested-object
  (is (= "{\"outer\":{\"inner\":42}}" (json/write-str {"outer" {"inner" 42}}))))

(deftest write-object-with-array-value
  (is (= "{\"items\":[1,2,3]}" (json/write-str {"items" [1 2 3]}))))

;;;; Round-trip

(deftest round-trip-integer
  (is (= 42 (json/read-str (json/write-str 42)))))

(deftest round-trip-string
  (is (= "hello\nworld" (json/read-str (json/write-str "hello\nworld")))))

(deftest round-trip-vector
  (is (= [1 "two" true nil] (json/read-str (json/write-str [1 "two" true nil])))))

(deftest round-trip-object
  (is (= {"a" 1 "b" [2 3]} (json/read-str (json/write-str {"a" 1 "b" [2 3]})))))

(deftest round-trip-nested
  (let [data {"users" [{"name" "alice" "age" 30} {"name" "bob" "age" 25}]}]
    (is (= data (json/read-str (json/write-str data))))))

(deftest round-trip-unicode
  (is (= "\u00e9" (json/read-str (json/write-str "\u00e9")))))

(deftest write-str-rejects-unsupported-options
  ;; This port does not implement the writer options yet; rather than
  ;; silently ignoring them and returning output that differs from what
  ;; the caller asked for, write-str throws (works-or-throws).
  (is (= "{\"a\":1}" (json/write-str {:a 1})))
  (is (thrown? (json/write-str {:a 1} :key-fn name)))
  (is (thrown? (json/write-str {:a 1} :escape-unicode false))))

(run-tests-and-exit)
