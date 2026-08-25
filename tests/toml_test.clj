(require "tests/test")
(require '[mino.toml :as toml])

;; TOML 1.0 reader. The golden values were captured by running each
;; document through python3 tomllib (3.14) on this machine and porting
;; its output; dates stay the raw source text (the v1 no-coercion
;; choice, with {:parse-values f} as the coercion hook). Where mino
;; is deliberately stricter than tomllib (64-bit int range) the
;; divergence is noted at the vector and in decisions.edn.
;;
;; Error vectors pin :kind, :reason, and :location {:line :col};
;; :col counts codepoints from column 1 on a construct's first line
;; and bytes on continuation lines of a multi-line value (identical
;; on ASCII documents). Escape-decode errors locate the string value
;; start, not the escape itself.

(defn- err
  "ex-data of parse-string on s, or :no-throw when it succeeds."
  [s]
  (try (toml/parse-string s) :no-throw
       (catch e (ex-data e))))

(defn- err-at
  "parse-string on s must throw :toml/parse with the given reason
  and line/col."
  [s reason line col]
  (let [d (err s)]
    (and (map? d)
         (= :toml/parse (:kind d))
         (= reason (:reason d))
         (= line (get-in d [:location :line]))
         (= col (get-in d [:location :col])))))

;;; Tables, nesting, dotted keys

(deftest tables-and-nesting
  (is (= {:server {:host "localhost"
                   :port 8080
                   :tls {:enabled false}}
          :client {:timeout 30.5}}
         (toml/parse-string
           "[server]\nhost = \"localhost\"\nport = 8080\n[server.tls]\nenabled = false\n[client]\ntimeout = 30.5\n"))))

(deftest super-table-after-sub-table
  ;; tomllib: defining [a] after [a.b] is allowed
  (is (= {:a {:b {:x 1} :y 2}}
         (toml/parse-string "[a.b]\nx=1\n[a]\ny=2\n"))))

(deftest super-table-after-sub-table-empty
  (is (= {:a {:b {:x 1}}}
         (toml/parse-string "[a.b]\nx=1\n[a]\n"))))

(deftest deep-dotted-key
  (is (= {:a {:b {:c {:d {:e {:f 1}}}}}}
         (toml/parse-string "a.b.c.d.e.f = 1\n"))))

(deftest dotted-key-then-deeper-header
  (is (= {:a {:b {:c 1 :d {:e 2}}}}
         (toml/parse-string "a.b.c = 1\n[a.b.d]\ne=2\n"))))

(deftest dotted-key-whitespace-and-quotes
  (is (= {:a {:b 1}}
         (toml/parse-string "a . b = 1\n")))
  (is (= {:a {(keyword "b.c") {:d 1}}}
         (toml/parse-string "a.\"b.c\".d = 1\n"))))

(deftest quoted-and-numeric-and-dashed-keys
  (is (= {:"127.0.0.1" "v" :key2 2}
         (toml/parse-string "\"127.0.0.1\" = \"v\"\n'key2' = 2\n")))
  (is (= 3 (get (toml/parse-string "\"naïve key\" = 3\n")
                (keyword "naïve key"))))
  (is (= 1 (get (toml/parse-string "\"\" = 1\n") (keyword ""))))
  (is (= {:a-b_c 1}
         (toml/parse-string "a-b_c = 1\n")))
  (is (= 2 (get (toml/parse-string "1key = 2\n") (keyword "1key"))))
  (is (= 3 (get (toml/parse-string "-key = 3\n") :-key))))

(deftest header-space-quotes-and-comments
  (is (= {:a {(keyword "b c") {:x 1}}}
         (toml/parse-string "[a.\"b c\"]\nx=1\n")))
  (is (= {:a {:b {:x 1}}}
         (toml/parse-string "[ a . b ]\nx=1\n[a] # hello\n"))))

;;; Arrays of tables

(deftest array-of-tables-appends-in-order
  (is (= {:a [{:x 1 :b [{:y 2}]} {:x 3 :b [{:y 4}]}]}
         (toml/parse-string "[[a]]\nx=1\n[[a.b]]\ny=2\n[[a]]\nx=3\n[[a.b]]\ny=4\n")))
  (is (= {:a [{:x 1} {:x 2}]}
         (toml/parse-string "[[a]]\nx=1\n[[a]]\nx=2\n"))))

(deftest plain-header-inside-last-array-element
  (is (= {:a [{:x 1 :b {:y 2}}]}
         (toml/parse-string "[[a]]\nx=1\n[a.b]\ny=2\n")))
  (is (= {:a [{:b [{:c {}}]}]}
         (toml/parse-string "[[a]]\n[[a.b]]\n[a.b.c]\n"))))

(deftest dotted-keys-inside-array-elements
  (is (= {:a [{:b {:c 1}} {:b {:c 2}}]}
         (toml/parse-string "[[a]]\nb.c = 1\n[[a]]\nb.c = 2\n"))))

(deftest array-header-with-quoted-dotted-path
  (is (= {:"a b" {:"c d" [{:x 1}]}}
         (toml/parse-string "[[\"a b\".\"c d\"]]\nx=1\n"))))

;;; Inline tables

(deftest inline-tables
  (is (= {:a {:x 1 :y 2}}
         (toml/parse-string "a = { x = 1 , y = 2 }\n")))
  (is (= {:a {}}
         (toml/parse-string "a = {}\n")))
  (is (= {:a2 {}}
         (toml/parse-string "a2 = {   }\n")))
  (is (= {:a {:x {:y {:z 1}}}}
         (toml/parse-string "a = {x = {y = {z = 1}}}\n")))
  (is (= {:a {:b {:c 1 :d 2}}}
         (toml/parse-string "a = {b.c = 1, b.d = 2}\n")))
  ;; newlines are legal only inside values (here: an array)
  (is (= {:a {:x [1 2]}}
         (toml/parse-string "a = {x=[1,\n2]}\n"))))

;;; Strings

(deftest basic-string-escapes
  ;; tomllib: \b \t \n \f \r \" \\ and the \u/\U forms
  (is (= {:a (str (char 8) (char 9) (char 10) (char 12) (char 13) "\"" "\\")}
         (toml/parse-string "a = \"\\b\\t\\n\\f\\r\\\"\\\\\"\n")))
  (is (= {:a "é😀"}
         (toml/parse-string "a = \"\\u00E9\\U0001F600\"\n"))))

(deftest literal-strings-are-raw
  (is (= {:a "C:\\path\\no\ttab"}
         (toml/parse-string "a = 'C:\\path\\no\ttab'\n")))
  (is (= {:a ""}
         (toml/parse-string "a = ''\n"))))

(deftest multiline-basic-strings
  ;; leading newline right after the opener is trimmed
  (is (= {:a "line\n"}
         (toml/parse-string "a = \"\"\"\nline\n\"\"\"\n")))
  ;; line-ending backslash trims whitespace and newlines
  (is (= {:a "trimmed together"}
         (toml/parse-string "a = \"\"\"\\\n  trimmed \\\n  together\"\"\"\n")))
  (is (= {:a "ab"}
         (toml/parse-string "a = \"\"\"\\\n    a\\\n    b\"\"\"\n")))
  ;; runs of one or two quotes stay content, three need escaping
  (is (= {:a "Here are two quotation marks: \"\". Simple enough."}
         (toml/parse-string "a = \"\"\"Here are two quotation marks: \"\". Simple enough.\"\"\"\n")))
  (is (= {:a "three: \"\"\" ok"}
         (toml/parse-string "a = \"\"\"three: \"\\\"\\\"\\\" ok\"\"\"\n")))
  ;; escaped quote then two bare quotes then y then the terminator
  (is (= {:a "x\"\"\"y"}
         (toml/parse-string "a = \"\"\"x\\\"\"\"y\"\"\"\n")))
  ;; a fourth closing quote folds back into the content
  (is (= {:a "q\""}
         (toml/parse-string "a = \"\"\"q\"\"\"\"\n")))
  (is (= {:a ""}
         (toml/parse-string "a = \"\"\"\"\"\"\n"))))

(deftest multiline-literal-strings
  ;; a run of five closing quotes folds two back into the content
  (is (= {:a "I [drew] a 'happy' ''duck''"}
         (toml/parse-string "a = '''I [drew] a 'happy' ''duck'''''\n")))
  (is (= {:a ""}
         (toml/parse-string "a = ''''''\n")))
  ;; no escapes, no leading-newline surprises beyond the first
  (is (= {:a "raw \\n stay\n"}
         (toml/parse-string "a = '''\nraw \\n stay\n'''\n"))))

(deftest multiline-strings-in-crlf-documents
  ;; CRLF is normalized to LF everywhere, including multiline strings
  (is (= {:a "line2\n"}
         (toml/parse-string "a = \"\"\"\r\nline2\r\n\"\"\"\r\n")))
  (is (= {:a 1 :b {:c "x"}}
         (toml/parse-string "a = 1\r\n[b]\r\nc = \"x\"\r\n"))))

(deftest multiline-string-inside-array
  (is (= {:a ["multi\n" 2]}
         (toml/parse-string "a = [\"\"\"\nmulti\n\"\"\", 2]\n"))))

;;; Integers

(deftest integer-forms
  (is (= {:a 99 :b 0 :c 0 :d 0}
         (toml/parse-string "a = +99\nb = 0\nc = -0\nd = +0\n")))
  (is (= {:a 123}
         (toml/parse-string "a = 1_2_3\n")))
  (is (= {:a 9223372036854775807 :b -9223372036854775808}
         (toml/parse-string "a = 9223372036854775807\nb = -9223372036854775808\n"))))

(deftest radix-integers
  (is (= {:a 3735928559 :b 493 :c 13 :d 3735928559}
         (toml/parse-string "a = 0xDEAD_beef\nb = 0o755\nc = 0b1101\nd = 0Xdead_BEEF\n"))))

;;; Floats

(deftest float-forms
  (is (= {:a 1.0 :b 3.1415 :c 1000000.0 :d -0.02 :e 5.0E22 :f 10.0}
         (toml/parse-string "a = +1.0\nb = 3.1415\nc = 1e6\nd = -2E-2\ne = 5e+22\nf = 1_0.0\n")))
  (is (= {:a 6.626E-34}
         (toml/parse-string "a = 6.626e-34\n")))
  (is (= {:a 10000000000.0}
         (toml/parse-string "a = 1e1_0\n")))
  (is (= {:a 0.0}
         (toml/parse-string "a = 1e-400\n"))))

(deftest float-overflow-is-infinity
  ;; tomllib: 1e400 parses to float infinity
  (is (= {:a ##Inf}
         (toml/parse-string "a = 1e400\n"))))

(deftest inf-and-nan
  (let [m (toml/parse-string "a = inf\nb = -inf\nc = +inf\nd = nan\ne = +nan\nf = -nan\n")]
    (is (= ##Inf (:a m)))
    (is (= ##-Inf (:b m)))
    (is (= ##Inf (:c m)))
    (is (NaN? (:d m)))
    (is (NaN? (:e m)))
    (is (NaN? (:f m)))))

;;; Booleans and dates

(deftest booleans
  (is (= {:a true :b false}
         (toml/parse-string "a = true\nb = false\n"))))

(deftest dates-and-times-stay-raw-strings
  (is (= {:a "1979-05-27T07:32:00Z"
          :b "1979-05-27T00:32:00-07:00"
          :c "1979-05-27T00:32:00.999999-07:00"
          :d "1979-05-27"
          :e "07:32:00"
          :f "00:32:00.999999"
          :g "00:32:00.99"}
         (toml/parse-string
           "a = 1979-05-27T07:32:00Z\nb = 1979-05-27T00:32:00-07:00\nc = 1979-05-27T00:32:00.999999-07:00\nd = 1979-05-27\ne = 07:32:00\nf = 00:32:00.999999\ng = 00:32:00.99\n")))
  ;; lowercase t/z and the space-separated form are legal
  (is (= {:a "1979-05-27t07:32:00z"}
         (toml/parse-string "a = 1979-05-27t07:32:00z\n")))
  (is (= {:a "1979-05-27 07:32:00Z"}
         (toml/parse-string "a = 1979-05-27 07:32:00Z\n"))))

;;; Arrays

(deftest arrays
  (is (= {:a [1 2] :b [] :c [[]] :d [[1 2] [3 4]]}
         (toml/parse-string "a = [1, 2, ]\nb = [ ]\nc = [ [] ]\nd = [ [ 1, 2 ], [3, 4,] ]\n")))
  (is (= {:a [1 [2 3] 4]}
         (toml/parse-string "a = [\n 1, # one\n [2, 3], # nested\n # comment line\n 4,\n]\n")))
  (is (= {:a [1 2]}
         (toml/parse-string "a = [\n\n 1,\n\n 2\n\n]\n")))
  ;; brackets inside strings are not structure
  (is (= {:a ["[not a bracket]" "x"]}
         (toml/parse-string "a = [\"[not a bracket]\", 'x']\n")))
  (is (= {:a [true false "s" 1 2.5]}
         (toml/parse-string "a = [true, false, \"s\", 1, 2.5]\n"))))

;;; Whole-document shapes

(deftest blank-and-comment-documents
  (is (= {} (toml/parse-string "")))
  (is (= {} (toml/parse-string "# just\n# comments\n")))
  (is (= {:a 1} (toml/parse-string "a = 1 # trailing, no newline"))))

(deftest duplicate-keys-in-different-tables-are-fine
  (is (= {:a {:x 1} :b {:x 2}}
         (toml/parse-string "[a]\nx=1\n[b]\nx=2\n"))))

;;; The {:parse-values f} hook

(deftest parse-values-sees-every-leaf-scalar
  (let [seen  (atom [])
        hook  (fn [v] (swap! seen conj v) v)
        m     (toml/parse-string
                "a = 1\nb = \"s\"\nc = [1, true]\nd = {x = 2.5, y = \"1979-05-27\"}\n"
                {:parse-values hook})]
    (is (= {:a 1 :b "s" :c [1 true] :d {:x 2.5 :y "1979-05-27"}} m))
    (is (= [1 "s" 1 true 2.5 "1979-05-27"] @seen))))

(deftest parse-values-can-coerce-dates
  (let [coerce (fn [v]
                 (if (string? v)
                   (if-let [m (re-matches #"(\d{4})-(\d{2})-(\d{2})" v)]
                     [:date (nth m 1) (nth m 2) (nth m 3)]
                     v)
                   v))]
    (is (= {:published [:date "2024" "01" "15"] :name "x" :n 1}
           (toml/parse-string "published = 2024-01-15\nname = \"x\"\nn = 1\n"
                              {:parse-values coerce})))))

(deftest parse-values-must-be-callable
  (is (thrown? (toml/parse-string "a = 1\n" {:parse-values 42})))
  (is (thrown? (toml/parse-string "a = 1\n" {:parse-values :kw}))))

(deftest parse-string-requires-a-string
  (is (thrown? (toml/parse-string 42)))
  (is (thrown? (toml/parse-string nil)))
  (is (thrown? (toml/parse-string "a = 1\n" :not-a-map))))

;;; Error vectors: kind, reason, position

(deftest duplicate-key-errors
  (is (err-at "a = 1\na = 2\n"                     :duplicate-key  2 1))
  (is (err-at "a.b = 1\na.b = 2\n"                 :duplicate-key  2 1))
  (is (err-at "a = {x=1, x=2}\n"                   :duplicate-key  1 11)))

(deftest dotted-key-collision-errors
  ;; scalar in the path cannot be extended
  (is (err-at "a.b = 1\na.b.c = 2\n"               :overwrite-value 2 1))
  (is (err-at "a = 1\n[a.b]\n"                     :overwrite-value 2 1)))

(deftest value-shape-errors
  (is (err-at "a = 1 2\n"                          :unexpected-text 1 7))
  (is (err-at "a = 01\n"                           :unexpected-text 1 6))
  (is (err-at "a =\n"                              :invalid-value   1 4))
  (is (err-at "a =\t \n"                           :invalid-value   1 7))
  (is (err-at "a = .7\n"                           :invalid-value   1 5))
  (is (err-at "a = 7.\n"                           :unexpected-text 1 6))
  (is (err-at "a = True\n"                         :invalid-value   1 5))
  (is (err-at "a = Infinity\n"                     :invalid-value   1 5))
  (is (err-at "a = 1e 2\n"                         :unexpected-text 1 6))
  (is (err-at "a = _1\n"                           :invalid-value   1 5))
  (is (err-at "a = 1_\n"                           :unexpected-text 1 6))
  (is (err-at "a = 1__0\n"                         :unexpected-text 1 6))
  (is (err-at "a = 0_0\n"                          :unexpected-text 1 6))
  (is (err-at "a = 1._0\n"                         :unexpected-text 1 6))
  (is (err-at "a = 00.1\n"                         :unexpected-text 1 6))
  (is (err-at "a = 0x_1\n"                         :unexpected-text 1 6))
  (is (err-at "a = -0x1\n"                         :unexpected-text 1 6))
  ;; tomllib also accepts past-int64 literals; the TOML 1.0 spec
  ;; requires lossless 64-bit, so mino errors (recorded divergence)
  (is (err-at "a = 9223372036854775808\n"          :int-overflow    1 5)))

(deftest string-errors
  (is (err-at "a = \"x\\q\"\n"                     :invalid-escape  1 5))
  (is (err-at "a = \"\\u00\"\n"                    :invalid-escape  1 5))
  (is (err-at "a = \"\\uD800\"\n"                  :bad-codepoint  1 5))
  (is (err-at "a = \"\\U00110000\"\n"              :bad-codepoint  1 5))
  (is (err-at "a = \"abc\n"                        :unterminated-string 1 5))
  (is (err-at "a = 'abc\n"                         :unterminated-string 1 5))
  (is (err-at "a = [\"x\n"                         :unterminated-string 1 6))
  (is (err-at "a = \"\"\"abc\n"                    :unterminated-string 1 5))
  (is (err-at "a = '''abc\n"                       :unterminated-string 1 5))
  ;; stray carriage return outside a CRLF pair
  (is (err-at "a = 1\rb = 2\n"                     :invalid-character 1 6)))

(deftest collection-errors
  (is (err-at "a = [1 2]\n"                        :expected-separator 1 8))
  (is (err-at "a = [,1]\n"                         :invalid-value   1 6))
  (is (err-at "a = [1,,2]\n"                       :invalid-value   1 8))
  (is (err-at "a = [1,\n"                          :unterminated-array 1 5))
  (is (err-at "a = {\nb = 1}\n"                    :newline-in-inline 1 6))
  (is (err-at "a = {x=1,}\n"                       :trailing-comma  1 10))
  (is (err-at "a = {,x=1}\n"                       :invalid-inline  1 6))
  (is (err-at "a = {\n"                            :unterminated-inline 1 5)))

(deftest statement-errors
  (is (err-at "= 1\n"                              :invalid-statement 1 1))
  (is (err-at "a\n"                                :missing-equals  1 1))
  (is (err-at "[a\n"                               :invalid-statement 1 1))
  (is (err-at "a = 1\nb\n"                         :missing-equals  2 1))
  ;; BOM is rejected like tomllib
  (is (err-at (str (char 65279) "a = 1\n")         :invalid-statement 1 1)))

(deftest table-redeclaration-errors
  (is (err-at "[a]\n[a]\n"                         :redeclare       2 1))
  (is (err-at "[a]\n[a.b]\ny=1\n[a]\nz=2\n"        :redeclare       4 1))
  (is (err-at "[a]\nx=1\n[[a]]\n"                  :redeclare       3 1))
  (is (err-at "[[a]]\nx=1\n[a]\n"                  :redeclare       3 1))
  (is (err-at "a.b = 1\n[a.b]\nc=2\n"              :redeclare       2 1))
  (is (err-at "a.b.c = 1\n[a.b]\nx=2\n"            :redeclare       2 1))
  (is (err-at "a.b = 1\n[[a.b]]\n"                 :redeclare       2 1))
  (is (err-at "a = {x=1}\n[a]\n"                   :redeclare       2 1))
  (is (err-at "a = {x=1}\n[a.x.y]\n"               :redeclare       2 1))
  (is (err-at "[a.b]\n[a]\nb.c = 1\n"              :redeclare       3 1))
  (is (err-at "[t]\na = [1]\n[t.a.b]\n"            :redeclare       3 1))
  (is (err-at "[[a.b]]\nx=1\n[[a]]\n"              :overwrite-value 3 1))
  (is (err-at "a = {x=1}\na.y = 2\n"               :redefine-inline 2 1)))

;;; Real-world shape: a pyproject.toml document

(def ^:private pyproject-text
  "[project]
name = \"sample-project\"
version = \"2.1.0\"
description = \"\"\"\nA sample project description with an é and\ttab,\nspanning lines.\"\"\"
requires-python = \">=3.11\"
published = 2024-06-01T08:00:00Z
keywords = [\"sample\", \"cli\", \"testing\"]
rating = 4.75
downloads = 1_234_567
verified = true

[project.scripts]
sample = \"sample_project.cli:main\"

[project.entry-points.\"package.metadata\"]
author = \"Jane Doe\"

[dependency-groups]
dev = [\"pytest>=8\", \"ruff\"]\n")

(def ^:private pyproject-golden
  {:project
   {:name "sample-project"
    :version "2.1.0"
    :description "A sample project description with an é and\ttab,\nspanning lines."
    :requires-python ">=3.11"
    :published "2024-06-01T08:00:00Z"
    :keywords ["sample" "cli" "testing"]
    :rating 4.75
    :downloads 1234567
    :verified true
    :scripts {:sample "sample_project.cli:main"}
    :entry-points {(keyword "package.metadata") {:author "Jane Doe"}}}
   :dependency-groups {:dev ["pytest>=8" "ruff"]}})

(deftest pyproject-shaped-document
  (is (= pyproject-golden (toml/parse-string pyproject-text))))

(run-tests-and-exit)
