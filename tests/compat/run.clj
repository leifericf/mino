;; Compatibility test runner: loads real-world .clj/.cljc/.cljs files
;; through mino to measure language coverage.
;;
;; Usage:
;;   ./mino tests/compat/run.clj           ;; run all repos
;;   REPOS=medley,hiccup ./mino tests/compat/run.clj  ;; specific repos

(require '[clojure.string :refer [ends-with? starts-with? includes? split join]])

(def cache-dir
  (str (getenv "HOME") "/.cache/mino-compat"))

;; Only repos with a realistic portable surface — JVM-bound libraries
;; (clojure.java.io, java.util.concurrent, clojure.lang.* interfaces,
;; JVM streams, bytecode walking, JVM logging) are out of scope, as
;; are repos that contain a (:import …) form we can't make pass.
(def repos
  {"aero"               {:url "https://github.com/juxt/aero.git" :src "src"}
   "algo.monads"        {:url "https://github.com/clojure/algo.monads.git" :src "src"}
   "arrangement"        {:url "https://github.com/greglook/clj-arrangement.git" :src "src"}
   "camel-snake-kebab"  {:url "https://github.com/clj-commons/camel-snake-kebab.git" :src "src"}
   "cats"               {:url "https://github.com/funcool/cats.git" :src "src"}
   "cuerdas"            {:url "https://github.com/funcool/cuerdas.git" :src "src"}
   "deep-diff2"         {:url "https://github.com/lambdaisland/deep-diff2.git" :src "src"}
   "edamame"            {:url "https://github.com/borkdude/edamame.git" :src "src"}
   "fipp"               {:url "https://github.com/brandonbloom/fipp.git" :src "src"}
   "honeysql"           {:url "https://github.com/seancorfield/honeysql.git" :src "src"}
   "integrant"          {:url "https://github.com/weavejester/integrant.git" :src "src"}
   "macrovich"          {:url "https://github.com/cgrand/macrovich.git" :src "src"}
   "malli"              {:url "https://github.com/metosin/malli.git" :src "src"}
   "math.combinatorics" {:url "https://github.com/clojure/math.combinatorics.git" :src "src"}
   "medley"             {:url "https://github.com/weavejester/medley.git" :src "src"}
   "plumbing"           {:url "https://github.com/plumatic/plumbing.git" :src "src"}
   "reitit"             {:url "https://github.com/metosin/reitit.git" :src "modules/reitit-core/src"}
   "rewrite-clj"        {:url "https://github.com/clj-commons/rewrite-clj.git" :src "src" :ref "main"}
   "specter"            {:url "https://github.com/redplanetlabs/specter.git" :src "src/clj"}
   "test.check"         {:url "https://github.com/clojure/test.check.git" :src "src"}
   "tongue"             {:url "https://github.com/tonsky/tongue.git" :src "src"}
   "tools.macro"        {:url "https://github.com/clojure/tools.macro.git" :src "src"}
   "uri"                {:url "https://github.com/lambdaisland/uri.git" :src "src"}
   "xforms"             {:url "https://github.com/cgrand/xforms.git" :src "src"}})

(def shim-code "")

(defn source-file? [path]
  (or (ends-with? path ".clj")
      (ends-with? path ".cljc")
      (ends-with? path ".cljs")))

;; Save core functions that loaded files might redefine.
;; We restore them after each file to prevent namespace pollution.
(def saved-filter filter)
(def saved-map map)
(def saved-reduce reduce)
(def saved-concat concat)
(def saved-sort sort)
(def saved-get get)
(def saved-assoc assoc)
(def saved-merge merge)
(def saved-apply apply)
(def saved-str str)
(def saved-count count)
(def saved-first first)
(def saved-rest rest)
(def saved-cons cons)
(def saved-seq seq)
(def saved-conj conj)
(def saved-into into)
(def saved-swap! swap!)
(def saved-deref deref)
(def saved-println println)
(def saved-pr-str pr-str)
(def saved-frequencies frequencies)
(def saved-sort-by sort-by)
(def saved-take take)
(def saved-subs subs)

(defn restore-core-fns! []
  (def filter saved-filter)
  (def map saved-map)
  (def reduce saved-reduce)
  (def concat saved-concat)
  (def sort saved-sort)
  (def get saved-get)
  (def assoc saved-assoc)
  (def merge saved-merge)
  (def apply saved-apply)
  (def str saved-str)
  (def count saved-count)
  (def first saved-first)
  (def rest saved-rest)
  (def cons saved-cons)
  (def seq saved-seq)
  (def conj saved-conj)
  (def into saved-into)
  (def swap! saved-swap!)
  (def deref saved-deref)
  (def println saved-println)
  (def pr-str saved-pr-str)
  (def frequencies saved-frequencies)
  (def sort-by saved-sort-by)
  (def take saved-take)
  (def subs saved-subs))

(defn root-cause
  "Extract root cause from chained error messages like
   'in foo.clj: unhandled exception: in bar.clj: ... actual error'
   Returns the last segment after all 'unhandled exception:' prefixes."
  [err]
  (if (nil? err) err
    (let [parts (split err "unhandled exception: ")
          last-part (last parts)]
      ;; Strip leading 'in file.clj: ' if present
      (if (starts-with? last-part "in ")
        (let [segs (split last-part ": ")]
          (if (> (count segs) 1)
            (join ": " (rest segs))
            last-part))
        last-part))))

(defn try-load-file [path]
  (try
    (let [code (slurp path)]
      (eval (read-string (str "(do " shim-code "\n" code "\n)")))
      (restore-core-fns!)
      :pass)
    (catch e
      (restore-core-fns!)
      (cond
        (string? e) e
        (map? e) (or (:mino/message e) (str e))
        :else (str e)))))

(defn test-repo [repo-name {:keys [src]} original-dir]
  (let [src-dir (str cache-dir "/" repo-name "/" src)
        files (try (do (chdir src-dir) (file-seq src-dir)) (catch _ []))]
    (when (empty? files)
      (println (str "  [skip] " repo-name " (not cloned — run: git clone <url> " cache-dir "/" repo-name ")")))
    (let [source-files (sort (filter source-file? files))
          results (mapv (fn [f]
                          (try
                            (let [relpath (subs f (+ (count src-dir) 1))
                                  _ (chdir src-dir)
                                  result (try-load-file f)]
                              {:file relpath
                               :status (if (= result :pass) :pass :fail)
                               :error (when (not= result :pass) result)})
                            (catch e
                              {:file (subs f (+ (count src-dir) 1))
                               :status :fail
                               :error (str "unhandled exception: " (if (string? e) e (str e)))})))
                        source-files)]
      (chdir original-dir)
      results)))

(defn run []
  (let [original-dir (getcwd)
        repo-filter (when-let [env-repos (getenv "REPOS")]
                      (into #{} (split env-repos ",")))
        verbose? (getenv "VERBOSE")
        repos-to-run (if repo-filter
                       (select-keys repos (vec repo-filter))
                       repos)
        all-results (atom [])
        pass-count (atom 0)
        fail-count (atom 0)]

    (doseq [[repo-name config] (sort repos-to-run)]
      (println (str "\n=== " repo-name " ==="))
      (try
        (let [results (test-repo repo-name config original-dir)]
          (doseq [r results]
            (swap! all-results conj (assoc r :repo repo-name))
            (if (= (:status r) :pass)
              (do (swap! pass-count inc)
                  (when verbose?
                    (println (str "  PASS " (:file r)))))
              (do (swap! fail-count inc)
                  (println (str "  FAIL " (:file r)))
                  (when (:error r)
                    (println (str "       " (:error r))))))))
        (catch e
          (chdir original-dir)
          (println (str "  [crash] " repo-name ": " (if (string? e) e (str e)))))))

    ;; Classify errors by root cause
    (let [failures (filter #(= (:status %) :fail) @all-results)
          classify (fn [err]
                     (let [rc (root-cause err)]
                       (cond
                         (nil? rc) :unknown
                         (or (includes? rc "is not available in mino")
                             (includes? rc "interop disabled")
                             (includes? rc "clojure/java/")
                             (includes? rc "cljs/")
                             (includes? rc "goog/")
                             (includes? rc "System/")
                             (includes? rc "clojure.lang.")
                             (includes? rc "Runtime")
                             (includes? rc "TimeUnit/"))
                         :platform

                         (or (includes? rc "taoensso/")
                             (includes? rc "schema/")
                             (includes? rc "riddley/")
                             (includes? rc "arrangement/")
                             (includes? rc "bencode/")
                             (includes? rc "cheshire/")
                             (includes? rc "manifold/")
                             (includes? rc "promesa/")
                             (includes? rc "weavejester/")
                             (includes? rc "meta_merge/")
                             (includes? rc "borkdude/")
                             (includes? rc "cherry/")
                             (includes? rc "somnium/")
                             (includes? rc "franzy/"))
                         :missing-dep

                         :else :fixable)))
          grouped (group-by #(classify (:error %)) failures)
          n-platform (count (get grouped :platform []))
          n-dep      (count (get grouped :missing-dep []))
          n-fixable  (count (get grouped :fixable []))
          n-unknown  (count (get grouped :unknown []))
          total      (+ @pass-count @fail-count)
          effective  (- total n-platform n-dep)
          pct        (if (> total 0) (int (* 100 (/ @pass-count total))) 0)
          eff-pct    (if (> effective 0) (int (* 100 (/ @pass-count effective))) 0)]

      (println (str "\n=== SUMMARY ==="))
      (println (str "Total: " total " | Pass: " @pass-count " | Fail: " (count failures)))
      (println (str "  Platform-incompatible: " n-platform " (JVM types, Java interop, CLJS)"))
      (println (str "  Missing third-party:   " n-dep))
      (println (str "  Fixable/other:         " (+ n-fixable n-unknown)))
      (println (str "Raw pass rate: " pct "%"))
      (println (str "Effective pass rate (excl. platform+deps): " eff-pct
                    "% (" @pass-count "/" effective ")"))

      ;; Per-repo classification
      (let [per-repo (group-by :repo @all-results)
            repo-rows (for [[rn rs] (sort per-repo)]
                        (let [p (count (filter #(= (:status %) :pass) rs))
                              total-r (count rs)
                              fails (filter #(= (:status %) :fail) rs)
                              cats (frequencies (map #(classify (:error %)) fails))
                              n-plat (get cats :platform 0)
                              n-dep* (get cats :missing-dep 0)
                              n-fix* (+ (get cats :fixable 0) (get cats :unknown 0))]
                          [rn total-r p n-plat n-dep* n-fix*]))]
        (println "\n=== PER-REPO ===")
        (println "repo                  total pass  plat  dep   fix")
        (doseq [[rn total-r p np nd nf] repo-rows]
          (println (str (subs (str rn "                      ") 0 22)
                        (subs (str "    " total-r) (- (count (str "    " total-r)) 4)) " "
                        (subs (str "    " p) (- (count (str "    " p)) 4)) "  "
                        (subs (str "    " np) (- (count (str "    " np)) 4)) "  "
                        (subs (str "    " nd) (- (count (str "    " nd)) 4)) "  "
                        (subs (str "    " nf) (- (count (str "    " nf)) 4))))))

      ;; Top fixable failure reasons (deduplicated by root cause)
      (let [fixable-errors (map (comp root-cause :error) (get grouped :fixable []))
            reasons (->> fixable-errors
                         (frequencies)
                         (sort-by (fn [[_ c]] (- c)))
                         (take 20))]
        (when (seq reasons)
          (println "\n=== FIXABLE FAILURE REASONS ===")
          (doseq [[reason cnt] reasons]
            (println (str "  " cnt " " reason))))))))

(run)
