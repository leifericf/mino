(ns clojure.data.json)

;; JSON codec for mino. The reader is the native json-parse primitive
;; (see docs/adr/23-json-reader-in-c-writer-stays-clojure.md); the
;; writer is pure Clojure, linear through the str builder. API mirrors
;; clojure.data.json (read-str / write-str).

(require '[clojure.string :as str])

;;;; Public API

(defn read-str
  "Parse a JSON string into Clojure data. Objects return as maps with
   string keys by default. Pass {:key-fn keyword} for keyword keys."
  [string & {:as opts}]
  (json-parse string (:key-fn opts)))

;;;; String escaping (writer)

(def ^:private escape-map
  {"\"" "\\\"" "\\" "\\\\" "\n" "\\n" "\r" "\\r"
   "\t" "\\t" "\b" "\\b" "\f" "\\f"})

(defn- escape-char
  "Return the JSON escape sequence for a single character string, or
   nil if no escape is needed."
  [c]
  (get escape-map c))

(defn- non-ascii? [c]
  (let [cp (int (first c))]
    (or (< cp 32) (> cp 127))))

(defn- hex-digit [n]
  (if (< n 10)
    (str n)
    (str (char (+ 55 n)))))

(defn- unicode-escape [n]
  (if (> n 0xFFFF)
    ;; Astral codepoint: JSON \u escapes are 16-bit, so emit the
    ;; surrogate pair. The reader concatenates the pair's CESU-8
    ;; halves, which is exactly the UTF-8 encoding of the codepoint.
    (let [v (- n 0x10000)]
      (str (unicode-escape (+ 0xD800 (quot v 1024)))
           (unicode-escape (+ 0xDC00 (mod v 1024)))))
    (str "\\u"
         (hex-digit (quot n 4096))
         (hex-digit (quot (mod n 4096) 256))
         (hex-digit (quot (mod n 256) 16))
         (hex-digit (mod n 16)))))

(def ^:private plain-run-re
  #"^[a-zA-Z0-9 ,.;:!?(){}<>+*=_&%$#'`~/|-]+$")

(defn- write-string-chars
  "Escape a string for JSON. Strings made entirely of characters
   that need no escape pass through untouched (one C-backed scan);
   otherwise each character is checked against the escape map and
   the printable range, with non-ASCII and astral characters
   emitted as \\uXXXX / surrogate pairs."
  [s]
  (if (re-matches plain-run-re s)
    s
    (apply str
           (map (fn [c]
                  (cond
                    (escape-char c) (escape-char c)
                    (non-ascii? c)   (unicode-escape (int (first c)))
                    :else            c))
                (map #(subs s % (inc %)) (range (count s)))))))

;;;; Number formatting (writer)
(defn- format-double
  "Format a double at the shortest %g precision that reads back as
  the same value, so doubles round-trip exactly through write-str and
  read-str. Integral results keep a .0 suffix so they read back as
  doubles, not integers."
  ([x]
   (format-double x 15))
  ([x prec]
   (let [f (format (str "%." prec "g") x)]
     (cond
     (= x (read-string f)) (if (or (str/includes? f ".")
                                  (str/includes? f "e"))
                            f
                            (str f ".0"))
       (< prec 17)        (format-double x (inc prec))
       :else              f))))

;;;; Type dispatch (writer)

(defn- write-json
  "Dispatch on Clojure type and serialize to a JSON string."
  [x]
  (cond
    (nil? x)       "null"
    (true? x)      "true"
    (false? x)     "false"
    (string? x)    (str "\"" (write-string-chars x) "\"")
    (number? x)    (cond
                     (or (ratio? x)
                         (infinite? x)
                         ;; NaN compares false against both bounds;
                         ;; every finite value clears one of them.
                         (not (or (< x ##Inf) (> x ##-Inf))))
                     (throw (ex-info (str "Cannot serialize " (str x)
                                          " to JSON") {}))

                     (double? x) (format-double x)
                     :else       (str x))
    (keyword? x)   (str "\"" (write-string-chars (name x)) "\"")
    (map? x)       (write-object x)
    (vector? x)    (write-array x)
    (seq? x)       (write-array (vec x))
    :else          (throw (ex-info (str "Cannot serialize " (str x) " to JSON") {}))))

(defn- write-array-rest
  "Serialize remaining vector elements after the first. Returns a
   string fragment."
  [v i]
  (if (>= i (count v))
    "]"
    (str "," (write-json (nth v i)) (write-array-rest v (+ i 1)))))

(defn- write-array [v]
  (if (empty? v)
    "[]"
    (str "[" (write-json (first v)) (write-array-rest v 1))))

(defn- write-object-rest
  "Serialize remaining map entries after the first. Returns a string
   fragment."
  [entries]
  (if (empty? entries)
    "}"
    (let [[k v] (first entries)
          key-str (cond
                    (string? k)  k
                    (keyword? k) (name k)
                    :else        (str k))]
      (str "," (str "\"" (write-string-chars key-str) "\"")
           ":" (write-json v)
           (write-object-rest (rest entries))))))

(defn- write-object [m]
  (if (empty? m)
    "{}"
    (let [[k v] (first m)
          key-str (cond
                    (string? k)  k
                    (keyword? k) (name k)
                    :else        (str k))]
      (str "{" (str "\"" (write-string-chars key-str) "\"")
           ":" (write-json v)
           (write-object-rest (rest m))))))

;;;; Public API (writer)

(defn write-str
  "Serialize Clojure data to a JSON string. nil becomes null, keywords
   become strings (via name), maps become objects, vectors and seqs
   become arrays.

   This port does not yet implement clojure.data.json's writer options
   (:key-fn, :value-fn, :escape-unicode, :escape-slash). Passing any of
   them throws rather than silently ignoring them and producing output
   that differs from what the caller asked for."
  [x & {:as opts}]
  (when (seq opts)
    (throw (ex-info (str "write-str options are not supported: "
                         (vec (keys opts)))
                    {:unsupported (vec (keys opts))})))
  (write-json x))
