#!/usr/bin/env bb
;; tools/bump_satellites.bb
;;
;; Move the mino submodule in every downstream satellite to a given
;; CalVer tag, run each satellite's cheap lane, commit direct to
;; master, and (when a token is present) push. This is the one-command
;; release step that replaces the hand-made "Embed: bump mino submodule
;; to <tag>" commit previously made per satellite per release.
;;
;;   bb tools/bump_satellites.bb <CalVer-tag>              # stage only
;;   bb tools/bump_satellites.bb <CalVer-tag> --verify     # stage + validate cheap lane
;;   SATELLITE_BUMP_TOKEN=<pat> bb tools/bump_satellites.bb <CalVer-tag> --verify
;;                                                          # validate + commit + push
;;   bb tools/bump_satellites.bb <tag> --verify --only=mino-tests   # one satellite
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

(require '[clojure.string :as str]
         '[clojure.java.shell :refer [sh]]
         '[clojure.java.io :as io])
(import '[java.io File])

(def argv *command-line-args*)
(def tag (first argv))
(def verify? (some #(#{ "--verify" } %) argv))
(def only (first (filter #(str/starts-with? % "--only=") argv)))
(def only-name (when only (subs only (count "--only="))))
(def token (System/getenv "SATELLITE_BUMP_TOKEN"))

(when (or (nil? tag) (= tag "--verify") (str/starts-with? tag "--"))
  (println "usage: bb tools/bump_satellites.bb <CalVer-tag> [--verify] [--only <name>]")
  (System/exit 2))

(def mino-root (-> (io/file *file*) .getCanonicalFile .getParentFile .getParentFile))
(def satellites-dir
  (io/file (or (System/getenv "MINO_SATELLITES_DIR")
              (-> mino-root .getParentFile .getPath))))

;; Each satellite: name, github repo slug, bootstrap (builds the mino
;; the cheap lane runs against; nil for satellites that do not build
;; mino), and the cheap-lane command vector run in the satellite root.
(def satellites
  [{:name      "mino-tests"
    :repo      "leifericf/mino-tests"
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

(defn sh-ok?
  ([dir args] (sh-ok? dir args true))
  ([dir args strict?]
   (let [r (apply sh (conj (vec args) :dir (str dir)))]
     (when (seq (:out r)) (print (:out r)))
     (when (seq (:err r)) (print (:err r)))
     (cond
       (not (zero? (:exit r))) (do (println "  ! exit" (:exit r)) false)
       (not strict?)           true
       :else                   true))))

(defn git [dir & args] (sh-ok? (io/file dir) (into ["git"] args)))

(defn push-url [repo]
  (str "https://x-access-token:" token "@github.com/" repo ".git"))

(defn run-bootstrap [dir bootstrap]
  (if (nil? bootstrap)
    true
    (do (println "  . bootstrap") (sh-ok? dir bootstrap))))

(defn bump-one [{:keys [name repo bootstrap cheap] :as sat}]
  (let [dir (io/file satellites-dir name)
        msg (format "Embed: bump mino submodule to %s" tag)]
    (println)
    (println "== mino/" name "==>")
    (cond
      (not (.exists dir))
      (do (println "  ! no checkout at" (str dir)) :missing)

      (not (.isDirectory (io/file dir "mino")))
      (do (println "  ! no mino/ submodule at" (str dir)) :missing)

      (not (and (git (io/file dir "mino") "fetch" "origin" "tag" tag)
                (git (io/file dir "mino") "checkout" tag)
                (git dir "add" "mino")))
      (do (println "  ! bump git steps failed") :failed)

      :else
      (let [;; git diff --cached --quiet exits 1 when a staged change
            ;; exists, 0 when the index is clean.
            moved? (= 1 (:exit (sh "git" "-C" (str dir) "diff" "--cached" "--quiet" "mino")))]
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
              (not (sh-ok? dir cheap))
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
  (println "satellites dir:" (str satellites-dir))
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
      (System/exit (if (or (seq failed) (empty? results)) 1 0)))))

(-main)
