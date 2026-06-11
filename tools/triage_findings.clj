(ns tools.triage-findings)

;; Fold reviewer findings EDN into one ordered punch list.
;;
;; Input: <run-dir>/findings/*.edn, each a vector of finding maps:
;;
;;   {:id        "sec-gc-001"          ; unique within the run
;;    :dimension :security             ; :memory :security :conformance
;;                                     ; :style :factoring :portability
;;    :module    "src/gc"              ; module shard reviewed
;;    :file      "src/gc/driver.c"
;;    :line      123                   ; 0 for file-level findings
;;    :severity  :high                 ; :high :medium :low
;;    :level     :correctness          ; :correctness :factoring :style
;;    :title     "one-line summary"
;;    :detail    "what + why"
;;    :suggestion "optional fix sketch"}
;;
;; Output: <run-dir>/punch-list.edn
;;   {:items [...ordered findings...]
;;    :counts {:total N :by-level {...} :by-severity {...}}
;;    :duplicates-dropped D}
;;
;; Ordering: fix level (correctness -> factoring -> style), then
;; severity (high -> medium -> low), then file, then line. Exact
;; duplicates ([file line title]) from overlapping reviewers are
;; dropped. Invalid findings throw -- the spine never guesses.
;;
;; CLI: ./mino tools/triage_findings.clj <run-dir>

(require '[clojure.string :as str])

(def dimensions #{:memory :security :conformance :style :factoring :portability})
(def severities #{:high :medium :low})
(def levels     #{:correctness :factoring :style})

(def ^:private level-rank    {:correctness 0 :factoring 1 :style 2})
(def ^:private severity-rank {:high 0 :medium 1 :low 2})

(defn validate-finding
  "Throw if `f` does not match the canonical finding shape; nil if ok.
   `source` names the findings file for the error message."
  [f source]
  (let [problem
        (cond
          (not (map? f))                          "finding is not a map"
          (not (string? (:id f)))                 ":id must be a string"
          (not (dimensions (:dimension f)))       ":dimension not one of the six review dimensions"
          (not (string? (:module f)))             ":module must be a string"
          (not (string? (:file f)))               ":file must be a string"
          (not (int? (:line f)))                  ":line must be an int (0 for file-level)"
          (not (severities (:severity f)))        ":severity must be :high, :medium, or :low"
          (not (levels (:level f)))               ":level must be :correctness, :factoring, or :style"
          (not (string? (:title f)))              ":title must be a string"
          (not (string? (:detail f)))             ":detail must be a string"
          :else nil)]
    (when problem
      (throw (ex-info (str "triage: invalid finding in " source ": " problem)
                      {:source source :finding f})))))

(defn- findings-files [run-dir]
  (sort (filterv #(str/ends-with? % ".edn")
                 (file-seq (str run-dir "/findings")))))

(defn- read-findings [path]
  (let [v (read-string (slurp path))]
    (when-not (vector? v)
      (throw (ex-info (str "triage: " path " is not a vector of findings") {:path path})))
    (doseq [f v] (validate-finding f path))
    v))

(defn- dedupe-findings [findings]
  (loop [seen #{} kept [] dropped 0 fs findings]
    (if (empty? fs)
      {:kept kept :dropped dropped}
      (let [f (first fs)
            k [(:file f) (:line f) (:title f)]]
        (if (seen k)
          (recur seen kept (inc dropped) (rest fs))
          (recur (conj seen k) (conj kept f) dropped (rest fs)))))))

(defn- count-by [k items]
  (reduce (fn [m item] (update m (get item k) (fnil inc 0))) {} items))

(defn triage!
  "Fold <run-dir>/findings/*.edn into <run-dir>/punch-list.edn and
   return the punch-list map."
  [run-dir]
  (let [all     (vec (mapcat read-findings (findings-files run-dir)))
        {:keys [kept dropped]} (dedupe-findings all)
        ordered (vec (sort-by (juxt #(level-rank (:level %))
                                    #(severity-rank (:severity %))
                                    :file
                                    :line)
                              kept))
        result  {:items ordered
                 :counts {:total       (count ordered)
                          :by-level    (count-by :level ordered)
                          :by-severity (count-by :severity ordered)}
                 :duplicates-dropped dropped}]
    (spit (str run-dir "/punch-list.edn") (pr-str result))
    result))

(defn -main [& args]
  (let [[run-dir] args]
    (when-not run-dir
      (throw (ex-info "usage: triage_findings.clj <run-dir>" {})))
    (let [{:keys [counts duplicates-dropped]} (triage! run-dir)]
      (println (str "triage: " (:total counts) " item(s)"
                    " (" (get-in counts [:by-level :correctness] 0) " correctness, "
                    (get-in counts [:by-level :factoring] 0) " factoring, "
                    (get-in counts [:by-level :style] 0) " style), "
                    duplicates-dropped " duplicate(s) dropped"
                    " -> " run-dir "/punch-list.edn")))))

(when (str/ends-with? (str *file*) "tools/triage_findings.clj")
  (apply -main *command-line-args*))
