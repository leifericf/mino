(ns tests.xml-fixture
  "The megabyte-scale strict-XML pom/rss/svg mix generator (html-xml
  campaign, design D9): one deterministic well-formed document cycling
  three seeded-arithmetic block kinds. Extracted from the suite file
  so the perf gate (tests/xml_perf_test.clj) parses exactly this
  generator's output without dragging test runs along (the
  require_env_helper and tests.html-fixture precedents).")

(require '[clojure.string :as str])

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
      "  <rss version=\"2.0\" xml:lang=\"en\">\n"
      "    <channel>\n"
      "      <title>Deps &amp; feeds, channel " i "</title>\n"
      "      <link>https://example.invalid/feeds/" i "</link>\n"
      "      <description>Notes &lt;b&gt;rich&lt;/b&gt; text &amp; more for feed " i "</description>\n"
      "      <item>\n"
      "        <title>Item &quot;" (+ 1000 i) "&quot; &#8212; update</title>\n"
      "        <guid isPermaLink=\"false\">urn:item:" i ":a</guid>\n"
      "        <pubDate>Tue, 25 Aug 2026 0" (+ 1 r) ":00:00 +0200</pubDate>\n"
      "        <description>Price &amp; availability for &lt;code&gt;res-" i "&lt;/code&gt;: &#8364;" (+ 9 r) ",&#160;in stock</description>\n"
      "      </item>\n"
      "      <item>\n"
      "        <title>Follow-up &amp; errata " r "</title>\n"
      "        <guid isPermaLink=\"false\">urn:item:" i ":b</guid>\n"
      "        <description>Terms apply&#46; See &amp; accept before use&#8230;</description>\n"
      "      </item>\n"
      "    </channel>\n"
      "  </rss>\n")))

(defn- xml-fx-svg-block
  [i]
  (let [r (rem i 8)]
    (str
      "  <svg-pack id=\"pack-" i "\" viewBox='0 0 100 100' xmlns=\"http://www.w3.org/2000/svg\">\n"
      "    <g fill=\"none\" stroke='#333' stroke-width=\"" (+ 1 r) "\">\n"
      "      <rect x=\"5\" y=\"5\" width=\"" (+ 80 r) "\" height=\"" (+ 70 r) "\" rx=\"8\"/>\n"
      "      <path d=\"M10 50 Q 30 " (+ 10 r) " 50 50 T 90 50\"/>\n"
      "      <circle cx=\"" (+ 40 r) "\" cy=\"50\" r=\"" (+ 20 r) "\"/>\n"
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
  the three kinds) under a catalog root. Every attribute value is
  quoted (XML 1.0 requires it); entities are the five predefined
  plus numeric references only. Deterministic; identical on every
  call. Reader gates (p5t4) parse exactly this string."
  []
  (str
    "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
    "<catalog kind='mix' generated-by='xml-fixture-doc' count=\"1452\">\n"
    (let [acc (transient [])]
      (loop [i 0]
        (if (= i 1452)
          (str/join (persistent! acc))
          (do
            (conj! acc (xml-fx-block i))
            (recur (inc i))))))
    "</catalog>\n"))
