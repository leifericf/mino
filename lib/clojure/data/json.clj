(ns clojure.data.json)

;; JSON encoder and decoder for mino. Pure Clojure, no C primitive.
;; API mirrors clojure.data.json (read-str / write-str).

(defn read-str
  "Parse a JSON string into Clojure data. Objects return as maps with
   string keys by default. Pass {:key-fn keyword} for keyword keys."
  [string & {:as opts}]
  (throw (ex-info "clojure.data.json/read-str: not yet implemented" {})))

(defn write-str
  "Serialize Clojure data to a JSON string. nil becomes null, keywords
   become strings (via name), maps become objects, vectors and seqs
   become arrays."
  [x & {:as opts}]
  (throw (ex-info "clojure.data.json/write-str: not yet implemented" {})))
