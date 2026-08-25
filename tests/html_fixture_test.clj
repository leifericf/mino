(require "tests/test")
(require '[clojure.string :as str])
(require '[tests.html-fixture :as hfix])

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

;; The block generators and html-fixture-doc live in the
;; tests.html-fixture helper namespace (the require_env_helper
;; precedent) so the perf gate and future suites parse exactly the
;; same document without running this suite.

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

(def ^:private html-fx-doc (hfix/html-fixture-doc))
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
  (let [b (hfix/html-fixture-doc)]
    (is (= html-fx-doc b) "regenerating the fixture must yield the identical string")
    (is (= html-fx-doc-size (count b)))))

(run-tests-and-exit)
