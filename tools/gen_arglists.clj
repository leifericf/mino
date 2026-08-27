(ns tools.gen-arglists
  (:require [clojure.edn :as edn]
            [clojure.string :as str]))

;; Regenerate src/prim/arglists_data.h, the committed install-time
;; :arglists table for the C prims, from the census oracle surface.
;; Oracle-conformant and lax prims attach the oracle shapes verbatim;
;; real arity gaps attach mino-true shapes derived from the prim
;; sources. The script runs only when the oracle or the emit lists
;; change; the Makefile never invokes it.
;;
;;   bb tools/gen_arglists.clj [path-to-surface.edn]
;;
;; Surface path resolves from the first CLI arg, else $CENSUS_SURFACE,
;; else the default census checkout below. Run from the repo root; the
;; output path is relative.

(def default-surface "/Users/leif/Code/clojure-census/clojure/1.12.4-surface.edn")

(def surface-path
  (or (first *command-line-args*)
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
;; ("ns/name") frozen from the 2026-08-27 sweep plus the ten the
;; slice-2/3 fixes admitted (ADR 34); regenerate via the sweep.
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

   "clojure.core/ref"
   {:arglists '([x])
    :reason "prim_ref requires an initial value; canonical keyword option pairs are accepted (sweep sentinels MAR001 as bad option keys), so only [x] is claimed"}

   "clojure.core/resolve"
   {:arglists '([sym])
    :reason "prim_resolve accepts exactly one symbol; the env-arity is unimplemented"}

   "clojure.core/slurp"
   {:arglists '([f])
    :reason "prim_slurp accepts exactly one path argument; the opts tail is unimplemented"}

   "clojure.core/with-bindings*"
   {:arglists '([binding-map f])
    :reason "prim_with_bindings_star accepts exactly (binding-map f); the variadic args tail is unimplemented"}})

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

(defn -main []
  (let [vars    (oracle-vars (edn/read-string (slurp surface-path)))
        emitted (into emit-list (concat lax-prims (keys mino-true)))
        _       (assert (and (empty? (clojure.set/intersection emit-list lax-prims))
                             (empty? (clojure.set/intersection emit-list (set (keys mino-true))))
                             (empty? (clojure.set/intersection lax-prims (set (keys mino-true)))))
                        "emit populations must be disjoint")
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
               "0 excluded prims)"))))

(-main)
