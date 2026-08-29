(require "tests/test")
(require '[clojure.string :as str])
(require '[mino.term :as term])

;; mino.term/progress: the single-line tqdm-shaped bar. render-progress
;; is the pure data-in render fn (bar map + explicit width -> string)
;; and every assertion here pins literal bytes, so the shapes are
;; contract. progress is the gated shell: shaped when stdout is a
;; terminal or {:force true}, plain label+pct when it is not, width
;; from {:width n} or terminal-width. Gated shapes run in a subprocess
;; (capture pipe = deterministically not a tty), the term_test
;; strategy; the COLUMNS variant proves the width flows through
;; terminal-width's env fallback end to end.

(def ^:private windows? (some? (getenv "OS")))

(def ^:private bin
  (or (System/getenv "MINO_TEST_BIN")
      (when (file-exists? "./mino") "./mino")))

(defn- rep
  [n s]
  (str/join (repeat n s)))

(deftest render-pins-the-shape-at-width-80
  ;; head is 9 chars ("demo" space " 50%"), so the bar room is
  ;; 80-9-2 = 69; half of 69 is 34.5 eighths-rounded to 34 full
  ;; blocks, a half block, and 34 spaces.
  (is (= (str "demo  50%|" (rep 34 "█") "▌" (rep 34 " ") "|")
         (term/render-progress {:label "demo" :ratio 0.5} 80)))
  (is (= 80 (count (term/render-progress {:label "demo" :ratio 0.5} 80)))
      "the rendered line fills the width exactly"))

(deftest render-pins-the-shape-at-width-40
  (is (= (str "demo  50%|" (rep 14 "█") "▌" (rep 14 " ") "|")
         (term/render-progress {:label "demo" :ratio 0.5} 40)))
  (is (= 40 (count (term/render-progress {:label "demo" :ratio 0.5} 40)))))

(deftest render-pins-the-shape-at-width-0
  ;; no room for the adornment at all: the bar and its rails drop,
  ;; the label and percentage survive
  (is (= "demo  50%"
         (term/render-progress {:label "demo" :ratio 0.5} 0))))

(deftest render-quarter-and-three-quarter-shapes
  ;; head "t  75%" is 6 chars, room 40-6-2 = 32: exact quarters land
  ;; on whole cells (24 of 32, then 8 of 32)
  (is (= (str "t  75%|" (rep 24 "█") (rep 8 " ") "|")
         (term/render-progress {:label "t" :ratio 0.75} 40)))
  (is (= (str "t  25%|" (rep 8 "█") (rep 24 " ") "|")
         (term/render-progress {:label "t" :ratio 0.25} 40)))
  ;; width 39 leaves room 31, which is not divisible by 4: the
  ;; remainders render as partial blocks (2/8 and 6/8)
  (is (= (str "t  75%|" (rep 23 "█") "▎" (rep 7 " ") "|")
         (term/render-progress {:label "t" :ratio 0.75} 39)))
  (is (= (str "t  25%|" (rep 7 "█") "▊" (rep 23 " ") "|")
         (term/render-progress {:label "t" :ratio 0.25} 39))))

(deftest render-extreme-ratios
  (is (= "x   0%|            |"
         (term/render-progress {:label "x" :ratio 0} 20)))
  (is (= "x 100%|████████████|"
         (term/render-progress {:label "x" :ratio 1} 20))))

(deftest render-partial-eighth-and-empty-label
  ;; 1/16 of a 2-char bar is one eighth: the smallest partial block
  (is (= "  6%|▏ |"
         (term/render-progress {:label "" :ratio (/ 1 16)} 8)))
  ;; an empty label drops the separator space: head shrinks to 4
  ;; chars, room grows to 74, and 37 of 74 is exact so no partial
  (is (= (str " 50%|" (rep 37 "█") (rep 37 " ") "|")
         (term/render-progress {:label "" :ratio 0.5} 80))))

(deftest render-is-pure
  (let [bar {:label "demo" :ratio 0.5}]
    (is (= (term/render-progress bar 40) (term/render-progress bar 40)))))

(defn- bad
  "kind keyword of calling f, or :no-throw when it succeeds. Reads the
  promoted :mino/kind (ADR 37), so classed catch dispatches on it."
  [f]
  (try (f) :no-throw (catch e (:mino/kind e))))

(deftest progress-validates-its-data
  (is (= :term/opts (bad (fn [] (term/render-progress {:label 42
                                                       :ratio 0.5} 40)))))
  (is (= :term/opts (bad (fn [] (term/render-progress {:label "x"} 40)))))
  (is (= :term/opts (bad (fn [] (term/render-progress {:label "x"
                                                       :ratio 2} 40)))))
  (is (= :term/opts (bad (fn [] (term/render-progress {:label "x"
                                                       :ratio -0.1} 40)))))
  (is (= :term/opts (bad (fn [] (term/render-progress {:label "x"
                                                       :ratio 1.1} 40)))))
  (is (= :term/opts (bad (fn [] (term/render-progress {:label "x"
                                                       :ratio :half}
                                                      40)))))
  (is (= :term/opts (bad (fn [] (term/render-progress [:label "x"] 40)))))
  ;; NaN fails the 0..1 comparison and throws like any other bad ratio
  (is (= :term/opts (bad (fn [] (term/render-progress
                                 {:label "x" :ratio (/ 0.0 0.0)} 40)))))
  ;; the width must be a non-negative integer
  (is (= :term/opts (bad (fn [] (term/render-progress {:label "x"
                                                       :ratio 0.5} :w)))))
  (is (= :term/opts (bad (fn [] (term/render-progress {:label "x"
                                                       :ratio 0.5} -1)))))
  (is (= :term/opts (bad (fn [] (term/render-progress {:label "x"
                                                       :ratio 0.5} 3.5)))))
  ;; the shell fn revalidates: bad bar and bad opts throw either way
  (is (= :term/opts (bad (fn [] (term/progress {:label "x" :ratio 2})))))
  (is (= :term/opts (bad (fn [] (term/progress {:label "x" :ratio 0.5}
                                               :nope))))))

(deftest progress-opts-dispatch-classed-catch
  ;; ADR 37: :term/opts is a real class a classed catch can select, and
  ;; the offending arg rides ex-data, not a :kind in the detail map
  (is (= :caught (try (term/render-progress {:label "x" :ratio 2} 40)
                      (catch :term/opts _ :caught))))
  (let [e (try (term/render-progress [:label "x"] 40) :no-throw
               (catch e (ex-data e)))]
    (is (map? e))
    (is (nil? (:kind e)))
    (is (= [:label "x"] (:arg e)))))

(deftest progress-plain-when-not-tty-shaped-when-forced
  (when (and (not windows?) bin)
    (let [f (str (or (getenv "TMPDIR") "/tmp") "/mino_term_progress_probe.clj")
          w40 (str "demo  50%|" (rep 14 "█") "▌" (rep 14 " ") "|")]
      (spit f (str "(require '[mino.term :as term])\n"
                   "(println (term/progress {:label \"demo\" :ratio 0.5}))\n"
                   "(println (term/progress {:label \"demo\" :ratio 0.5}"
                   " {:force true :width 40}))\n"
                   "(println (term/progress {:label \"demo\" :ratio 0.5}"
                   " {:width 40}))\n"))
      ;; stdout is the capture pipe: plain by default, shaped when
      ;; forced, plain again when only the width is pinned
      (let [r (sh bin f)]
        (is (zero? (:exit r)) "probe script loads and runs cleanly")
        (is (= ["demo  50%" w40 "demo  50%"]
               (str/split-lines (str/trim (:out r)))))))
    ;; COLUMNS feeds terminal-width's env fallback, so a forced bar
    ;; with no explicit width still lands at 40 columns
    (let [f (str (or (getenv "TMPDIR") "/tmp")
                 "/mino_term_progress_cols_probe.clj")
          w40 (str "demo  50%|" (rep 14 "█") "▌" (rep 14 " ") "|")]
      (spit f (str "(require '[mino.term :as term])\n"
                   "(println (term/progress {:label \"demo\" :ratio 0.5}"
                   " {:force true}))\n"))
      (let [r (sh "sh" "-c" (str "env -u COLUMNS -u ROWS COLUMNS=40 "
                                 bin " " f))]
        (is (zero? (:exit r)))
        (is (= [w40] (str/split-lines (str/trim (:out r)))))))))

(run-tests-and-exit)
