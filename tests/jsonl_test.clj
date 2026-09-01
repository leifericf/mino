(require "tests/test")

;; JSON Lines codec: lazy line-seq reader and line emitter.
;;
;; (mino.jsonl/read-lines s-or-reader) -> lazy seq of Clojure values
;; (mino.jsonl/write-lines coll) -> string with one JSON doc per line

(require '[mino.jsonl :as jsonl])

;;;; read-lines from string: basic cases

(deftest jsonl-read-lines-from-string-single-value
  (is (= [42] (vec (jsonl/read-lines "42\n")))))

(deftest jsonl-read-lines-from-string-multiple-values
  (is (= [1 2 3] (vec (jsonl/read-lines "1\n2\n3\n")))))

(deftest jsonl-read-lines-empty-string
  (is (= [] (vec (jsonl/read-lines "")))))

(deftest jsonl-read-lines-blank-lines-skipped
  (is (= [1 2] (vec (jsonl/read-lines "1\n\n2\n")))))

(deftest jsonl-read-lines-trailing-newline-not-extra
  ;; A trailing newline produces no extra element.
  (is (= [1 2 3] (vec (jsonl/read-lines "1\n2\n3\n")))))

(deftest jsonl-read-lines-no-trailing-newline
  ;; Input without trailing newline still parses.
  (is (= [1 2 3] (vec (jsonl/read-lines "1\n2\n3")))))

(deftest jsonl-read-lines-objects
  (is (= [{"a" 1} {"b" 2}]
         (vec (jsonl/read-lines "{\"a\":1}\n{\"b\":2}\n")))))

(deftest jsonl-read-lines-arrays
  (is (= [[1 2] [3 4]] (vec (jsonl/read-lines "[1,2]\n[3,4]\n")))))

(deftest jsonl-read-lines-mixed-types
  (is (= [1 "two" nil true [1] {"a" 1}]
         (vec (jsonl/read-lines "1\n\"two\"\nnull\ntrue\n[1]\n{\"a\":1}\n")))))

;;;; read-lines: blank-line and whitespace-only policy

(deftest jsonl-read-lines-only-blank-lines
  (is (= [] (vec (jsonl/read-lines "\n\n\n")))))

(deftest jsonl-read-lines-spaces-only-line-skipped
  ;; A line with only spaces is blank and skips.
  (is (= [1] (vec (jsonl/read-lines "1\n   \n")))))

;;;; read-lines: key-fn option

(deftest jsonl-read-lines-key-fn-keyword
  (is (= [{:a 1} {:b 2}]
         (vec (jsonl/read-lines "{\"a\":1}\n{\"b\":2}\n" :key-fn keyword)))))

;;;; write-lines: basic cases

(deftest jsonl-write-lines-empty-seq
  (is (= "" (jsonl/write-lines []))))

(deftest jsonl-write-lines-single-value
  (is (= "42\n" (jsonl/write-lines [42]))))

(deftest jsonl-write-lines-multiple-values
  (is (= "1\n2\n3\n" (jsonl/write-lines [1 2 3]))))

(deftest jsonl-write-lines-objects
  (let [result (jsonl/write-lines [{"a" 1} {"b" 2}])]
    ;; Each line is valid JSON followed by newline
    (is (clojure.string/ends-with? result "\n"))
    (is (= 2 (count (filter (complement clojure.string/blank?)
                             (clojure.string/split-lines result)))))))

(deftest jsonl-write-lines-always-trailing-newline
  ;; Even a single element ends with \n.
  (is (clojure.string/ends-with? (jsonl/write-lines [1]) "\n")))

;;;; Round-trip: write then read back

(deftest jsonl-round-trip-scalars
  (let [data [1 2.5 "hello" true false nil]]
    (is (= data (vec (jsonl/read-lines (jsonl/write-lines data)))))))

(deftest jsonl-round-trip-objects
  (let [data [{"key" "value" "n" 42} {"a" [1 2 3]}]]
    (is (= data (vec (jsonl/read-lines (jsonl/write-lines data)))))))

(deftest jsonl-round-trip-empty-collections
  (let [data [{} []]]
    (is (= data (vec (jsonl/read-lines (jsonl/write-lines data)))))))

(run-tests-and-exit)
