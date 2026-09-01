(ns mino.toml
  "TOML 1.0 reader and emitter: parse TOML text into plain data and
  generate TOML text back from it.

  parse-string reads a document into nested maps; generate-string
  writes one back such that (parse-string (generate-string x)) = x
  for representable data.

  (require '[mino.toml :as toml])
  (toml/parse-string \"a = 1\\n\")            ; => {:a 1}
  (toml/parse-string doc {:parse-values f})   ; f applied to leaves
  (toml/generate-string {:a 1})               ; => \"a = 1\\n\"

  Tables become plain maps with keyword keys, arrays become vectors,
  arrays of tables become vectors of maps, and scalars map onto the
  Clojure tower: 64-bit ints, doubles (inf/nan included), booleans,
  strings. RFC 3339 dates and times are kept as the raw source text
  (a v1 choice); coerce them with {:parse-values f}, which is applied
  to every leaf scalar (array elements and inline-table values
  included) and must be total.

  Semantics follow the TOML 1.0 spec, with two recorded
  divergences: integer literals must fit signed 64-bit (the spec
  allows wider), and dates are shape-checked but not calendar-
  validated. CRLF is normalized to LF everywhere up front, so any
  remaining carriage return is an error.

  The reader itself is the native single-pass toml-parse prim (ADR
  25; the same reader-in-C call ADR 23 made for JSON and ADR 24 for
  CSV after the mino-side readers measured far over budget). This
  namespace owns argument validation and the error contract: errors
  are thrown as a diagnostic with :mino/kind :toml/parse (ADR 37),
  and :mino/data carrying a :reason keyword, :location {:line :col}
  (1-based byte positions), and the offending :text."
  (:require [clojure.string :as str]))

(defn- toml-fail
  "Throws a classified mino.toml diagnostic (ADR 37): :mino/kind names
  the error class so classed catch dispatches on it, :mino/message the
  human string ex-message returns, :mino/data the detail ex-data reads."
  [kind code msg data]
  (throw {:mino/kind kind :mino/code code :mino/message msg
          :mino/data data}))

(defn- throw-opts
  [msg arg]
  (toml-fail :toml/opts "MTOO001" (str "mino.toml: " msg) {:arg arg}))

(defn parse-string
  "Parses TOML text into plain nested maps with keyword keys;
  generate-string is the companion emitter back to TOML.
  opts map: {:parse-values f} applies f to every leaf scalar (it must
  be total); the intended use is date coercion, since RFC 3339
  values stay raw strings otherwise. Throws a diagnostic with
  :mino/kind :toml/parse and :mino/data {:reason, :location
  {:line :col}, :text} on malformed documents."
  ([s] (parse-string s nil))
  ([s opts]
   (when-not (string? s)
     (throw-opts "parse-string requires a string" s))
   (when-not (or (nil? opts) (map? opts))
     (throw-opts "parse-string opts must be a map" opts))
   (let [pv (get opts :parse-values)]
     (when (and (some? pv) (not (fn? pv)))
       (throw-opts ":parse-values must be a function" pv))
     (let [r (toml-parse s pv)]
       (if (and (vector? r)
                (= :toml/error (nth r 0)))
         (toml-fail :toml/parse "MTOP001"
                    (str "mino.toml: " (nth r 1))
                    {:reason (keyword (nth r 1))
                     :location {:line (nth r 2)
                                :col (nth r 3)}
                     :text (nth r 4)})
         r)))))

;;;; Emitter

(defn- emit-fail
  [msg data]
  (toml-fail :toml/emit "MTOE001" (str "mino.toml: " msg) data))

(defn- key-name
  [k]
  (cond (keyword? k) (subs (str k) 1)
        (string? k) k
        :else (emit-fail "a TOML key must be a keyword or a string"
                         {:key k})))

(defn- esc-char
  [c]
  (let [i (int c)]
    (cond (= c \") "\\\""
          (= c \\) "\\\\"
          (= i 8) "\\b"
          (= i 9) "\\t"
          (= i 10) "\\n"
          (= i 12) "\\f"
          (= i 13) "\\r"
          (or (< i 32) (= i 127)) (format "\\u%04x" i)
          :else c)))

(defn- string-repr
  [s]
  (str "\"" (apply str (map esc-char s)) "\""))

(defn- key-repr
  [k]
  (let [s (key-name k)]
    (if (re-find #"^[-A-Za-z0-9_]+$" s) s (string-repr s))))

(defn- float-repr
  [x]
  (cond (NaN? x) "nan"
        (= x ##Inf) "inf"
        (= x ##-Inf) "-inf"
        :else (pr-str x)))

(defn- table-array?
  "A nonempty vector of maps emits as an array of tables."
  [v]
  (and (sequential? v) (seq v) (every? map? v)))

(declare inline-value)

(defn- inline-table
  [m]
  (str "{" (str/join ", " (map (fn [[k v]]
                                 (str (key-repr k) " = "
                                      (inline-value v)))
                               m))
       "}"))

(defn- inline-value
  [x]
  (cond (string? x) (string-repr x)
        (integer? x) (str x)
        (number? x) (float-repr x)
        (true? x) "true"
        (false? x) "false"
        (map? x) (inline-table x)
        (sequential? x)
        (str "[" (str/join ", " (map inline-value x)) "]")
        (nil? x) (emit-fail "TOML has no null; omit the key instead"
                            {:value x})
        (keyword? x)
        (emit-fail "TOML has no keyword scalar; use a string" {:value x})
        :else (emit-fail "unrepresentable value" {:value x})))

(defn- header-line
  [path array?]
  (let [p (str/join "." path)]
    (if array? (str "[[" p "]]") (str "[" p "]"))))

(defn- emit-table
  "Appends table body lines for m at path to the lines vector:
  scalar entries first, then subtables and arrays of tables, each
  under a blank line and its header."
  [lines path m]
  (let [table-entry? (fn [v] (or (map? v) (table-array? v)))
        lines (reduce (fn [ls [k v]]
                        (if (table-entry? v)
                          ls
                          (conj ls (str (key-repr k) " = "
                                        (inline-value v)))))
                      lines m)]
    (reduce (fn [ls [k v]]
              (cond (map? v)
                    (let [p (conj path (key-repr k))]
                      (emit-table (conj ls "" (header-line p false))
                                  p v))
                    (table-array? v)
                    (let [p (conj path (key-repr k))]
                      (reduce (fn [ls m*]
                                (emit-table
                                 (conj ls "" (header-line p true))
                                 p m*))
                              ls v))
                    :else ls))
            lines m)))

(defn generate-string
  "Emits the map x as TOML text such that
  (parse-string (generate-string x)) = x for representable data.
  Nested maps become tables, nonempty vectors of maps become arrays
  of tables, other vectors become inline arrays, and scalar entries
  precede subtables in every table body. TOML has no null and no
  keyword scalar, so nil values, keyword values, non keyword-or-string
  keys, and a non-map x throw a diagnostic with :mino/kind :toml/emit.
  opts is a keyword map, reserved and ignored in v1."
  ([x] (generate-string x nil))
  ([x opts]
   (when-not (or (nil? opts) (map? opts))
     (throw-opts "generate-string opts must be a map" opts))
   (when-not (map? x)
     (emit-fail "generate-string requires a map at the top level"
                {:value x}))
   (let [lines (drop-while (fn [l] (= l "")) (emit-table [] [] x))]
     (if (empty? lines)
       ""
       (str (str/join "\n" lines) "\n")))))
