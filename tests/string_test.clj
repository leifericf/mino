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

(deftest str-of-float-matches-jvm-double-to-string
  ;; str of a double uses Double.toString, the same formatter pr-str
  ;; reads, so the two agree for every double.
  (testing "scientific-notation doubles carry the JVM exponent form"
    (is (= "1.0E21" (str 1e21)))
    (is (= "2.0E20" (str 2e20)))
    (is (= "1.0E-7" (str 1e-7)))
    (is (= "9.999E-10" (str 9.999e-10)))
    (is (= "1.2345678901234569E23" (str 123456789012345678901234.5))))
  (testing "plain doubles keep their decimal point"
    (is (= "1.5" (str 1.5)))
    (is (= "1.0" (str 1.0)))
    (is (= "9999999.0" (str 9999999.0)))
    (is (= "0.001" (str 0.001))))
  (testing "str and pr-str agree for every double in the sweep"
    (doseq [x [1e21 1e-7 1.5 1.0 2e20 9.999e-10 0.00001 -0.0 0.0]]
      (is (= (pr-str x) (str x)) x)))
  (testing "non-finite doubles print the plain Java names"
    (is (= "NaN" (str ##NaN)))
    (is (= "Infinity" (str ##Inf)))
    (is (= "-Infinity" (str ##-Inf)))))

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

(deftest trim-and-blank-cover-all-ascii-whitespace
  ;; Parity with Character/isWhitespace over the ASCII/Latin-1 range:
  ;; HT, LF, VT, FF, CR, the FS/GS/RS/US separators, and space. A
  ;; non-breaking space (0xA0) is not whitespace and must survive.
  (let [ff (str (char 12)) vt (str (char 11)) fs (str (char 28))
        nbsp (str (char 160))]
    (is (= "x" (trim (str ff vt "x" vt ff))))
    (is (= "x" (str/triml (str vt ff "x"))))
    (is (= "x" (str/trimr (str "x" ff vt))))
    (is (str/blank? (str ff vt fs)))
    (is (str/blank? (str (char 9) (char 10) (char 13))))
    (is (not (str/blank? nbsp)))
    (is (= nbsp (trim nbsp)))))

(deftest index-of-empty-needle-clamps-like-jvm
  ;; An empty needle returns the clamped from-index [0, len], matching
  ;; java.lang.String/indexOf.
  (is (= 2 (str/index-of "ab" "" 5)))
  (is (= 1 (str/index-of "ab" "" 1)))
  (is (= 0 (str/index-of "ab" "" 0)))
  (is (= 0 (str/index-of "ab" "")))
  (is (= 2 (str/last-index-of "ab" "" 5))))

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

(deftest find-keyword-is-lookup-only
  ;; find-keyword never interns: a not-yet-interned name answers nil
  ;; (whether plain or namespaced), while a previously interned
  ;; keyword is still found.
  (is (nil? (find-keyword "fkw-never")))
  (is (nil? (find-keyword "fkw-ns" "fkw-name")))
  (is (= :fkw-have (do (keyword "fkw-have") (find-keyword "fkw-have")))))

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

;; --- format: canonical directive coverage ---
;; Expected strings captured from the reference implementation. Two
;; deliberate accommodations for mino's single int tier (documented at
;; the prim): %c accepts an int codepoint and %d accepts a bigint,
;; where the JVM's Formatter rejects Long-for-%c and BigInt-for-%d.

(deftest format-directives-canon
  (is (= "FF" (format "%X" 255)))
  (is (= "false" (format "%b" nil)))
  (is (= "false" (format "%b" false)))
  (is (= "true" (format "%b" 42)))
  (is (= "TRUE" (format "%B" "x")))
  (is (= "a" (format "%c" \a)))
  (is (= "a\nb" (format "a%nb")))
  (is (= "1,234,567" (format "%,d" 1234567)))
  (is (= "-1,234,567" (format "%,d" -1234567)))
  (is (= "123" (format "%,d" 123)))
  (is (= "(42)" (format "%(d" -42)))
  (is (= "42" (format "%(d" 42)))
  (is (= "(1,234,567)" (format "%(,d" -1234567)))
  (is (= "b a" (format "%2$s %1$s" "a" "b")))
  (is (= "abc" (format "%.3s" "abcdef")))
  (is (= "ABC" (format "%S" "abc")))
  (is (= "null" (format "%s" nil)))
  (is (= "   ab" (format "%5s" "ab")))
  (is (= "ab   |" (format "%-5s|" "ab")))
  (is (= "       abc|" (format "%10.3s|" "abcdef"))))

(deftest format-float-directives-canon
  (is (= "0x1.8p0" (format "%a" 1.5)))
  (is (= "0X1.8P0" (format "%A" 1.5)))
  (is (= "0x1.999999999999ap-4" (format "%a" 0.1)))
  (is (= "1.23450e-05" (format "%g" 1.2345E-5)))
  (is (= "123.450" (format "%g" 123.45)))
  (is (= "0.00000" (format "%g" 0.0)))
  (is (= "1.23e+03" (format "%.3g" 1234.5)))
  (is (= "1.23450E-05" (format "%G" 1.2345E-5)))
  (is (= "0.000000e+00" (format "%e" 0.0)))
  (is (= "    3.14" (format "%8.2f" 3.14159)))
  (is (= "-0000042" (format "%08d" -42))))

(deftest format-accommodations-and-teeth
  ;; Single int tier: int codepoint for %c, bigint for %d.
  (is (= "a" (format "%c" 97)))
  (is (= "10" (format "%d" 10N)))
  ;; An unknown directive throws instead of leaking literal text.
  (is (thrown? Exception (format "%q" 1)))
  (is (thrown? Exception (format "%"))))

(deftest format-date-and-hash-directives-stay-absent
  ;; ADR 53: %t/%T and %h/%H throw as unknown directives, loudly, as
  ;; classified data; nothing passes through as literal text.
  (doseq [f ["%tY" "%TY" "%h" "%H"]]
    (is (= [:eval/type "MTY001"]
           (try (format f 0)
                (catch :eval/type e [(:mino/kind e) (:mino/code e)])))
        f)
    (is (thrown-with-msg? #"unsupported directive" (format f 0)) f)))

(deftest format-uppercase-char-directive-unicode
  ;; %C uppercases through the generated case tables, not ASCII-only.
  (is (= "A" (format "%C" \a)))
  (is (= "É" (format "%C" (char 233))))
  (is (= "Σ" (format "%C" (char 963))))
  (is (= "А" (format "%C" (char 1072))))
  (is (= "Ა" (format "%C" (char 4304))))
  ;; A codepoint with no uppercase mapping passes through.
  (is (= "5" (format "%C" \5))))

(deftest format-zero-pad-and-width-on-g-and-a
  ;; The 0 flag zero-fills after the sign and hex prefix; width on
  ;; %a/%g runs through the shared numeric pad path.
  (is (= "0000003.14" (format "%010.3g" 3.14)))
  (is (= "-000003.14" (format "%010.3g" -3.14)))
  (is (= "000123.450" (format "%010g" 123.45)))
  (is (= "-0000123.450" (format "%012g" -123.45)))
  (is (= "01.2e+03" (format "%08.2g" 1234.5)))
  (is (= "00001.23e+03" (format "%012.3g" 1234.5)))
  (is (= "01.23450E-05" (format "%012G" 1.2345E-5)))
  (is (= "0x000000001.8p0" (format "%015a" 1.5)))
  (is (= "-0x00000001.8p0" (format "%015a" -1.5)))
  (is (= "0X000000001.8P0" (format "%015A" 1.5)))
  (is (= "        0x1.8p0" (format "%15a" 1.5)))
  (is (= "0x1.8p0        |" (str (format "%-15a" 1.5) "|")))
  (is (= "     123.450" (format "%12g" 123.45)))
  (is (= "123.450     |" (str (format "%-12g" 123.45) "|")))
  ;; A non-numeric rendering (NaN) pads with spaces, never zeros.
  (is (not (str/includes? (format "%010g" ##NaN) "0")))
  (is (= 10 (count (format "%010g" ##NaN)))))

(deftest format-precision-counts-codepoints
  ;; Precision truncates the rendered string at a codepoint boundary,
  ;; never mid-sequence.
  (is (= "É" (format "%.1s" "Éx")))
  (is (= "Éx" (format "%.2s" "Éxy")))
  (is (= "    É" (format "%5.1s" "Éx")))
  (is (= "    ÉC" (format "%6.2S" "école"))))

(deftest format-pad-width-counts-codepoints
  ;; Width measures the rendered argument in codepoints, matching
  ;; count, not UTF-8 bytes.
  (is (= "  É" (format "%3s" "É")))
  (is (= "  É" (format "%3c" (char 201))))
  (is (= "  É" (format "%3C" (char 233))))
  (is (= "   ÉA" (format "%5S" "éa")))
  (is (= "Éx  |" (format "%-4s|" "Éx")))
  ;; An astral char is one codepoint wide, like count; canon counts
  ;; its surrogate halves as two.
  (is (= (str "  " (char 128169)) (format "%3s" (str (char 128169))))))

(deftest format-grouped-decimal-zero-pad
  ;; The 0 flag zero-fills inside the parens and after the sign; the
  ;; fill zeros are plain, never grouped.
  (is (= "(0000012345)" (format "%(012d" -12345)))
  (is (= "(0012345)" (format "%(09d" -12345)))
  (is (= "00000012,345" (format "%,012d" 12345)))
  (is (= "-0000012,345" (format "%,012d" -12345)))
  (is (= "(000000012,345)" (format "%(,015d" -12345)))
  (is (= "1,234,567" (format "%,08d" 1234567))))

(deftest format-uppercase-string-directive-unicode
  ;; %S/%B uppercase through the generated case tables, matching
  ;; upper-case, not byte-wise ASCII.
  (is (= "ÉCOLE" (format "%S" "école")))
  (is (= "ΛX" (format "%S" "λx")))
  (is (= "ДА" (format "%S" "да")))
  (is (= "MIXÉD 5" (format "%S" "mixÉd 5")))
  (is (= "TRUE" (format "%B" 1))))

(deftest format-hex-float-zero-keeps-mantissa-point
  ;; C's %a of zero omits the fraction; canon always keeps one
  ;; fractional digit, and the zero fill lands after the 0x prefix.
  (is (= "0x0.0p0" (format "%a" 0.0)))
  (is (= "-0x0.0p0" (format "%a" -0.0)))
  (is (= "0X0.0P0" (format "%A" 0.0)))
  (is (= "0x000000.0p0" (format "%012a" 0.0)))
  (is (= "-0x00000.0p0" (format "%012a" -0.0)))
  (is (= " 0x0.0p0" (format "%8a" 0.0)))
  (is (= "0x0.0p0  |" (str (format "%-9a" 0.0) "|"))))

(deftest format-hex-float-sign-and-precision
  ;; The + and space flags and the precision thread into the hex
  ;; float directive; canon keeps precision 0 at one digit.
  (is (= "+0x1.8p0" (format "%+a" 1.5)))
  (is (= " 0x1.8p0" (format "% a" 1.5)))
  (is (= "-0x1.8p0" (format "%+a" -1.5)))
  (is (= "0x1.800p0" (format "%.3a" 1.5)))
  (is (= "+0x1.800p0" (format "%+.3a" 1.5)))
  (is (= "0x1.99ap-4" (format "%.3a" 0.1)))
  (is (= "0x1.ap-4" (format "%.1a" 0.1)))
  (is (= "0x1.8p0" (format "%.0a" 1.5)))
  (is (= "0x1.ap-4" (format "%.0a" 0.1)))
  (is (= "0X1.800P0" (format "%.3A" 1.5)))
  ;; Precision past the mantissa zero-extends.
  (is (= "0x1.999999999999a0000000p-4" (format "%.20a" 0.1)))
  (is (= (str "0x1.8" (apply str (repeat 199 "0")) "p0")
         (format "%.200a" 1.5)))
  ;; Zero keeps the mantissa point at any precision, signs included.
  (is (= "+0x0.0p0" (format "%+a" 0.0)))
  (is (= " 0x0.0p0" (format "% a" 0.0)))
  (is (= "-0x0.0p0" (format "%+a" -0.0)))
  (is (= "0x0.000p0" (format "%.3a" 0.0)))
  (is (= "0x0.0p0" (format "%.1a" 0.0)))
  ;; Width composes: spaces by default, zero fill after the prefix.
  ;; (Canon's zero fill overshoots its own width on a precisioned
  ;; hex float; mino honors the width.)
  (is (= "      0x1.800p0" (format "%15.3a" 1.5)))
  (is (= "0x0000001.800p0" (format "%015.3a" 1.5))))

(deftest format-paren-flag-on-finite-floats
  ;; The ( flag renders a finite negative inside parens on f/e/g,
  ;; matching canon; positives are untouched.
  (is (= "(1.500000)" (format "%(f" -1.5)))
  (is (= "1.500000" (format "%(f" 1.5)))
  (is (= "(1.500000e+00)" (format "%(e" -1.5)))
  (is (= "(1.23e+03)" (format "%(.2e" -1234.5)))
  (is (= "(1.50000)" (format "%(g" -1.5)))
  (is (= "(1.23e+03)" (format "%(10.3g" -1234.5)))
  ;; Negative zero is negative to the paren style.
  (is (= "(0.000000)" (format "%(f" -0.0)))
  ;; Width pads with spaces; the 0 flag zero-fills after the paren.
  (is (= "    (3.14)" (format "%(10.2f" -3.14)))
  (is (= "(00003.14)" (format "%(010.2f" -3.14)))
  (is (= "0000003.14" (format "%(010.2f" 3.14)))
  (is (= "(3.14)    |" (format "%(-10.2f|" -3.14)))
  (is (= "(001.23e+03)" (format "%(012.3g" -1234.5)))
  ;; The + flag composes; parens still win on a negative.
  (is (= "(1.500000)" (format "%+(f" -1.5)))
  ;; A rendering past the stack buffer keeps its parens.
  (let [s (format "%(f" -1.0E300)]
    (is (= 310 (count s)))
    (is (str/starts-with? s "(1"))
    (is (str/ends-with? s ".000000)"))))

(deftest format-nonfinite-floats-canon-spelling
  ;; Non-finite doubles spell the canon way across the float
  ;; directives, not C's lowercase forms.
  (is (= "NaN" (format "%f" ##NaN)))
  (is (= "NaN" (format "%e" ##NaN)))
  (is (= "NaN" (format "%g" ##NaN)))
  (is (= "NaN" (format "%a" ##NaN)))
  (is (= "Infinity" (format "%f" ##Inf)))
  (is (= "Infinity" (format "%g" ##Inf)))
  (is (= "Infinity" (format "%a" ##Inf)))
  (is (= "-Infinity" (format "%f" ##-Inf)))
  (is (= "-Infinity" (format "%e" ##-Inf)))
  (is (= "-Infinity" (format "%a" ##-Inf)))
  (is (= "NAN" (format "%E" ##NaN)))
  (is (= "INFINITY" (format "%G" ##Inf)))
  (is (= "-INFINITY" (format "%A" ##-Inf)))
  ;; Sign flags apply to infinities, never to NaN.
  (is (= "+Infinity" (format "%+f" ##Inf)))
  (is (= "-Infinity" (format "%+f" ##-Inf)))
  (is (= "NaN" (format "%+f" ##NaN)))
  (is (= " Infinity" (format "% f" ##Inf)))
  (is (= "(Infinity)" (format "%(f" ##-Inf)))
  (is (= "Infinity" (format "%(f" ##Inf)))
  ;; Precision is ignored; width pads with spaces even under 0.
  (is (= "NaN" (format "%.2f" ##NaN)))
  (is (= "       NaN" (format "%10f" ##NaN)))
  (is (= "Infinity  |" (format "%-10f|" ##Inf)))
  (is (= " -Infinity" (format "%010f" ##-Inf)))
  (is (= "   -Infinity" (format "%12g" ##-Inf))))

(deftest format-grouping-separator-fixed-comma
  ;; ADR 52: the , flag groups with a hardcoded comma on every host;
  ;; mino output is byte-identical across hosts, so no locale lookup.
  (is (= "1,234,567" (format "%,d" 1234567)))
  (is (= "-1,234" (format "%,d" -1234)))
  (is (= "999" (format "%,d" 999)))
  (is (= "1,000" (format "%,d" 1000)))
  (is (= "(12,345)" (format "%(,d" -12345)))
  (is (= "-9,223,372,036,854,775,808" (format "%,d" -9223372036854775808))))

(deftest format-char-codepoint-accommodation
  ;; ADR 51: %c and %C take an int codepoint in mino's single int
  ;; tier, bounded to the Unicode scalar range.
  (is (= "a" (format "%c" 97)))
  (is (= "λ" (format "%c" 955)))
  (is (= "A" (format "%C" 97)))
  (is (= (str (char 128169)) (format "%c" 128169)))
  (is (thrown-with-msg? #"char" (format "%c" -1)))
  (is (thrown-with-msg? #"char" (format "%c" 1114112)))
  (is (thrown-with-msg? #"char" (format "%c" "a"))))

(deftest format-integer-directives-reject-floats
  ;; Reference semantics: a float argument to an integer directive is
  ;; an illegal conversion and throws; truncating toward zero was a
  ;; silent wrong answer.
  (is (thrown-with-msg? #"integer" (format "%d" 3.99)))
  (is (thrown-with-msg? #"integer" (format "%x" 3.99)))
  (is (thrown-with-msg? #"integer" (format "%o" 3.99)))
  (is (thrown-with-msg? #"integer" (format "%d" -0.5)))
  (is (thrown-with-msg? #"integer" (format "%,d" 1234.5)))
  (is (thrown-with-msg? #"integer" (format "%d" ##NaN)))
  (is (thrown-with-msg? #"integer" (format "%d" ##Inf)))
  (is (thrown-with-msg? #"integer" (format "%d" 1/2)))
  ;; The error is classified data, catchable by kind.
  (is (= :eval/type
         (try (format "%d" 3.99) (catch :eval/type e (:mino/kind e)))))
  ;; The single-int-tier bigint accommodation stays: within 64 bits
  ;; formats, beyond 64 bits throws.
  (is (= "ff" (format "%x" 255N)))
  (is (thrown? Exception (format "%d" 123456789012345678901234N))))
