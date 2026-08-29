(ns clojure.xml
  "Read XML into the JVM clojure.xml node shape (ADR 28).

  (require '[clojure.xml :as xml])
  (xml/parse \"<a k=\\\"1\\\">t &amp; more</a>\")
  => {:tag :a :attrs {:k \"1\"} :content [\"t & more\"]}

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
  full contract).

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
