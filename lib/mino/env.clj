(ns mino.env
  "dotenv files: parse .env text into plain string maps and load
  files into an overlay that getenv reads.

  (require '[mino.env :as env])
  (env/parse-dotenv \"KEY=value\\n\")        ; => {\"KEY\" \"value\"}
  (env/load-env \"conf.env\")               ; installs the overlay
  (env/load-env \"more.env\" {:merge true})  ; merges over it
  (env/getenv \"KEY\")                      ; overlay, else process env

  Parsing follows the common dotenv conventions (docker-compose and
  ruby-dotenv): KEY=VALUE lines, an optional `export ` prefix,
  full-line and trailing # comments (a trailing comment needs
  whitespace before the hash, so values may contain #), blank lines
  skipped, KEY= is the empty string, and a later duplicate key wins.
  Single-quoted values are literal; double-quoted values strip the
  quotes and interpret \\\\ \\\" \\n \\r \\t \\f \\b escapes (any other
  backslash pair passes through). CRLF is tolerated and a leading
  UTF-8 BOM is dropped. A line that is neither blank, a comment, nor
  KEY=VALUE throws a diagnostic with :mino/kind :env/parse, the
  1-based :line, and the raw :text.

  load-env reads the file via slurp and installs the parsed map as
  the overlay mino.env/getenv consults before the process
  environment; {:merge true} merges the file over the loaded overlay
  instead of replacing it (later loads win on conflicts). The
  process environment is never mutated: subprocesses and the core
  getenv prim see nothing, the clj-dotenv / environ accessor model.

  getenv takes one string name and answers the overlay value, else
  the process environment value, else nil. Parsing needs the regex
  capability (the assignment grammar runs through the regex prims,
  the mino.cli precedent); the process fallback resolves the core
  getenv prim at call time, so reading through the fallback also
  needs the io capability, like every other getenv caller."
  (:require [clojure.string :as str]))

;;;; Tables and patterns
;; char-at answers one-character strings, so the escape table holds
;; those. tiny-regex-c has no \s, so the patterns spell out [ \t].

(def ^:private line-pattern
  "Assignment grammar: optional export prefix, KEY, =, raw value."
  "(?:export[ \\t]+)?([A-Za-z_][A-Za-z0-9_]*)[ \\t]*=[ \\t]*(.*)")

(def ^:private dq-escapes
  "Double-quoted escape sequences; any other backslash pair passes
  through with the backslash kept."
  {"n" "\n"
   "r" "\r"
   "t" "\t"
   "f" (str \formfeed)
   "b" (str \backspace)
   "\\" "\\"
   "\"" "\""})

(def ^:private utf8-bom (str (char 65279)))

;;;; Errors

(defn- env-fail
  "Throws a classified mino.env diagnostic (ADR 37): :mino/kind names
  the error class so classed catch dispatches on it, :mino/message the
  human string ex-message returns, :mino/data the detail ex-data reads."
  [kind code msg data]
  (throw {:mino/kind kind :mino/code code :mino/message msg
          :mino/data data}))

(defn- throw-parse
  [msg line text]
  (env-fail :env/parse "MEP001"
            (str "mino.env: " msg " (line " line ")")
            {:line line :text text}))

(defn- throw-opts
  [msg arg]
  (env-fail :env/opts "MEO001"
            (str "mino.env: " msg)
            {:arg arg}))

;;;; Value scanning

(defn- ws?
  [c]
  (or (= c " ") (= c "\t")))

(defn- scan-double
  "v starts with the opening double quote. Answers [decoded more]
  where more is everything after the closing quote. Only this walk
  stays character-by-character: escape decoding needs the context."
  [v line text]
  (let [n (count v)]
    (loop [i 1 acc []]
      (if (>= i n)
        (throw-parse "unterminated double-quoted value" line text)
        (let [c (char-at v i)]
          (cond
            (= c "\\")
            (let [j (inc i)]
              (if (>= j n)
                (throw-parse "unterminated double-quoted value" line text)
                (if-let [d (get dq-escapes (char-at v j))]
                  (recur (inc j) (conj acc d))
                  (recur j (conj acc "\\")))))

            (= c "\"")
            [(str/join acc) (subs v (inc i))]

            :else
            (recur (inc i) (conj acc c))))))))

(defn- scan-single
  "v starts with the opening single quote. Answers [literal more]
  where more is everything after the closing quote. No escapes."
  [v line text]
  (let [m (re-find-from "'" v 1)]
    (if (nil? m)
      (throw-parse "unterminated single-quoted value" line text)
      (let [close (nth m 1)]
        [(subs v 1 close) (subs v (inc close))]))))

(defn- rest-ok?
  "May text follow a closing quote? Only whitespace and an optional
  trailing comment. The remainder is short, so the walk is cheap."
  [r]
  (let [n (count r)]
    (loop [i 0]
      (cond
        (>= i n)            true
        (ws? (char-at r i)) (recur (inc i))
        (= "#" (char-at r i)) true
        :else               false))))

(defn- comment-cut
  "Index of the whitespace before an unquoted trailing # (the hash
  must follow whitespace so values may contain #), else nil. Runs
  through re-find-from, a single C-speed pass."
  [v]
  (let [m (re-find-from "[ \t]#" v 0)]
    (when m (nth m 1))))

(defn- parse-value
  "v is the raw value after KEY= with surrounding whitespace gone."
  [v line text]
  (cond
    (= "" v)
    ""

    (= "\"" (subs v 0 1))
    (let [[val more] (scan-double v line text)]
      (if-not (rest-ok? more)
        (throw-parse "unexpected text after closing quote" line text)
        val))

    (= "'" (subs v 0 1))
    (let [[val more] (scan-single v line text)]
      (if-not (rest-ok? more)
        (throw-parse "unexpected text after closing quote" line text)
        val))

    (= "#" (subs v 0 1))
    ""

    :else
    (let [cut (comment-cut v)]
      (if cut (str/trimr (subs v 0 cut)) v))))

;;;; Line and document parsing

(defn- strip-bom
  [s]
  (if (str/starts-with? s utf8-bom) (subs s 1) s))

(defn- parse-line
  "Trimmed line -> [key value], nil for blank and comment lines;
  throws a diagnostic with :mino/kind :env/parse on anything else.
  The grammar runs through one
  anchored regex match; walk loops only touch quoted values."
  [t line]
  (when-not (or (= "" t) (= "#" (subs t 0 1)))
    (let [m (re-matches line-pattern t)]
      (if (nil? m)
        (throw-parse "line is not KEY=VALUE" line t)
        [(nth m 1) (parse-value (nth m 2) line t)]))))

(defn parse-dotenv
  "Parses dotenv text into a plain map of string keys and string
  values. Pure: no overlay, no environment, no IO. See the
  namespace docstring for the full format contract."
  [s]
  (when-not (string? s)
    (throw-opts "parse-dotenv requires a string" s))
  (loop [lines (seq (str/split-lines (strip-bom s)))
         line  1
         acc   (transient {})]
    (if-let [t (first lines)]
      (if-let [kv (parse-line (str/trim t) line)]
        (recur (next lines) (inc line) (assoc! acc (nth kv 0) (nth kv 1)))
        (recur (next lines) (inc line) acc))
      (persistent! acc))))

;;;; The getenv overlay

(def ^:private overlay (atom {}))

(defn load-env
  "Reads file f via slurp, parses it as dotenv text, and installs
  the parsed map as the overlay mino.env/getenv reads before the
  process environment. A plain load replaces the overlay;
  {:merge true} merges the file over the loaded overlay (the file
  wins on conflicts, earlier keys survive). Answers the overlay map
  after the load."
  ([f] (load-env f nil))
  ([f opts]
   (when-not (string? f)
     (throw-opts "load-env requires a file path string" f))
   (when (and (some? opts) (not (map? opts)))
     (throw-opts "load-env opts must be a map" opts))
   (let [parsed (parse-dotenv (slurp f))]
     (if (get opts :merge)
       (do (swap! overlay (fn [cur] (reduce-kv assoc cur parsed)))
           @overlay)
       (do (reset! overlay parsed)
           parsed)))))

(defn getenv
  "Name (a string) -> value from the loaded overlay, falling back to
  the process environment, else nil. This is the getenv view
  load-env installs into; the process env is never mutated."
  [name]
  (when-not (string? name)
    (throw-opts "getenv requires a string name" name))
  (or (get @overlay name)
      (clojure.core/getenv name)))
