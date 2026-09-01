(ns mino.yaml
  "YAML 1.2 subset reader and emitter: parse YAML text into plain data
  (ADR 26) and generate YAML text back from it.

  parse-string reads the first document of a stream, parse-string-all
  every document, and generate-string writes one document such that
  (parse-string (generate-string x)) = x for representable data.

  (require '[mino.yaml :as yaml])
  (yaml/parse-string \"a: 1\\n\")              ; => {:a 1}
  (yaml/parse-string-all \"--- a\\n--- b\\n\") ; => [\"a\" \"b\"]
  (yaml/generate-string {:a 1})                ; => \"a: 1\\n\"

  clj-yaml surface: keyword keys are the default ({:keywords false}
  keeps string keys, applied recursively), parse-string reads the
  first document of a stream, parse-string-all returns every document
  as a vector. Duplicate keys: last wins. Plain scalars (and keys)
  resolve through the YAML 1.2 core schema (yes/no/on/off are
  strings; that is 1.1): 23 stays 23, ~ is nil, quoted keys stay
  strings and keywordize.

  In-subset v1: block mappings and sequences by indentation, compact
  forms, flow collections, plain/single/double scalars with folding,
  literal and folded block scalars with chomping and indentation
  indicators, comments, --- / ... documents. Out of subset, thrown as
  errors with their own reasons: anchors, aliases, tags, complex
  keys, directives.

  The reader itself is the native single-pass yaml-parse prim (ADR
  26; the mino-side Clojure reader it replaced was algorithmically
  correct and linear but measured 82 s for 1.05 MB, 41x over the
  2 s/1MB bar, in interpreter dispatch and per-call regex compiles,
  the same primitive-contract wall ADRs 23, 24, and 25 record). This
  namespace owns argument validation and the error contract: errors
  are thrown as a diagnostic with :mino/kind :yaml/parse (ADR 37, so
  classed catch dispatches on the class), a :reason keyword, and
  :location {:line :col} over bytes in the :mino/data detail."
  (:require [clojure.string :as str]))

(defn- yaml-fail
  "Throws a classified mino.yaml diagnostic (ADR 37): :mino/kind names
  the error class so classed catch dispatches on it, :mino/message the
  human string ex-message returns, :mino/data the detail ex-data reads."
  [kind code msg data]
  (throw {:mino/kind kind :mino/code code :mino/message msg
          :mino/data data}))

(defn- throw-opts
  [msg arg]
  (yaml-fail :yaml/opts "MYO001" (str "mino.yaml: " msg) {:arg arg}))

(defn- parse-opts
  [who s opts]
  (when-not (string? s)
    (throw-opts (str who " requires a string") s))
  (when-not (or (nil? opts) (map? opts))
    (throw-opts (str who " opts must be a map") opts))
  (let [kw (get opts :keywords true)]
    (when-not (or (true? kw) (false? kw))
      (throw-opts ":keywords must be a boolean" kw))
    kw))

(defn- run-prim
  [s kw]
  (let [r (yaml-parse s kw)]
    (if (and (vector? r)
             (seq r)
             (= :yaml/error (nth r 0)))
      (yaml-fail :yaml/parse "MYP001" (str "mino.yaml: " (nth r 1))
                 {:reason (keyword (nth r 1))
                  :location {:line (nth r 2)
                             :col (nth r 3)}
                  :text (nth r 4)})
      r)))

(defn parse-string
  "Parses the first YAML document in s into plain data; generate-string
  is the companion emitter back to YAML. Keyword keys by default;
  {:keywords false} keeps string keys. Throws a diagnostic with
  :mino/kind :yaml/parse, :reason and :location {:line :col} in its
  data, on malformed or out-of-subset input."
  ([s] (parse-string s nil))
  ([s opts]
   (let [kw (parse-opts "parse-string" s opts)
         docs (run-prim s kw)]
     (if (empty? docs) nil (nth docs 0)))))

(defn parse-string-all
  "Parses every YAML document in s into a vector; an empty stream
  yields []. Options as in parse-string."
  ([s] (parse-string-all s nil))
  ([s opts]
   (let [kw (parse-opts "parse-string-all" s opts)]
     (run-prim s kw))))

;;;; Emitter

(defn- emit-fail
  [msg data]
  (yaml-fail :yaml/emit "MYE001" (str "mino.yaml: " msg) data))

;; The 1.1 ambiguity set (the norway problem) is quoted even though the
;; 1.2 core schema reads it back as strings; the output stays
;; unambiguous for downstream 1.1 consumers.
(def ^:private plain-reserved
  #{"y" "n" "yes" "no" "on" "off" "true" "false" "null" "~"})

(defn- plain-safe?
  [s]
  (and (re-find #"^[A-Za-z_]([-A-Za-z0-9_@ ./]*[-A-Za-z0-9_@./])?$" s)
       (not (contains? plain-reserved (str/lower-case s)))))

(defn- has-control?
  [s]
  (boolean (some (fn [c] (let [i (int c)] (or (< i 32) (= i 127)))) s)))

(defn- control-beyond-newline?
  [s]
  (boolean (some (fn [c]
                   (let [i (int c)]
                     (or (and (< i 32) (not= i 10)) (= i 127))))
                 s)))

(defn- dq-escape
  [s]
  (apply str
         (map (fn [c]
                (let [i (int c)]
                  (cond (= c \\) "\\\\"
                        (= c \") "\\\""
                        (= c \newline) "\\n"
                        (= c \tab) "\\t"
                        (= c \return) "\\r"
                        (or (< i 32) (= i 127)) (format "\\u%04x" i)
                        :else c)))
              s)))

(defn- quote-scalar
  [s]
  (cond (plain-safe? s) s
        (not (has-control? s))
        (str "'" (str/replace s "'" "''") "'")
        :else (str "\"" (dq-escape s) "\"")))

(defn- literal-eligible?
  "A string can ride a literal block when its only control character is
  the newline, no line starts or ends with a space, it does not open
  with a blank line, and at most one newline trails."
  [s]
  (and (str/includes? s "\n")
       (not (str/starts-with? s "\n"))
       (not (control-beyond-newline? s))
       (let [body (if (str/ends-with? s "\n")
                    (subs s 0 (dec (count s)))
                    s)]
         (and (seq body)
              (not (str/ends-with? body "\n"))
              (every? (fn [line]
                        (and (not (str/starts-with? line " "))
                             (not (str/ends-with? line " "))))
                      (str/split body #"\n" -1))))))

(defn- literal-lines
  "[head lines] for a literal block: the chomping header and the
  unindented body lines."
  [s]
  (let [clip? (str/ends-with? s "\n")
        body (if clip? (subs s 0 (dec (count s))) s)]
    [(if clip? "|" "|-") (str/split body #"\n" -1)]))

(defn- num-str
  [x]
  (cond (integer? x) (str x)
        (NaN? x) ".nan"
        (= x ##Inf) ".inf"
        (= x ##-Inf) "-.inf"
        :else (pr-str x)))

(defn- key-str
  [k]
  (cond (keyword? k) (quote-scalar (subs (str k) 1))
        (string? k) (quote-scalar k)
        (nil? k) "~"
        (true? k) "true"
        (false? k) "false"
        (number? k) (num-str k)
        :else (emit-fail "unrepresentable key; a YAML subset key must be scalar"
                         {:key k})))

(defn- scalar-inline
  "Inline rendering for a non-collection value, or nil when x needs
  block or flow treatment."
  [x]
  (cond (nil? x) "null"
        (true? x) "true"
        (false? x) "false"
        (number? x) (num-str x)
        (keyword? x) (quote-scalar (subs (str x) 1))
        :else nil))

(defn- flow-str
  [x]
  (or (scalar-inline x)
      (cond (string? x) (quote-scalar x)
            (map? x)
            (str "{" (str/join ", " (map (fn [[k v]]
                                           (str (key-str k) ": "
                                                (flow-str v)))
                                         x))
                 "}")
            (sequential? x)
            (str "[" (str/join ", " (map flow-str x)) "]")
            :else (emit-fail "unrepresentable value" {:value x}))))

(defn- indent-lines
  [lines]
  (map (fn [line] (if (= line "") "" (str "  " line))) lines))

(declare block-render)

(defn- map-lines
  [m]
  (mapcat (fn [[k v]]
            (let [ks (key-str k)
                  [tag a b] (block-render v)]
              (case tag
                :inline [(str ks ": " a)]
                :literal (cons (str ks ": " a) (indent-lines b))
                :block (cons (str ks ":") (indent-lines a)))))
          m))

(defn- vec-lines
  [v]
  (mapcat (fn [x]
            (let [[tag a b] (block-render x)]
              (case tag
                :inline [(str "- " a)]
                :literal (cons (str "- " a) (indent-lines b))
                ;; The reader takes a quoted scalar right after "- " as
                ;; a sequence item, not a mapping key (tracked in
                ;; .local/BUGS.md), so a map opening with a quoted key
                ;; gets the dash on its own line.
                :block (let [f (first a)]
                         (if (or (str/starts-with? f "'")
                                 (str/starts-with? f "\""))
                           (cons "-" (indent-lines a))
                           (cons (str "- " f)
                                 (indent-lines (rest a))))))))
          v))

(defn- block-render
  "[:inline s], [:literal head lines], or [:block lines] for x in
  block context."
  [x]
  (if-let [s (scalar-inline x)]
    [:inline s]
    (cond (string? x) (if (literal-eligible? x)
                        (let [[head lines] (literal-lines x)]
                          [:literal head lines])
                        [:inline (quote-scalar x)])
          (map? x) (if (empty? x) [:inline "{}"] [:block (map-lines x)])
          (sequential? x) (if (empty? x)
                            [:inline "[]"]
                            [:block (vec-lines x)])
          :else (emit-fail "unrepresentable value" {:value x}))))

(defn generate-string
  "Emits x as one YAML document, block style by default, such that
  (parse-string (generate-string x)) = x for representable data.
  Strings that would read back as non-string scalars are quoted (the
  1.1 yes/no/on/off set included), control characters ride
  double-quoted escapes, and multiline strings become literal blocks.
  opts map: {:flow true} emits flow style instead. Values and keys
  with no representation in the subset (functions, collection keys)
  throw a diagnostic with :mino/kind :yaml/emit."
  ([x] (generate-string x nil))
  ([x opts]
   (when-not (or (nil? opts) (map? opts))
     (throw-opts "generate-string opts must be a map" opts))
   (let [flow (get opts :flow false)]
     (when-not (or (true? flow) (false? flow))
       (throw-opts ":flow must be a boolean" flow))
     (if flow
       (str (flow-str x) "\n")
       (let [[tag a b] (block-render x)]
         (case tag
           :inline (str a "\n")
           :literal (str a "\n"
                         (str/join "\n" (indent-lines b)) "\n")
           :block (str (str/join "\n" a) "\n")))))))
