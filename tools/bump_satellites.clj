(ns tools.bump-satellites)

;; Move the mino submodule in every downstream satellite to a given
;; CalVer tag, run each satellite's cheap lane, commit direct to
;; master, and (when a token is present) push. This is the one-command
;; release step that replaces the hand-made "Embed: bump mino submodule
;; to <tag>" commit previously made per satellite per release.
;;
;;   ./mino tools/bump_satellites.clj <CalVer-tag>              # stage only
;;   ./mino tools/bump_satellites.clj <CalVer-tag> --verify     # stage + validate cheap lane
;;   SATELLITE_BUMP_TOKEN=<pat> ./mino tools/bump_satellites.clj <CalVer-tag> --verify
;;                                                               # validate + commit + push
;;   ./mino tools/bump_satellites.clj <tag> --verify --only=mino-tests   # one satellite
;;
;; Tag is a bare CalVer, e.g. 2026.08.08-alpha1 (matches src/mino.h's
;; MINO_VERSION and the release-build tag filter). No "v" prefix.
;;
;; The satellite checkouts are read from $MINO_SATELLITES_DIR (default
;; the parent of the mino root), one dir per satellite name. The CI job
;; clones them fresh and points $MINO_SATELLITES_DIR at the workdir;
;; local dev points it at wherever the siblings live.
;;
;; Per-satellite failure is isolated: one broken satellite aborts just
;; that repo (it stays on its old pin and is reported in the summary),
;; never the whole run. The push step uses an explicit push URL with
;; the token embedded, so the remote config is never mutated.
;;
;; PAT scope: a fine-grained token with contents:write on the six
;; satellite repos. Store it as SATELLITE_BUMP_TOKEN in the mino repo's
;; Actions secrets; the CI job (bump-satellites) passes it through.

(require '[clojure.string :as str])

(def argv *command-line-args*)
(def tag (first argv))
(def verify? (some #(= % "--verify") argv))
(def only (first (filter #(str/starts-with? % "--only=") argv)))
(def only-name (when only (subs only (count "--only="))))
(def token (getenv "SATELLITE_BUMP_TOKEN"))

(when (or (nil? tag) (= tag "--verify") (str/starts-with? tag "--"))
  (println "usage: ./mino tools/bump_satellites.clj <CalVer-tag> [--verify] [--only=<name>]")
  (exit 2))

(def satellites-dir
  (or (getenv "MINO_SATELLITES_DIR") ".."))

(def satellites
  [{:name      "mino-tests"
    :repo      "leiferacf/mino-tests"
    :bootstrap ["sh" "-c" "cd mino && make"]
    :cheap     ["sh" "-c" "./mino/mino task adv-test"]}
   {:name      "mino-bench"
    :repo      "leifericf/mino-bench"
    :bootstrap ["sh" "-c" "cd mino && make"]
    :cheap     ["sh" "-c" "./mino/mino task fuzz-smoke"]}
   {:name      "mino-examples"
    :repo      "leifericf/mino-examples"
    :bootstrap ["sh" "-c" "cd mino && make"]
    :cheap     ["make"]}
   {:name      "mino-lsp"
    :repo      "leifericf/mino-lsp"
    :bootstrap ["sh" "-c" "cd mino && make"]
    :cheap     ["make"]}
   {:name      "mino-nrepl"
    :repo      "leifericf/mino-nrepl"
    :bootstrap ["sh" "-c" "cd mino && make"]
    :cheap     ["make"]}
   {:name      "mino-site"
    :repo      "leifericf/mino-site"
    :bootstrap nil
    :cheap     ["sh" "-c" "clojure -M:test"]}])

(defn redact
  "Scrub the push token wherever it appears in git output so it never
  reaches the logs. git echoes the token-bearing push URL in its stderr
  on a failed push. A no-op when no token is set."
  [s]
  (if (and token (seq token) s)
    (str/replace s token "***")
    s))

(defn run-ok?
  "Run a command in dir, echo its (redacted) output, and return true on a
  zero exit."
  [dir args]
  (let [r (apply run {:dir dir} args)]
    (when (seq (:out r)) (print (redact (:out r))))
    (when (seq (:err r)) (print (redact (:err r))))
    (if (zero? (:exit r))
      true
      (do (println "  ! exit" (:exit r)) false))))

(defn git [dir & args]
  (run-ok? dir (into ["git"] args)))

(defn push-url [repo]
  (str "https://x-access-token:" token "@github.com/" repo ".git"))

(defn run-bootstrap [dir bootstrap]
  (if (nil? bootstrap)
    true
    (do (println "  . bootstrap") (run-ok? dir bootstrap))))

(defn bump-one [{:keys [name repo bootstrap cheap] :as sat}]
  (let [dir    (str satellites-dir "/" name)
        submod (str dir "/mino")
        msg    (format "Embed: bump mino submodule to %s" tag)]
    (println)
    (println "== mino/" name "==>")
    (cond
      (not (file-exists? dir))
      (do (println "  ! no checkout at" dir) :missing)

      (not (file-exists? submod))
      (do (println "  ! no mino/ submodule at" dir) :missing)

      (not (and (git submod "fetch" "origin" "tag" tag)
                (git submod "checkout" tag)
                (git dir "add" "mino")))
      (do (println "  ! bump git steps failed") :failed)

      :else
      (let [moved? (= 1 (:exit (run {:dir dir} "git" "diff" "--cached" "--quiet" "mino")))]
        (cond
          (not moved?)
          (do (println "  . submodule already at" tag "; nothing to commit") :uptodate)

          (not verify?)
          (do (println "  . staged; rerun with --verify to gate") :staged)

          (not (run-bootstrap dir bootstrap))
          (do (println "  ! bootstrap failed; staged bump left uncommitted") :failed)

          :else
          (do
            (println "  . cheap lane")
            (cond
              (not (run-ok? dir cheap))
              (do (println "  ! cheap lane failed; staged bump left uncommitted") :failed)

              (nil? token)
              (do (println "  . validated; would commit + push with SATELLITE_BUMP_TOKEN") :validated)

              (not (git dir "commit" "-m" msg))
              (do (println "  ! commit failed") :failed)

              (not (git dir "push" (push-url repo) "master"))
              (do (println "  ! push failed") :failed)

              :else
              (do (println "  . pushed master") :pushed))))))))

(defn -main []
  (println "bump mino submodule ->" tag)
  (println "satellites dir:" satellites-dir)
  (println "verify:" (boolean verify?) "| push:" (if token "enabled" "no token (validate only)"))
  (let [targets (if only-name (filter #(= (:name %) only-name) satellites) satellites)
        results (doall
                  (for [sat targets]
                    [(:name sat) (bump-one sat)]))]
    (println)
    (println "summary:")
    (doseq [[n r] results] (println "  " n "->" (name r)))
    (let [failed (filter #(#{:failed :missing} (second %)) results)]
      (when (seq failed)
        (println " " (count failed) "satellite(s) need triage"))
      (exit (if (or (seq failed) (empty? results)) 1 0)))))

(-main)
