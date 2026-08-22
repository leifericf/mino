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

(defn- scan-ms [text expected-tokens]
  (let [t0   (nano-time)
        toks (doall (re-seq scan-re text))]
    (is (= expected-tokens (count toks)))
    (quot (- (nano-time) t0) 1000000)))

(defn- scan-ratio
  "Time re-seq over one text and its double; return the time ratio.
  A linear scan doubles, a quadratic one quadruples, so the ratio is
  machine- and sanitizer-independent evidence of the scan shape."
  [make-text tokens-per-unit]
  (let [small (make-text 4000)
        big   (make-text 8000)]
    (let [t-small (scan-ms small (* 4000 tokens-per-unit))
          t-big   (scan-ms big (* 8000 tokens-per-unit))]
      {:ratio (max 1.0 (if (zero? t-big) 1.0 (/ (inc t-big) (inc t-small))))
       :small-ms t-small :big-ms t-big})))

(deftest re-seq-ascii-scan-stays-linear
  (let [{:keys [ratio small-ms big-ms]} (scan-ratio
                                          #(apply str (repeat % "abc 123 de 7 fghi "))
                                          10)]
    (is (< ratio 3.2)
        (str "ascii re-seq scaling ratio " ratio
             " (" small-ms "ms -> " big-ms "ms)"))))

(deftest re-seq-mixed-scan-stays-linear
  (let [{:keys [ratio small-ms big-ms]} (scan-ratio
                                          #(apply str (repeat % "abc é 123 中 de 7 fghi "))
                                          12)]
    (is (< ratio 3.2)
        (str "mixed re-seq scaling ratio " ratio
             " (" small-ms "ms -> " big-ms "ms)"))))

(deftest re-seq-tiles-the-text
  ;; Contiguity: the matches tile the text with no gaps, so the
  ;; lengths sum to the text length.
  (let [toks (doall (re-seq scan-re ascii-text))]
    (is (= (count ascii-text)
           (reduce + 0 (map count toks))))))

(deftest re-seq-matches-in-order
  (is (= ["abc" " " "123"] (take 3 (re-seq scan-re "abc 123 de")))
      "matches arrive in document order"))

(run-tests-and-exit)
