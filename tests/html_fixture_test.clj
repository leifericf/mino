(require "tests/test")
(require '[clojure.string :as str])

;; HTML fixture generator (html-xml campaign, design D9/A-6).
;;
;; Builds the megabyte-scale realistic page mix the p2 reader gate
;; parses: seeded arithmetic blocks (the yaml_perf_test.clj style; no
;; runtime randomness, no committed blob). One realistic document
;; cycling eight block kinds: nav lists, entity-dense article text,
;; data tables, svg-flavored self-closing media, forms with valueless
;; and mixed-quoted attributes, depth-varied div nesting, script and
;; style blocks, comment runs.
;;
;; Realism record (stats measured with the regex scanner in this
;; file over the committed generator; bands assert the same invariants
;; so realism stays checkable, A-6):
;;
;;   metric                measured        asserted band
;;   -------------------   -------------   ----------------
;;   size (chars=bytes)    1026904         950000..1050000
;;   start-tag elements    22282           20000..24500
;;   max open depth        15              13..20
;;   attributes            32641           29500..36000
;;   entity references     12952           11000..15000
;;
;; The scanner is deliberately parser-independent (a tag-shaped regex
;; walk honoring void elements and solidus): it cross-checks the
;; generator's output shape without the reader, so the bands bind at
;; p1, before html-parse exists. p2's perf test re-asserts reader-side
;; shape over the same fixture.

(defn- html-fx-nav-block
  [i]
  (let [r (rem i 9)]
    (str
      "<header class='site-nav' data-section=" i ">\n"
      "  <nav aria-label='Section " i "'>\n"
      "    <ul class='nav nav-" r "'>\n"
      "      <li><a href=\"/s" i "/a\" title='Alpha &amp; beta'>Alpha</a></li>\n"
      "      <li><a href='/s" i "/b'>Beta &amp; more</a></li>\n"
      "      <li><a href=/s" i "/c title=\"Gamma\">Gamma</a></li>\n"
      "      <li><a href=\"/s" i "/d\" aria-current=page>Current</a></li>\n"
      "    </ul>\n"
      "  </nav>\n"
      "</header>\n")))

(defn- html-fx-article-block
  [i]
  (let [r (rem i 11)]
    (str
      "<article class='post post-" r "' id=post-" i " lang=en>\n"
      "  <h2>Field notes &amp; observations, entry " i "</h2>\n"
      "  <p>The caf&eacute; on Rue &amp; Co&#116;&eacute; served " (+ 2 r)
      " pots for 4&#8364; each &mdash; a bargain&hellip; or was it&#63;</p>\n"
      "  <p>Measured " (+ 6 r) "&#160;mm &plusmn; 0&#46;5&hellip; margin of error &GT; zero &amp; climbing.</p>\n"
      "  <blockquote cite=\"/sources/" i "\">\n"
      "    <p>&quot;All models are wrong&semi; some are useful.&quot; &mdash; an old saw &amp; a true one</p>\n"
      "  </blockquote>\n"
      "  <figure>\n"
      "    <img src=\"/img/chart-" i ".png\" alt=\"Chart of &amp; over &lt; time\" width=560 height=315 loading=lazy>\n"
      "    <figcaption>Yield &percnt; over time &copy; 2026</figcaption>\n"
      "  </figure>\n"
      "  <p>Fees &euro;" (+ 10 r) "&nbsp;&mdash;&nbsp;terms apply&#46; See <a href='/terms-" i "'>terms</a> &amp; conditions.</p>\n"
      "</article>\n")))

(defn- html-fx-table-block
  [i]
  (let [r (rem i 7)]
    (str
      "<table class='data stats' id=q" i ">\n"
      "  <caption>Q" (+ 1 r) " results (all figures in &euro;1k)</caption>\n"
      "  <colgroup><col span=2><col class=total></colgroup>\n"
      "  <thead>\n"
      "    <tr><th scope=col>Region</th><th scope=\"col\">Units</th><th scope='col'>Total</th></tr>\n"
      "  </thead>\n"
      "  <tbody>\n"
      "    <tr><td>Nordics</td><td>1&thinsp;" (+ 100 r) "</td><td>98,5</td></tr>\n"
      "    <tr><td>DACH &amp; al.</td><td>2&nbsp;" (+ 1000 r) "</td><td>" (* 95 (+ 1 r)) "</td></tr>\n"
      "    <tr><td>Benelux</td><td>" (+ 500 r) "</td><td>44,1</td></tr>\n"
      "  </tbody>\n"
      "</table>\n")))

(defn- html-fx-media-block
  [i]
  (let [r (rem i 8)]
    (str
      "<div class='media' role=img aria-label='figure " i "'>\n"
      "<svg viewBox=\"0 0 100 100\" xmlns=\"http://www.w3.org/2000/svg\" width='10em' height='8em'>\n"
      "  <defs>\n"
      "    <linearGradient id=\"g" i "\"><stop offset=\"0%\" stop-color=\"#fee\"/><stop offset=\"100%\" stop-color=\"#eef\"/></linearGradient>\n"
      "  </defs>\n"
      "  <g fill=\"url(#g" i ")\" stroke='#333' stroke-width=0.5>\n"
      "    <rect x=\"5\" y=\"5\" width=\"" (+ 80 r) "\" height=\"" (+ 70 r) "\" rx=\"8\"/>\n"
      "    <path d=\"M10 50 Q 30 " (+ 10 r) " 50 50 T 90 50\" fill=\"none\"/>\n"
      "    <circle cx=" (+ 40 r) " cy=50 r=" (+ 20 r) "/>\n"
      "    <use href=\"#g" i "\"/>\n"
      "    <text x=\"50\" y=\"90\" text-anchor=\"middle\">fig &amp; " i "</text>\n"
      "  </g>\n"
      "</svg>\n"
      "</div>\n")))

(defn- html-fx-form-block
  [i]
  (let [r (rem i 6)]
    (str
      "<form action=\"/search/" i "\" method=get class=\"search search-" r "\">\n"
      "  <fieldset>\n"
      "    <legend>Refine &amp; filter " i "</legend>\n"
      "    <label for=q" i ">Query</label>\n"
      "    <input id=q" i " name=q type=search placeholder=\"cats &amp; dogs " i "\" required maxlength=" (+ 60 r) ">\n"
      "    <input type=hidden name=lang value=en>\n"
      "    <select name=sort id=sort" i ">\n"
      "      <option value=relevance selected>Relevance</option>\n"
      "      <option value='date'>Date &amp; time</option>\n"
      "      <option value=size>Size</option>\n"
      "    </select>\n"
      "    <textarea name=notes rows=" (+ 2 r) " cols=30>Type &lt;b&gt;notes&lt;/b&gt; &amp; more here&hellip;</textarea>\n"
      "    <button type=submit disabled>Go</button>\n"
      "  </fieldset>\n"
      "</form>\n")))

(defn- html-fx-depth-block
  [i]
  (let [d (+ 5 (rem i 7))
        open (str/join (map #(str "<div class='level level-" % " depth-" i "'>\n")
                            (range d)))
        close (str/join (repeat d "</div>\n"))]
    (str
      "<section class='tree tree-" (rem i 5) "' id=sec-" i ">\n"
      open
      "<p>nested " d " deep, entry " i " &amp; holding</p>\n"
      close
      "</section>\n")))

(defn- html-fx-script-style-block
  [i]
  (let [r (rem i 9)]
    (str
      "<style media=screen>\n"
      "  .post-" r " { margin: " (+ 4 r) " " r "px; color: #2" r "2; }\n"
      "  .post-" r " > h2::after { content: \" &amp; " i "\"; }\n"
      "  a[aria-current=page] { font-weight: bold; }\n"
      "</style>\n"
      "<script src=\"/assets/app-" r ".js\" defer></script>\n"
      "<script type=\"text/javascript\">\n"
      "  var entry = " i ", bucket = 'b" r "';\n"
      "  if (entry < " (+ i 10) " && entry >= " i ") {\n"
      "    render(\"<p class='x'>entry \" + entry + \"</p>\");\n"
      "  }\n"
      "  // tail comment with <p> and </p> literals\n"
      "</script>\n")))

(defn- html-fx-comment-block
  [i]
  (str
    "<!-- section divider " i " -- dashes and <tags> kept -->\n"
    "<div class=divider role=separator data-n=" i "></div>\n"
    "<hr>\n"
    "<p class=muted>&hellip;continued below&#46;</p>\n"))

(defn- html-fx-block
  [i]
  (let [r (rem i 8)]
    (case r
      0 (html-fx-nav-block i)
      1 (html-fx-article-block i)
      2 (html-fx-table-block i)
      3 (html-fx-media-block i)
      4 (html-fx-form-block i)
      5 (html-fx-depth-block i)
      6 (html-fx-script-style-block i)
      7 (html-fx-comment-block i))))

(defn html-fixture-doc
  "The megabyte-scale realistic HTML page mix (design D9): one
  document, 2072 seeded-arithmetic blocks (259 full cycles of the
  eight kinds). Deterministic; identical on every call. Reader gates
  (p2t3) parse exactly this string."
  []
  (str
    "<!DOCTYPE html>\n"
    "<html lang=\"en\">\n"
    "<head>\n"
    "  <meta charset=\"utf-8\">\n"
    "  <title>Dashboard &amp; reports</title>\n"
    "  <link rel='stylesheet' href='/assets/main.css' media=screen>\n"
    "</head>\n"
    "<body class='dashboard' data-kind=mixed>\n"
    (let [acc (transient [])]
      (loop [i 0]
        (if (= i 2072)
          (str/join (persistent! acc))
          (do
            (conj! acc (html-fx-block i))
            (recur (inc i))))))
    "<footer><p>Generated fixture &mdash; " 2072 " blocks</p></footer>\n"
    "</body>\n"
    "</html>\n"))

(def ^:private html-fx-void-tags
  #{"area" "base" "br" "col" "embed" "hr" "img" "input" "link" "meta"
    "param" "source" "track" "wbr"})

(def ^:private html-fx-tag-re #"<(/?)([a-zA-Z][a-zA-Z0-9:-]*)[^>]*>")

(def ^:private html-fx-attr-re #"\s[a-zA-Z_:][-a-zA-Z0-9_:.]*")

(def ^:private html-fx-entity-re #"&[a-zA-Z#][a-zA-Z0-9]{1,10};")

(defn- html-fx-scan
  "Parser-independent shape scan over markup proper: HTML comments
  and script/style interiors are stripped first (a parser never sees
  them as markup), then a tag-token walk honoring the void list and a
  trailing solidus, attribute-name count per open tag, and the
  entity-reference count over the whole document. Returns the realism
  stats the bands assert."
  [doc]
  (let [markup (-> doc
                   (str/replace #"<!--.*?-->" "")
                   (str/replace #"<script[^>]*>.*?</script>" "")
                   (str/replace #"<style[^>]*>.*?</style>" ""))
        toks (vec (re-seq html-fx-tag-re markup))
        n (count toks)]
    (loop [i 0 depth 0 max-depth 0 elems 0 attrs 0]
      (if (= i n)
        {:elems elems
         :attrs attrs
         :max-depth max-depth
         :entities (count (re-seq html-fx-entity-re doc))}
        (let [m (nth toks i)
              close (= "/" (nth m 1))
              name (nth m 2)
              full (nth m 0)]
          (if close
            (recur (inc i) (max 0 (dec depth)) max-depth elems attrs)
            (let [self-closing (str/ends-with? full "/>")
                  void (contains? html-fx-void-tags name)
                  attrs' (+ attrs (count (re-seq html-fx-attr-re full)))
                  depth' (if (or void self-closing) depth (inc depth))]
              (recur (inc i)
                     depth'
                     (max max-depth depth')
                     (inc elems)
                     attrs'))))))))

(def ^:private html-fx-doc (html-fixture-doc))
(def ^:private html-fx-doc-size (count html-fx-doc))

(deftest html-fixture-size-in-band
  (is (<= 950000 html-fx-doc-size 1050000)
      (str "page mix must be 1MB +/-5%, got " html-fx-doc-size)))

(deftest html-fixture-realism-bands
  (let [{:keys [elems attrs max-depth entities]} (html-fx-scan html-fx-doc)]
    (is (<= 20000 elems 24500) (str "element count out of band: " elems))
    (is (<= 13 max-depth 20) (str "max depth out of band: " max-depth))
    (is (<= 29500 attrs 36000) (str "attribute count out of band: " attrs))
    (is (<= 11000 entities 15000) (str "entity density out of band: " entities))))

(deftest html-fixture-deterministic-regeneration
  (let [b (html-fixture-doc)]
    (is (= html-fx-doc b) "regenerating the fixture must yield the identical string")
    (is (= html-fx-doc-size (count b)))))

(run-tests-and-exit)
