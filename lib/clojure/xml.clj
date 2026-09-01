(ns clojure.xml
  "Read and emit XML in the JVM clojure.xml node shape (ADR 28).

  parse reads XML text into the element tree; emit and emit-element
  print an element tree back to *out* in the reference shape, with
  entity escaping so emitted documents always reparse (the one
  recorded divergence from the reference body, which prints raw).

  (require '[clojure.xml :as xml])
  (xml/parse \"<a k=\\\"1\\\">t &amp; more</a>\")
  => {:tag :a :attrs {:k \"1\"} :content [\"t & more\"]}
  (with-out-str (xml/emit-element {:tag :a :attrs {} :content nil}))
  => \"<a/>\\n\"

  Elements are plain maps {:tag keyword :attrs {keyword string}
  :content [string|node]}, the exact JVM shape: :attrs {} and
  :content [] are always present, names keep their case (a QName
  prefix keywordizes at its first colon, so dc:creator reads back
  as :dc/creator), and parse returns the root element only, with
  comments, processing instructions, and the DOCTYPE dropped.
  Character data merges across them into one string per position;
  whitespace-only text and tails are kept verbatim inside the root
  and dropped outside it.

  The reader is the native single-pass xml-parse prim (ADR 28, the
  json/csv/toml/yaml/html lineage), strict: only the five
  predefined entities plus numeric character references resolve;
  any other named reference, a bare ampersand, or an out-of-range
  code point throws :undefined-entity. A DOCTYPE is accepted and
  dropped, but any internal-subset ENTITY declaration throws
  :unsupported-doctype, so XXE is impossible by construction:
  nothing is ever honored or fetched. Attribute values must be
  quoted and get XML 1.0 3.3.3 whitespace normalization (literal
  tabs and newlines to spaces; character references pass through).
  Line endings normalize to LF and a leading UTF-8 BOM drops.

  Errors throw a diagnostic with :mino/kind :xml/parse, a :code
  keyword, :location {:line :col} (1-based, bytes, at the failing
  token), and :text (the source line) in :mino/data. v1 codes:
  :unexpected-eof,
  :unexpected-token, :undefined-entity, :unsupported-doctype,
  :mismatched-end-tag, :duplicate-attribute, :multiple-roots,
  :content-before-root, :invalid-prolog, :invalid-name,
  :max-depth.

  Divergence from the JVM, pinned by tests: input is a STRING
  first (the JVM takes File/InputStream/URI); there is no
  startparse injection; undeclared namespace prefixes parse (the
  JVM's non-namespace-aware SAX behavior, not a resolving
  parser's); character data merges across comments, PIs, and CDATA
  (the JVM emits one string per SAX characters event). opts is a
  keyword map, reserved and ignored in v1."
  (:require [clojure.string :as str]))

(defn- xml-fail
  "Throws a classified clojure.xml diagnostic (ADR 37): :mino/kind
  names the error class so classed catch dispatches on it,
  :mino/message the human string ex-message returns, :mino/data the
  detail ex-data reads."
  [kind code msg data]
  (throw {:mino/kind kind :mino/code code :mino/message msg
          :mino/data data}))

(defn- throw-opts
  [msg arg]
  (xml-fail :xml/opts "MXO001" (str "clojure.xml: " msg) {:arg arg}))

(defn parse
  "Parses s, a string of XML 1.0, into the root element map
  {:tag keyword :attrs {keyword string} :content [string|node]}
  (the JVM clojure.xml shape; see the namespace docstring for the
  full contract). emit is the companion printer back to XML text.

  (xml/parse \"<rss version=\\\"2.0\\\"/>\")
  => {:tag :rss :attrs {:version \"2.0\"} :content []}

  opts is a keyword map, reserved and accepted but ignored in v1.
  Throws :mino/kind :xml/parse with :code, :location {:line :col},
  and :text on malformed input; strict XML never silently
  misparses. Non-string input and non-map opts throw :xml/opts."
  ([s] (parse s nil))
  ([s opts]
   (when-not (string? s)
     (throw-opts "parse requires a string" s))
   (when-not (or (nil? opts) (map? opts))
     (throw-opts "parse opts must be a map" opts))
   (let [r (xml-parse s opts)]
     (if (and (vector? r)
              (seq r)
              (= :xml/error (nth r 0)))
       (xml-fail :xml/parse "MXP001"
                 (str "clojure.xml: " (nth r 1))
                 {:code (keyword (nth r 1))
                  :location {:line (nth r 2)
                             :col (nth r 3)}
                  :text (nth r 4)})
       r))))

;;;; Emitter

(defn- emit-fail
  [msg data]
  (xml-fail :xml/emit "MXE001" (str "clojure.xml: " msg) data))

(defn- elem-name
  [k]
  (cond (keyword? k) (if (namespace k)
                       (str (namespace k) ":" (name k))
                       (name k))
        (string? k) k
        :else (emit-fail "a tag or attribute name must be a keyword or string"
                         {:name k})))

(defn- escape-content
  [s]
  (-> s
      (str/replace "&" "&amp;")
      (str/replace "<" "&lt;")
      (str/replace ">" "&gt;")))

;; Attribute whitespace rides character references: a literal tab or
;; newline would normalize to a space on reparse (XML 1.0 3.3.3).
(defn- escape-attr
  [s]
  (-> s
      (str/replace "&" "&amp;")
      (str/replace "<" "&lt;")
      (str/replace ">" "&gt;")
      (str/replace "'" "&apos;")
      (str/replace "\"" "&quot;")
      (str/replace "\r" "&#13;")
      (str/replace "\n" "&#10;")
      (str/replace "\t" "&#9;")))

(defn emit-element
  "Prints node e to *out* in the reference shape: a string on its own
  line, an element as its tag line, contents, and closing line, with
  nil content self-closing. Entity escaping of & < > (and quotes in
  attribute values) is the recorded divergence from the reference
  body, so emitted documents always reparse. Throws :mino/kind
  :xml/emit for a value that is neither element map nor string."
  [e]
  (cond
    (string? e) (println (escape-content e))
    (and (map? e) (:tag e))
    (do (print (str "<" (elem-name (:tag e))))
        (doseq [attr (:attrs e)]
          (print (str " " (elem-name (key attr)) "='"
                      (escape-attr (str (val attr))) "'")))
        (if (:content e)
          (do (println ">")
              (doseq [c (:content e)]
                (emit-element c))
              (println (str "</" (elem-name (:tag e)) ">")))
          (println "/>")))
    :else (emit-fail "emit-element requires an element map or a string"
                     {:node e})))

(defn emit
  "Prints the element tree x to *out* as an XML document: the
  declaration line, then emit-element."
  [x]
  (println "<?xml version='1.0' encoding='UTF-8'?>")
  (emit-element x))
