(require "tests/test")
(require '[clojure.string :as str])
(require '[clojure.edn :as edn])
(require '[mino.html :as html])

;; Tolerant HTML reader tests (html-xml campaign p2, ADR 28).
;;
;; Three layers, one file:
;;
;; 1. The golden corpus: tests/fixtures/html/golden.edn is the
;;    python3 html.parser oracle dump (p1t3). Every vector whose
;;    input avoids the ledgered divergence classes is parsed by the
;;    reader and compared against the golden :tree keywordized per
;;    the hickory node spec (p1 decision: names keywordize at the
;;    comparison boundary). The golden :tree is a naive stack
;;    reconstruction over tier rules 1-4 only (ledger line 12), so
;;    inputs where rules 15/16 would fire are filtered by the
;;    conservative trigger walk below and pinned instead by the hand
;;    vectors. The other filters name their ledger lines inline.
;;
;; 2. The tolerance tier: the enumerated rule set from the campaign
;;    technical design ("The Tolerance Tier"), one or more vectors
;;    per rule, spec-cited in comments. These are hand-written
;;    expectations (D11: tree fixups are not python-oracled).
;;
;; 3. Error shapes and the surface audit: the :max-depth edge, the
;;    prim error descriptor, and the documented divergences from
;;    hickory (parse returns the node map directly; positioned
;;    ex-info where hickory never throws).

;;; ---- node builders ----

(defn- html-el
  ([tag] {:type :element :tag tag :attrs {} :content []})
  ([tag content] {:type :element :tag tag :attrs {} :content content})
  ([tag attrs content] {:type :element :tag tag :attrs attrs
                       :content content}))

(defn- html-com [s] {:type :comment :content [s]})
(defn- html-doct [s] {:type :document-type :content [s]})
(defn- html-doc [& content] {:type :document :content (vec content)})

;;; ---- golden corpus ----

(def ^:private html-golden-all
  (edn/read-string (slurp "tests/fixtures/html/golden.edn")))

(def ^:private html-golden-void
  #{"area" "base" "br" "col" "embed" "hr" "img" "input" "link" "meta"
    "param" "source" "track" "wbr"})

(def ^:private html-golden-r15
  "Tier rule 15 like-tag close set."
  #{"li" "dd" "dt" "option" "optgroup" "p" "rb" "rp" "rt" "rtc" "tr"
    "td" "th" "tbody" "thead" "tfoot" "caption" "colgroup"})

(def ^:private html-golden-p-closers
  "Tier rule 16 p-closing list, verbatim (WHATWG 13.2.6.3)."
  #{"address" "article" "aside" "blockquote" "center" "details"
    "dialog" "dir" "div" "dl" "fieldset" "figcaption" "figure"
    "footer" "form" "h1" "h2" "h3" "h4" "h5" "h6" "header" "hgroup"
    "hr" "main" "menu" "nav" "ol" "p" "pre" "search" "section"
    "summary" "table" "ul"})

(def ^:private html-golden-hs #{"h1" "h2" "h3" "h4" "h5" "h6"})

(def ^:private html-golden-tag-tok-re
  #"<(/?)([a-zA-Z][^\t\n\r\f />]*)[^>]*>")

(defn- html-golden-pop-until
  [open name]
  (loop [st open]
    (if (or (empty? st) (= (first st) name))
      (rest st)
      (recur (rest st)))))

(defn- html-golden-tier-trigger?
  "Conservative flag for inputs where an implied close (tier rules
  15/16) fires, so the naive golden :tree (rules 1-4 only, ledger
  line 12) is not comparable: a like-named open element, an open p
  under any rule-16 closer, or an h1-h6 on top of an h1-h6. The walk
  honors void elements and solidus; it over-flags, never misses."
  [input]
  (let [toks (vec (re-seq html-golden-tag-tok-re input))
        n (count toks)]
    (loop [i 0 open () tri? false]
      (if (or tri? (= i n))
        tri?
        (let [m (nth toks i)
              close (= "/" (nth m 1))
              name (str/lower-case (nth m 2))
              full (nth m 0)]
          (if close
            (if (some #(= % name) open)
              (recur (inc i) (html-golden-pop-until open name) tri?)
              (recur (inc i) open tri?))
            (let [self-close (str/ends-with? full "/>")
                  void (contains? html-golden-void name)
                  fire (or (and (contains? html-golden-r15 name)
                                (some #(= % name) open))
                           (and (contains? html-golden-p-closers name)
                                (some #(= % "p") open))
                           (and (contains? html-golden-hs name)
                                (some #(contains? html-golden-hs %)
                                      (take 1 open))))]
              (recur (inc i)
                     (if (or void self-close) open (cons name open))
                     (or tri? fire)))))))))

(defn- html-golden-skip?
  "The ledgered divergence classes the p2 comparison must not apply
  python expectations for: raw-text scope wider than the tier
  (ledger 8), NUL in text (ledger 9), semicolonless attribute
  entities (ledger 4), marked sections whose oracle reconstruction
  differs from the bogus-comment span (ledger 6), and the rule 15/16
  tier triggers (ledger 12). Oracle-skipped vectors carry no :events
  at all."
  [v]
  (let [input (:input v)]
    (or (not (contains? v :events))
        (str/includes? input "\u0000")
        (boolean (re-find
                   #"(?i)<(/?)(xmp|iframe|noembed|noframes)([\t\n\r\f />]|$)"
                   input))
        (boolean (re-find
                   #"<[a-zA-Z/][^>]*&[a-zA-Z][a-zA-Z0-9]*[^;a-zA-Z0-9]"
                   input))
        (boolean (re-find #"<!\[" input))
        (html-golden-tier-trigger? input))))

(defn- html-golden-kw-node
  "Keywordize a golden :tree node per the hickory node spec (element
  tags and attr keys become keywords; text strings pass through)."
  [node]
  (if (string? node)
    node
    (case (:type node)
      :element {:type :element
                :tag (keyword (:tag node))
                :attrs (reduce (fn [m [k v]] (assoc m (keyword k) v))
                               {} (:attrs node))
                :content (mapv html-golden-kw-node (:content node))}
      node)))

(deftest html-golden-corpus-matches-the-reader
  (let [res (reduce
              (fn [acc v]
                (if (html-golden-skip? v)
                  (assoc acc :skipped (inc (:skipped acc)))
                  (if (= (mapv html-golden-kw-node (:tree v))
                         (html/parse-fragment (:input v)))
                    (assoc acc :ok (inc (:ok acc)))
                    (-> acc
                        (assoc :fail (inc (:fail acc)))
                        (assoc :first-fail (:id v))))))
              {:ok 0 :fail 0 :skipped 0 :first-fail nil}
              html-golden-all)]
    (is (zero? (:fail res))
        (str "golden mismatches: " (:fail res) " first id "
             (:first-fail res)))
    (is (> (:ok res) 3000)
        (str "corpus coverage collapsed: only " (:ok res) " compared"))
    (is (> (:skipped res) 100)
        (str "divergence filters went inert: " (:skipped res)
             " skipped"))))

;;; ---- tolerance tier vectors (spec-cited, hand-pinned) ----

;; Rule 1: unclosed tags close at EOF, innermost first.
(deftest html-tier-eof-auto-balance
  (is (= [(html-el :div [(html-el :span ["a"])])]
         (html/parse-fragment "<div><span>a")))
  (is (= [(html-el :b ["x"])]
         (html/parse-fragment "<b>x"))))

;; Rule 2: stray end tags are dropped; parsing continues.
(deftest html-tier-stray-end-tag-dropped
  (is (= ["a" "b"] (html/parse-fragment "a</div>b")))
  (is (= [(html-el :ul [(html-el :li ["1"])])]
         (html/parse-fragment "<ul><li>1</li></ul></ul>"))))

;; Rule 3: misnested end tags pop-until the match; the design's
;; pinned example <b><i>x</b></i>.
(deftest html-tier-misnested-pop-until
  (is (= [(html-el :b [(html-el :i ["x"])]) "y"]
         (html/parse-fragment "<b><i>x</b></i>y")))
  (is (= [(html-el :p [(html-el :em ["1"])])]
         (html/parse-fragment "<p><em>1</p>"))))

;; Rule 4: void elements never await end tags; void end tags drop.
(deftest html-tier-void-elements
  (is (= ["a" (html-el :br) "b"]
         (html/parse-fragment "a<br>b")))
  (is (= [] (html/parse-fragment "</br>")))
  (is (= [(html-el :img {:src "x.png"} [])]
         (html/parse-fragment "<img src=x.png>")))
  (is (= [(html-el :input {:disabled ""} [])]
         (html/parse-fragment "<input disabled>"))))

;; Rule 5: the trailing solidus is honored on every start tag (D7).
(deftest html-tier-solidus-self-close
  (is (= [(html-el :div []) "x"] (html/parse-fragment "<div/>x")))
  (is (= [(html-el :circle {:cx "5"} [])]
         (html/parse-fragment "<circle cx=\"5\"/>")))
  ;; solidus inside an unquoted value is value bytes, not a close
  (is (= [(html-el :a {:href "x/"} ["y"])]
         (html/parse-fragment "<a href=x/>y"))))

;; Rule 6: attribute forms, duplicates keep the first, valueless
;; normalize to "", whitespace-flexible around =; an unterminated
;; quoted value at EOF drops the whole tag.
(deftest html-tier-attribute-forms
  (is (= [(html-el :input
                   {:a "1" :b "two" :c "3" :d "" :e "f"} [])]
         (html/parse-fragment "<input a=1 b = 'two' c=\"3\" d e= f>")))
  (is (= [(html-el :input {:a "1"} [])]
         (html/parse-fragment "<input a=1 a=2>")))
  (is (= [] (html/parse-fragment "<div a=\"x"))))

;; Rule 7: script/style are RAWTEXT (verbatim); title/textarea are
;; RCDATA (references resolve, no tag interpretation) per D6.
(deftest html-tier-rawtext-and-rcdata
  (is (= [(html-el :script ["if (a<b) x && y</b>"]) "tail"]
         (html/parse-fragment "<script>if (a<b) x && y</b></script>tail")))
  (is (= [(html-el :style ["p { content: '&amp;' }"])]
         (html/parse-fragment "<style>p { content: '&amp;' }</style>")))
  (is (= [(html-el :title ["a & < b"])]
         (html/parse-fragment "<title>a &amp; &lt; b</title>")))
  (is (= [(html-el :textarea ["<b>x"])]
         (html/parse-fragment "<textarea><b>x")))
  ;; raw text ends only at a matching end tag
  (is (= [(html-el :script ["a</scriptx>b"])]
         (html/parse-fragment "<script>a</scriptx>b")))
  ;; attrs on a raw-text end tag are ignored
  (is (= [(html-el :style ["a"])]
         (html/parse-fragment "<style>a</style attr>"))))

;; Rule 8: character references. Semicolonless legacy names resolve
;; in text via longest match; attribute values decode
;; semicolon-terminated names only (the ledger-4 divergence); numeric
;; refs carry range policing and the windows-1252 remap; a bare
;; ampersand that matches nothing stays literal.
(deftest html-tier-character-references
  (is (= ["&<AB>"] (html/parse-fragment "&amp;&lt;&#65;&#x42;&gt;")))
  (is (= ["a & b"] (html/parse-fragment "a & b")))
  (is (= ["&unknown;"] (html/parse-fragment "&unknown;")))
  (is (= ["¬i;"] (html/parse-fragment "&noti;")))
  (is (= ["\uFFFD"] (html/parse-fragment "&#0;")))
  (is (= ["\uFFFD"] (html/parse-fragment "&#xD800;")))
  (is (= ["\uFFFD"] (html/parse-fragment "&#x110000;")))
  (is (= ["\u20AC"] (html/parse-fragment "&#0128;")))
  ;; attribute context: names decode only when semicolon-terminated
  ;; (the ledger-4 divergence; python decodes some semicolonless
  ;; legacy forms there)
  (is (= [(html-el :p {:title "&not x"} [])]
         (html/parse-fragment "<p title=\"&not x\">")))
  (is (= [(html-el :p {:title "&notx"} [])]
         (html/parse-fragment "<p title=&notx>")))
  (is (= [(html-el :p {:title "\u00AC"} [])]
         (html/parse-fragment "<p title=\"&not;\">")))
  (is (= [(html-el :p {:a "A&B"} [])]
         (html/parse-fragment "<p a=\"&#65;&amp;B\">"))))

;; Rule 9: comments verbatim; bogus comments (<? >, <!foo>, <![CDATA
;; in HTML content); unterminated comments close at EOF.
(deftest html-tier-comments-and-bogus
  (is (= [(html-com " hi ")] (html/parse-fragment "<!-- hi -->")))
  (is (= [(html-com "php echo 1?")]
         (html/parse-fragment "<?php echo 1?>")))
  (is (= [(html-com "foo")] (html/parse-fragment "<!foo>")))
  (is (= [(html-com "[CDATA[raw]]")]
         (html/parse-fragment "<![CDATA[raw]]>")))
  (is (= [(html-com "x")] (html/parse-fragment "<!--x")))
  (is (= ["</>"] (html/parse-fragment "&lt;/&gt;")))
  (is (= [] (html/parse-fragment "</>")))
  (is (= [(html-com " x")] (html/parse-fragment "</ x>")))
  ;; --!> closes a comment (oracle behavior)
  (is (= [(html-com " x ") "after"]
         (html/parse-fragment "<!-- x --!>after"))))

;; Rule 10: the first DOCTYPE is captured; later ones are dropped.
;; Rule 5 of the node spec: raw text between <!DOCTYPE and >, trimmed.
(deftest html-tier-doctype
  (is (= [(html-doct "html")]
         (html/parse-fragment "<!DOCTYPE html>")))
  (is (= [(html-doct "html")]
         (html/parse-fragment "<!DOCTYPE html ")))
  (is (= [(html-doct "html")]
         (html/parse-fragment "<!doctype html>"))))

;; Rule 11: tag and attribute names lowercase in HTML mode.
(deftest html-tier-case-folding
  (is (= [(html-el :div {:class "x" :id "y"} ["T"])]
         (html/parse-fragment "<DIV CLASS='x' ID=y>T</DIV>")))
  (is (= [(html-el :em ["a"])]
         (html/parse-fragment "<EM>a</EM>"))))

;; Rule 12: PLAINTEXT makes the rest of the input one verbatim run.
(deftest html-tier-plaintext
  (is (= [(html-el :plaintext ["<b>x&amp;"])]
         (html/parse-fragment "<plaintext><b>x&amp;")))
  (is (= [(html-el :plaintext ["abc</plaintext>def"])]
         (html/parse-fragment "<plaintext>abc</plaintext>def"))))

;; Rule 13: NUL in text becomes U+FFFD.
(deftest html-tier-nul-replacement
  (is (= ["a\uFFFDb"]
         (html/parse-fragment (str "a" (char 0) "b"))))
  (is (= [(html-el :title ["x\uFFFD"])]
         (html/parse-fragment (str "<title>x" (char 0) "</title>")))))

;; Rule 15: like-tag implied closes bounded by scope barriers
;; (table td th caption template html).
(deftest html-tier-like-tag-implied-close
  ;; like-tag closes are literal: dt closes dt, dd closes dd (the
  ;; tier's simplified rule, not WHATWG's dt/dd cross-close)
  (is (= [(html-el :dl
                   [(html-el :dt ["a" (html-el :dd ["x"])])
                    (html-el :dt ["b" (html-el :dd ["y"])])])]
         (html/parse-fragment "<dl><dt>a<dd>x<dt>b<dd>y")))
  (is (= [(html-el :ul [(html-el :li ["a"]) (html-el :li ["b"])])]
         (html/parse-fragment "<ul><li>a<li>b</ul>")))
  ;; table is a barrier: the second p nests, it does not close the
  ;; outer p (and the first p was closed by table per rule 16)
  (is (= [(html-el :p ["1"])
          (html-el :table [(html-el :p ["2"])])]
         (html/parse-fragment "<p>1<table><p>2"))))

;; Rule 16: the verbatim p-closing list; h1-h6 additionally close an
;; open heading.
(deftest html-tier-p-closing-list
  (is (= [(html-el :p ["one"]) (html-el :p ["two"])]
         (html/parse-fragment "<p>one<p>two")))
  (is (= [(html-el :p ["text"]) (html-el :div ["box"])]
         (html/parse-fragment "<p>text<div>box")))
  (is (= [(html-el :p ["x"]) (html-el :hr)]
         (html/parse-fragment "<p>x<hr>")))
  (is (= [(html-el :h1 ["a"]) (html-el :h2 ["b"])]
         (html/parse-fragment "<h1>a<h2>b"))))

;; Rule 19: text inside a table outside any cell stays a direct
;; string child of the table (no foster parenting).
(deftest html-tier-table-text-placement
  (is (= [(html-el :table
                   ["stray" (html-el :tr [(html-el :td ["cell"])])])]
         (html/parse-fragment "<table>stray<tr><td>cell"))))

;;; ---- implied wrapper synthesis (rule 17, document mode) ----

(deftest html-wrapper-implied-html-head-body
  (is (= (html-doc (html-el :html
                           [(html-el :head)
                            (html-el :body [(html-el :p ["hello"])])]))
         (html/parse "<p>hello")))
  (is (= (html-doc (html-el :html [(html-el :head) (html-el :body ["x"])]))
         (html/parse "x")))
  (is (= (html-doc (html-el :html [(html-el :head) (html-el :body)]))
         (html/parse ""))))

(deftest html-wrapper-head-routing
  (is (= (html-doc
           (html-el :html
                    [(html-el :head
                              [(html-el :meta {:charset "x"} [])
                               (html-el :title ["T"])])
                     (html-el :body [(html-el :p ["x"])])]))
         (html/parse "<meta charset=x><title>T</title><p>x")))
  ;; leading whitespace before any element is dropped
  (is (= (html-doc (html-el :html
                           [(html-el :head)
                            (html-el :body [(html-el :p ["x"])])]))
         (html/parse "   <p>x")))
  ;; script lands in head when it precedes body content
  (is (= (html-doc
           (html-el :html
                    [(html-el :head [(html-el :script ["var x=1"])])
                     (html-el :body [(html-el :p ["after"])])]))
         (html/parse "<script>var x=1</script><p>after"))))

(deftest html-wrapper-explicit-tags-win
  (is (= (html-doc
           (html-el :html
                    [(html-el :head [(html-el :title ["t"])])
                     (html-el :body [(html-el :p ["x"])])]))
         (html/parse "<head><title>t</title><p>x")))
  (is (= (html-doc
           (html-el :html
                    [(html-el :head [(html-el :title ["t"])])
                     (html-el :body [(html-el :p ["x"])])]))
         (html/parse
           "<html><head><title>t</title></head><body><p>x</p></body></html>")))
  ;; explicit body closes the implied head first
  (is (= (html-doc
           (html-el :html
                    [(html-el :head [(html-el :meta {:x ""} [])])
                     (html-el :body ["y"])]))
         (html/parse "<meta x><body>y")))
  ;; html/body start tags adopt attrs onto the implied wrappers
  (is (= (html-doc (html-el :html {:lang "en"}
                            [(html-el :head)
                             (html-el :body {:class "c"} ["z"])]))
         (html/parse "<html lang=en><body class=c>z")))
  ;; end tags naming still-open implied wrappers are dropped
  (is (= (html-doc
           (html-el :html
                    [(html-el :head)
                     (html-el :body
                              [(html-el :div ["a"]) (html-el :div ["b"])])]))
         (html/parse "<div>a</div></body><div>b</div>")))
  ;; head elements between </head> and body stay html children
  (is (= (html-doc
           (html-el :html
                    [(html-el :head [(html-el :meta {:x ""} [])])
                     (html-el :body ["y"])]))
         (html/parse "</head><meta x><body>y"))))

(deftest html-wrapper-doctype-and-comments-at-document-level
  (is (= (html-doc (html-doct "html")
                   (html-el :html
                            [(html-el :head)
                             (html-el :body [(html-el :p ["x"])])]))
         (html/parse "<!DOCTYPE html><p>x")))
  (is (= (html-doc (html-doct "a")
                   (html-el :html
                            [(html-el :head)
                             (html-el :body [(html-el :p ["x"])])]))
         (html/parse "<!DOCTYPE a><p>x<!DOCTYPE b>")))
  (is (= (html-doc (html-com "c")
                   (html-el :html
                            [(html-el :head)
                             (html-el :body [(html-el :p ["x"])])]))
         (html/parse "<!--c--><p>x")))
  ;; a title in the head phase is RCDATA; a heading after it starts
  ;; the body
  (is (= (html-doc
           (html-el :html
                    [(html-el :head [(html-el :title ["t"])])
                     (html-el :body [(html-el :h1 ["x"])])]))
         (html/parse "<title>t</title><h1>x"))))

(deftest html-fragment-never-synthesizes
  (is (= [(html-el :p ["x"])] (html/parse-fragment "<p>x")))
  (is (= ["x"] (html/parse-fragment "x")))
  (is (= [] (html/parse-fragment "")))
  (is (= [(html-el :head [(html-el :title ["t"])])]
         (html/parse-fragment "<head><title>t</title></head>"))))

;;; ---- error shapes ----

(defn- html-deep-open [n] (str/join (repeat n "<div>")))

(defn- html-err-data
  [thunk]
  (try (thunk) :no-throw (catch e (ex-data e))))

;; Rule 14: open-element depth beyond 256 throws :max-depth with a
;; position. 255 open parses; 257 throws (A-2 boundary pins).
(deftest html-error-max-depth-boundary
  (let [ok255 (html/parse-fragment (html-deep-open 255))]
    (is (vector? ok255))
    (is (= 255 (loop [n (nth ok255 0) d 1]
                 (if (and (map? n) (seq (:content n)))
                   (recur (nth (:content n) 0) (inc d))
                   d)))))
  (let [d (html-err-data #(html/parse-fragment (html-deep-open 257)))]
    (is (map? d))
    (is (= :html/parse (:kind d)))
    (is (= :max-depth (:code d)))
    (is (= {:line 1 :col 1281} (:location d)))
    (is (string? (:text d)))))

;; The prim returns the error descriptor vector; the facade owns the
;; ex-info throw (the toml.c split).
(deftest html-error-prim-descriptor-shape
  (let [r (html-parse (html-deep-open 257) {:fragment true})]
    (is (vector? r))
    (is (= :html/error (nth r 0)))
    (is (= "max-depth" (nth r 1)))
    (is (= 1 (nth r 2)))
    (is (= 1281 (nth r 3)))
    (is (string? (nth r 4)))))

;;; ---- surface audit (documented divergences, AC-5) ----

;; Divergence: parse returns the node map directly, no parser object.
(deftest html-surface-parse-returns-the-node-map
  (let [t (html/parse "<p>x")]
    (is (map? t))
    (is (= :document (:type t)))
    (is (= :element (:type (nth (:content t) 0))))))

;; Divergence: positioned ex-info where hickory never throws.
(deftest html-surface-positioned-ex-info
  (let [d (html-err-data #(html/parse (html-deep-open 300)))]
    (is (map? d))
    (is (= :html/parse (:kind d)))
    (is (contains? d :location))))

;; Divergence: opts are keyword maps, reserved in v1 (accepted and
;; ignored); non-map opts and non-string input are argument errors.
(deftest html-surface-opts-validation
  (is (map? (html/parse "<p>x" nil)))
  (is (map? (html/parse "<p>x" {})))
  (is (vector? (html/parse-fragment "<p>x" nil)))
  (is (= :html/opts (:kind (html-err-data #(html/parse "<p>x" :nope))))
      "opts must be a map")
  (is (= :html/opts (:kind (html-err-data #(html/parse-fragment 5))))
      "input must be a string"))

(run-tests-and-exit)
