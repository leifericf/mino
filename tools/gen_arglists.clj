(ns tools.gen-arglists
  (:require [clojure.edn :as edn]
            [clojure.java.shell :as shell]
            [clojure.string :as str]))

;; Regenerate src/prim/arglists_data.h, the committed install-time
;; :arglists table for the C prims, from the census oracle surface.
;; Oracle-conformant and lax prims attach the oracle shapes verbatim;
;; real arity gaps attach mino-true shapes derived from the prim
;; sources. The script runs only when the oracle or the emit lists
;; change; the Makefile never invokes it.
;;
;; The 2026-08-28 extension covers the namespaces beyond
;; clojure.core/string/repl: no C prims exist in them, so their bare
;; vars (the def-alias re-exports in lib/) attach at their def sites
;; and are recorded here only as the lib-alias class.
;;
;;   bb tools/gen_arglists.clj [path-to-surface.edn]
;;
;; Surface path resolves from the first CLI arg, else $CENSUS_SURFACE,
;; else the default census checkout below. Run from the repo root; the
;; output path is relative.
;;
;; Report mode prints the CURRENT divergences instead of emitting the
;; header:
;;
;;   bb tools/gen_arglists.clj --report-only [path-to-surface.edn]
;;
;; The mino binary is probed one-shot (a small script it evaluates,
;; printing the live ns-publics arglists as EDN, mirroring the census
;; capture) and every oracle var with :arglists is classified as
;; exact, name-diff (same arity shape, different parameter names; the
;; rename targets), shape-diff (documented mino-true versus genuine
;; gap), bare-var, or var-absent. Exit 1 when name-diffs remain, so a
;; lane can gate on zero. Binary resolution: $MINO_BIN, else ./mino,
;; else PATH; rebuild first after core.clj or lib/ changes.

(def default-surface "/Users/leif/Code/clojure-census/clojure/1.12.4-surface.edn")

(def cli-args (remove #{"--report-only"} *command-line-args*))

(def report-only? (boolean (some #{"--report-only"} *command-line-args*)))

(def surface-path
  (or (first cli-args)
      (System/getenv "CENSUS_SURFACE")
      default-surface))

;; Non-prim names the sweep tables list alongside prims: special-form
;; macros and core.clj def aliases. Their arglists come from their own
;; definitions or later commits, never this table.
(def non-prims
  #{
  "array-map"
  "binding"
  "declare"
  "defmacro"
  "fn"
  "get-in"
  "interleave"
  "lazy-seq"
  "let"
  "loop"
  "ns"
  "partition"    })

;; The base emit list: the 253 arity-conformant prim names
;; ("ns/name") frozen from the 2026-08-27 sweep plus the eleven the
;; follow-up class fixes admitted (ADR 34); regenerate via the sweep.
(def emit-list
  #{
  "clojure.core/*"
  "clojure.core/*'"
  "clojure.core/+"
  "clojure.core/+'"
  "clojure.core/-"
  "clojure.core/-'"
  "clojure.core//"
  "clojure.core/NaN?"
  "clojure.core/add-watch"
  "clojure.core/agent"
  "clojure.core/agent-error"
  "clojure.core/alength"
  "clojure.core/alias"
  "clojure.core/alter"
  "clojure.core/alter-meta!"
  "clojure.core/alter-var-root"
  "clojure.core/apply"
  "clojure.core/assoc"
  "clojure.core/assoc!"
  "clojure.core/atom"
  "clojure.core/await"
  "clojure.core/await-for"
  "clojure.core/bigdec"
  "clojure.core/bigint"
  "clojure.core/biginteger"
  "clojure.core/bit-not"
  "clojure.core/bit-shift-left"
  "clojure.core/bit-shift-right"
  "clojure.core/boolean-array"
  "clojure.core/boolean?"
  "clojure.core/byte"
  "clojure.core/bytes?"
  "clojure.core/char"
  "clojure.core/char-array"
  "clojure.core/char?"
  "clojure.core/chunk"
  "clojure.core/chunk-append"
  "clojure.core/chunk-buffer"
  "clojure.core/chunk-cons"
  "clojure.core/chunk-first"
  "clojure.core/chunk-next"
  "clojure.core/chunk-rest"
  "clojure.core/chunked-seq?"
  "clojure.core/class"
  "clojure.core/commute"
  "clojure.core/comp"
  "clojure.core/compare"
  "clojure.core/compare-and-set!"
  "clojure.core/complement"
  "clojure.core/conj"
  "clojure.core/cons"
  "clojure.core/contains?"
  "clojure.core/count"
  "clojure.core/create-ns"
  "clojure.core/dec"
  "clojure.core/dec'"
  "clojure.core/decimal?"
  "clojure.core/deliver"
  "clojure.core/denominator"
  "clojure.core/deref"
  "clojure.core/destructure"
  "clojure.core/disj"
  "clojure.core/disj!"
  "clojure.core/dissoc"
  "clojure.core/dissoc!"
  "clojure.core/doall"
  "clojure.core/dorun"
  "clojure.core/double"
  "clojure.core/double-array"
  "clojure.core/empty"
  "clojure.core/empty?"
  "clojure.core/ensure"
  "clojure.core/error-handler"
  "clojure.core/error-mode"
  "clojure.core/eval"
  "clojure.core/even?"
  "clojure.core/every?"
  "clojure.core/false?"
  "clojure.core/file-seq"
  "clojure.core/filterv"
  "clojure.core/find"
  "clojure.core/find-keyword"
  "clojure.core/find-ns"
  "clojure.core/find-var"
  "clojure.core/first"
  "clojure.core/float"
  "clojure.core/float-array"
  "clojure.core/float?"
  "clojure.core/flush"
  "clojure.core/fn?"
  "clojure.core/format"
  "clojure.core/frequencies"
  "clojure.core/future-call"
  "clojure.core/future-cancel"
  "clojure.core/future-cancelled?"
  "clojure.core/future-done?"
  "clojure.core/future?"
  "clojure.core/gensym"
  "clojure.core/get"
  "clojure.core/get-validator"
  "clojure.core/group-by"
  "clojure.core/hash"
  "clojure.core/hash-map"
  "clojure.core/hash-set"
  "clojure.core/in-ns"
  "clojure.core/inc"
  "clojure.core/inc'"
  "clojure.core/infinite?"
  "clojure.core/int"
  "clojure.core/int-array"
  "clojure.core/int?"
  "clojure.core/intern"
  "clojure.core/juxt"
  "clojure.core/keys"
  "clojure.core/keyword"
  "clojure.core/keyword?"
  "clojure.core/list"
  "clojure.core/list?"
  "clojure.core/load-file"
  "clojure.core/load-string"
  "clojure.core/long"
  "clojure.core/long-array"
  "clojure.core/macroexpand"
  "clojure.core/macroexpand-1"
  "clojure.core/map?"
  "clojure.core/mapv"
  "clojure.core/merge-with"
  "clojure.core/meta"
  "clojure.core/mod"
  "clojure.core/name"
  "clojure.core/namespace"
  "clojure.core/neg?"
  "clojure.core/newline"
  "clojure.core/nil?"
  "clojure.core/not"
  "clojure.core/not-any?"
  "clojure.core/not-every?"
  "clojure.core/ns-aliases"
  "clojure.core/ns-imports"
  "clojure.core/ns-interns"
  "clojure.core/ns-map"
  "clojure.core/ns-name"
  "clojure.core/ns-publics"
  "clojure.core/ns-refers"
  "clojure.core/ns-resolve"
  "clojure.core/ns-unalias"
  "clojure.core/ns-unmap"
  "clojure.core/nth"
  "clojure.core/number?"
  "clojure.core/numerator"
  "clojure.core/odd?"
  "clojure.core/parse-double"
  "clojure.core/parse-long"
  "clojure.core/parse-uuid"
  "clojure.core/partial"
  "clojure.core/peek"
  "clojure.core/persistent!"
  "clojure.core/pop"
  "clojure.core/pop!"
  "clojure.core/pos?"
  "clojure.core/pr"
  "clojure.core/pr-str"
  "clojure.core/print"
  "clojure.core/printf"
  "clojure.core/println"
  "clojure.core/prn"
  "clojure.core/promise"
  "clojure.core/quot"
  "clojure.core/rand"
  "clojure.core/random-uuid"
  "clojure.core/range"
  "clojure.core/ratio?"
  "clojure.core/rational?"
  "clojure.core/rationalize"
  "clojure.core/re-pattern"
  "clojure.core/read-line"
  "clojure.core/read-string"
  "clojure.core/record?"
  "clojure.core/reduced"
  "clojure.core/reduced?"
  "clojure.core/ref"
  "clojure.core/ref-set"
  "clojure.core/refer"
  "clojure.core/rem"
  "clojure.core/remove-ns"
  "clojure.core/remove-watch"
  "clojure.core/require"
  "clojure.core/requiring-resolve"
  "clojure.core/reset!"
  "clojure.core/reset-vals!"
  "clojure.core/restart-agent"
  "clojure.core/rest"
  "clojure.core/reverse"
  "clojure.core/rseq"
  "clojure.core/rsubseq"
  "clojure.core/send"
  "clojure.core/send-off"
  "clojure.core/seq"
  "clojure.core/seq?"
  "clojure.core/set"
  "clojure.core/set-error-handler!"
  "clojure.core/set-error-mode!"
  "clojure.core/set-validator!"
  "clojure.core/set?"
  "clojure.core/short"
  "clojure.core/short-array"
  "clojure.core/shutdown-agents"
  "clojure.core/some"
  "clojure.core/some?"
  "clojure.core/sort"
  "clojure.core/sorted-map"
  "clojure.core/sorted-map-by"
  "clojure.core/sorted-set"
  "clojure.core/sorted-set-by"
  "clojure.core/str"
  "clojure.core/string?"
  "clojure.core/subs"
  "clojure.core/subseq"
  "clojure.core/subvec"
  "clojure.core/swap!"
  "clojure.core/swap-vals!"
  "clojure.core/symbol?"
  "clojure.core/the-ns"
  "clojure.core/transient"
  "clojure.core/true?"
  "clojure.core/type"
  "clojure.core/unchecked-add"
  "clojure.core/unchecked-add-int"
  "clojure.core/unchecked-byte"
  "clojure.core/unchecked-char"
  "clojure.core/unchecked-dec"
  "clojure.core/unchecked-dec-int"
  "clojure.core/unchecked-divide-int"
  "clojure.core/unchecked-double"
  "clojure.core/unchecked-float"
  "clojure.core/unchecked-inc"
  "clojure.core/unchecked-inc-int"
  "clojure.core/unchecked-int"
  "clojure.core/unchecked-long"
  "clojure.core/unchecked-multiply"
  "clojure.core/unchecked-multiply-int"
  "clojure.core/unchecked-negate"
  "clojure.core/unchecked-negate-int"
  "clojure.core/unchecked-remainder-int"
  "clojure.core/unchecked-short"
  "clojure.core/unchecked-subtract"
  "clojure.core/unchecked-subtract-int"
  "clojure.core/unsigned-bit-shift-right"
  "clojure.core/use"
  "clojure.core/uuid?"
  "clojure.core/vals"
  "clojure.core/var-get"
  "clojure.core/var-set"
  "clojure.core/var?"
  "clojure.core/vary-meta"
  "clojure.core/vector"
  "clojure.core/vector?"
  "clojure.core/volatile!"
  "clojure.core/volatile?"
  "clojure.core/vreset!"
  "clojure.core/zero?"
  "clojure.core/zipmap"
  "clojure.string/join"
  "clojure.string/split"    })

;; Lax prims: mino accepts every oracle arity plus extras it tolerates
;; silently. Lax arity acceptance verified by the 2026-08-27 sweep;
;; oracle arglists attached, extra tolerated arities unclaimed (the
;; extras become a census divergence entry in a later slice). spit was
;; not swept (it writes files); prim_spit genuinely supports its
;; option tail (:append honored, :encoding validated), so the oracle
;; shape is true for it and it rides this class.
(def lax-prims
  #{
  "clojure.core/<"
  "clojure.core/<="
  "clojure.core/="
  "clojure.core/>"
  "clojure.core/>="
  "clojure.core/all-ns"
  "clojure.core/bit-and"
  "clojure.core/bit-or"
  "clojure.core/bit-xor"
  "clojure.core/byte-array"
  "clojure.core/conj!"
  "clojure.core/distinct?"
  "clojure.core/get-thread-bindings"
  "clojure.core/identical?"
  "clojure.core/loaded-libs"
  "clojure.core/object-array"
  "clojure.core/realized?"
  "clojure.core/ref-history-count"
  "clojure.core/ref-max-history"
  "clojure.core/ref-min-history"
  "clojure.core/release-pending-sends"
  "clojure.core/send-via"
  "clojure.core/spit"
  "clojure.core/symbol"
  "clojure.core/to-array"
  "clojure.core/with-meta"    })

;; Real arity gaps: the prim rejects oracle-claimed arities, so the
;; oracle shape would be lying metadata. Each shape is derived from
;; the prim's own arg-count logic, borrowing oracle param names where
;; the shape coincides; a key here overrides the oracle lookup. The
;; missing arities are deferred (ADR 34) and become census
;; divergences.
(def mino-true
  {"clojure.core/aget"
   {:arglists '([array idx])
    :reason "prim_aget accepts exactly (array idx); variadic index dims unimplemented"}

   "clojure.core/aset"
    {:arglists '([array idx val])
     :reason "prim_aset accepts exactly (array idx val); variadic index dims unimplemented"}

   "clojure.core/resolve"
   {:arglists '([sym])
    :reason "prim_resolve accepts exactly one symbol; the env-arity is unimplemented"}

   "clojure.core/slurp"
   {:arglists '([f])
    :reason "prim_slurp accepts exactly one path argument; the opts tail is unimplemented"}

    "clojure.core/with-bindings*"
    {:arglists '([binding-map f])
     :reason "prim_with_bindings_star accepts exactly (binding-map f); the variadic args tail is unimplemented"}})

;; Lib def-alias re-exports: plain (def name clojure.core/name) forms
;; in lib/ whose vars were bare while the oracle carries arglists
;; (2026-08-28 probe). No prim table installs into their namespaces,
;; so the table cannot attach them; the shapes ride def metadata at
;; each alias site (the array-map precedent in core.clj), preserving
;; fn identity with the core twins. Entries without :reason are frozen
;; oracle-verbatim and must stay equal to the oracle arglists; a
;; :reason marks a mino-true override whose oracle arities the alias
;; target rejects.
(def lib-aliases
  {"clojure.walk/walk"            {:arglists '([inner outer form])}
   "clojure.walk/postwalk"        {:arglists '([f form])}
   "clojure.walk/prewalk"         {:arglists '([f form])}
   "clojure.walk/postwalk-replace" {:arglists '([smap form])}
   "clojure.walk/prewalk-replace" {:arglists '([smap form])}
   "clojure.core.protocols/coll-reduce"
   {:arglists '([coll f init])
    :reason "the defprotocol dispatch fn accepts exactly (coll f init); the oracle 2-arity is unimplemented"}
   "clojure.core.protocols/kv-reduce" {:arglists '([amap f init])}
   "clojure.core.protocols/datafy"    {:arglists '([o])}
   "clojure.core.protocols/nav"       {:arglists '([coll k v])}
   "clojure.datafy/datafy"       {:arglists '([x])}
   "clojure.datafy/nav"          {:arglists '([coll k v])}})

(defn c-string [s]
  (-> s
      (str/replace "\\" "\\\\")
      (str/replace "\"" "\\\"")))

(defn oracle-vars [surface]
  (into {}
        (mapcat (fn [[ns nsdef]]
                  (map (fn [[nm info]] [(str ns "/" nm) info])
                       (:vars nsdef))))
        (:namespaces surface)))

;;;; Report mode =====================================================

;; Shapes with a documented divergence: the mino-true prim overrides
;; plus the lib-alias overrides (both carry :reason). Every other
;; shape mismatch is a genuine gap the report surfaces but no slice
;; has dispositioned.
(def documented-shape-divergences
  (into {}
        (mapcat (fn [m] (map (fn [[k v]] [k (:reason v)])
                              (filter (fn [[_ v]] (:reason v)) m))))
        [mino-true lib-aliases]))

(defn mino-bin []
  (or (System/getenv "MINO_BIN")
      (when (.exists (java.io.File. "mino")) "./mino")
      "mino"))

(defn strip-non-edn-prefix
  "Skip any text a host prints before the first EDN collection."
  [s]
  (let [idx (str/index-of s "{")]
    (if (pos? idx) (subs s idx) s)))

(def probe-form-template
  '(do
     (def probe-out (atom {}))
     (doseq [n probe-nss]
       (try (require n)
            (let [the-ns (find-ns n)]
              (when the-ns
                (swap! probe-out assoc n
                       (into {} (map (fn [e] [(key e)
                                               (:arglists (meta (val e)))])
                                     (ns-publics the-ns))))))
            (catch err
              (binding [*out* *err*]
                (println "; report probe: could not require" n)))))
     (prn @probe-out)))

(defn probe-script
  "The one-shot introspection script the binary evaluates: require
  every oracle namespace best-effort, print one EDN map of ns to
  var-name to its live :arglists (nil when bare). Diagnostics go to
  stderr only. mino's catch binds a single symbol. The body rides a
  quoted template so a paren slip breaks the tool load, not the run."
  [nss]
  (str "(def probe-nss " (pr-str (list 'quote (vec nss))) ")\n"
       (pr-str probe-form-template)))

(defn probe-current-arglists
  "Run the binary over the probe script and parse the printed EDN."
  [bin nss]
  (let [script (java.io.File.
                (str (System/getProperty "java.io.tmpdir")
                     "/mino_gen_arglists_probe.clj"))
        _      (spit script (probe-script nss))
        {:keys [exit out err]} (shell/sh bin (str script))]
    (when-not (zero? exit)
      (binding [*out* *err*]
        (println "probe failed (exit" (str exit ")") "stderr:" err))
      (System/exit 2))
    (edn/read-string (strip-non-edn-prefix out))))

(defn norm-arglists [al]
  (when al (mapv vec al)))

(defn clause-shape
  "One arglist clause as :& / :sym / :form per element; destructure
  forms collapse to :form."
  [clause]
  (mapv (fn [f] (cond (and (symbol? f) (= '& f)) :&
                      (symbol? f) :sym
                      :else :form))
        clause))

(defn pure-name-diff?
  "True when oracle and mino arglists agree on clause count, order,
  length, & placement, and every destructure form, and differ only in
  plain parameter names."
  [o m]
  (and (= (count o) (count m))
       (every? (fn [[oc mc]]
                 (and (= (count oc) (count mc))
                      (= (clause-shape oc) (clause-shape mc))
                      (every? (fn [[of mf]]
                                (or (symbol? of)
                                    (and (not (symbol? of)) (= of mf))))
                              (map vector oc mc))))
               (map vector o m))))

(defn classify-var
  [{:keys [oracle mino var-exists]}]
  (cond
    (nil? mino)                        (if var-exists :bare-var :var-absent)
    (= (norm-arglists oracle)
       (norm-arglists mino))           :exact
    (pure-name-diff? oracle mino)      :name-diff
    :else                              :shape))

(defn report-main []
  (let [surface   (edn/read-string (slurp surface-path))
        nss       (sort (keys (:namespaces surface)))
        bin       (mino-bin)
        current   (probe-current-arglists bin nss)
        rows      (for [[ns nsd] (:namespaces surface)
                        [nm info] (:vars nsd)
                        :when (:arglists info)
                        :let [inner (get current ns)]]
                    {:id         (str ns "/" nm)
                     :ns         ns
                     :oracle     (:arglists info)
                     :mino       (get inner nm)
                     :var-exists (contains? inner nm)})
        by-class  (group-by classify-var rows)
        count-of  (fn [k] (count (get by-class k [])))
        name-diff (sort-by :id (by-class :name-diff))
        shape     (sort-by :id (by-class :shape))
        documented (filter (fn [r] (contains? documented-shape-divergences (:id r))) shape)
        genuine    (remove (fn [r] (contains? documented-shape-divergences (:id r))) shape)]
    (println "mino arglists report")
    (println "  surface:" surface-path)
    (println "  binary: " bin)
    (println "  oracle vars with :arglists:" (count rows))
    (println "  exact:     " (count-of :exact))
    (println "  name-diff: " (count-of :name-diff) "(rename targets)")
    (println "  shape-diff:" (count-of :shape)
             "(" (count documented) "documented mino-true,"
             (count genuine) "genuine gaps)")
    (println "  bare-var:  " (count-of :bare-var))
    (println "  var-absent:" (count-of :var-absent))
    (when (seq name-diff)
      (println)
      (println "-- name diffs (rename params to oracle names) --")
      (doseq [r name-diff]
        (println (:id r))
        (println "  mino:  " (pr-str (:mino r)))
        (println "  oracle:" (pr-str (:oracle r)))))
    (when (seq shape)
      (println)
      (println "-- shape diffs --")
      (doseq [[label rs] [["documented mino-true" documented]
                          ["genuine gaps" genuine]]]
        (doseq [r rs]
          (println "[" label "]" (:id r))
          (println "  mino:  " (pr-str (:mino r)))
          (println "  oracle:" (pr-str (:oracle r)))
          (when-let [reason (get documented-shape-divergences (:id r))]
            (println "  reason:" reason))))
      (println))
    (when-let [absent (seq (by-class :var-absent))]
      (println "-- vars absent from mino:" (count absent) "--")
      (doseq [r (sort-by :id absent)]
        (print (:id r) " "))
      (println))
    (when-let [bare (seq (by-class :bare-var))]
      (println "-- vars present but bare:" (count bare) "--")
      (doseq [r (sort-by :id bare)]
        (println (:id r))))
    (when (pos? (count-of :name-diff))
      (binding [*out* *err*]
        (println "name diffs remain:" (count-of :name-diff)))
      (System/exit 1))
    (println "name diffs: 0")))

(defn -main []
  (let [vars    (oracle-vars (edn/read-string (slurp surface-path)))
        emitted (into emit-list (concat lax-prims (keys mino-true)))
        _       (assert (and (empty? (clojure.set/intersection emit-list lax-prims))
                              (empty? (clojure.set/intersection emit-list (set (keys mino-true))))
                              (empty? (clojure.set/intersection lax-prims (set (keys mino-true)))))
                         "emit populations must be disjoint")
        _       (assert (empty? (clojure.set/intersection
                                 (set (concat emit-list lax-prims (keys mino-true)))
                                 (set (keys lib-aliases))))
                        "lib-alias class must stay disjoint from the prim classes")
        ;; Lib-alias cross-check against the oracle: every entry must
        ;; exist there, oracle-verbatim entries must match its
        ;; arglists exactly, and overrides must carry a reason.
        _       (doseq [[k {:keys [arglists reason]}] lib-aliases]
                  (let [o (get vars k)]
                    (assert (some? o) (str "lib-alias " k " absent from the oracle"))
                    (if reason
                      (assert (not= arglists (:arglists o))
                              (str "lib-alias " k " claims divergence but matches the oracle"))
                      (assert (= arglists (:arglists o))
                              (str "lib-alias " k " drifted from the oracle shape")))))
        ;; Pair order, not composite-string order: install.c bsearches
        ;; by strcmp on ns then name, and the two orders diverge once
        ;; one namespace is a proper prefix of another.
        emit    (sort-by (fn [k] (let [[ns name] (str/split k #"/" 2)]
                                    [ns name]))
                          emitted)
        banned  (into #{} (map #(str "clojure.core/" %)) non-prims)]
    (when (some banned emit)
      (binding [*out* *err*]
        (println "emit list intersects the frozen exclusions:"
                 (pr-str (seq (filter banned emit)))))
      (System/exit 1))
    (let [missing (remove (fn [k]
                            (or (some? (:arglists (get mino-true k)))
                                (some? (:arglists (get vars k)))))
                          emit)]
      (when (seq missing)
        (binding [*out* *err*]
          (doseq [m missing] (println "no oracle arglists for" m)))
        (System/exit 1)))
    (let [rows   (map (fn [k]
                        (let [[ns name] (str/split k #"/" 2)
                              al (if-some [override (:arglists (get mino-true k))]
                                   (pr-str override)
                                   (pr-str (:arglists (get vars k))))]
                          [ns name al]))
                      emit)
          lines  (map (fn [[ns name al]]
                        (format "    {\"%s\", \"%s\", \"%s\"},"
                                (c-string ns) (c-string name) (c-string al)))
                      rows)
          header (str/join
                  "\n"
                  ["/* Generated by tools/gen_arglists.clj -- DO NOT EDIT BY HAND."
                   " * Regenerate: bb tools/gen_arglists.clj [path-to-surface.edn] */"
                   "#ifndef MINO_PRIM_ARGLISTS_DATA_H"
                   "#define MINO_PRIM_ARGLISTS_DATA_H"
                   ""
                   "typedef struct { const char *ns; const char *name; const char *arglists; } mino_prim_arglist_t;"
                   "static const mino_prim_arglist_t k_prim_arglists[] = {"
                   (str/join "\n" lines)
                   "};"
                   "#define K_PRIM_ARGLISTS_COUNT (sizeof(k_prim_arglists)/sizeof(k_prim_arglists[0]))"
                   ""
                   "#endif /* MINO_PRIM_ARGLISTS_DATA_H */"
                   ""])]
      (spit "src/prim/arglists_data.h" header)
      (println "wrote src/prim/arglists_data.h:" (count rows)
               "entries from" surface-path
               "(" (- (count rows) (count mino-true)) "oracle,"
               (count mino-true) "mino-true,"
               "0 excluded prims)")
      (println "lib-alias class (def-site metadata, not in the table):"
               (count lib-aliases) "entries"
               "(" (- (count lib-aliases)
                      (count (filter :reason (vals lib-aliases))))
               "oracle,"
               (count (filter :reason (vals lib-aliases))) "mino-true)"))))

(if report-only?
  (report-main)
  (-main))
