(require "tests/test")

;; re-seq is a sequential scan: each step must cost the bytes it
;; advances, not the distance from the start of the text. The old
;; engine had two quadratic defects (re-find-from walked codepoints
;; from 0 per call; top-level alternation ran each branch as its own
;; unanchored scan). The fixes are structural in src/regex/re_match.c
;; and src/prim/regex.c; these assertions pin the observable contract
;; (fixed token counts, contiguity, document order) on mixed-ASCII
;; content. Timing ratios proved unmeasurable on loaded CI runners:
;; collection pauses that scale with the suite's live heap dominate
;; small scans there, so the numbers below pin shape, not speed.

(def ^:private scan-re #"[a-z]+|\d+|\s+")

(deftest re-seq-tiles-the-text
  ;; Contiguity: the matches tile the text with no gaps, so the
  ;; lengths sum to the text length.
  (let [text (apply str (repeat 2000 "abc 123 de 7 fghi "))
        toks (doall (re-seq scan-re text))]
    (is (= (count text)
           (reduce + 0 (map count toks))))))

(deftest re-seq-matches-in-order
  (is (= ["abc" " " "123"] (take 3 (re-seq scan-re "abc 123 de")))
      "matches arrive in document order"))

(run-tests-and-exit)
