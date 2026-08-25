(require "tests/test")
(require '[clojure.string :as str])
(require '[tests.xml-fixture :as xfix])

;; XML fixture realism record (html-xml campaign, design D9/A-6).
;;
;; The generator lives in tests/xml_fixture.clj (ns tests.xml-fixture,
;; the tests.html-fixture precedent) so reader gates can require it
;; standalone; this suite file owns the parser-independent realism
;; bands and the determinism checks. The p1 generator emitted
;; unquoted svg/catalog attribute values, which strict XML rejects;
;; they are quoted now (the p5 discovery, pinned by the reader gate).
;;
;; Realism record (stats measured with the regex scanner in this
;; file over the committed generator; bands assert the same
;; invariants so realism stays checkable, A-6):
;;
;;   metric                measured        asserted band
;;   -------------------   -------------   ----------------
;;   size (chars=bytes)    1019865         950000..1050000
;;   start-tag elements    20813           18500..23000
;;   max open depth        5               4..7
;;   attributes            15975           14000..18000
;;   entity references     10164           9000..11500
;;
;; The scanner is parser-independent (tag-shaped regex walk honoring
;; the trailing solidus) so the bands bind before and after the p5
;; reader; tests/xml_perf_test.clj re-asserts reader-side shape.

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

(def ^:private xml-fx-doc (xfix/xml-fixture-doc))
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
  (let [b (xfix/xml-fixture-doc)]
    (is (= xml-fx-doc b) "regenerating the fixture must yield the identical string")
    (is (= xml-fx-doc-size (count b)))))

(run-tests-and-exit)
