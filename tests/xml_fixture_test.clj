(require "tests/test")
(require '[clojure.string :as str])

;; XML fixture generator (html-xml campaign, design D9/A-6).
;;
;; Builds the megabyte-scale strict-XML mix the p5 reader gate
;; parses: seeded arithmetic blocks (the yaml_perf_test.clj style; no
;; runtime randomness, no committed blob). One well-formed document
;; cycling three realistic feed shapes: maven pom modules, rss
;; channels with entity-dense item text, and svg packs with
;; self-closing geometry. Attributes use both quote styles, elements
;; carry comments, and text exercises the five predefined entities
;; plus numeric character references.
;;
;; Realism record (stats measured with the regex scanner in this
;; file over the committed generator; bands assert the same
;; invariants so realism stays checkable, A-6):
;;
;;   metric                measured        asserted band
;;   -------------------   -------------   ----------------
;;   size (chars=bytes)    1015023         950000..1050000
;;   start-tag elements    20813           18500..23000
;;   max open depth        5               4..7
;;   attributes            15975           14000..18000
;;   entity references     10164           9000..11500
;;
;; The scanner is parser-independent (tag-shaped regex walk honoring
;; the trailing solidus) so the bands bind at p1, before xml-parse
;; exists; p5's perf test re-asserts reader-side shape.

(defn- xml-fx-pom-block
  [i]
  (let [r (rem i 9)]
    (str
      "  <maven-project xmlns=\"http://maven.apache.org/POM/4.0.0\">\n"
      "    <modelVersion>4.0.0</modelVersion>\n"
      "    <groupId>io.mino.demo</groupId>\n"
      "    <artifactId>resizer-" i "</artifactId>\n"
      "    <version>2." r "." (rem i 20) "</version>\n"
      "    <packaging>jar</packaging>\n"
      "    <!-- runtime deps for module " i " -->\n"
      "    <dependencies>\n"
      "      <dependency>\n"
      "        <groupId>org.webjars</groupId>\n"
      "        <artifactId>chart-" r "</artifactId>\n"
      "        <version>1." (rem i 30) "</version>\n"
      "        <scope>runtime</scope>\n"
      "      </dependency>\n"
      "      <dependency>\n"
      "        <groupId>io.mino</groupId>\n"
      "        <artifactId>codec</artifactId>\n"
      "        <version>0." (+ 10 r) "</version>\n"
      "      </dependency>\n"
      "    </dependencies>\n"
      "    <properties>\n"
      "      <max.pixels>" (+ 4096 r) "</max.pixels>\n"
      "      <trace.enabled>" (if (even? i) "true" "false") "</trace.enabled>\n"
      "    </properties>\n"
      "  </maven-project>\n")))

(defn- xml-fx-rss-block
  [i]
  (let [r (rem i 7)]
    (str
      "  <rss version=\"2.0\" xml:lang='en'>\n"
      "    <channel>\n"
      "      <title>Deps &amp; feeds, channel " i "</title>\n"
      "      <link>https://example.invalid/feeds/" i "</link>\n"
      "      <description>Notes &lt;b&gt;rich&lt;/b&gt; text &amp; more for feed " i "</description>\n"
      "      <item>\n"
      "        <title>Item &quot;" (+ 1000 i) "&quot; &#8212; update</title>\n"
      "        <guid isPermaLink='false'>urn:item:" i ":a</guid>\n"
      "        <pubDate>Tue, 25 Aug 2026 0" (+ 1 r) ":00:00 +0200</pubDate>\n"
      "        <description>Price &amp; availability for &lt;code&gt;res-" i "&lt;/code&gt;: &#8364;" (+ 9 r) ",&#160;in stock</description>\n"
      "      </item>\n"
      "      <item>\n"
      "        <title>Follow-up &amp; errata " r "</title>\n"
      "        <guid isPermaLink='false'>urn:item:" i ":b</guid>\n"
      "        <description>Terms apply&#46; See &amp; accept before use&#8230;</description>\n"
      "      </item>\n"
      "    </channel>\n"
      "  </rss>\n")))

(defn- xml-fx-svg-block
  [i]
  (let [r (rem i 8)]
    (str
      "  <svg-pack id=pack-" i " viewBox='0 0 100 100' xmlns=\"http://www.w3.org/2000/svg\">\n"
      "    <g fill=\"none\" stroke='#333' stroke-width=" (+ 1 r) ">\n"
      "      <rect x=\"5\" y=\"5\" width=\"" (+ 80 r) "\" height=\"" (+ 70 r) "\" rx=\"8\"/>\n"
      "      <path d=\"M10 50 Q 30 " (+ 10 r) " 50 50 T 90 50\"/>\n"
      "      <circle cx=" (+ 40 r) " cy=50 r=" (+ 20 r) "/>\n"
      "      <use href=\"#glyph-" i "\"/>\n"
      "      <text x=\"50\" y=\"90\" text-anchor='middle'>pack " i " &amp; glyph</text>\n"
      "    </g>\n"
      "    <g transform='translate(" (* 2 r) " " (* 3 r) ")'>\n"
      "      <path d='M0 0 L10 10 Z'/>\n"
      "      <path d='M20 0 L30 10 Z'/>\n"
      "    </g>\n"
      "  </svg-pack>\n")))

(defn- xml-fx-block
  [i]
  (let [r (rem i 3)]
    (case r
      0 (xml-fx-pom-block i)
      1 (xml-fx-rss-block i)
      2 (xml-fx-svg-block i))))

(defn xml-fixture-doc
  "The megabyte-scale strict-XML pom/rss/svg mix (design D9): one
  well-formed document, 1452 seeded-arithmetic blocks (484 cycles of
  the three kinds) under a catalog root. Deterministic; identical on
  every call. Reader gates (p5t4) parse exactly this string."
  []
  (str
    "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
    "<catalog kind='mix' generated-by='xml-fixture-doc' count=1452>\n"
    (let [acc (transient [])]
      (loop [i 0]
        (if (= i 1452)
          (str/join (persistent! acc))
          (do
            (conj! acc (xml-fx-block i))
            (recur (inc i))))))
    "</catalog>\n"))

(def ^:private xml-fx-tag-re #"<(/?)([a-zA-Z][a-zA-Z0-9:_.-]*)[^>]*>")

(def ^:private xml-fx-attr-re #"\s[a-zA-Z_:][-a-zA-Z0-9_:.]*")

(def ^:private xml-fx-entity-re #"&(amp|lt|gt|quot|apos|#[0-9]+|#[xX][0-9a-fA-F]+);")

(defn- xml-fx-scan
  "Parser-independent shape scan: XML comments stripped, then a
  tag-token walk honoring the trailing solidus (XML has no void
  elements), attribute-name count per start tag, and the predefined
  plus numeric entity-reference count over the whole document."
  [doc]
  (let [markup (str/replace doc #"<!--.*?-->" "")
        toks (vec (re-seq xml-fx-tag-re markup))
        n (count toks)]
    (loop [i 0 depth 0 max-depth 0 elems 0 attrs 0]
      (if (= i n)
        {:elems elems
         :attrs attrs
         :max-depth max-depth
         :entities (count (re-seq xml-fx-entity-re doc))}
        (let [m (nth toks i)
              close (= "/" (nth m 1))
              full (nth m 0)]
          (if close
            (recur (inc i) (dec depth) max-depth elems attrs)
            (let [self-closing (str/ends-with? full "/>")
                  attrs' (+ attrs (count (re-seq xml-fx-attr-re full)))
                  depth' (if self-closing depth (inc depth))]
              (recur (inc i)
                     depth'
                     (max max-depth depth')
                     (inc elems)
                     attrs'))))))))

(def ^:private xml-fx-doc (xml-fixture-doc))
(def ^:private xml-fx-doc-size (count xml-fx-doc))

(deftest xml-fixture-size-in-band
  (is (<= 950000 xml-fx-doc-size 1050000)
      (str "xml mix must be 1MB +/-5%, got " xml-fx-doc-size)))

(deftest xml-fixture-realism-bands
  (let [{:keys [elems attrs max-depth entities]} (xml-fx-scan xml-fx-doc)]
    (is (<= 18500 elems 23000) (str "element count out of band: " elems))
    (is (<= 4 max-depth 7) (str "max depth out of band: " max-depth))
    (is (<= 14000 attrs 18000) (str "attribute count out of band: " attrs))
    (is (<= 9000 entities 11500) (str "entity density out of band: " entities))))

(deftest xml-fixture-deterministic-regeneration
  (let [b (xml-fixture-doc)]
    (is (= xml-fx-doc b) "regenerating the fixture must yield the identical string")
    (is (= xml-fx-doc-size (count b)))))

(run-tests-and-exit)
