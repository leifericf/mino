(ns mino.jsonl
  "Lazy JSON Lines (JSONL / NDJSON) reader and writer.

  (require '[mino.jsonl :as jsonl])

  ;; read from a string
  (jsonl/read-lines \"1\\n2\\n3\\n\")   ; => (1 2 3)

  ;; read from a reader handle (lazy)
  (with-open [r (io/reader \"data.jsonl\")]
    (doall (jsonl/read-lines r)))

  ;; write a seq of values
  (jsonl/write-lines [{:a 1} {:b 2}])   ; \"{'a':1}\\n{\\\"b\\\":2}\\n\"
  ;; returns a single string; each line is compact JSON followed by \\n.

  Format policy: blank lines in input are silently skipped (each
  non-blank line is a complete JSON document). A trailing newline
  in the input is not required and does not produce an extra nil.
  write-lines always emits a trailing newline after the last document.

  Rides the :json capability (json-parse and clojure.data.json/write-str).")

(require '[clojure.data.json :as json])

;;;; Reading

(defn- parse-line
  "Parses one non-blank JSON line. Throws on invalid JSON (let the
  json-parse error propagate as-is; the caller sees a :json/parse kind)."
  [line]
  (json/read-str line))

(defn read-lines
  "Lazily parses JSON Lines from s (a string) or r (a reader handle opened
  with clojure.java.io/reader or mino's open-read). Blank lines are
  skipped. Returns a lazy seq of parsed Clojure values.

  When given a string, splits on newlines and parses each non-blank line.
  When given a reader, reads lines lazily (the caller is responsible for
  closing the reader)."
  [s-or-reader & {:as opts}]
  (let [lines (if (string? s-or-reader)
                (clojure.string/split-lines s-or-reader)
                (line-seq s-or-reader))]
    (->> lines
         (remove clojure.string/blank?)
         (map (fn [line]
                (let [key-fn (:key-fn opts)]
                  (if key-fn
                    (json/read-str line :key-fn key-fn)
                    (json/read-str line))))))))

;;;; Writing

(defn write-lines
  "Serialises each element of coll as compact JSON followed by a newline,
  concatenates all lines, and returns the result as a single string. An
  empty coll returns an empty string. Each element must be JSON-serializable
  (see clojure.data.json/write-str for the rules)."
  [coll]
  (apply str (map #(str (json/write-str %) "\n") coll)))
