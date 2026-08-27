(ns tools.gen-arglists
  (:require [clojure.edn :as edn]
            [clojure.string :as str]))

;; Regenerate src/prim/arglists_data.h, the committed install-time
;; :arglists table for the arity-conformant C prims, from the census
;; oracle surface. The script runs only when the oracle or the emit
;; list changes; the Makefile never invokes it.
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

;; Divergent-arity prims, frozen from the 2026-08-27 arity sweep;
;; regenerate via the sweep before extending. Their arities or error
;; classes are fixed by later commits in the campaign.
(def divergent-arities
  #{
  "clojure.core/<"
  "clojure.core/<="
  "clojure.core/="
  "clojure.core/>"
  "clojure.core/>="
  "clojure.core/agent"
  "clojure.core/aget"
  "clojure.core/all-ns"
  "clojure.core/aset"
  "clojure.core/assoc"
  "clojure.core/atom"
  "clojure.core/bit-and"
  "clojure.core/bit-or"
  "clojure.core/bit-xor"
  "clojure.core/byte-array"
  "clojure.core/conj!"
  "clojure.core/disj!"
  "clojure.core/distinct?"
  "clojure.core/get-thread-bindings"
  "clojure.core/hash-map"
  "clojure.core/identical?"
  "clojure.core/loaded-libs"
  "clojure.core/object-array"
  "clojure.core/realized?"
  "clojure.core/ref"
  "clojure.core/ref-history-count"
  "clojure.core/ref-max-history"
  "clojure.core/ref-min-history"
  "clojure.core/release-pending-sends"
  "clojure.core/require"
  "clojure.core/resolve"
  "clojure.core/restart-agent"
  "clojure.core/send-via"
  "clojure.core/slurp"
  "clojure.core/sorted-map"
  "clojure.core/sorted-map-by"
  "clojure.core/spit"
  "clojure.core/symbol"
  "clojure.core/to-array"
  "clojure.core/use"
  "clojure.core/with-bindings*"
  "clojure.core/with-meta"    })

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

;; The authoritative emit list for commit 1: 253 arity-conformant
;; prim names ("ns/name"), frozen from the 2026-08-27 arity sweep;
;; regenerate via the sweep before extending. The oracle lookup
;; supplies each name's arglists text.
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
  "clojure.core/agent-error"
  "clojure.core/alength"
  "clojure.core/alias"
  "clojure.core/alter"
  "clojure.core/alter-meta!"
  "clojure.core/alter-var-root"
  "clojure.core/apply"
  "clojure.core/assoc!"
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
  "clojure.core/requiring-resolve"
  "clojure.core/reset!"
  "clojure.core/reset-vals!"
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
        ;; Pair order, not composite-string order: install.c bsearches
        ;; by strcmp on ns then name, and the two orders diverge once
        ;; one namespace is a proper prefix of another.
        emit    (sort-by (fn [k] (let [[ns name] (str/split k #"/" 2)]
                                   [ns name]))
                         emit-list)
        banned  (into divergent-arities
                      (map #(str "clojure.core/" %))
                      non-prims)]
    (when (some banned emit)
      (binding [*out* *err*]
        (println "emit list intersects the frozen exclusions:"
                 (pr-str (seq (filter banned emit)))))
      (System/exit 1))
    (let [missing (remove #(some? (:arglists (get vars %))) emit)]
      (when (seq missing)
        (binding [*out* *err*]
          (doseq [m missing] (println "no oracle arglists for" m)))
        (System/exit 1)))
    (let [rows   (map (fn [k]
                        (let [[ns name] (str/split k #"/" 2)]
                          [ns name (pr-str (:arglists (get vars k)))]))
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
               "entries from" surface-path))))

(-main)
