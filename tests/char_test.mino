(require "tests/test")

;; First-class character values.

(deftest char-is-distinct-type
  (is (= :char (type \A)))
  (is (char? \A))
  (is (char? \space))
  (is (char? \newline))
  (is (not (char? "A")))
  (is (not (char? 65)))
  (is (not (string? \A)))
  (is (not (int? \A))))

(deftest char-equality
  (is (= \A \A))
  (is (not (= \A \B)))
  (is (not (= \A "A")))
  (is (not (= \A 65)))
  (is (= \space \space))
  (is (= \u0041 \A)))

(deftest char-named-literals
  (is (= 32  (int \space)))
  (is (= 10  (int \newline)))
  (is (= 9   (int \tab)))
  (is (= 13  (int \return)))
  (is (= 8   (int \backspace)))
  (is (= 12  (int \formfeed))))

(deftest char-unicode-literals
  (is (= 65     (int \u0041)))
  (is (= 97     (int \u0061)))
  (is (= 0x2603 (int \u2603)))
  (is (= \A     \u0041))
  (is (= \a     \u0061)))

(deftest char-terminator-literals
  (is (= 123 (int \{)))
  (is (= 125 (int \})))
  (is (= 40  (int \()))
  (is (= 41  (int \))))
  (is (= 91  (int \[)))
  (is (= 93  (int \])))
  (is (= 59  (int \;)))
  (is (= 44  (int \,)))
  (is (= 34  (int \"))))

(deftest char-to-string
  (is (= "A"    (str \A)))
  (is (= "abc"  (str \a \b \c)))
  (is (= " "    (str \space)))
  (is (= "\n"   (str \newline))))

(deftest char-to-codepoint
  (is (= 65  (int \A)))
  (is (= 97  (int \a)))
  (is (= 48  (int \0)))
  (is (= 0x2603 (int \u2603))))

(deftest char-roundtrip-via-read-string
  (is (= \A (read-string "\\A")))
  (is (= \space (read-string "\\space")))
  (is (= \newline (read-string "\\newline")))
  (is (= \u0041 (read-string "\\u0041"))))

(deftest char-print-forms
  (is (= "\\A"       (pr-str \A)))
  (is (= "\\space"   (pr-str \space)))
  (is (= "\\newline" (pr-str \newline)))
  (is (= "\\tab"     (pr-str \tab)))
  (is (= "\\u2603"   (pr-str \u2603))))

(deftest char-as-map-key
  (let [m {\a 1 \b 2 \c 3}]
    (is (= 1 (get m \a)))
    (is (= 2 (get m \b)))
    (is (= 3 (get m \c)))
    ;; Char keys are distinct from string keys; no accidental aliasing.
    (is (nil? (get m "a")))))

(deftest char-as-set-member
  (let [s #{\a \b \c}]
    (is (contains? s \a))
    (is (contains? s \b))
    (is (not (contains? s "a")))))
