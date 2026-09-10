(ns mino.tasks.amalgam
  "Shared build helper for repos that embed mino as a `mino/` git
   submodule and build against its single-file amalgamation
   (dist/mino.{c,h,hpp}).

   A consumer's task runner requires this namespace instead of
   re-deriving the recipe, so how the amalgam is materialized lives in
   one place that ships with mino itself (ADR 62). A change here reaches
   every consumer through the submodule; there is no per-consumer copy to
   drift.

   Usage from a consumer's lib/<name>/tasks.clj:

     (require '[mino.tasks.amalgam :as amalgam])
     ;; once, before compiling anything against the amalgam:
     (amalgam/ensure-dist! cc cflags)   ; cflags includes -Imino/dist
     ;; then link each consumer object against amalgam/dist-obj, e.g.
     ;;   cc <cflags> -o bin src.c amalgam/dist-obj -lm -lpthread

   The submodule is assumed at `mino/` relative to the consumer root
   (the layout every mino embedding consumer uses). All paths below are
   consumer-root relative, and the shell/file builtins (sh, sh!,
   file-exists?, file-mtime, slurp, spit, println) are provided by the
   mino task runtime that loads this namespace."
  (:require [clojure.string :as str]))

;; The amalgamation artifacts under the submodule's dist/.
(def dist-c   "mino/dist/mino.c")
(def dist-obj "mino/dist/mino.o")
(def dist-pin "mino/dist/.pin")

;; The amalgamation is the sole mino include root: dist/mino.h (and
;; dist/mino.hpp for C++) are the only mino headers a consumer includes.
(def include-flags "-Imino/dist")

(defn stale?
  "True if output does not exist or any input is newer. A general mtime
   guard consumers use to decide whether a binary needs relinking."
  [inputs output]
  (let [out-mtime (file-mtime output)]
    (if (nil? out-mtime)
      true
      (some #(let [in-mtime (file-mtime %)]
               (and in-mtime (> in-mtime out-mtime)))
            inputs))))

(defn submodule-sha
  "The mino submodule's checked-out commit, or nil if it can't be read."
  []
  (let [r (sh "git" "-C" "mino" "rev-parse" "HEAD")]
    (when (zero? (:exit r)) (str/trim (str (:out r))))))

(defn- recorded-pin []
  (when (file-exists? dist-pin) (str/trim (slurp dist-pin))))

(defn- amalgam-fresh?
  "True when dist/mino.o exists and was built from the currently
   checked-out submodule pin. Keys regeneration to the pin (FR-4) so a
   pin bump forces a rebuild but an unchanged pin does not."
  [sha]
  (and (file-exists? dist-obj)
       (file-exists? dist-c)
       (some? sha)
       (= sha (recorded-pin))))

(defn ensure-dist!
  "Materialize mino/dist/mino.o from the pinned submodule, once per pin.
   Bootstraps the submodule (its Makefile generates the bundled headers
   and the mino binary), runs the amalgam task from inside the submodule,
   and compiles the single TU to an object with the caller's `cc` and
   `cflags` (which must carry the include root, `-Imino/dist`). A no-op
   when the object already matches the checked-out pin."
  [cc cflags]
  (let [sha (submodule-sha)]
    (if (amalgam-fresh? sha)
      (println (str "  amalgam up to date (pin " sha ")"))
      (do
        (println "  bootstrapping mino submodule")
        (let [r (sh "sh" "-c" "cd mino && make")]
          (println (:out r))
          (when-not (zero? (:exit r))
            (println (:err r))
            (throw (ex-info "mino bootstrap failed" {:exit (:exit r)}))))
        (println "  amalgamating mino -> mino/dist/mino.{c,h,hpp}")
        (let [r (sh "sh" "-c" "cd mino && ./mino task amalgamate")]
          (println (:out r))
          (when-not (zero? (:exit r))
            (println (:err r))
            (throw (ex-info "mino amalgamate failed" {:exit (:exit r)}))))
        (println (str "  compiling amalgam -> " dist-obj))
        (let [args (into [cc] (concat cflags ["-c" "-o" dist-obj dist-c]))]
          (println (str "  " (str/join " " args)))
          (apply sh! args))
        (spit dist-pin (or sha ""))))))
