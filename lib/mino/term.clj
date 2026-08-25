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
  data throws ex-info with :kind :term/style and the offending :key;
  bad argument shapes throw :kind :term/opts."
  (:require [clojure.string :as str]))

(def ^:private esc "\033")

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

(defn- throw-style
  [msg spec key]
  (throw (ex-info (str "mino.term: " msg)
                  {:kind :term/style :spec spec :key key})))

(defn- throw-opts
  [msg arg]
  (throw (ex-info (str "mino.term: " msg)
                  {:kind :term/opts :arg arg})))

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
