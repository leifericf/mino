(ns clojure.data.json)

;; JSON encoder and decoder for mino. Pure Clojure, no C primitive.
;; API mirrors clojure.data.json (read-str / write-str).

(require '[clojure.string :as str])

;;;; Pushback reader

(defn- make-reader
  "Build a 1-char pushback reader over `s`. Returns a map of closures
   over mutable position and pushback atoms. read-char returns a
   1-character string, or nil at EOF."
  [s]
  (let [pos    (atom 0)
        len    (count s)
        pushed (atom nil)]
    {:read-char   (fn []
                    (if-some [c @pushed]
                      (do (reset! pushed nil) c)
                      (if (< @pos len)
                        (let [p @pos]
                          (swap! pos inc)
                          (char-at s p))
                        nil)))
     :unread-char (fn [c]
                    (reset! pushed c))
     :pos         (fn [] @pos)}))

;;;; Predicates

(def ^:private ws-set #{" " "\t" "\n" "\r"})

(defn- ws? [c]
  (contains? ws-set c))

(def ^:private terminators #{"," "]" "}"})

(defn- terminator? [c]
  (or (nil? c) (ws? c) (contains? terminators c)))

(def ^:private digit-or-minus #{"0" "1" "2" "3" "4" "5" "6" "7" "8" "9" "-"})

;;;; Tokenizer

(defn- next-token
  "Skip whitespace and return the first non-whitespace character, or
   nil at EOF."
  [r]
  (let [c ((:read-char r))]
    (cond
      (nil? c) nil
      (ws? c)  (next-token r)
      :else    c)))

;;;; String parsing

(def ^:private hex-vals
  {"0" 0  "1" 1  "2" 2  "3" 3  "4" 4  "5" 5  "6" 6  "7" 7
   "8" 8  "9" 9  "a" 10 "b" 11 "c" 12 "d" 13 "e" 14 "f" 15
   "A" 10 "B" 11 "C" 12 "D" 13 "E" 14 "F" 15})

(defn- read-hex-4
  "Read four hex digits and return the codepoint as a 1-char string."
  [r]
  (let [read-char (:read-char r)
        h (fn [] (or (get hex-vals (read-char))
                     (throw (ex-info "Invalid hex digit in \\u escape" {}))))]
    (let [n (+ (* 4096 (h)) (* 256 (h)) (* 16 (h)) (h))]
      (str (char n)))))

(defn- read-escape
  "Read the character after a backslash and return the unescaped
   1-char string."
  [r]
  (let [c ((:read-char r))]
    (cond
      (= c "\"")  "\""
      (= c "\\")  "\\"
      (= c "/")   "/"
      (= c "b")   "\b"
      (= c "f")   "\f"
      (= c "n")   "\n"
      (= c "r")   "\r"
      (= c "t")   "\t"
      (= c "u")   (read-hex-4 r)
      (nil? c)    (throw (ex-info "Unexpected EOF in escape" {}))
      :else       (throw (ex-info (str "Invalid escape: \\" c) {})))))

(defn- read-string*
  "Read a quoted JSON string (opening quote already consumed)."
  ([r]
   (read-string* r (transient [])))
  ([r buf]
   (let [c ((:read-char r))]
     (cond
       (nil? c)   (throw (ex-info "Unexpected EOF in string" {}))
       (= c "\"") (apply str (persistent! buf))
       (= c "\\") (read-string* r (conj! buf (read-escape r)))
       :else      (read-string* r (conj! buf c))))))

;;;; Number parsing

(defn- read-number
  "Read a JSON number (first digit or minus already pushed back into
   r). Collects characters until a terminator, then parses via EDN
   read-string which handles the overlapping number syntax."
  ([r]
   (read-number r (transient [])))
  ([r buf]
   (let [c ((:read-char r))]
     (cond
       (terminator? c)
       (do (when c ((:unread-char r) c))
           (let [s (apply str (persistent! buf))]
             (try
               (read-string s)
               (catch e
                 (throw (ex-info (str "Invalid JSON number: " s) {}))))))
       :else
       (read-number r (conj! buf c))))))

;;;; Literals

(defn- read-literal
  "Read the remaining characters of a keyword literal (true, false,
   null) and return `result` if they match `expected`."
  [r expected result]
  (let [read-char (:read-char r)]
    (dotimes [i (count expected)]
      (let [c (read-char)]
        (when (not (= c (subs expected i (+ i 1))))
          (throw (ex-info (str "Invalid literal: expected \"" expected "\"") {})))))
    result))

;;;; Composite parsing

(defn- read-value
  "Dispatch on the first non-whitespace character and parse one JSON
   value."
  [r opts]
  (let [c (next-token r)]
    (cond
      (nil? c)                   (throw (ex-info "Unexpected EOF" {}))
      (= c "{")                  (read-object r opts)
      (= c "[")                  (read-array r opts)
      (= c "\"")                 (read-string* r)
      (= c "t")                  (read-literal r "rue" true)
      (= c "f")                  (read-literal r "alse" false)
      (= c "n")                  (read-literal r "ull" nil)
      (contains? digit-or-minus c)
                                 (do ((:unread-char r) c) (read-number r))
      :else                      (throw (ex-info (str "Unexpected character: " c) {})))))

(defn- read-array-rest
  "Read remaining array elements after the first value (already in
   result). Returns the final persistent vector."
  [r opts result]
  (let [sep (next-token r)]
    (cond
      (= sep "]") (persistent! result)
      (= sep ",") (read-array-rest r opts (conj! result (read-value r opts)))
      :else       (throw (ex-info "Expected , or ]" {})))))

(defn- read-array
  "Read a JSON array (opening bracket already consumed)."
  [r opts]
  (let [c (next-token r)]
    (cond
      (= c "]") []
      (nil? c)  (throw (ex-info "Unexpected EOF in array" {}))
      :else
      (do ((:unread-char r) c)
          (read-array-rest r opts (conj! (transient []) (read-value r opts)))))))

(defn- read-key
  "Read an object key (must be a quoted string)."
  [r]
  (let [c (next-token r)]
    (if (= c "\"")
      (read-string* r)
      (throw (ex-info "Expected string key" {})))))

(defn- expect-colon
  "Consume whitespace and assert the next character is a colon."
  [r]
  (let [c (next-token r)]
    (when (not (= c ":"))
      (throw (ex-info "Expected : after key" {})))))

(defn- read-object-rest
  "Read remaining key-value pairs after the first pair (already in
   result). Returns the final persistent map."
  [r opts key-fn result]
  (let [raw-k (read-key r)
        k     (if key-fn (key-fn raw-k) raw-k)]
    (expect-colon r)
    (let [v   (read-value r opts)
          sep (next-token r)]
      (cond
        (= sep "}") (persistent! (assoc! result k v))
        (= sep ",") (read-object-rest r opts key-fn (assoc! result k v))
        :else       (throw (ex-info "Expected , or }" {}))))))

(defn- read-object
  "Read a JSON object (opening brace already consumed)."
  [r opts]
  (let [key-fn (:key-fn opts)
        c (next-token r)]
    (cond
      (= c "}") {}
      (nil? c)  (throw (ex-info "Unexpected EOF in object" {}))
      :else
      (do ((:unread-char r) c)
          (read-object-rest r opts key-fn (transient {}))))))

;;;; Public API

(defn read-str
  "Parse a JSON string into Clojure data. Objects return as maps with
   string keys by default. Pass {:key-fn keyword} for keyword keys."
  [string & {:as opts}]
  (read-value (make-reader string) opts))

(defn write-str
  "Serialize Clojure data to a JSON string. nil becomes null, keywords
   become strings (via name), maps become objects, vectors and seqs
   become arrays."
  [x & {:as opts}]
  (throw (ex-info "clojure.data.json/write-str: not yet implemented" {})))
