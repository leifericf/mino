(ns mino.yaml
  "YAML 1.2 subset reader: parse YAML text into plain data (ADR 26).

  (require '[mino.yaml :as yaml])
  (yaml/parse-string \"a: 1\\n\")              ; => {:a 1}
  (yaml/parse-string-all \"--- a\\n--- b\\n\") ; => [\"a\" \"b\"]

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
  "Parses the first YAML document in s into plain data. Keyword keys
  by default; {:keywords false} keeps string keys. Throws a diagnostic
  with :mino/kind :yaml/parse, :reason and :location {:line :col} in
  its data, on malformed or out-of-subset input."
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
