(require "tests/test")

;; with-out-str capture must stay near-linear in total output size.
;; The *out* sink is a growable buffer whose appends are O(1)
;; amortized. A sink that copies the whole accumulated output on
;; every print costs O(n^2) over n prints: at the sizes below that
;; is tens of seconds, blowing the budgets several times over, while
;; the amortized buffer clears them with two orders of magnitude of
;; slack so CI noise cannot flip them.

(def ^:private line "0123456789012345678901234567890123456789")

(defn- capture-ms
  "Milliseconds to capture n println calls of the 40-char line;
  asserts the captured length along the way (41 bytes per line)."
  [n]
  (let [t0 (nano-time)
        s  (with-out-str (dotimes [_ n] (println line)))
        ms (quot (- (nano-time) t0) 1000000)]
    (is (= (* n 41) (count s)) (str "captured length at n=" n))
    ms))

;; A sanitizer build (MINO_SLOW_HOST) runs ~10x slower; relax the
;; absolute budget there. The quadratic cost at n=200k is ~37s native,
;; so even the relaxed budget keeps its teeth.
(def ^:private budget-ms (if (getenv "MINO_SLOW_HOST") 30000 6000))

(deftest with-out-str-capture-stays-within-budget
  (let [ms (capture-ms 200000)]
    (is (< ms budget-ms) (str "200k println capture took " ms "ms"))))

(deftest with-out-str-capture-grows-near-linearly
  ;; 4x the prints must cost well under 10x the time (linear is ~4x,
  ;; quadratic ~16x). The denominator is floored at 25ms so a fast
  ;; host's timer noise cannot inflate the ratio.
  (let [t1 (capture-ms 50000)
        t4 (capture-ms 200000)]
    (is (< t4 (* 10 (max t1 25)))
        (str "50k->200k grew " t1 "ms -> " t4 "ms"))))

(deftest with-out-str-content-is-exact
  (let [s (with-out-str (dotimes [_ 1000] (println line)))]
    (is (= 41000 (count s)))
    (is (= (str line "\n") (subs s 0 41)))
    (is (= (str line "\n") (subs s 40959 41000)))))

(deftest with-out-str-nests-independently
  (let [inner (atom nil)
        outer (with-out-str
                (print "a")
                (reset! inner (with-out-str (print "x") (print "y")))
                (print "b"))]
    (is (= "xy" @inner))
    (is (= "ab" outer))))

(deftest with-out-str-captures-the-print-method-path
  ;; prn routes through the print-method hook, whose per-value capture
  ;; uses the same buffer mechanism.
  (is (= "\"s\"\n" (with-out-str (prn "s"))))
  (is (= "{:a [1 2]}\n" (with-out-str (prn {:a [1 2]})))))

(deftest with-out-str-handles-multibyte-content
  (let [s (with-out-str (print "é中") (print "!"))]
    (is (= "é中!" s))
    (is (= 3 (count s)))))

(deftest string-atom-sink-still-captures
  ;; The documented embedder sink shape: *out* bound directly to an
  ;; atom holding a string appends printed output to the atom.
  (let [a (atom "")]
    (binding [*out* a]
      (print "em")
      (println "bed"))
    (is (= "embed\n" @a))))

(run-tests-and-exit)
