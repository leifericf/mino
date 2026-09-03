(require "tests/test")

;; Strict UTF-8 decode at the native edge: bytes->string validates a
;; string or bytes value per the mino.http decode-utf8 contract and
;; returns a mino string, with an optional byte-count cap enforced
;; before any decode work. Overlong forms, UTF-16 surrogates, code
;; points past U+10FFFF, bad continuations, and truncated tails throw.

;; UTF-8 bytes of a string, routed through the hex codec since
;; byte-array does not accept strings.
(defn utf8-bytes [s] (hex-decode (hex-encode s)))

;;; valid multibyte

(deftest ascii-decodes
  (is (= "hi" (bytes->string (byte-array [104 105])))))

(deftest two-byte-decodes
  ;; é = U+00E9 = [0xC3 0xA9]
  (is (= "é" (bytes->string (byte-array [0xC3 0xA9])))))

(deftest three-byte-decodes
  ;; € = U+20AC = [0xE2 0x82 0xAC]
  (is (= "€" (bytes->string (byte-array [0xE2 0x82 0xAC])))))

(deftest four-byte-decodes
  ;; 😀 = U+1F600 = [0xF0 0x9F 0x98 0x80]
  (is (= "😀" (bytes->string (byte-array [0xF0 0x9F 0x98 0x80])))))

(deftest round-trips-mixed-string
  (let [s "aé€😀z"]
    (is (= s (bytes->string (utf8-bytes s))))))

;;; empty and string-arg paths

(deftest empty-input-yields-empty-string
  (is (= "" (bytes->string (byte-array []))))
  (is (= "" (bytes->string ""))))

(deftest accepts-string-argument
  ;; codec_text_arg accepts strings; a valid UTF-8 string round-trips.
  (is (= "héllo" (bytes->string "héllo"))))

;;; invalid sequences

(deftest lone-continuation-throws
  (is (thrown-with-msg? #"invalid UTF-8"
        (bytes->string (byte-array [0x80])))))

(deftest invalid-lead-bytes-throw
  (is (thrown-with-msg? #"invalid UTF-8" (bytes->string (byte-array [0xC0]))))
  (is (thrown-with-msg? #"invalid UTF-8" (bytes->string (byte-array [0xC1]))))
  (is (thrown-with-msg? #"invalid UTF-8" (bytes->string (byte-array [0xF5]))))
  (is (thrown-with-msg? #"invalid UTF-8" (bytes->string (byte-array [0xFF])))))

(deftest bad-continuation-throws
  (is (thrown-with-msg? #"invalid UTF-8"
        (bytes->string (byte-array [0xC3 0x00])))))

;;; truncated multibyte tails

(deftest truncated-three-byte-throws
  (is (thrown-with-msg? #"invalid UTF-8"
        (bytes->string (byte-array [0xE2 0x82])))))

(deftest truncated-four-byte-throws
  (is (thrown-with-msg? #"invalid UTF-8"
        (bytes->string (byte-array [0xF0 0x9F 0x98])))))

;;; overlong encodings

(deftest overlong-encodings-throw
  ;; [0xE0 0x80 0x80] overlong NUL; the E0 lead requires cont 0xA0-0xBF.
  (is (thrown-with-msg? #"invalid UTF-8"
        (bytes->string (byte-array [0xE0 0x80 0x80]))))
  ;; [0xF0 0x80 0x80 0x80] overlong; the F0 lead requires cont 0x90-0xBF.
  (is (thrown-with-msg? #"invalid UTF-8"
        (bytes->string (byte-array [0xF0 0x80 0x80 0x80]))))
  ;; [0xC0 0xAF] overlong '/'; 0xC0 is never a valid lead.
  (is (thrown-with-msg? #"invalid UTF-8"
        (bytes->string (byte-array [0xC0 0xAF])))))

;;; UTF-16 surrogate range

(deftest surrogates-throw
  ;; U+D800 = [0xED 0xA0 0x80]; the ED lead caps cont at 0x9F.
  (is (thrown-with-msg? #"invalid UTF-8"
        (bytes->string (byte-array [0xED 0xA0 0x80]))))
  ;; U+DFFF = [0xED 0xBF 0xBF].
  (is (thrown-with-msg? #"invalid UTF-8"
        (bytes->string (byte-array [0xED 0xBF 0xBF])))))

;;; byte-count bound

(deftest over-cap-input-throws-bounds
  (let [bs (byte-array [104 105 106])]      ; length 3
    (is (thrown-with-msg? #"exceeds max-bytes" (bytes->string bs 2)))))

(deftest cap-classifies-as-bounds
  (let [bs (byte-array [104 105 106])
        k  (try (bytes->string bs 2)
                (catch e (:mino/kind e)))]
    (is (= :eval/bounds k))))

(deftest under-cap-input-succeeds
  (let [bs (byte-array [104 105 106])]      ; length 3
    (is (= "hij" (bytes->string bs 5)))))

(deftest exact-length-cap-succeeds
  (let [bs (byte-array [104 105 106])]      ; length 3
    (is (= "hij" (bytes->string bs 3)))))

(deftest zero-cap-empty-input-succeeds
  (is (= "" (bytes->string (byte-array []) 0))))

(deftest zero-cap-nonempty-input-throws
  (is (thrown-with-msg? #"exceeds max-bytes"
        (bytes->string (byte-array [104]) 0))))

(deftest negative-cap-rejected
  (is (thrown-with-msg? #"non-negative integer"
        (bytes->string (byte-array [104]) -1))))

(deftest bound-checked-before-decode
  ;; Invalid UTF-8 that also exceeds the cap fails as bounds, not
  ;; contract: the cap is enforced before any decode work.
  (let [bs (byte-array [0xFF 0xFF 0xFF])
        k  (try (bytes->string bs 1)
                (catch e (:mino/kind e)))]
    (is (= :eval/bounds k))))

;;; arity and type

(deftest arity-and-type-errors
  (is (thrown? (bytes->string)))
  (is (thrown? (bytes->string (byte-array [104]) 1 2)))
  (is (thrown? (bytes->string 42)))
  (is (thrown? (bytes->string :kw))))

(run-tests-and-exit)
