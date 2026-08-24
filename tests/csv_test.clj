(require "tests/test")
(require '[clojure.data.csv :as csv])

;; Golden vectors derived from the python3 csv module (the RFC 4180
;; reference oracle) run on this machine; expected values are real
;; oracle output, not recalled from docs.

(def ^:private tmpdir
  (or (getenv "TMPDIR") (getenv "TEMP") (getenv "TMP") "/tmp"))

;;;; Reader: records and fields

(deftest read-basic-row
  (is (= [["a" "b" "c"]] (csv/read-csv "a,b,c\n"))))

(deftest read-row-without-trailing-newline
  (is (= [["a" "b" "c"]] (csv/read-csv "a,b,c"))))

(deftest read-multiple-rows
  (is (= [["1" "2" "3"] ["4" "5" "6"]] (csv/read-csv "1,2,3\n4,5,6\n"))))

(deftest read-single-column
  (is (= [["x"]] (csv/read-csv "x\n"))))

(deftest read-empty-input-yields-no-rows
  (is (= () (csv/read-csv ""))))

(deftest read-empty-line-yields-empty-row
  (is (= [[]] (csv/read-csv "\n"))))

(deftest read-blank-line-between-rows
  (is (= [["a" "b"] [] ["c" "d"]] (csv/read-csv "a,b\n\nc,d\n"))))

(deftest read-empty-fields
  (is (= [["a" "" "c"]] (csv/read-csv "a,,c\n"))))

(deftest read-trailing-empty-field
  (is (= [["a" "b" ""]] (csv/read-csv "a,b,\n")))
  ;; oracle: a separator at end of input still closes an empty field
  (is (= [["a" "b" ""]] (csv/read-csv "a,b,"))))

(deftest read-quoted-empty-field
  (is (= [["" "x"]] (csv/read-csv "\"\",x\n"))))

(deftest read-preserves-unquoted-spaces
  (is (= [[" a " " b "]] (csv/read-csv " a , b \n"))))

(deftest read-unicode-fields
  (is (= [["café" "über"]] (csv/read-csv "café,über\n"))))

;;;; Reader: newline handling

(deftest read-crlf-endings
  (is (= [["a" "b"] ["c" "d"]] (csv/read-csv "a,b\r\nc,d\r\n"))))

(deftest read-lone-cr-endss-record
  (is (= [["a"] ["b"]] (csv/read-csv "a\rb\n"))))

;;;; Reader: RFC 4180 quoting

(deftest read-quoted-separator
  (is (= [["a,b" "c"]] (csv/read-csv "\"a,b\",c\n"))))

(deftest read-escaped-quote
  (is (= [["a\"b" "c"]] (csv/read-csv "\"a\"\"b\",c\n"))))

(deftest read-quoted-newline
  (is (= [["a\nb" "c"]] (csv/read-csv "\"a\nb\",c\n"))))

(deftest read-quoted-crlf-inside-field
  (is (= [["a\r\nb" "c"]] (csv/read-csv "\"a\r\nb\",c\n"))))

(deftest read-quoted-lone-cr-inside-field
  (is (= [["a\rb" "c"]] (csv/read-csv "\"a\rb\",c\n"))))

(deftest read-bare-quote-in-unquoted-field-is-data
  ;; oracle: csv.reader("ab\"cd,x") yields "ab\"cd" as an ordinary field
  (is (= [["ab\"cd" "x"]] (csv/read-csv "ab\"cd,x\n"))))

(deftest read-unterminated-quote-takes-remainder
  ;; oracle: csv.reader("a,\"b\n") yields the unterminated field
  ;; verbatim, newline included
  (is (= [["a" "b\n"]] (csv/read-csv "a,\"b\n"))))

(deftest read-chars-after-closing-quote-append
  ;; oracle: csv.reader("\"a\"x,c") yields "ax"
  (is (= [["ax" "c"]] (csv/read-csv "\"a\"x,c\n"))))

;;;; Reader: options

(deftest read-custom-separator
  (is (= [["a" "b,c"]] (csv/read-csv "a;b,c\n" {:separator \;}))))

(deftest read-tab-separator
  (is (= [["a" "b" "c"]] (csv/read-csv "a\tb\tc\n" {:separator \tab}))))

(deftest read-custom-quote-char
  (is (= [["a,b" "c"]] (csv/read-csv "'a,b',c\n" {:quote \'}))))

(deftest read-accepts-kv-options
  (is (= [["a" "b,c"]] (csv/read-csv "a;b,c\n" :separator \;))))

;;;; Reader: reader-atom input

(deftest read-from-cursor-atom
  (with-in-str "a,b\nc,d\n"
    (is (= [["a" "b"] ["c" "d"]] (doall (csv/read-csv *in*))))))

(deftest read-from-cursor-atom-consumes-incrementally
  (with-in-str "a,b\nc,d\n"
    (let [rows (csv/read-csv *in*)]
      (is (= ["a" "b"] (first rows)))
      (is (= "c,d\n" @*in*)))))

;;;; Writer

(deftest write-rows-to-writer
  (is (= "a,b\nc,d\n"
         (with-out-str (csv/write-csv *out* [["a" "b"] ["c" "d"]])))))

(deftest write-returns-nil
  (with-out-str
    (is (nil? (csv/write-csv *out* [["a"]])))))

(deftest write-quotes-separator
  (is (= "\"a,b\",c\n" (with-out-str (csv/write-csv *out* [["a,b" "c"]])))))

(deftest write-quotes-and-doubles-quote-char
  (is (= "\"a\"\"b\"\n" (with-out-str (csv/write-csv *out* [["a\"b"]])))))

(deftest write-quotes-newline-field
  (is (= "\"a\nb\"\n" (with-out-str (csv/write-csv *out* [["a\nb"]])))))

(deftest write-quotes-cr-field
  (is (= "\"a\rb\"\n" (with-out-str (csv/write-csv *out* [["a\rb"]])))))

(deftest write-empty-fields-and-empty-row
  (is (= ",\n\n"
         (with-out-str (csv/write-csv *out* [["" ""] []])))))

(deftest write-stringifies-cells
  ;; oracle prints Python's True; canonical (str cell) wins here, so
  ;; true lowercase and nil empty, per clojure.data.csv
  (is (= "1,2.5,true,,x\n"
         (with-out-str (csv/write-csv *out* [[1 2.5 true nil "x"]])))))

(deftest write-accepts-any-seq-of-seqs
  (is (= "a,b\n" (with-out-str (csv/write-csv *out* '(("a" "b")))))))

;;;; Writer: options

(deftest write-custom-separator
  (is (= "a;b,c\n"
         (with-out-str (csv/write-csv *out* [["a" "b,c"]] {:separator \;})))))

(deftest write-crlf-newline-opt
  (is (= "a,b\r\n" (with-out-str (csv/write-csv *out* [["a" "b"]] {:newline :crlf})))))

(deftest write-cr-newline-opt
  (is (= "a,b\r" (with-out-str (csv/write-csv *out* [["a" "b"]] {:newline :cr})))))

(deftest write-custom-quote-char
  (is (= "'a''b',c\n"
         (with-out-str (csv/write-csv *out* [["a'b" "c"]] {:quote \'})))))

(deftest write-quote-predicate-opt
  (is (= "\"a\",\"b,c\"\n"
         (with-out-str
           (csv/write-csv *out* [["a" "b,c"]] {:quote? (fn [_] true)})))))

(deftest write-bad-newline-throws
  (is (thrown? (csv/write-csv *out* [["a"]] {:newline :nolf}))))

(deftest write-bad-newline-error-data
  (try
    (csv/write-csv *out* [["a"]] {:newline :nolf})
    (is false "expected a throw")
    (catch e
      (is (= :csv/write (:kind (ex-data e)))))))

;;;; Writer: path destination

(deftest write-to-path-and-slurp-back
  (let [f (str tmpdir "/mino_csv_write_test.csv")]
    (csv/write-csv f [["a" "b"] ["c,d" "e"]])
    (is (= "a,b\n\"c,d\",e\n" (slurp f)))))

;;;; Round-trip

(deftest read-write-round-trip
  (let [rows [["name" "note"] ["x,y" "line\nbreak"] ["plain" "quote\"inside"]]
        text (with-out-str (csv/write-csv *out* rows))]
    (is (= rows (doall (csv/read-csv text))))))

(run-tests-and-exit)
