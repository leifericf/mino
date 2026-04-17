(require "tests/test")

;; Regex reentrancy tests: verify that multiple compiled patterns
;; can coexist without state bleed.

(deftest re-alternating-patterns
  (testing "two patterns alternate matches without interference"
    (let [pat-digit "\\d+"
          pat-alpha "[a-z]+"]
      ;; Alternate between digit and alpha matching
      (is (= "123" (re-find pat-digit "abc123def")))
      (is (= "abc" (re-find pat-alpha "abc123def")))
      (is (= "456" (re-find pat-digit "xyz456uvw")))
      (is (= "xyz" (re-find pat-alpha "xyz456uvw")))
      ;; Repeat to verify no state accumulation
      (is (= "123" (re-find pat-digit "abc123def")))
      (is (= "abc" (re-find pat-alpha "abc123def"))))))

(deftest re-nested-compile-match
  (testing "nested re-find calls in same expression"
    (is (= "123"
           (if (re-find "^[a-z]" "abc")
             (re-find "\\d+" "abc123def")
             "wrong"))))
  (testing "re-find inside map over collection"
    (let [texts ["abc123" "def456" "ghi789"]
          nums  (map (fn (t) (re-find "\\d+" t)) texts)]
      (is (= "123" (first nums)))
      (is (= "456" (second nums)))
      (is (= "789" (nth nums 2))))))

(deftest re-many-patterns-no-bleed
  (testing "ten different patterns used in sequence"
    (is (= "a" (re-find "a" "abc")))
    (is (= "bb" (re-find "b+" "abbc")))
    (is (= "123" (re-find "\\d+" "x123y")))
    (is (= " " (re-find "\\s" "a b")))
    (is (= "HELLO" (re-find "[A-Z]+" "xxHELLOyy")))
    (is (= "test" (re-find "\\w+" "  test  ")))
    (is (= nil (re-find "zzz" "abc")))
    (is (= "abc" (re-find "^abc" "abcdef")))
    (is (= "def" (re-find "def$" "abcdef")))
    (is (= "12" (re-find "[0-9][0-9]" "a12b")))))

(deftest re-pattern-reuse
  (testing "same pattern used repeatedly produces consistent results"
    (let [pat "\\d+"
          results (map (fn (s) (re-find pat s))
                       ["a1b" "c22d" "e333f" "no-match" "g4444h"])]
      (is (= "1" (nth results 0)))
      (is (= "22" (nth results 1)))
      (is (= "333" (nth results 2)))
      (is (= nil (nth results 3)))
      (is (= "4444" (nth results 4))))))

(deftest re-matches-reentrancy
  (testing "re-matches and re-find interleaved"
    (is (= "12345" (re-matches "\\d+" "12345")))
    (is (= "abc" (re-find "[a-z]+" "123abc")))
    (is (= nil (re-matches "\\d+" "123abc")))
    (is (= "123" (re-find "\\d+" "123abc")))
    (is (= "hello" (re-matches "[a-z]+" "hello")))
    (is (= "HELLO" (re-find "[A-Z]+" "xxHELLOyy")))))
