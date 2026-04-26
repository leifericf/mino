(require "tests/test")

;; mino's #"..." reader runs through the regular string-escape path,
;; so \d / \s / \w lose their backslash before reaching the regex
;; engine. Tests that rely on those escapes pass the pattern as a
;; literal string ("\\d+" → \d+) until the reader gains a regex-aware
;; escape mode.
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
