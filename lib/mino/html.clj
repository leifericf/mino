(ns mino.html
  "Tolerant HTML reader over hickory-shaped node maps (ADR 28).

  (require '[mino.html :as html])
  (html/parse \"<p>hello\")                ; => the document node map
  (html/parse-fragment \"<p>a<p>b\")       ; => [node node]
  (html/to-html (html/parse \"<p>x\"))     ; => \"<html>...</html>\"

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

;;; ---- serializer (design D10; the writer stays Clojure, ADR 23/24) ----

;; The serializer is one explicit worklist loop, no per-node closures
;; and no case dispatch (the interpreter's per-call constants make
;; recursive closure walkers 30x slower at megabyte scale; the html
;; scaling gate pins this).

(def ^:private html-text-re #"[&<>]")
(def ^:private html-text-esc {"&" "&amp;" "<" "&lt;" ">" "&gt;"})
(def ^:private html-attr-re #"[&\"]")
(def ^:private html-attr-esc {"&" "&amp;" "\"" "&quot;"})

(def ^:private html-tag-class
  "Element classes with special emission: :void elements emit no end
  tag (WHATWG tier rule 4 list); :script/:style are RAWTEXT and
  :plaintext rest-of-input (their string children emit verbatim,
  D6/D10); everything else (nil) opens and closes normally."
  {:area :void :base :void :br :void :col :void :embed :void :hr :void
   :img :void :input :void :link :void :meta :void :param :void
   :source :void :track :void :wbr :void
   :script :raw :style :raw :plaintext :plain})

(defn- html-escape-text
  "Text content: amp lt gt, the D10 minimal spelling for text. One
  regex pass, skipped when clean (re-find is the cheap scan);
  escaping amp first is inherent (no double escapes)."
  [s]
  (if (re-find html-text-re s)
    (str/replace s html-text-re html-text-esc)
    s))

(defn- html-escape-attr
  "Attribute values: amp quot. Double quotes delimit the value, so
  quot is structural; lt and gt stay raw (a quoted value reparses
  them unchanged)."
  [s]
  (if (re-find html-attr-re s)
    (str/replace s html-attr-re html-attr-esc)
    s))

(defn- html-attrs-str
  "Attributes in map order (written order survives parse), each
  value double-quoted; a nil or empty map emits nothing."
  [attrs]
  (if (seq attrs)
    (loop [es (seq attrs) acc ""]
      (if es
        (let [e (first es)]
          (recur (next es)
                 (str acc " " (name (key e)) "=\""
                      (html-escape-attr (str (val e))) "\"")))
        acc))
    ""))

(defn- html-push-all
  "Conjs the entries onto the worklist in reverse, so they pop in
  written order."
  [st entries]
  (if (vector? entries)
    (into st (rseq entries))
    (into st (reverse entries))))

(defn- html-push-raw
  "Like html-push-all but wraps string children as verbatim markers
  (RAWTEXT and PLAINTEXT interiors; anything else serializes
  normally). Index walk: seq materialization over large vectors is
  O(n) in this runtime."
  [st children]
  (if (vector? children)
    (let [n (count children)]
      (loop [i (dec n) st st]
        (if (>= i 0)
          (let [c (nth children i)]
            (recur (dec i) (conj st (if (string? c) [c] c))))
          st)))
    (loop [cs (reverse children) st st]
      (if cs
        (let [c (first cs)]
          (recur (next cs) (conj st (if (string? c) [c] c))))
        st))))

(defn- html-push-element
  "Rewrites the worklist for the element's content: normal elements
  get their close tag beneath the children, void elements none,
  RAWTEXT a close tag beneath verbatim children, PLAINTEXT no close
  tag at all (an end tag would join the verbatim run on reparse).
  Returns the new worklist."
  [x st]
  (let [tag (name (:tag x))
        cls (get html-tag-class (:tag x))
        content (or (:content x) [])]
    (if (nil? cls)
      (html-push-all (conj st [(str "</" tag ">")]) content)
      (if (identical? cls :void)
        (html-push-all st content)
        (if (identical? cls :raw)
          (html-push-raw (conj st [(str "</" tag ">")]) content)
          (html-push-raw st content))))))

(defn- html-serialize
  "The worklist walk: entries are node maps (emitted by type), bare
  strings (escaped text), or one-element vectors (verbatim string
  fragments: close tags, RAWTEXT interiors)."
  [node]
  (loop [st (if (sequential? node)
              (html-push-all [] node)
              [node])
         acc (transient [])]
    (if (pos? (count st))
      (let [x (peek st)
            st (pop st)]
        (cond
          (string? x)
          (recur st (conj! acc (html-escape-text x)))

          (vector? x)
          (recur st (conj! acc (nth x 0)))

          (map? x)
          (let [typ (:type x)]
            (cond
              (= :element typ)
              (recur (html-push-element x st)
                     (conj! acc (str "<" (name (:tag x))
                                     (html-attrs-str (:attrs x)) ">")))

              (= :document typ)
              (recur (html-push-all st (or (:content x) [])) acc)

              (= :comment typ)
              (recur st (conj! acc (str "<!--"
                                        (or (first (:content x)) "")
                                        "-->")))

              (= :document-type typ)
              (recur st (conj! acc (str "<!DOCTYPE "
                                        (or (first (:content x)) "")
                                        ">")))

              (nil? typ)
              ;; the shared JVM clojure.xml element shape: XML trees
              ;; round-trip through to-html on :tag/:attrs/:content
              (if (contains? x :tag)
                (recur (html-push-element x st)
                       (conj! acc (str "<" (name (:tag x))
                                       (html-attrs-str (:attrs x)) ">")))
                (throw-opts "to-html requires a node with :type or :tag" x))

              :else (throw-opts "to-html got an unknown node :type" typ)))

          (sequential? x)
          (recur (html-push-all st x) acc)

          :else (throw-opts "to-html requires a node" x)))
      (str/join (persistent! acc)))))

(defn to-html
  "Serializes node into HTML text. node may be a hickory node map
  (:document, :element, :comment, :document-type), a bare text
  string, a collection of nodes (fragment serialization), or a JVM
  clojure.xml element map ({:tag :attrs :content}; the shared shape,
  so XML trees round-trip through this fn).

  (html/to-html (html/parse \"<p>hello\"))
  => \"<html><head></head><body><p>hello</p></body></html>\"

  Normalization (design D10, pinned by tests/html_serialize_test):
  names emit as written (parse lowercased them in HTML mode);
  attribute order preserved as written; attribute values always
  double-quoted with amp and quot escaped (lt and gt stay raw);
  valueless attributes emit as name=\"\"; adjacent text runs merge
  and empty text runs vanish (unrepresentable in HTML); entities
  re-emit minimally (amp lt gt escaped in text and RCDATA content;
  nothing escaped in RAWTEXT content); void elements emit without
  end tags; every non-void element is explicitly closed, so implied
  closes and the synthesized html/head/body wrappers materialize;
  comments and the DOCTYPE text emit verbatim; script, style, and
  PLAINTEXT content emits verbatim (RCDATA title/textarea content
  re-encodes like text, D6).

  Contract: (parse (to-html (parse s))) equals (parse s) modulo the
  adjacent-run merge for every input the tolerance tier accepts, and
  output is byte-exact over canonical-form fixtures (lowercase
  names, double-quoted values, explicit closes). opts is a keyword
  map, reserved and accepted but ignored in v1; invalid nodes and
  opts throw ex-info :kind :html/opts."
  ([node] (to-html node nil))
  ([node opts]
   (when-not (or (nil? opts) (map? opts))
     (throw-opts "to-html opts must be a map" opts))
   (html-serialize node)))
