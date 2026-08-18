(require "tests/test")

;; Per-character indexing over strings (subs, count) must stay
;; near-linear in string length. Both primitives cache the
;; codepoint count on the string value, so a loop calling subs once
;; per character pays O(1) per call on ASCII content instead of
;; rescanning the whole string. The budgets carry two orders of
;; magnitude of slack over the fixed path so CI noise cannot flip
;; them, while a quadratic regression (8s+ at these sizes) clears
;; them by far.

(defn- drain-subs
  "Call (subs s i (inc i)) for every index of the pre-built s;
  returns the count of single-character results."
  [s n]
  (loop [i 0 acc 0]
    (if (>= i n)
      acc
      (recur (inc i)
             (if (= 1 (count (subs s i (inc i))))
               (inc acc) acc)))))

(defn- drain-count [s n]
  (loop [i 0 acc 0]
    (if (>= i n)
      acc
      (recur (inc i) (rem (+ acc (count s)) 1000000007)))))

(def ascii-100k (apply str (repeat 100000 "x")))

(def budget-ms 2000)

(deftest per-char-subs-stays-within-budget
  (let [t0 (nano-time)
        n  (drain-subs ascii-100k 100000)
        ms (quot (- (nano-time) t0) 1000000)]
    (is (= 100000 n))
    (is (< ms budget-ms) (str "subs drain took " ms "ms"))))

(deftest repeated-count-stays-within-budget
  (let [t0 (nano-time)
        n  (drain-count ascii-100k 100000)
        ms (quot (- (nano-time) t0) 1000000)]
    ;; 100000 iterations summing 100000 each, mod 1e9+7.
    (is (= 999999937 n) "mod-sum of 100k counts of a 100k string")
    (is (< ms budget-ms) (str "count drain took " ms "ms"))))

(deftest subs-and-count-still-codepoint-accurate
  ;; The cache must not change semantics on multi-byte content.
  (let [s "a\u00e9b\u4e2d"]
    (is (= 4 (count s)))
    (is (= "\u4e2d" (subs s 3 4)))
    (is (= "a\u00e9b\u4e2d" (subs s 0)))
    (is (= "\u00e9b" (subs s 1 3)))))

(deftest repeated-count-is-stable-across-calls
  ;; Cached count and fresh count agree on mixed content.
  (let [s "x\u00e9y\u4e2dz"]
    (is (= (count s) (count s)))
    (is (= 5 (count s)))))

(run-tests-and-exit)
