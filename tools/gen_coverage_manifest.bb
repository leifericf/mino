#!/usr/bin/env bb
;; Regenerate the clojure.core / string / set / walk / edn / zip expected
;; surface sets in tests/clojure_coverage_test.clj from the canonical
;; Clojure reference surface that clojure-census captures.
;;
;;   bb tools/gen_coverage_manifest.bb [path/to/1.12.4-surface.edn]
;;
;; The census reference (clojure/1.12.4-surface.edn) is the single
;; source of truth for which vars Clojure ships; this script turns it
;; into the hand-pinned set literals the coverage test reads at run
;; time. mino cannot reach the census file at test time, so the set is
;; materialized here and re-derived whenever the census baseline moves.
;; See docs/adr/19-census-as-source-of-truth.md.
;;
;; Output: EDN forms printed to stdout, one set per namespace, formatted
;; to match the test's existing column style. Paste each block into the
;; matching expected-* def in tests/clojure_coverage_test.clj.

(require '[clojure.edn :as edn]
         '[clojure.string :as str])

(def surface-path
  (or (first *command-line-args*)
      "../clojure-census/clojure/1.12.4-surface.edn"))

(def surface (edn/read-string (slurp surface-path)))

(defn vars-of [ns-sym]
  (-> surface :namespaces (get ns-sym) :vars keys set))

(defn format-set [syms per-line]
  (let [ordered (sort (map str syms))
        rows (partition-all per-line ordered)
        body (str/join "\n" (map #(str "     " (str/join " " %)) rows))]
    (str "#{\n" body "\n     }")))

(defn emit [ns-sym set-name per-line]
  (println (format "(def %s\n  '%s)" set-name (format-set (vars-of ns-sym) per-line))))

(println ";; Generated from" surface-path)
(println ";; Clojure version:" (:clojure-version surface))
(println)
(emit 'clojure.core  'expected-clojure-core  12)
(emit 'clojure.string 'expected-clojure-string 8)
(emit 'clojure.set    'expected-clojure-set    6)
(emit 'clojure.walk   'expected-clojure-walk   5)
(emit 'clojure.edn    'expected-clojure-edn    4)
(emit 'clojure.zip    'expected-clojure-zip    6)
