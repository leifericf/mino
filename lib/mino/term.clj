(ns mino.term
  "ANSI terminal styling as plain data, the --color=auto idiom as the
  default.

  (require '[mino.term :as term])
  (term/style {:fg :red :bold true} \"error\")
  ;; => \"\\033[1;31merror\\033[0m\" when stdout is a terminal,
  ;; => \"error\" unchanged when it is not (pipe, file, CI log)
  (term/style {:fg :red} \"error\" {:force true})  ; always styled
  (term/ansi {:fg :red})                           ; => \"\\033[31m\"

  A style spec is a plain map: :fg and :bg name colors from the
  16-color palette (:black :red :green :yellow :blue :magenta :cyan
  :white plus the :bright-* variants) and :bold :italic :underline
  :reverse carry booleans. ansi answers the escape string for a spec
  (\"\" for an empty spec); style wraps one text string in the escape
  and the reset \\033[0m. SGR parameters emit in a fixed order, :bold
  :italic :underline :reverse then :fg then :bg, so the bytes are
  stable data.

  The gate: nothing is emitted unless (tty? :stdout) is true, the
  --color=auto default; {:force true} in the trailing opts map
  overrides it for redirects that still want the bytes. Bad style
  data throws a diagnostic with :mino/kind :term/style and the
  offending :key; bad argument shapes throw :mino/kind :term/opts.

  Progress bars:

  (term/progress {:label \"downloading\" :ratio 0.42})
  ;; => \"downloading  42%|████████▏        |\" on a terminal
  ;; => \"downloading  42%\" when stdout is not a terminal
  (term/render-progress {:label \"downloading\" :ratio 0.42} 40)
  ;; the pure render at an explicit width, gate-free

  progress renders one progress line: label, percentage, then the
  bar between rails, sized so the whole line fills the width (from
  {:width n} or terminal-width). The bar fills in eighth blocks
  (U+2588 and the U+258F..U+2589 partials). render-progress is the
  pure data-in fn; progress is the gated shell that answers the
  plain label-and-percentage line when stdout is not a terminal and
  {:force true} is absent. :ratio must be a number within 0..1;
  anything else throws a diagnostic with :mino/kind :term/opts.

  Terminal capabilities:

  (term/tty? :stdout)      ; is a standard stream a terminal?
  (term/terminal-width)    ; columns (ioctl, else COLUMNS, else 80)
  (term/terminal-height)   ; rows (ioctl, else ROWS, else 24)
  (term/size)              ; => {:cols 80 :rows 24}
  (term/read-password)     ; a no-echo line read from the terminal
  (term/prompt msg opts)   ; read a line, re-prompt until valid
  (term/confirm msg opts)  ; read y/n, re-prompt until valid

  tty?, terminal-width, and terminal-height re-export the floor prims
  of the same name so callers of this namespace never reach back into
  clojure.core for the --color=auto gate. size bundles the two getters
  into one {:cols :rows} map. prompt reads a line with optional
  :default, :validate, and :password support; confirm reads a y/n
  answer; both re-prompt on invalid input up to :max-tries (default 3)
  and throw :mino/kind :term/prompt on cap-exhaustion."
  (:require [clojure.string :as str]))

(def ^:private esc "\033")

;;;; Terminal capabilities

(defn tty?
  "Returns true when the given standard stream (:stdout, :stderr, or
  :stdin) is attached to a terminal, false for files, pipes, and other
  redirects. The floor of the --color=auto gate this namespace applies
  by default."
  [stream]
  (clojure.core/tty? stream))

(defn terminal-width
  "Returns the terminal width in columns: the ioctl size when a
  standard stream is a terminal (stdout first), else the COLUMNS
  environment variable when it is a plain numeric value, else 80."
  []
  (clojure.core/terminal-width))

(defn terminal-height
  "Returns the terminal height in rows: the ioctl size when a standard
  stream is a terminal (stdout first), else the ROWS environment
  variable when it is a plain numeric value, else 24."
  []
  (clojure.core/terminal-height))

(defn size
  "The terminal size as one map: {:cols <terminal-width> :rows
  <terminal-height>}. Both dimensions fall back independently, so the
  map always carries positive integers."
  []
  {:cols (terminal-width) :rows (terminal-height)})

(defn read-password
  "Reads one line from stdin with terminal echo turned off, so a typed
  secret is not shown, and returns it without the trailing newline. The
  terminal mode is restored on every exit path. When stdin is not a
  terminal this throws :mino/kind :term/not-a-tty; pass {:allow-pipe
  true} to read a piped secret with a plain line read instead."
  ([] (clojure.core/read-password))
  ([opts] (clojure.core/read-password opts)))

(def ^:private fg-codes
  {:black 30 :red 31 :green 32 :yellow 33 :blue 34 :magenta 35
   :cyan 36 :white 37
   :bright-black 90 :bright-red 91 :bright-green 92 :bright-yellow 93
   :bright-blue 94 :bright-magenta 95 :bright-cyan 96 :bright-white 97})

(def ^:private bg-codes
  {:black 40 :red 41 :green 42 :yellow 43 :blue 44 :magenta 45
   :cyan 46 :white 47
   :bright-black 100 :bright-red 101 :bright-green 102
   :bright-yellow 103 :bright-blue 104 :bright-magenta 105
   :bright-cyan 106 :bright-white 107})

;; The vector order is the emission order.
(def ^:private attr-codes
  [[:bold 1] [:italic 3] [:underline 4] [:reverse 7]])

(def ^:private attr-keys #{:bold :italic :underline :reverse})

;;;; Errors

(defn- term-fail
  "Throws a classified mino.term diagnostic (ADR 37): :mino/kind names
  the error class so classed catch dispatches on it, :mino/message the
  human string ex-message returns, :mino/data the detail ex-data reads."
  [kind code msg data]
  (throw {:mino/kind kind :mino/code code :mino/message msg
          :mino/data data}))

(defn- throw-style
  [msg spec key]
  (term-fail :term/style "MTS001" (str "mino.term: " msg)
             {:spec spec :key key}))

(defn- throw-opts
  [msg arg]
  (term-fail :term/opts "MTO001" (str "mino.term: " msg)
             {:arg arg}))

(defn- check-spec
  "spec must be a map whose keys are the style keys; attribute values
  must be booleans (nil reads as absent)."
  [spec]
  (when-not (map? spec)
    (throw-opts "style spec must be a map" spec))
  (doseq [k (keys spec)]
    (when-not (or (= k :fg) (= k :bg) (contains? attr-keys k))
      (throw-style (str "unknown style key " (pr-str k)) spec k)))
  (doseq [k attr-keys]
    (let [v (get spec k)]
      (when (and (some? v) (not (or (true? v) (false? v))))
        (throw-style (str "style attribute " (pr-str k)
                          " must be true or false, got " (pr-str v))
                     spec k)))))

(defn- color-code
  "Palette lookup for :fg or :bg; nil passes through as absent."
  [table which spec v]
  (cond
    (nil? v)                nil
    (contains? table v)     (get table v)
    :else                   (throw-style
                             (str (name which) " color " (pr-str v)
                                  " is not in the 16-color palette")
                             spec which)))

(defn- spec-codes
  "Validated spec -> the vector of SGR codes it denotes, in the fixed
  emission order. An empty spec answers the empty vector."
  [spec]
  (check-spec spec)
  (let [attrs (reduce (fn [acc [k code]]
                        (if (get spec k) (conj acc code) acc))
                      [] attr-codes)
        fg    (color-code fg-codes :fg spec (get spec :fg))
        bg    (color-code bg-codes :bg spec (get spec :bg))
        with-fg (if fg (conj attrs fg) attrs)]
    (if bg (conj with-fg bg) with-fg)))

(defn- check-opts
  [opts]
  (when (and (some? opts) (not (map? opts)))
    (throw-opts "opts must be a map" opts)))

(defn- color-on?
  "The --color=auto gate: stdout is a terminal, or the caller forced
  color on."
  [opts]
  (or (get opts :force) (tty? :stdout)))

(defn- escape
  [codes]
  (if (= [] codes)
    ""
    (str esc "[" (str/join ";" (map str codes)) "m")))

(defn ansi
  "The ANSI escape string for the style spec: one SGR sequence with
  the codes in the fixed order :bold :italic :underline :reverse :fg
  :bg. Answers \"\" for an empty spec and, like style, when stdout is
  not a terminal unless {:force true} rides opts."
  ([spec] (ansi spec nil))
  ([spec opts]
   (check-opts opts)
   (let [codes (spec-codes spec)]
     (if (color-on? opts) (escape codes) ""))))

(defn style
  "text wrapped in the spec's escape sequence and the reset \\033[0m.
  The text is returned unchanged when the spec is empty or when color
  is gated off (stdout not a terminal, no {:force true}); styled bytes
  only ever come out of a terminal or a forced call."
  ([spec text] (style spec text nil))
  ([spec text opts]
   (check-opts opts)
   (when-not (string? text)
     (throw-opts "style text must be a string" text))
   (let [codes (spec-codes spec)]
     (if (or (= [] codes) (not (color-on? opts)))
       text
       (str (escape codes) text esc "[0m")))))

;;;; progress bars

(def ^:private full-block (char 0x2588))

;; Index 0 is the one-eighth block; frac n takes (nth blocks (dec n)).
(def ^:private eighth-blocks
  [(char 0x258F) (char 0x258E) (char 0x258D) (char 0x258C)
   (char 0x258B) (char 0x258A) (char 0x2589)])

(defn- bar->parts
  "Validated bar map -> [label ratio]."
  [bar]
  (when-not (map? bar)
    (throw-opts "progress bar must be a map" bar))
  (let [label (get bar :label)
        ratio (get bar :ratio)]
    (when-not (string? label)
      (throw-opts "progress :label must be a string" bar))
    (when-not (number? ratio)
      (throw-opts "progress :ratio must be a number" bar))
    (when-not (<= 0 ratio 1)
      (throw-opts "progress :ratio must be within 0..1" bar))
    [label ratio]))

(defn- pad3
  "n (0..100) right-aligned in three columns."
  [n]
  (let [s (str n)]
    (cond
      (= 3 (count s)) s
      (= 2 (count s)) (str " " s)
      :else           (str "  " s))))

(defn- pct-of
  "Percentage for a ratio; the epsilon keeps 0.42 a 42 rather than a
  41 (0.42 is not exact in binary floating point)."
  [ratio]
  (int (Math/floor (+ (* 100.0 ratio) 0.000001))))

(defn- label-prefix
  [label]
  (if (= "" label) "" (str label " ")))

(defn- bar-chars
  "n cells of bar at ratio: full blocks, one eighth-partial, spaces."
  [n ratio]
  (let [eighths (Math/round (* 8 n ratio))
        full    (quot eighths 8)
        frac    (rem eighths 8)
        partial (if (pos? frac) (nth eighth-blocks (dec frac)) "")
        empties (- n full (if (pos? frac) 1 0))]
    (str (str/join (repeat full full-block))
         partial
         (str/join (repeat empties " ")))))

(defn render-progress
  "The pure bar renderer: bar map {:label string :ratio 0..1} and an
  explicit total width -> the one-line string. label, percentage,
  then the bar between rails; when the width leaves no room for even
  one rail-to-rail cell the rails drop and the label and percentage
  survive alone. The result fills the width exactly whenever the
  rails are present."
  [bar width]
  (let [[label ratio] (bar->parts bar)]
    (when-not (and (int? width) (<= 0 width))
      (throw-opts "progress width must be a non-negative integer" width))
    (let [head (str (label-prefix label) (pad3 (pct-of ratio)) "%")
          room (- width (count head) 2)]
      (if (< room 1)
        head
        (str head "|" (bar-chars room ratio) "|")))))

(defn progress
  "One progress line for bar {:label string :ratio
  0..1}. Width comes from {:width n} in opts or terminal-width. On a
  terminal (or with {:force true}) the answer is the shaped bar from
  render-progress; when stdout is not a terminal the answer is the
  plain label-and-percentage line, so piped and logged output stays
  clean. Returns the string; printing is the caller's business."
  ([bar] (progress bar nil))
  ([bar opts]
   (check-opts opts)
   (let [[label ratio] (bar->parts bar)
         width (or (get opts :width) (terminal-width))]
     (if (color-on? opts)
       (render-progress bar width)
       (str (label-prefix label) (pad3 (pct-of ratio)) "%")))))

;;;; prompt and confirm

(defn parse-yes-no
  "Pure fn: maps a raw input string to true (y/yes), false (n/no), or nil
  (blank or unrecognized). Case-insensitive. Blank input signals that the
  caller should apply the :default; nil for unrecognized triggers a re-prompt."
  [s]
  (let [low (str/lower-case (str/trim (or s "")))]
    (cond
      (= "" low)                nil
      (or (= "y" low)
          (= "yes" low))        true
      (or (= "n" low)
          (= "no" low))         false
      :else                     nil)))

(defn prompt-result
  "Pure fn: given a raw input string and opts, return the accepted value
  or nil to signal a re-prompt.

  - blank input (after trim) uses :default when supplied, else nil.
  - non-blank input is passed to :validate when supplied; falsy means re-prompt.
  - :default short-circuits before :validate, so a valid default is never
    rejected by the validator."
  [raw opts]
  (let [trimmed (str/trim (or raw ""))
        blank?  (= "" trimmed)]
    (if blank?
      (get opts :default)
      (let [validate (get opts :validate)]
        (if (and validate (not (validate trimmed)))
          nil
          trimmed)))))

(defn- throw-prompt-cap
  [msg tries]
  (term-fail :term/prompt "MTP001" (str "mino.term: " msg)
             {:max-tries tries}))

(defn- default-read-fn
  "The default 0-arg reader: read-line from stdin."
  []
  clojure.core/read-line)

(defn prompt
  "Read a line from the terminal, re-prompting until a valid answer is
  entered or :max-tries (default 3) attempts are exhausted, at which
  point it throws :mino/kind :term/prompt.

  opts map:
    :default   value returned on blank input (skips :validate)
    :validate  1-arg predicate; falsy return re-prompts
    :password  when true, read without echo (or via :read-fn if injected)
    :max-tries attempt cap, default 3
    :read-fn   0-arg fn returning the next line; defaults to read-line
               (inject for deterministic tests)"
  ([msg] (prompt msg {}))
  ([msg opts]
   (check-opts opts)
   (let [max-tries (or (get opts :max-tries) 3)
         read-fn   (or (get opts :read-fn)
                       (if (get opts :password)
                         read-password
                         clojure.core/read-line))]
     (loop [tries 0]
       (when (>= tries max-tries)
         (throw-prompt-cap "too many invalid attempts" max-tries))
       (print (str msg " "))
       (flush)
       (let [raw (read-fn)
             val (prompt-result raw opts)]
         (if (some? val)
           val
           (recur (inc tries))))))))

(defn confirm
  "Read a y/n answer from the terminal, re-prompting until a recognized
  answer is entered or :max-tries (default 3) attempts are exhausted, at
  which point it throws :mino/kind :term/prompt.

  opts map:
    :default   boolean (or nil) returned on blank input; default false
    :max-tries attempt cap, default 3
    :read-fn   0-arg fn returning the next line (inject for tests)"
  ([msg] (confirm msg {}))
  ([msg opts]
   (check-opts opts)
   (let [max-tries (or (get opts :max-tries) 3)
         ;; :default must be a boolean or nil; false is the safe no-op side
         dflt      (get opts :default false)
         read-fn   (or (get opts :read-fn) clojure.core/read-line)]
     (loop [tries 0]
       (when (>= tries max-tries)
         (throw-prompt-cap "too many invalid attempts" max-tries))
       (print (str msg " "))
       (flush)
       (let [raw    (read-fn)
             parsed (parse-yes-no raw)]
         (cond
           ;; blank: apply the default
           (and (nil? parsed) (= "" (str/trim (or raw ""))))
           dflt
           ;; recognized y/n answer
           (some? parsed)
           parsed
           ;; unrecognized: re-prompt
           :else
           (recur (inc tries))))))))
