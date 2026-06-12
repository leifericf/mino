(require "tests/test")
(require '[clojure.pprint :as pp])
(require '[clojure.string :as str])

;; Spec-first tests for cl-format, formatter, formatter-out, and
;; code-dispatch. All assertions are red today: cl-format is a throwing
;; stub; formatter, formatter-out, and code-dispatch do not exist yet.

;; ---------------------------------------------------------------------------
;; API presence
;; ---------------------------------------------------------------------------

(deftest clf-api-presence
  (doseq [s '[clojure.pprint/cl-format
              clojure.pprint/formatter
              clojure.pprint/formatter-out
              clojure.pprint/code-dispatch]]
    (is (some? (resolve s)) (str s " must be defined"))))

;; ---------------------------------------------------------------------------
;; ~a and ~s — basic formatting
;; ---------------------------------------------------------------------------

(deftest clf-tilde-a-basic
  (is (= "hello" (pp/cl-format nil "~a" "hello")))
  (is (= "42"    (pp/cl-format nil "~a" 42)))
  (is (= "nil"   (pp/cl-format nil "~a" nil)))
  (is (= "[1 2]" (pp/cl-format nil "~a" [1 2]))))

(deftest clf-tilde-s-readably
  ;; ~s prints via pr-str, so strings are double-quoted
  (is (= "\"hello\"" (pp/cl-format nil "~s" "hello")))
  (is (= "42"        (pp/cl-format nil "~s" 42)))
  (is (= "nil"       (pp/cl-format nil "~s" nil))))

(deftest clf-tilde-a-mincol-padding
  ;; ~10a pads on the right to at least 10 columns
  (is (= "hello     " (pp/cl-format nil "~10a" "hello")))
  ;; value wider than mincol: no truncation
  (is (= "hello world" (pp/cl-format nil "~10a" "hello world"))))

(deftest clf-tilde-a-at-right-align
  ;; ~10@a pads on the left (right-aligns)
  (is (= "     hello" (pp/cl-format nil "~10@a" "hello"))))

;; ---------------------------------------------------------------------------
;; Integer directives — ~d ~x ~o ~b ~r
;; ---------------------------------------------------------------------------

(deftest clf-tilde-d-plain
  (is (= "0"    (pp/cl-format nil "~d" 0)))
  (is (= "42"   (pp/cl-format nil "~d" 42)))
  (is (= "-7"   (pp/cl-format nil "~d" -7)))
  (is (= "1000" (pp/cl-format nil "~d" 1000))))

(deftest clf-tilde-d-commas
  ;; ~:d inserts comma separators every three digits
  (is (= "1,234,567" (pp/cl-format nil "~:d" 1234567)))
  (is (= "1,000"     (pp/cl-format nil "~:d" 1000)))
  (is (= "999"       (pp/cl-format nil "~:d" 999)))
  (is (= "0"         (pp/cl-format nil "~:d" 0))))

(deftest clf-tilde-d-mincol-padchar
  ;; ~10,'0d pads with zeros to width 10
  (is (= "0000000042" (pp/cl-format nil "~10,'0d" 42)))
  ;; ~6d pads with spaces
  (is (= "    42" (pp/cl-format nil "~6d" 42))))

(deftest clf-tilde-d-at-sign
  ;; ~@d forces a + sign on positive numbers
  (is (= "+42"  (pp/cl-format nil "~@d" 42)))
  (is (= "-42"  (pp/cl-format nil "~@d" -42)))
  (is (= "+0"   (pp/cl-format nil "~@d" 0))))

(deftest clf-tilde-x-hex
  (is (= "ff"   (pp/cl-format nil "~x" 255)))
  (is (= "0"    (pp/cl-format nil "~x" 0)))
  (is (= "10"   (pp/cl-format nil "~x" 16))))

(deftest clf-tilde-o-octal
  (is (= "377"  (pp/cl-format nil "~o" 255)))
  (is (= "0"    (pp/cl-format nil "~o" 0)))
  (is (= "10"   (pp/cl-format nil "~o" 8))))

(deftest clf-tilde-b-binary
  (is (= "1111" (pp/cl-format nil "~b" 15)))
  (is (= "0"    (pp/cl-format nil "~b" 0)))
  (is (= "1"    (pp/cl-format nil "~b" 1))))

(deftest clf-tilde-r-cardinal-english
  ;; ~r without a radix prefix produces cardinal english
  (is (= "zero"         (pp/cl-format nil "~r" 0)))
  (is (= "one"          (pp/cl-format nil "~r" 1)))
  (is (= "four"         (pp/cl-format nil "~r" 4)))
  (is (= "twelve"       (pp/cl-format nil "~r" 12)))
  (is (= "one hundred"  (pp/cl-format nil "~r" 100)))
  (is (= "minus one"    (pp/cl-format nil "~r" -1))))

(deftest clf-tilde-r-radix
  ;; ~Nr prints in base N
  (is (= "11"  (pp/cl-format nil "~3r" 4)))   ; 4 in base 3 = 11
  (is (= "ff"  (pp/cl-format nil "~16r" 255)))
  (is (= "10"  (pp/cl-format nil "~8r" 8))))

;; ---------------------------------------------------------------------------
;; Float directives — ~f ~e ~$
;; ---------------------------------------------------------------------------

(deftest clf-tilde-f-basic
  ;; ~f fixed notation
  (is (= "0.0"  (pp/cl-format nil "~f" 0.0)))
  (is (= "-1.5" (pp/cl-format nil "~f" -1.5))))

(deftest clf-tilde-f-width-digits
  ;; ~w,df — width w, d decimal places
  (is (= "3.14"  (pp/cl-format nil "~,2f" Math/PI)))
  (is (= " 3.14" (pp/cl-format nil "~5,2f" Math/PI)))
  (is (= "3.1"   (pp/cl-format nil "~,1f" Math/PI)))
  (is (= "0.00"  (pp/cl-format nil "~,2f" 0.0))))

(deftest clf-tilde-e-scientific
  ;; ~e exponential notation; shape check — must contain E or e
  (let [s (pp/cl-format nil "~e" 12345.678)]
    (is (string? s))
    (is (boolean (re-find #"[Ee][+-]\d+" s))))
  (let [s (pp/cl-format nil "~,2e" 0.001)]
    (is (string? s))
    (is (boolean (re-find #"[Ee]-" s)))))

(deftest clf-tilde-dollar-currency
  ;; ~$ two decimal places by default
  (is (= "3.14"   (pp/cl-format nil "~$" 3.14159)))
  (is (= "0.00"   (pp/cl-format nil "~$" 0.0)))
  (is (= "-1.23"  (pp/cl-format nil "~$" -1.23)))
  (is (= "100.00" (pp/cl-format nil "~$" 100.0))))

;; ---------------------------------------------------------------------------
;; Newline, fresh-line, tilde, character
;; ---------------------------------------------------------------------------

(deftest clf-tilde-percent-newline
  (is (= "a\nb"   (pp/cl-format nil "a~%b")))
  (is (= "a\n\nb" (pp/cl-format nil "a~2%b"))))

(deftest clf-tilde-ampersand-fresh-line
  ;; ~& ensures a fresh line — does not duplicate a newline already present
  (is (= "a\nb"  (pp/cl-format nil "a~&b")))
  ;; Two ~& in a row: still one newline when already on a fresh line
  (is (= "a\nb"  (pp/cl-format nil "a~&~&b"))))

(deftest clf-tilde-tilde-literal
  (is (= "~"    (pp/cl-format nil "~~")))
  (is (= "a~b"  (pp/cl-format nil "a~~b")))
  (is (= "~~~"  (pp/cl-format nil "~3~"))))

(deftest clf-tilde-c-character
  (is (= "A"   (pp/cl-format nil "~c" \A)))
  (is (= " "   (pp/cl-format nil "~c" \space)))
  (is (= "\n"  (pp/cl-format nil "~c" \newline))))

;; ---------------------------------------------------------------------------
;; Iteration — ~{...~}
;; ---------------------------------------------------------------------------

(deftest clf-iteration-basic
  ;; ~{~a~^, ~} iterates over a seq, separating with ", "
  (is (= "1, 2, 3"  (pp/cl-format nil "~{~a~^, ~}" [1 2 3])))
  (is (= "a, b, c"  (pp/cl-format nil "~{~a~^, ~}" ["a" "b" "c"])))
  (is (= "x"        (pp/cl-format nil "~{~a~^, ~}" ["x"])))
  (is (= ""         (pp/cl-format nil "~{~a~^, ~}" []))))

(deftest clf-iteration-kv-pairs
  ;; ~{~a=~a~^ ~} consumes pairs from a flat seq
  (is (= "a=1 b=2" (pp/cl-format nil "~{~a=~a~^ ~}" ["a" 1 "b" 2]))))

(deftest clf-iteration-colon-sublists
  ;; ~:{...~} treats each element as a sublist
  (is (= "1, 2, 3"
         (pp/cl-format nil "~:{~a~^, ~}" [[1] [2] [3]])))
  (is (= "a=1 b=2"
         (pp/cl-format nil "~:{~a=~a~}" [["a" 1] ["b" 2]]))))

;; ---------------------------------------------------------------------------
;; Conditional — ~[...~]
;; ---------------------------------------------------------------------------

(deftest clf-conditional-indexed
  ;; ~[zero~;one~;two~] selects by 0-based integer arg
  (is (= "zero" (pp/cl-format nil "~[zero~;one~;two~]" 0)))
  (is (= "one"  (pp/cl-format nil "~[zero~;one~;two~]" 1)))
  (is (= "two"  (pp/cl-format nil "~[zero~;one~;two~]" 2))))

(deftest clf-conditional-else-clause
  ;; ~[a~;b~:;default~] — ~:; is the else clause
  (is (= "a"       (pp/cl-format nil "~[a~;b~:;default~]" 0)))
  (is (= "b"       (pp/cl-format nil "~[a~;b~:;default~]" 1)))
  (is (= "default" (pp/cl-format nil "~[a~;b~:;default~]" 5))))

(deftest clf-conditional-boolean
  ;; ~:[false-case~;true-case~] selects on truthiness
  (is (= "yes"  (pp/cl-format nil "~:[no~;yes~]" true)))
  (is (= "no"   (pp/cl-format nil "~:[no~;yes~]" false)))
  (is (= "no"   (pp/cl-format nil "~:[no~;yes~]" nil)))
  (is (= "yes"  (pp/cl-format nil "~:[no~;yes~]" 42))))

(deftest clf-conditional-at-optional
  ;; ~@[...~] executes the clause only when the arg is truthy
  (is (= "got: 5"  (pp/cl-format nil "~@[got: ~a~]" 5)))
  (is (= ""        (pp/cl-format nil "~@[got: ~a~]" nil)))
  (is (= ""        (pp/cl-format nil "~@[got: ~a~]" false))))

;; ---------------------------------------------------------------------------
;; Plural, arg-skip, recursive format
;; ---------------------------------------------------------------------------

(deftest clf-tilde-p-plural
  ;; ~:p re-checks the previous arg (the count)
  (is (= "1 cat"   (pp/cl-format nil "~d cat~:p" 1)))
  (is (= "2 cats"  (pp/cl-format nil "~d cat~:p" 2)))
  (is (= "0 cats"  (pp/cl-format nil "~d cat~:p" 0))))

(deftest clf-tilde-p-basic
  ;; ~p alone consumes one arg
  (is (= "s"  (pp/cl-format nil "~p" 2)))
  (is (= ""   (pp/cl-format nil "~p" 1))))

(deftest clf-tilde-star-skip
  ;; ~* skips one arg forward
  (is (= "b"  (pp/cl-format nil "~*~a" "a" "b")))
  ;; ~2* skips two args
  (is (= "c"  (pp/cl-format nil "~2*~a" "a" "b" "c"))))

(deftest clf-tilde-question-recursive
  ;; ~? takes a format string and a list of args from the arg stream
  (is (= "1 + 2 = 3"
         (pp/cl-format nil "~?" "~d + ~d = ~d" [1 2 3]))))

;; ---------------------------------------------------------------------------
;; Column tabulation — ~t
;; ---------------------------------------------------------------------------

(deftest clf-tilde-t-tabulation
  ;; ~Nt pads with spaces to reach column N
  (is (= "ab        c"  (pp/cl-format nil "ab~10tc")))
  ;; already past the column: at least a space is still inserted
  (let [s (pp/cl-format nil "abcdefghijklm~10t!")]
    (is (string? s))
    (is (str/ends-with? s "!"))))

;; ---------------------------------------------------------------------------
;; Destination variants — nil, true
;; ---------------------------------------------------------------------------

(deftest clf-destination-nil-returns-string
  (let [result (pp/cl-format nil "~a ~a" "hello" "world")]
    (is (string? result))
    (is (= "hello world" result))))

(deftest clf-destination-true-writes-to-out
  ;; true writes to *out*, returns nil
  (let [ret (atom ::unset)
        out (with-out-str (reset! ret (pp/cl-format true "~a" "hi")))]
    (is (= "hi" out))
    (is (nil? @ret))))

;; ---------------------------------------------------------------------------
;; formatter macro
;; ---------------------------------------------------------------------------

(deftest clf-formatter-returns-fn
  ;; (formatter "~a-~a") returns a function
  (let [f (pp/formatter "~a-~a")]
    (is (fn? f))))

(deftest clf-formatter-nil-stream-returns-string
  ;; calling the function with nil as the stream returns a string
  (let [f (pp/formatter "~a-~a")]
    (is (= "1-2" (f nil 1 2)))))

(deftest clf-formatter-various-directives
  (is (= "x=42"    ((pp/formatter "~a=~d") nil "x" 42)))
  (is (= "1, 2, 3" ((pp/formatter "~{~a~^, ~}") nil [1 2 3]))))

(deftest clf-formatter-true-stream-writes-to-out
  ;; (formatter "~a") called with true writes to *out*, returns nil
  (let [f   (pp/formatter "~a")
        ret (atom ::unset)
        out (with-out-str (reset! ret (f true 5)))]
    (is (= "5" out))
    (is (nil? @ret))))

;; ---------------------------------------------------------------------------
;; formatter-out macro
;; ---------------------------------------------------------------------------

(deftest clf-formatter-out-returns-fn
  (is (fn? (pp/formatter-out "~a"))))

(deftest clf-formatter-out-writes-to-dynamic-out
  ;; formatter-out writes to *out* directly; returns nil
  (let [f   (pp/formatter-out "~a")
        ret (atom ::unset)
        out (with-out-str (reset! ret (f 5)))]
    (is (= "5" out))
    (is (nil? @ret))))

(deftest clf-formatter-out-multiple-args
  (let [f   (pp/formatter-out "~a ~a")
        out (with-out-str (f "hello" "world"))]
    (is (= "hello world" out))))

;; ---------------------------------------------------------------------------
;; code-dispatch
;; ---------------------------------------------------------------------------

(deftest clf-code-dispatch-presence
  (is (some? (resolve 'clojure.pprint/code-dispatch)))
  (is (ifn? pp/code-dispatch)))

(deftest clf-code-dispatch-short-form-one-line
  ;; A short defn fits on one line with code-dispatch
  (let [out (with-out-str
              (pp/with-pprint-dispatch pp/code-dispatch
                (pp/pprint '(defn f [x] (+ x 1)))))]
    (is (string? out))
    (is (= 1 (count (str/split-lines (str/trim-newline out)))))))

(deftest clf-code-dispatch-long-defn-breaks
  ;; A defn with a long body breaks across lines with indentation
  (let [form '(defn my-long-function
                [argument-one argument-two argument-three]
                (let [x (+ argument-one argument-two)]
                  (* x argument-three)))
        out  (binding [pp/*print-right-margin* 40]
               (with-out-str
                 (pp/with-pprint-dispatch pp/code-dispatch
                   (pp/pprint form))))]
    (is (string? out))
    (let [lines (str/split-lines out)]
      (is (> (count lines) 1))
      (is (str/starts-with? (first lines) "("))
      (is (every? (fn [line] (or (str/starts-with? line " ")
                                 (str/blank? line)))
                  (rest lines))))))

(deftest clf-code-dispatch-via-binding
  ;; binding [*print-pprint-dispatch* code-dispatch] is equivalent
  (let [out (binding [pp/*print-pprint-dispatch* pp/code-dispatch]
              (with-out-str (pp/pprint '(defn f [x] (+ x 1)))))]
    (is (string? out))
    (is (= 1 (count (str/split-lines (str/trim-newline out)))))))

(deftest clf-code-dispatch-indentation-prefix
  ;; Continuation lines of a broken defn are indented with at least one space
  (let [form '(defn long-name-function [a b c d e f] body-form another-form)
        out  (binding [pp/*print-right-margin* 30]
               (with-out-str
                 (pp/with-pprint-dispatch pp/code-dispatch
                   (pp/pprint form))))
        lines (str/split-lines out)]
    (when (> (count lines) 1)
      (is (every? (fn [line] (or (str/blank? line)
                                 (str/starts-with? line " ")))
                  (rest lines))))))

;; ---------------------------------------------------------------------------
;; Multi-arg and mixed-directive round-trips
;; ---------------------------------------------------------------------------

(deftest clf-mixed-directives-sentence
  ;; Canonical example from the cl-format docstring
  (let [results [46 38 22]
        out (with-out-str
              (pp/cl-format true
                "There ~[are~;is~:;are~]~:* ~d result~:p: ~{~d~^, ~}~%"
                (count results) results))]
    (is (= "There are 3 results: 46, 38, 22\n" out))))

(deftest clf-no-directives-passthrough
  ;; A format string with no directives is returned as-is
  (is (= "hello world" (pp/cl-format nil "hello world")))
  (is (= ""            (pp/cl-format nil ""))))

(run-tests-and-exit)
