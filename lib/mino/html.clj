(ns mino.html
  "Tolerant HTML reader over hickory-shaped node maps (ADR 28).

  (require '[mino.html :as html])
  (html/parse \"<p>hello\")                ; => the document node map
  (html/parse-fragment \"<p>a<p>b\")       ; => [node node]

  Nodes are plain maps: elements {:type :element :tag keyword
  :attrs {keyword string} :content [node|string]} with lowercase tag
  and attribute names, {:type :comment :content [text]},
  {:type :document-type :content [text]} for the first DOCTYPE, and
  text as bare strings inside :content. parse returns the document
  map {:type :document :content [...]} directly with synthesized
  html/head/body wrappers when absent (explicit tags win);
  parse-fragment returns a vector of top-level nodes with no
  synthesis and no wrapper.

  The reader is the native single-pass html-parse prim (ADR 28, the
  json/csv/toml/yaml reader lineage) implementing the campaign's
  pinned tolerance tier: unclosed tags auto-balance at EOF, stray end
  tags drop, misnested end tags pop-until, the WHATWG void list, the
  trailing solidus honored on every start tag, script/style RAWTEXT
  with title/textarea RCDATA, entity decoding through the
  python-oracle table (semicolonless legacy names resolve in text,
  semicolon-terminated names only inside attribute values), bogus
  comments, name lowercasing, PLAINTEXT rest-of-input, and NUL to
  U+FFFD in text. This namespace owns argument validation and the
  error contract: errors throw ex-info with :kind :html/parse, a
  :code keyword, :location {:line :col} (1-based, bytes), and the
  offending :text (source line). v1 code: :max-depth (the 256
  open-element cap; everything else recovers).

  Divergences from hickory, pinned by tests: parse returns the node
  map directly (no parser object), and malformed input a browser
  would recover from yields either the recovered tree or a positioned
  ex-info where hickory never throws. opts are keyword maps,
  reserved and ignored in v1."
  (:require [clojure.string :as str]))

(defn- throw-opts
  [msg arg]
  (throw (ex-info (str "mino.html: " msg)
                  {:kind :html/opts :arg arg})))

(defn- run-prim
  [who s opts fragment]
  (when-not (string? s)
    (throw-opts (str who " requires a string") s))
  (when-not (or (nil? opts) (map? opts))
    (throw-opts (str who " opts must be a map") opts))
  (let [r (html-parse s (if fragment {:fragment true} nil))]
    (if (and (vector? r)
             (seq r)
             (= :html/error (nth r 0)))
      (throw (ex-info (str "mino.html: " (nth r 1))
                      {:kind :html/parse
                       :code (keyword (nth r 1))
                       :location {:line (nth r 2)
                                  :col (nth r 3)}
                       :text (nth r 4)}))
      r)))

(defn parse
  "Parses s as an HTML document into the hickory-shaped document node
  map {:type :document :content [...]}, synthesizing html/head/body
  wrappers when absent (explicit tags win; head elements that precede
  body content route into head; leading whitespace drops).

  (html/parse \"<p>hello\")
  => {:type :document :content [{:type :element :tag :html ...}]}

  opts is a keyword map, reserved and accepted but ignored in v1.
  Throws ex-info :kind :html/parse with :code, :location {:line
  :col}, and :text on the only non-recovering edge, the 256-deep
  open-element cap (:code :max-depth)."
  ([s] (parse s nil))
  ([s opts]
   (run-prim "parse" s opts false)))

(defn parse-fragment
  "Parses s as a fragment: a vector of top-level nodes with no
  document wrapper, no implied html/head/body, and whitespace text
  kept verbatim. Tolerance rules 1-16 (implied closes, stray-end
  drop, pop-until, void, solidus, raw text, entities) still apply.

  (html/parse-fragment \"<li>a<li>b\")
  => [{:type :element :tag :li :content [\"a\"]} {:type :element :tag :li ...}]

  Options and the error contract are as in parse."
  ([s] (parse-fragment s nil))
  ([s opts]
   (run-prim "parse-fragment" s opts true)))
