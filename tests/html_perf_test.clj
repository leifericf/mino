(require "tests/test")
(require '[clojure.string :as str])
(require '[mino.html :as html])
(require '[tests.html-fixture :as hfix])

;; HTML reading must stay well inside the absolute budget at page
;; scale (html-xml campaign p2t3, design D9). The reader is the
;; native single-pass html-parse prim (ADR 28); the gate parses the
;; p1 fixture generator's exact output: the 1MB +/-5% realistic page
;; mix (tests.html-fixture/html-fixture-doc, 2072 seeded blocks).
;; Budgets are absolute, never wall-clock ratios (CI-runner lesson),
;; with in-suite headroom for resident-set GC pressure (the p7 toml
;; lesson, ~3x measured; the toml/yaml perf-gate precedent). The
;; parse+select+to-html pipeline gate lands with the serializer
;; (p3t2), when to-html exists. Standalone expectation recorded in
;; the campaign decisions: 500ms or better (measured 128ms at land,
;; 1.03MB). This file joins the nightly MINO_TEST_EXCLUDE list per
;; the toml/yaml perf precedent.

(def ^:private html-perf-doc (hfix/html-fixture-doc))
(def ^:private html-perf-size (count html-perf-doc))

(defn- html-perf-count
  "Element count and max depth of a parsed tree, mixed content
  included."
  [node]
  (let [walk (fn walk [n depth elems maxd]
               (if (map? n)
                 (let [d (inc depth)
                       e (inc elems)]
                   (reduce
                     (fn [acc c]
                       (if (map? c)
                         (walk c d (first acc) (max (second acc) d))
                         acc))
                     [e (max maxd d)]
                     (:content n)))
                 [elems maxd]))]
    (walk node 0 0 0)))

(defn- html-perf-find
  "First element child of node whose tag is name."
  [node name]
  (reduce (fn [_ c]
            (when (and (map? c) (= name (:tag c)))
              (reduced c)))
          nil
          (:content node)))

(deftest html-perf-one-megabyte-page-within-budget
  (let [t0 (nano-time)
        tree (html-parse html-perf-doc nil)
        ms (quot (- (nano-time) t0) 1000000)
        root (html-perf-find tree :html)
        head (html-perf-find root :head)
        body (html-perf-find root :body)]
    (is (> html-perf-size 1000000) "fixture must be megabyte scale")
    (is (< ms 3000) (str "megabyte parse took " ms "ms"))
    ;; spot checks so a fast-but-wrong reader cannot pass
    (is (= :document (:type tree)))
    (is (= :html (:tag root)))
    (is (= :head (:tag head)))
    (is (= :body (:tag body)))
    (is (= "en" (:lang (:attrs root))))
    (is (= "utf-8" (get-in (html-perf-find head :meta) [:attrs :charset])))
    (is (= "dashboard" (:class (:attrs body))))))

(deftest html-perf-reparse-determinism
  (let [a (html-parse html-perf-doc nil)
        b (html-parse html-perf-doc nil)]
    (is (= a b) "re-parsing the fixture must yield the identical tree")))

(deftest html-perf-shape-bands
  ;; Reader-side shape over the same fixture, asserted against the
  ;; p1 scanner bands (A-6: realism stays checkable end to end).
  (let [tree (html-parse html-perf-doc nil)
        [elems maxd] (html-perf-count tree)]
    (is (<= 20000 elems 24500)
        (str "element count out of band: " elems))
    (is (<= 13 maxd 20)
        (str "max depth out of band: " maxd))))

(defn- html-perf-count-tag
  "Explicit-stack tag traversal: every element with the tag. The
  select pass until mino.html.select lands (p4t2 swaps it in); no
  seq materialization over big vectors (the O(n) seq lesson)."
  [node tag]
  (loop [stack [node] acc 0]
    (if (pos? (count stack))
      (let [n (peek stack)]
        (if (map? n)
          (recur (into (pop stack) (:content n))
                 (if (= tag (:tag n)) (inc acc) acc))
          (recur (pop stack) acc)))
      acc)))

(deftest html-perf-pipeline-within-budget
  ;; parse + one select pass + to-html under 6000ms in-suite (design
  ;; D9). The serializer landed with p3t2; recorded at land: ~1.4s
  ;; standalone serialization of the 1.03MB fixture, the worklist
  ;; loop shape the interpreter allows (writers stay Clojure, ADR
  ;; 23/24 lineage).
  (let [t0 (nano-time)
        tree (html-parse html-perf-doc nil)
        ps (html-perf-count-tag tree :p)
        out (html/to-html tree)
        ms (quot (- (nano-time) t0) 1000000)]
    (is (<= 1500 ps 1600) (str "p-element count out of band: " ps))
    (is (> (count out) 1000000) "output must stay megabyte scale")
    (is (< ms 6000) (str "parse+select+to-html pipeline took " ms "ms"))))

(run-tests-and-exit)
