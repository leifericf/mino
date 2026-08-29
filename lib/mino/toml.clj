(ns mino.toml
  "TOML 1.0 reader: parse TOML text into plain data.

  Reader only, one-way. This namespace reads TOML into data via
  parse-string; there is no TOML emitter, so parsed data does not
  round-trip back to TOML text.

  (require '[mino.toml :as toml])
  (toml/parse-string \"a = 1\\n\")            ; => {:a 1}
  (toml/parse-string doc {:parse-values f})   ; f applied to leaves

  Tables become plain maps with keyword keys, arrays become vectors,
  arrays of tables become vectors of maps, and scalars map onto the
  Clojure tower: 64-bit ints, doubles (inf/nan included), booleans,
  strings. RFC 3339 dates and times are kept as the raw source text
  (a v1 choice); coerce them with {:parse-values f}, which is applied
  to every leaf scalar (array elements and inline-table values
  included) and must be total.

  Semantics follow python3 tomllib as the oracle, with two recorded
  divergences: integer literals must fit signed 64-bit (tomllib
  accepts wider), and dates are shape-checked but not calendar-
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
  "Parses TOML text into plain nested maps with keyword keys. Reading
  is one-way: there is no companion emitter back to TOML.
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
