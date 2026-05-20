(require "tests/test")

;; Regex literal `#"..."` body bytes pass to the regex engine
;; verbatim — backslashes are not consumed by string-escape, so
;; `\d` / `\s` / `\w` reach the engine as the two-character
;; sequences `\d`, `\s`, `\w`. The string-form `"\\d+"` path
;; remains supported and equivalent.

(deftest re-find-literal-class-escapes
  (is (= "42" (re-find #"\d+" "abc 42")))
  (is (= "  " (re-find #"\s+" "a  b")))
  (is (= "abc" (re-find #"\w+" "abc 42"))))

(deftest re-seq-literal-class-escapes
  (is (= 4 (count (re-seq #"\d+" "1 2 33 444")))))

(deftest re-find-groups-vector
  (is (= ["12-34" "12" "34"] (re-find "(\\d+)-(\\d+)" "12-34")))
  (is (= ["a/b" "a" "b"]     (re-matches "(.+)/(.+)" "a/b"))))

(deftest re-find-groupless-still-string
  (is (= "abc" (re-find "abc" "xabcx"))))

(deftest re-matcher-iterates
  (let [m (re-matcher "\\d+" "1 2 3")]
    (is (= "1" (re-find m)))
    (is (= "2" (re-find m)))
    (is (= "3" (re-find m)))
    (is (nil? (re-find m)))))

(deftest re-groups-returns-last-match
  (let [m (re-matcher "(\\d+)" "1 2 3")]
    (is (some? (re-find m)))
    (is (= ["1" "1"] (re-groups m)))
    (re-find m)
    (is (= ["2" "2"] (re-groups m)))))

(deftest re-groups-throws-without-match
  (let [m (re-matcher "\\d+" "abc")]
    (is (thrown? (re-groups m)))))

;; Regex: re-find and re-matches via bundled tiny-regex-c.

(deftest re-find-basic
  (testing "digit class"
    (is (= "123" (re-find "\\d+" "abc123def"))))
  (testing "word characters"
    (is (= "hello" (re-find "[a-z]+" "123hello456"))))
  (testing "no match returns nil"
    (is (= nil (re-find "xyz" "abc"))))
  (testing "dot matches any"
    (is (= "a" (re-find "." "abc"))))
  (testing "anchors"
    (is (= "abc" (re-find "^abc" "abcdef")))
    (is (= nil (re-find "^abc" "xabcdef")))))

(deftest re-find-patterns
  (testing "email-like pattern"
    (is (= "test@example.com"
           (re-find "\\w+@\\w+\\.\\w+" "email: test@example.com ok"))))
  (testing "whitespace class"
    (is (= " " (re-find "\\s" "hello world")))))

(deftest re-matches-basic
  (testing "full match"
    (is (= "12345" (re-matches "\\d+" "12345"))))
  (testing "partial match returns nil"
    (is (= nil (re-matches "\\d+" "123abc")))))

(deftest re-find-inverted-class
  (testing "inverted literal class"
    (is (= "d" (re-find "[^abc]" "abcdef"))))
  (testing "inverted class no match"
    (is (= nil (re-find "[^abc]" "aaa"))))
  (testing "inverted digit class"
    (is (= "x" (re-find "[^0-9]" "123x456"))))
  (testing "inverted range class"
    (is (= "W" (re-find "[^a-z ]" "hello World")))))

(deftest re-matches-patterns
  (testing "word pattern"
    (is (= "hello" (re-matches "[a-z]+" "hello"))))
  (testing "mixed fails"
    (is (= nil (re-matches "[a-z]+" "hello123")))))

;; Regression: re_compile used to silently truncate patterns past 30
;; tokens, which produced `MCT001 invalid regex pattern` on any input
;; (the truncated pattern always had an unclosed capture group).
;; v0.219.0 bumped MAX_REGEXP_OBJECTS / MAX_CHAR_CLASS_LEN and turned
;; overflow into an explicit compile failure.
(deftest re-find-long-pattern-with-capture
  (testing "long literal prefix + capture group + quantifier"
    (is (= ["#define MINO_STENCIL_RELOC_FOO 42"
            "MINO_STENCIL_RELOC_FOO" "42"]
           (re-find
            #"#define\s+(MINO_STENCIL_RELOC_[A-Z_0-9]+)\s+(\d+)u?"
            "#define MINO_STENCIL_RELOC_FOO 42u"))))
  (testing "same pattern on non-matching input returns nil (not MCT001)"
    (is (nil? (re-find
               #"#define\s+(MINO_STENCIL_RELOC_[A-Z_0-9]+)\s+(\d+)u?"
               "/*")))))

(deftest re-bounded-quantifier
  ;; Regression: mino's regex engine previously did not parse {n} /
  ;; {n,m} / {n,} bounded repeats. Patterns like #"\d{4}" silently
  ;; matched zero characters (returning nil for re-find) because the
  ;; compile path skipped the unknown '{' meta-char. Real Clojure
  ;; regex (and POSIX EREs) treat {n,m} as a standard quantifier.
  (testing "exact-count {n}"
    (is (= "2026" (re-find #"\d{4}" "year 2026")))
    (is (= "abcd" (re-find #"[a-z]{4}" "abcdef")))
    (is (nil? (re-find #"\d{5}" "1234"))))
  (testing "range {n,m}"
    (is (= "ab"   (re-find #"[a-z]{1,3}" "ab")))
    (is (= "abc"  (re-find #"[a-z]{1,3}" "abcdef")))
    (is (= "1234" (re-find #"\d{2,4}" "12345"))))
  (testing "open-ended {n,}"
    (is (= "abc"    (re-find #"[a-z]{2,}" "abc")))
    (is (= "abcdef" (re-find #"[a-z]{2,}" "abcdef")))
    (is (nil? (re-find #"[a-z]{4,}" "abc"))))
  (testing "re-matches with bounded quantifier + capture groups"
    (is (= ["2026-05-17" "2026" "05" "17"]
           (re-matches #"(\d{4})-(\d{2})-(\d{2})" "2026-05-17")))
    (is (nil? (re-matches #"(\d{4})-(\d{2})-(\d{2})" "26-05-17"))))
  (testing "literal { without digits stays a literal char"
    (is (= "{abc}" (re-find #"\{abc\}" "x{abc}y")))))

(deftest re-inline-flags
  ;; Regression: real Clojure regex supports JVM-style inline flags
  ;; (?<flags>). mino's regex engine previously parsed them as
  ;; literal characters, so `(?i)foo` matched the literal substring
  ;; "(?i)foo" rather than enabling case-insensitive matching. Now
  ;; the compiler emits SET_FLAGS pattern slots that the matcher
  ;; absorbs to update a per-match flag word; matchone, matchdot,
  ;; matchcharclass, BEGIN, and END all honor the active flags.
  (testing "(?i) case-insensitive single-char and class"
    (is (= "FOO"   (re-find #"(?i)foo" "FOO")))
    (is (= "Bar"   (re-find #"(?i)bar" "Bar")))
    (is (= "ABCD"  (re-find #"(?i)[a-z]+" "ABCD")))
    (is (= "hello" (re-find #"(?i)HELLO" "before hello after"))))
  (testing "(?-i) clears case-insensitivity downstream"
    (is (nil?      (re-find #"(?i)(?-i)foo" "FOO")))
    (is (= "foo"   (re-find #"(?i)(?-i)foo" "FOO foo"))))
  (testing "(?s) DOTALL makes . match \\n"
    (is (nil?      (re-find #"a.b"     "a\nb")))
    (is (= "a\nb"  (re-find #"(?s)a.b" "a\nb"))))
  (testing "(?m) multiline ^ at line starts"
    (is (nil?     (re-find #"^bar"     "foo\nbar")))
    (is (= "bar"  (re-find #"(?m)^bar" "foo\nbar"))))
  (testing "(?m) multiline $ before \\n"
    (is (= "foo"  (re-find #"(?m)foo$" "foo\nbar"))))
  (testing "(?x) extended ignores pattern whitespace and #-comments"
    (is (= "foobar" (re-find #"(?x) foo bar " "x foobar y")))
    ;; `#"..."` raw literals don't process escape sequences, so the
    ;; \n inside the pattern body is two literal chars. Use the
    ;; string form to embed a real newline that terminates the
    ;; #-line comment cleanly.
    (is (= "abc"    (re-find (re-pattern "(?x) a #comment\n b c ") "abc"))))
  (testing "Multiple flags combined: (?ix)"
    (is (= "FOO" (re-find #"(?ix) f o o " "FOO"))))
  (testing "Scoped flag groups (?<flags>:...) rejected with a clear error"
    ;; re-pattern stores the literal pattern string; compile happens
    ;; at re-find time. mino's engine returns NULL on unsupported
    ;; scoped flags, which prim_regex translates into a classified
    ;; MCT001. Verify the throw rather than the storage.
    (is (thrown? (re-find #"(?i:foo)" "FOO")))))

(deftest re-nested-alternation-in-groups
  ;; Groups with internal | alternation: each branch is tried in
  ;; left-to-right order. Without a trailing quantifier the group is
  ;; matched once.
  (testing "(foo|bar) picks the matching branch"
    (is (= ["foo" "foo"] (re-find #"(foo|bar)" "foo")))
    (is (= ["bar" "bar"] (re-find #"(foo|bar)" "bar")))
    (is (nil?            (re-find #"(foo|bar)" "garbage"))))
  (testing "Alternation with surrounding literals"
    (is (= ["abd" "b"] (re-find #"a(b|c)d" "abd")))
    (is (= ["acd" "c"] (re-find #"a(b|c)d" "acd"))))
  (testing "Three-way alternation"
    (is (= ["a" "a"] (re-find #"(a|b|c)" "abc")))
    (is (= ["b" "b"] (re-find #"(a|b|c)" "b"))))
  ;; Groups with internal alternation AND a trailing + quantifier:
  ;; repeat the group, trying each branch each iteration. The capture
  ;; for the group's last successful iteration wins.
  (testing "(foo|bar)+ repeats with mixed branches"
    (is (= ["foobarfoo" "foo"] (re-find #"(foo|bar)+" "foobarfoo")))
    (is (= ["foofoofoo" "foo"] (re-find #"(foo|bar)+" "foofoofoo")))
    (is (= ["barbar"    "bar"] (re-find #"(foo|bar)+" "barbar")))
    (is (nil? (re-find #"(foo|bar)+" "zzz"))))
  (testing "(a|b|c)* matches empty AND a non-empty run"
    (is (= ["abcacb" "b"] (re-matches #"^(a|b|c)*$" "abcacb")))
    (is (= ["" nil]       (re-matches #"^(a|b|c)*$" ""))))
  ;; Simple group with trailing quantifier (no internal alternation):
  ;; same dispatch path; the body is matched repeatedly.
  (testing "(foo)+ repeats a single-branch group"
    (is (= ["foofoofoo" "foo"] (re-find #"(foo)+" "foofoofoo")))
    (is (= ["foo"       "foo"] (re-find #"(foo)+" "foox")))
    (is (nil? (re-find #"(foo)+" "fofo")))))
