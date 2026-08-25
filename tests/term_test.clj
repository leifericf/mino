(require "tests/test")
(require '[clojure.string :as str])
(require '[mino.term :as term])

;; mino.term: ANSI styling as plain data, pinned here as literal
;; escape bytes. The emission order is contract: :bold :italic
;; :underline :reverse, then :fg, then :bg, one SGR sequence; style
;; appends the reset. The color gate is the --color=auto idiom: ansi
;; answers "" and style answers its text unchanged when stdout is not
;; a terminal, unless {:force true} rides the opts map. Byte-level
;; assertions run with {:force true} so they hold under any harness;
;; the gated NO-OP shapes run in a subprocess whose stdout is the
;; capture pipe (deterministically off) next to forced flips.

(def ^:private esc "\033")

(def ^:private windows? (some? (getenv "OS")))

(def ^:private bin
  (or (System/getenv "MINO_TEST_BIN")
      (when (file-exists? "./mino") "./mino")))

(def ^:private fg-palette
  {:black 30 :red 31 :green 32 :yellow 33 :blue 34 :magenta 35
   :cyan 36 :white 37
   :bright-black 90 :bright-red 91 :bright-green 92 :bright-yellow 93
   :bright-blue 94 :bright-magenta 95 :bright-cyan 96 :bright-white 97})

(def ^:private bg-palette
  {:black 40 :red 41 :green 42 :yellow 43 :blue 44 :magenta 45
   :cyan 46 :white 47
   :bright-black 100 :bright-red 101 :bright-green 102
   :bright-yellow 103 :bright-blue 104 :bright-magenta 105
   :bright-cyan 106 :bright-white 107})

(deftest ansi-pins-the-fg-palette-bytes
  (doseq [[c n] fg-palette]
    (is (= (str esc "[" n "m") (term/ansi {:fg c} {:force true}))
        (str "fg code for " c))))

(deftest ansi-pins-the-bg-palette-bytes
  (doseq [[c n] bg-palette]
    (is (= (str esc "[" n "m") (term/ansi {:bg c} {:force true}))
        (str "bg code for " c))))

(deftest ansi-emits-attrs-then-fg-then-bg
  (is (= (str esc "[1m") (term/ansi {:bold true} {:force true})))
  (is (= (str esc "[3m") (term/ansi {:italic true} {:force true})))
  (is (= (str esc "[4m") (term/ansi {:underline true} {:force true})))
  (is (= (str esc "[7m") (term/ansi {:reverse true} {:force true})))
  (is (= (str esc "[31;44m") (term/ansi {:fg :red :bg :blue} {:force true})))
  (is (= (str esc "[1;31m") (term/ansi {:bold true :fg :red} {:force true})))
  (is (= (str esc "[1;3;4;7;37;40m")
         (term/ansi {:reverse true :underline true :italic true
                     :bold true :bg :black :fg :white}
                    {:force true})))
  ;; a false attribute is omitted; a nil attribute is absent
  (is (= (str esc "[31m") (term/ansi {:fg :red :bold false} {:force true})))
  (is (= "" (term/ansi {:bold nil} {:force true})))
  ;; an empty style is no escape at all, not a bare reset
  (is (= "" (term/ansi {} {:force true}))))

(deftest style-wraps-with-reset
  (is (= (str esc "[31mtext" esc "[0m")
         (term/style {:fg :red} "text" {:force true})))
  (is (= (str esc "[1;31mboom" esc "[0m")
         (term/style {:fg :red :bold true} "boom" {:force true})))
  (is (= (str esc "[1;4;97;41mon" esc "[0m")
         (term/style {:bold true :underline true
                      :fg :bright-white :bg :red}
                     "on" {:force true})))
  ;; an empty style leaves the text untouched
  (is (= "plain" (term/style {} "plain" {:force true}))))

(deftest style-returns-plain-text-when-gated-off
  ;; the subprocess stdout is the capture pipe, so the gate is
  ;; deterministically off there; {:force true} flips it back on
  (when (and (not windows?) bin)
    (let [f (str (or (getenv "TMPDIR") "/tmp") "/mino_term_probe.clj")]
      (spit f (str "(require '[mino.term :as term])\n"
                   "(println (term/style {:fg :red} \"text\"))\n"
                   "(println (term/ansi {:fg :red}))\n"
                   "(println (term/style {:fg :red} \"text\" {:force true}))\n"
                   "(println (term/ansi {:fg :red} {:force true}))\n"))
      (let [r (sh bin f)]
        (is (zero? (:exit r)) "probe script loads and runs cleanly")
        (is (= ["text"
                ""
                (str esc "[31mtext" esc "[0m")
                (str esc "[31m")]
               (str/split-lines (str/trim (:out r)))))))))

(defn- bad
  "kind keyword of calling f, or :no-throw when it succeeds."
  [f]
  (try (f) :no-throw (catch e (:kind (ex-data e)))))

(deftest term-throws-on-unknown-style-data
  ;; unknown palette colors and unknown keys are :term/style
  (is (= :term/style (bad (fn [] (term/ansi {:fg :pink} {:force true})))))
  (is (= :term/style (bad (fn [] (term/ansi {:bg :pink} {:force true})))))
  (is (= :term/style (bad (fn [] (term/ansi {:fg :red :blink true}
                                             {:force true})))))
  (is (= :term/style (bad (fn [] (term/ansi {:bold "yes"} {:force true})))))
  ;; validation precedes the gate: bad data throws even when color is
  ;; turned off by a non-tty stdout
  (is (= :term/style (bad (fn [] (term/ansi {:fg :pink})))))
  (is (= :term/style (bad (fn [] (term/style {:fg :pink} "x")))))
  ;; argument-shape problems are :term/opts
  (is (= :term/opts (bad (fn [] (term/ansi [:fg :red])))))
  (is (= :term/opts (bad (fn [] (term/ansi {} :not-a-map)))))
  (is (= :term/opts (bad (fn [] (term/style {} 42 {:force true})))))
  (is (= :term/opts (bad (fn [] (term/style {} "x" :nope))))))

(deftest style-throw-carries-the-offending-key
  (let [e (try (term/ansi {:fg :pink} {:force true}) :no-throw
               (catch e (ex-data e)))]
    (is (map? e))
    (is (= :term/style (:kind e)))
    (is (= :fg (:key e)))))

(run-tests-and-exit)
