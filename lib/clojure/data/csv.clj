(ns clojure.data.csv)

;; CSV codec for mino, mirroring clojure.data.csv (read-csv /
;; write-csv). Behavior follows the python3 csv module, the RFC 4180
;; reference oracle the golden vectors were captured from: lenient
;; about stray quotes, a lone \r ends a record, blank lines read as
;; empty rows.

(require '[clojure.string :as str])

;;;; Options

(defn- check-char-opt
  "separator / quote must be characters; anything else compares false
  against every parsed char and silently yields garbage."
  [kind k v]
  (when-not (char? v)
    (throw (ex-info (str "csv: :" k " must be a character")
                    {:kind kind :option k :value v}))))

(defn- newline-str
  [newline]
  (case newline
    :lf   "\n"
    :crlf "\r\n"
    :cr   "\r"
    (throw (ex-info "csv: :newline must be :lf, :crlf or :cr"
                    {:kind :csv/write :option :newline :value newline}))))

;;;; Reader

(defn- scan-to-delim
  "Index of the next separator, newline or end at or after i."
  [s i len sep]
  (loop [j i]
    (if (>= j len)
      len
      (let [c (nth s j)]
        (if (or (= c sep) (= c \newline) (= c \return))
          j
          (recur (inc j)))))))

(defn- scan-quoted
  "From s[i], the opening quote. Doubled quotes escape; returns
  [field-text j] with j just past the closing quote, or at len when
  the field is unterminated (the remainder is then the field)."
  [s i len q]
  (loop [run-start (inc i)
         j         (inc i)
         acc       (transient [])]
    (if (>= j len)
      [(str/join (persistent! (conj! acc (subs s run-start len)))) len]
      (let [c (nth s j)]
        (if (= c q)
          (if (and (< (inc j) len) (= (nth s (inc j)) q))
            (recur (+ j 2) (+ j 2)
                   (conj! acc (str (subs s run-start j) q)))
            [(str/join (persistent! (conj! acc (subs s run-start j))))
             (inc j)])
          (recur run-start (inc j) acc))))))

(defn- parse-field
  "One field from i; returns [text j] with j at the separator, record
  end or end of input that follows it. Characters after a closing
  quote join the field raw, matching the oracle."
  [s i len sep q]
  (if (= (nth s i) q)
    (let [[text j] (scan-quoted s i len q)]
      (if (>= j len)
        [text j]
        (let [stop (scan-to-delim s j len sep)]
          (if (= stop j)
            [text j]
            [(str text (subs s j stop)) stop]))))
    (let [stop (scan-to-delim s i len sep)]
      [(subs s i stop) stop])))

(defn- skip-record-end
  "From i at a newline: just past it, folding a \r\n pair into one end."
  [s i len]
  (if (= (nth s i) \return)
    (if (and (< (inc i) len) (= (nth s (inc i)) \newline))
      (+ i 2)
      (inc i))
    (inc i)))

(defn- parse-record
  "One record from i; returns [row next-i] with next-i past the record
  end. A record end reached directly is an empty row; one reached
  after a separator closes an empty trailing field."
  [s i len sep q]
  (loop [i      i
         fields (transient [])
         start? true]
    (if (>= i len)
      [(persistent! (if start? fields (conj! fields ""))) i]
      (let [c (nth s i)]
        (if (or (= c \newline) (= c \return))
          [(persistent! (if start? fields (conj! fields "")))
           (skip-record-end s i len)]
          (let [[text j] (parse-field s i len sep q)
                fields'  (conj! fields text)]
            (if (>= j len)
              [(persistent! fields') j]
              (if (= (nth s j) sep)
                (recur (inc j) fields' false)
                [(persistent! fields') (skip-record-end s j len)]))))))))

(defn- string-records
  "Lazy seq of records over the whole input text, index-carried so no
  step re-slices the source."
  [s i len sep q]
  (lazy-seq
    (when (< i len)
      (let [[row next-i] (parse-record s i len sep q)]
        (cons row (string-records s next-i len sep q))))))

(defn- atom-records
  "Lazy seq of records consumed from a cursor atom (the *in* /
  with-in-str model); each realized record swaps the atom down to the
  unconsumed text."
  [a sep q]
  (lazy-seq
    (let [s @a]
      (when (and (string? s) (pos? (count s)))
        (let [len          (count s)
              [row next-i] (parse-record s 0 len sep q)]
          (reset! a (subs s next-i))
          (cons row (atom-records a sep q)))))))

(defn read-csv
  "Read CSV data and return a lazy sequence of vector rows of strings.
   input is the CSV text, or a string-cursor atom like *in* (see
   with-in-str) consumed as it is realized. The reader recognizes
   \n, \r\n and lone \r record ends and keeps quoted separators,
   quotes and newlines as field data.

   Opts: :separator (default \\,), :quote (default \\\").

   (read-csv \"a,b\\n\") => ([\"a\" \"b\"])"
  [input & {:as opts}]
  (let [sep (or (:separator opts) \,)
        q   (or (:quote opts) \")]
    (check-char-opt :csv/parse :separator sep)
    (check-char-opt :csv/parse :quote q)
    (cond
      (string? input) (string-records input 0 (count input) sep q)
      (atom? input)   (atom-records input sep q)
      :else           (throw (ex-info
                               "read-csv: input must be CSV text or a string-cursor atom"
                               {:kind :csv/parse :input input})))))

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
