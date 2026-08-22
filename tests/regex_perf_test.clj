(require "tests/test")

;; re-seq is a sequential scan: each step must cost the bytes it
;; advances, not the distance from the start of the text. The old
;; re-find-from recomputed the text's full codepoint count and
; walked codepoints from 0 on every call, making re-seq quadratic
;; in text length even on pure ASCII; these budgets sit far above
;; the linear scan and far below the quadratic blowup.

(def ^:private scan-re #"[a-z]+|\d+|\s+")

(def ^:private ascii-text
  (apply str (repeat 8000 "abc 123 de 7 fghi ")))

(def ^:private mixed-text
  (apply str (repeat 8000 "abc é 123 中 de 7 fghi ")))

(deftest re-seq-ascii-scan-within-budget
  (let [t0    (nano-time)
        toks  (doall (re-seq scan-re ascii-text))
        ms    (quot (- (nano-time) t0) 1000000)]
    (is (= 80000 (count toks)))
    ;; Contiguity: the matches tile the text with no gaps, so the
    ;; lengths sum to the text length.
    (is (= (count ascii-text)
           (reduce + 0 (map count toks))))
    (is (< ms 8000) (str "ascii re-seq took " ms "ms"))))

(deftest re-seq-mixed-scan-within-budget
  (let [t0    (nano-time)
        toks  (doall (re-seq scan-re mixed-text))
        ms    (quot (- (nano-time) t0) 1000000)]
    (is (= 96000 (count toks))
        "mixed text still tokenizes to a fixed count")
    ;; Non-ASCII codepoints fall to the single-character catch-all
    ;; only if the pattern has one; scan-re does not, so é and 中
    ;; split nothing here: every match is ASCII, and the multibyte
    ;; codepoints are simply absent from the matches. The counts
    ;; above pin the shape.
    (is (< ms 8000) (str "mixed re-seq took " ms "ms"))))

(deftest re-seq-matches-in-order
  (is (= ["abc" " " "123"] (take 3 (re-seq scan-re "abc 123 de")))
      "matches arrive in document order"))

(run-tests-and-exit)
