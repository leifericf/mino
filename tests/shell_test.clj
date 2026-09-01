(require "tests/test")

;; Shell quoting and word-splitting helpers.
;;
;; (mino.shell/quote s) wraps s in POSIX single quotes so the result is
;; safe as a shell word regardless of content.
;; (mino.shell/split s) splits a shell-style command string into a vector
;; of words, honouring single quotes, double quotes, and backslash escapes.

(require '[mino.shell :as shell])

;;;; quote: adversarial safety vectors

(deftest shell-quote-plain-word
  (is (= "'hello'" (shell/quote "hello"))))

(deftest shell-quote-word-with-space
  (is (= "'hello world'" (shell/quote "hello world"))))

(deftest shell-quote-empty-string
  (is (= "''" (shell/quote ""))))

(deftest shell-quote-embedded-single-quote
  ;; 'it'\''s fine' is the POSIX-safe encoding of: it's fine
  (is (= "'it'\\''s fine'" (shell/quote "it's fine"))))

(deftest shell-quote-dollar-sign-is-not-expanded
  ;; $(cmd) and `cmd` must be literal inside single quotes.
  (is (= "'$(rm -rf /)'" (shell/quote "$(rm -rf /)"))))

(deftest shell-quote-backtick-is-not-expanded
  (is (= "'`id`'" (shell/quote "`id`"))))

(deftest shell-quote-newline-stays-literal
  (is (= "'\n'" (shell/quote "\n"))))

(deftest shell-quote-double-quote-is-safe
  (is (= "'say \"hi\"'" (shell/quote "say \"hi\""))))

(deftest shell-quote-backslash-is-safe
  (is (= "'a\\b'" (shell/quote "a\\b"))))

(deftest shell-quote-backslash-is-preserved
  ;; A bare backslash inside a single-quoted word comes out verbatim.
  (is (= "'a\\\\b'" (shell/quote "a\\\\b"))))

(deftest shell-quote-all-special-chars
  ;; Verify a string that contains every common shell metacharacter
  ;; round-trips through quote+split back to itself.
  (let [s "& | ; < > ( ) { } $ ` \\ ! #"]
    (is (= [s] (shell/split (shell/quote s))))))

;;;; split: basic word splitting

(deftest shell-split-empty-string
  (is (= [] (shell/split ""))))

(deftest shell-split-single-word
  (is (= ["echo"] (shell/split "echo"))))

(deftest shell-split-two-words
  (is (= ["echo" "hello"] (shell/split "echo hello"))))

(deftest shell-split-extra-whitespace
  (is (= ["a" "b" "c"] (shell/split "  a   b\tc  "))))

;;;; split: single-quote handling

(deftest shell-split-single-quoted-string
  (is (= ["echo" "hello world"] (shell/split "echo 'hello world'"))))

(deftest shell-split-single-quote-prevents-dollar-expansion
  (is (= ["$(id)"] (shell/split "'$(id)'"))))

(deftest shell-split-single-quote-prevents-backtick-expansion
  (is (= ["`id`"] (shell/split "'`id`'"))))

(deftest shell-split-adjacent-single-quote-produces-single-word
  ;; 'hello''world' is one word: helloworld
  (is (= ["helloworld"] (shell/split "'hello''world'"))))

;;;; split: double-quote handling

(deftest shell-split-double-quoted-string
  (is (= ["echo" "hello world"] (shell/split "echo \"hello world\""))))

(deftest shell-split-double-quote-backslash-escapes
  ;; Inside double quotes, \\, \", \$, \`, \newline are special.
  (is (= ["a\"b"] (shell/split "\"a\\\"b\"")))
  (is (= ["a\\b"] (shell/split "\"a\\\\b\""))))

(deftest shell-split-double-quote-non-special-backslash-preserved
  ;; \n inside double quotes is \n (backslash kept), not a newline.
  (is (= ["a\\nb"] (shell/split "\"a\\nb\""))))

;;;; split: backslash outside quotes

(deftest shell-split-bare-backslash-escapes-space
  (is (= ["hello world"] (shell/split "hello\\ world"))))

(deftest shell-split-bare-backslash-escapes-newline
  (is (= ["ab"] (shell/split "a\\\nb"))))

;;;; split: comment handling

(deftest shell-split-comment-ignored-when-option-true
  (is (= ["echo"] (shell/split "echo # this is a comment" {:comments true}))))

(deftest shell-split-comment-hash-mid-word-not-treated-as-comment
  ;; A # that is part of a word (not preceded by whitespace) is not
  ;; a comment start even with {:comments true}.
  (is (= ["foo#bar"] (shell/split "foo#bar" {:comments true}))))

(deftest shell-split-comment-hash-without-option-is-literal
  (is (= ["echo" "#" "comment"] (shell/split "echo # comment"))))

;;;; split: error cases — unterminated quotes

(deftest shell-split-unterminated-single-quote-throws
  (is (= :shell/parse
         (try (shell/split "'unterminated")
              (catch e (:mino/kind e))))))

(deftest shell-split-unterminated-double-quote-throws
  (is (= :shell/parse
         (try (shell/split "\"unterminated")
              (catch e (:mino/kind e))))))

(deftest shell-split-trailing-backslash-throws
  (is (= :shell/parse
         (try (shell/split "abc\\")
              (catch e (:mino/kind e))))))

;;;; quote+split round-trip over adversarial strings

(deftest shell-round-trip-newline
  (let [s "line1\nline2"]
    (is (= [s] (shell/split (shell/quote s))))))

(deftest shell-round-trip-tab
  (let [s "a\tb"]
    (is (= [s] (shell/split (shell/quote s))))))

(deftest shell-round-trip-multiple-single-quotes
  (let [s "it's a dog's life"]
    (is (= [s] (shell/split (shell/quote s))))))

(deftest shell-round-trip-dollar-parens
  (let [s "$(whoami)"]
    (is (= [s] (shell/split (shell/quote s))))))

(run-tests-and-exit)
