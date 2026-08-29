(ns clojure.data.csv)

;; CSV codec for mino, mirroring clojure.data.csv (read-csv /
;; write-csv). The reader is the native csv-parse primitive; the
;; writer is pure Clojure (docs/adr/24-csv-reader-in-c-writer-stays-
;; clojure.md). Behavior follows the python3 csv module, the RFC 4180
;; reference oracle the golden vectors were captured from: lenient
;; about stray quotes, a lone \r ends a record, blank lines read as
;; empty rows.

(require '[clojure.string :as str])

;;;; Errors

(defn- csv-fail
  "Throws a classified diagnostic (ADR 37): :mino/kind is the dispatch
  axis, :mino/message the human string, :mino/data the ex-data detail."
  [kind code msg data]
  (throw {:mino/kind kind :mino/code code :mino/message msg
          :mino/data data}))

;;;; Options

(defn- check-char-opt
  "separator / quote must be characters; anything else compares false
  against every parsed character and silently yields garbage."
  [kind k v]
  (when-not (char? v)
    (csv-fail kind "MCS001" (str "csv: :" k " must be a character")
              {:option k :value v})))

(defn- newline-str
  [newline]
  (case newline
    :lf   "\n"
    :crlf "\r\n"
    :cr   "\r"
    (csv-fail :csv/write "MCS002" "csv: :newline must be :lf, :crlf or :cr"
              {:option :newline :value newline})))

;;;; Reader

(defn read-csv
  "Read CSV data and return a vector of vector rows of strings.
   input is the CSV text, or a string-cursor atom like *in* (see
   with-in-str), which is parsed whole and emptied. The reader
   recognizes \n, \r\n and lone \r record ends and keeps quoted
   separators, quotes and newlines as field data. Rows come back
   eagerly (the reader is native; ADR 24), like clojure.data.json's
   read-str.

   Opts: :separator (default \\,), :quote (default \\\").

   (read-csv \"a,b\\n\") => [[\"a\" \"b\"]]"
  [input & {:as opts}]
  (let [sep (or (:separator opts) \,)
        q   (or (:quote opts) \")]
    (check-char-opt :csv/parse :separator sep)
    (check-char-opt :csv/parse :quote q)
    (cond
      (string? input) (csv-parse input sep q)
      (atom? input)   (let [s @input]
                        (if (string? s)
                          (do (reset! input "")
                              (csv-parse s sep q))
                          (csv-fail :csv/parse "MCS003"
                                    "read-csv: cursor atom must hold a string"
                                    {:input input})))
      :else           (csv-fail :csv/parse "MCS003"
                                "read-csv: input must be CSV text or a string-cursor atom"
                                {:input input}))))

;;;; Writer

(defn- field-needs-quote?
  [t sep q]
  (or (str/includes? t (str sep))
      (str/includes? t (str q))
      (str/includes? t "\n")
      (str/includes? t "\r")))

(defn- write-field
  [cell sep-str q-str quote?]
  (let [t (str cell)]
    (if (quote? t)
      (str q-str (str/replace t q-str (str q-str q-str)) q-str)
      t)))

(defn- write-row
  "One row's text; fields accumulate in a transient and join once."
  [row sep-str q-str quote?]
  (loop [xs  (seq row)
         acc (transient [])]
    (if xs
      (recur (next xs)
             (conj! acc (write-field (first xs) sep-str q-str quote?)))
      (str/join sep-str (persistent! acc)))))

(defn write-csv
  "Write data, any seq of seqs, as CSV and return nil. out is a writer
   (the *out* model: an atom as bound by with-out-str, or
   :mino/stdout / :mino/stderr) or a file path string, in which case
   the whole document lands there in one write.

   Opts: :separator (default \\,), :quote (default \\\"),
   :newline :lf (default) | :crlf | :cr, :quote? a predicate over the
   field text deciding whether to quote it (default: when it contains
   the separator, the quote char or a newline). Cells stringify with
   str, so nil writes as an empty field.

   (with-out-str (write-csv *out* [[\"a\" \"b\"]])) => \"a,b\\n\""
  [out data & {:as opts}]
  (let [sep     (or (:separator opts) \,)
        q       (or (:quote opts) \")
        nl-str  (newline-str (or (:newline opts) :lf))
        sep-str (str sep)
        q-str   (str q)
        quote?  (or (:quote? opts)
                    (fn [t] (field-needs-quote? t sep q)))]
    (check-char-opt :csv/write :separator sep)
    (check-char-opt :csv/write :quote q)
    (if (string? out)
      (let [acc (transient [])]
        (loop [xs (seq data)]
          (when xs
            (conj! acc (write-row (first xs) sep-str q-str quote?))
            (recur (next xs))))
        (let [rows (persistent! acc)]
          (spit out (if (seq rows)
                      (str (str/join nl-str rows) nl-str)
                      ""))))
      (binding [*out* out]
        (loop [xs (seq data)]
          (when xs
            (print (write-row (first xs) sep-str q-str quote?))
            (print nl-str)
            (recur (next xs)))))))
  nil)
