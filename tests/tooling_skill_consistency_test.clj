(require "tests/test")
(require "tools/triage_findings")
(require "tools/merge_proposals")
(require '[clojure.string :as str])

;; Pins the skill-system wiring: every SKILL.md carries exactly one of
;; the two visibility flags (entry skills are human-only, recipes are
;; agent-only), and the EDN examples stated in agent bodies parse and
;; validate against the spine schemas, so an agent following its own
;; body produces files the spine accepts.

(def sk-skills-dir ".claude/skills")
(def sk-agents-dir ".claude/agents")

(def sk-entry-skills
  #{"implement-change" "fix-bug" "audit-code"
    "capture-guidance" "incorporate-feedback"})

(def sk-internal-skills
  #{"check-memory" "check-security" "check-conformance" "check-style"
    "check-factoring" "check-portability"
    "write-c" "write-clj" "write-tests"
    "apply-findings" "verify-lanes" "maintain-toolchain"
    "gather-module-context" "run-review-round" "write-changelog"})

;; Shared skills carry NEITHER flag: in the / menu for humans AND
;; invocable by agents via the Skill tool.
(def sk-shared-skills
  #{"record-decision"})

(defn- sk-skill-files []
  (sort (filterv #(str/ends-with? % "/SKILL.md") (file-seq sk-skills-dir))))

(defn- sk-skill-name [path]
  ;; .claude/skills/<name>/SKILL.md -> <name>
  (let [parts (str/split path #"/")]
    (nth parts (- (count parts) 2))))

(defn- sk-frontmatter
  "Parse the YAML frontmatter block into {string string}."
  [path]
  (let [lines (str/split-lines (slurp path))]
    (when-not (= "---" (first lines))
      (throw (ex-info (str path " does not start with frontmatter") {})))
    (loop [m {} ls (rest lines)]
      (let [l (first ls)]
        (cond
          (nil? l) (throw (ex-info (str path " frontmatter never closes") {}))
          (= "---" l) m
          :else (let [i (str/index-of l ":")]
                  (recur (if i
                           (assoc m
                                  (str/trim (subs l 0 i))
                                  (str/trim (subs l (inc i))))
                           m)
                         (rest ls))))))))

(deftest skill-inventory-is-complete
  (let [names (set (mapv sk-skill-name (sk-skill-files)))]
    (is (= (into (into sk-entry-skills sk-internal-skills) sk-shared-skills)
           names)
        "skills on disk must be exactly the entry + internal + shared sets")))

(deftest every-skill-has-the-right-visibility-flags
  (doseq [path (sk-skill-files)]
    (let [nm (sk-skill-name path)
          fm (sk-frontmatter path)
          entry?    (= "true" (get fm "disable-model-invocation"))
          internal? (= "false" (get fm "user-invocable"))]
      (if (sk-shared-skills nm)
        (is (and (not (contains? fm "disable-model-invocation"))
                 (not (contains? fm "user-invocable")))
            (str path " is shared: it must carry neither visibility flag"))
        (is (or (and entry? (not (contains? fm "user-invocable")))
                (and internal? (not (contains? fm "disable-model-invocation"))))
            (str path " must carry exactly one visibility flag"))))))

(deftest entry-and-internal-classification-matches
  (doseq [path (sk-skill-files)]
    (let [nm (sk-skill-name path)
          fm (sk-frontmatter path)]
      (when (sk-entry-skills nm)
        (is (= "true" (get fm "disable-model-invocation"))
            (str nm " is an entry skill: disable-model-invocation: true")))
      (when (sk-internal-skills nm)
        (is (= "false" (get fm "user-invocable"))
            (str nm " is a recipe: user-invocable: false"))))))

(deftest skill-frontmatter-name-matches-directory
  (doseq [path (sk-skill-files)]
    (let [fm (sk-frontmatter path)]
      (is (= (sk-skill-name path) (get fm "name"))
          (str path " frontmatter name must match its directory")))))

(deftest every-skill-has-description
  (doseq [path (sk-skill-files)]
    (is (not (str/blank? (get (sk-frontmatter path) "description")))
        (str path " needs a description"))))

;; ---- agent-body EDN examples must satisfy the spine schemas ----

(defn- sk-agent-files []
  (sort (filterv #(str/ends-with? % ".md") (file-seq sk-agents-dir))))

(defn- sk-edn-blocks
  "Extract the contents of ```edn fenced blocks."
  [path]
  (loop [blocks [] cur nil ls (str/split-lines (slurp path))]
    (let [l (first ls)]
      (cond
        (nil? l)                    blocks
        (and (nil? cur) (= "```edn" (str/trim l))) (recur blocks [] (rest ls))
        (and cur (= "```" (str/trim l))) (recur (conj blocks (str/join "\n" cur)) nil (rest ls))
        cur                         (recur blocks (conj cur l) (rest ls))
        :else                       (recur blocks cur (rest ls))))))

(deftest agent-edn-examples-parse-and-validate
  (let [blocks (vec (mapcat (fn [p] (mapv (fn [b] [p b]) (sk-edn-blocks p)))
                            (sk-agent-files)))]
    (is (pos? (count blocks)) "agent bodies must state their formats as edn blocks")
    (doseq [[path block] blocks]
      (let [v (read-string block)]
        (cond
          ;; findings example: vector of finding maps
          (and (vector? v) (:dimension (first v)))
          (doseq [f v]
            (is (nil? (tools.triage-findings/validate-finding f path))
                (str path ": findings example must satisfy the spine schema")))

          ;; proposal example
          (and (map? v) (:branch v) (:changelog v))
          (is (nil? (tools.merge-proposals/validate-proposal v path))
              (str path ": proposal example must satisfy the spine schema"))

          :else
          (is false (str path ": unrecognized edn example shape")))))))

(deftest reviewer-and-editor-state-the-formats
  (is (some #(and (vector? %) (:dimension (first %)))
            (mapv read-string (sk-edn-blocks (str sk-agents-dir "/reviewer.md"))))
      "reviewer.md must contain a findings example")
  (is (some #(and (map? %) (:branch %) (:changelog %))
            (mapv read-string (sk-edn-blocks (str sk-agents-dir "/editor.md"))))
      "editor.md must contain a proposal example"))

(run-tests-and-exit)
