(require "tests/test")
(require '[clojure.string :as str])
(require '[tests.xml-fixture :as xfix])

;; XML reading must stay well inside the absolute budget at feed
;; scale (html-xml campaign p5t4, design D9). The reader is the
;; native single-pass xml-parse prim (ADR 28, strict mode); the gate
;; parses the fixture generator's exact output: the 1MB +/-5%
;; pom/rss/svg mix (tests.xml-fixture/xml-fixture-doc, 1452 seeded
;; blocks, 20813 elements by the p1 scanner). Budgets are absolute,
;; never wall-clock ratios (CI-runner lesson), with in-suite
;; headroom for resident-set GC pressure (the p7 toml lesson, ~3x
;; measured; the toml/yaml/html perf-gate precedent). Standalone
;; expectation recorded in the campaign decisions: 500ms or better
;; (measured 98ms at land, 1.02MB, ~5x headroom). This file joins
;; the nightly MINO_TEST_EXCLUDE list per the toml/yaml/html
;; precedent (satellite gc-fuzz commit).

(def ^:private xml-perf-doc (xfix/xml-fixture-doc))
(def ^:private xml-perf-size (count xml-perf-doc))

(defn- xml-perf-count
  "Element count and max depth of a parsed JVM-shape tree, string
  children passed over."
  [node]
  (let [walk (fn walk [n depth elems maxd]
               (let [d (inc depth)]
                 (reduce
                   (fn [acc c]
                     (if (map? c)
                       (walk c d (first acc) (max (second acc) d))
                       acc))
                   [(inc elems) (max maxd d)]
                   (:content n))))]
    (walk node 0 0 0)))

(deftest xml-perf-one-megabyte-mix-within-budget
  (let [t0 (nano-time)
        tree (xml-parse xml-perf-doc nil)
        ms (quot (- (nano-time) t0) 1000000)]
    (is (> xml-perf-size 1000000) "fixture must be megabyte scale")
    (is (< ms 2000) (str "megabyte parse took " ms "ms"))
    ;; spot checks so a fast-but-wrong reader cannot pass
    (is (= :catalog (:tag tree)))
    (is (= "mix" (:kind (:attrs tree))))
    (is (= "1452" (:count (:attrs tree))))
    (is (= 3 (count (:attrs tree))))
    (is (= 2905 (count (:content tree)))
        "1452 block elements plus their newline text separators")))

(deftest xml-perf-reparse-determinism
  (let [a (xml-parse xml-perf-doc nil)
        b (xml-parse xml-perf-doc nil)]
    (is (= a b) "re-parsing the fixture must yield the identical tree")))

(deftest xml-perf-shape-bands
  ;; Reader-side shape over the same fixture, asserted against the
  ;; p1 scanner bands (A-6: realism stays checkable end to end).
  (let [tree (xml-parse xml-perf-doc nil)
        [elems maxd] (xml-perf-count tree)]
    (is (<= 18500 elems 23000)
        (str "element count out of band: " elems))
    (is (<= 4 maxd 7)
        (str "max depth out of band: " maxd))))

(run-tests-and-exit)
