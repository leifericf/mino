(ns mino.tasks.builtin)

(require '[clojure.string :as str])
(require '[clojure.set    :as set])

;; Build configuration -- matches Makefile defaults.

(def ^:private cc      (or (getenv "CC") "cc"))
(def ^:private include-flags
  (str "-Isrc -Isrc/public -Isrc/runtime -Isrc/gc -Isrc/eval"
       " -Isrc/values -Isrc/collections -Isrc/prim -Isrc/async"
       " -Isrc/interop -Isrc/diag -Isrc/vendor/imath"))
(def ^:private cflags  (str/split (or (getenv "CFLAGS")
                                  (str "-std=c99 -Wall -Wpedantic -Wextra -O2 "
                                       "-DMINO_CPJIT=1 "
                                       include-flags)) " "))
(def ^:private ldflags (let [v (or (getenv "LDFLAGS") "")]
                         (if (= v "") [] (str/split v " "))))
(def ^:private windows? (some? (getenv "OS")))
(def ^:private libs    (str/split (or (getenv "LIBS")
                                       (if windows? "-lm" "-lm -lpthread")) " "))
(def ^:private mino-bin (if windows? "mino.exe" "./mino"))

(def ^:private lib-srcs
  ["src/eval/eval.c" "src/diag/diag.c" "src/eval/special.c"
   "src/eval/special_registry.c"
   "src/eval/defs.c" "src/eval/bindings.c"
   "src/eval/control.c" "src/eval/fn.c"
   "src/eval/bc/vm.c" "src/eval/bc/compile.c"
   "src/eval/bc/gc_handlers.c"
   "src/eval/bc/jit/entry.c" "src/eval/bc/jit/stats.c"
   "src/eval/bc/jit/helpers.c" "src/eval/bc/jit/patcher.c"
   "src/eval/bc/jit/patcher_x86_64.c"
   "src/eval/bc/jit/emit.c"
   "src/runtime/state.c" "src/runtime/var.c"
   "src/runtime/error.c" "src/runtime/env.c"
   "src/runtime/ns_env.c"
   "src/runtime/path_buf.c"
   "src/runtime/host_threads.c"
   "src/runtime/capabilities.c"
   "src/gc/driver.c" "src/gc/roots.c" "src/gc/major.c"
   "src/gc/barrier.c" "src/gc/minor.c"
   "src/gc/trace.c" "src/gc/profile.c" "src/runtime/module.c"
   "src/public/gc.c" "src/public/embed.c"
   "src/values/val.c" "src/values/gc_handlers.c"
   "src/collections/vec.c" "src/collections/map.c"
   "src/collections/chunk.c"
   "src/collections/rbtree.c"
   "src/collections/builders.c"
   "src/collections/gc_handlers.c"
   "src/collections/iter.c" "src/eval/read.c" "src/eval/print.c"
   "src/prim/prim.c" "src/prim/install.c" "src/prim/install_stdlib.c"
   "src/prim/numeric.c" "src/prim/collections.c"
   "src/prim/sequences.c" "src/prim/lazy.c"
   "src/prim/string.c" "src/prim/io.c"
   "src/prim/reflection.c" "src/prim/meta.c" "src/prim/regex.c"
   "src/prim/stateful.c" "src/prim/stm.c" "src/prim/agent.c" "src/prim/module.c"
   "src/prim/ns.c"
   "src/prim/fs.c" "src/prim/proc.c"
   "src/prim/host.c" "src/interop/syntax.c"
   "src/collections/clone.c" "src/regex/re.c" "src/collections/transient.c"
   "src/async/scheduler.c" "src/async/timer.c" "src/prim/async.c"
   "src/prim/bignum.c" "src/prim/ratio.c" "src/prim/bigdec.c"
   "src/vendor/imath/imath.c"])

(def ^:private all-srcs (conj lib-srcs "main.c"))

(defn- src->obj [src]
  (str (subs src 0 (- (count src) 2)) ".o"))

(defn- src->dep [src]
  (str (subs src 0 (- (count src) 2)) ".d"))

(defn- stale?
  "True if output does not exist or any input is newer."
  [inputs output]
  (let [out-mtime (file-mtime output)]
    (if (nil? out-mtime)
      true
      (some #(let [in-mtime (file-mtime %)]
               (and in-mtime (> in-mtime out-mtime)))
            inputs))))

(defn- read-depfile
  "Parse a .d file into a vector of dependency paths.
   Format: target: dep1 dep2 \\ dep3 dep4"
  [path]
  (when (file-exists? path)
    (let [content (slurp path)
          ;; Remove backslash-newline continuations and trailing whitespace
          flat    (-> content
                      (str/replace "\\\n" " ")
                      (str/replace "\n" " ")
                      str/trim)
          ;; Split on whitespace, skip target (first token ending in :)
          tokens  (filter #(not (= "" %)) (str/split flat " "))
          deps    (rest tokens)]
      (vec (filter #(not (= "\\" %)) deps)))))

;; ---- Tasks ----

(defn- escape-source-as-c-string-literal
  "Returns the body of a C string literal: backslash-escaped quotes
   and backslashes, with newlines turned into \\n\" + indent + \" so
   the literal stays readable when printed."
  [src]
  (let [src     (if (str/ends-with? src "\n")
                  (subs src 0 (- (count src) 1))
                  src)
        escaped (-> src
                    (str/replace "\\" "\\\\")
                    (str/replace "\"" "\\\""))]
    (str/replace escaped "\n" "\\n\"\n    \"")))

(defn gen-core-header
  "Escape src/core.clj into src/core_mino.h as a C string literal."
  []
  (when (stale? ["src/core.clj"] "src/core_mino.h")
    (let [body (escape-source-as-c-string-literal (slurp "src/core.clj"))]
      (spit "src/core_mino.h"
            (str "/* AUTO-GENERATED -- DO NOT EDIT.\n"
                 " *\n"
                 " * Produced by `gen-core-header` from src/core.clj.\n"
                 " * Embeds the bundled mino-side core library as a C\n"
                 " * string literal so the runtime can install it without\n"
                 " * needing core.clj on disk at startup.\n"
                 " *\n"
                 " * Edit src/core.clj, then `./mino task build` (which\n"
                 " * regenerates this file). Gitignored.\n"
                 " */\n"
                 "static const char *core_mino_src =\n    \""
                 body "\\n\"\n    ;\n")))
    (println "gen-core-header: src/core_mino.h updated")))

;; The bundled-stdlib namespace set baked into the binary alongside
;; src/core.clj. Each entry is [src-path ns-name c-symbol], where
;; c-symbol is the basis for the generated header file name and the
;; static-source variable.  Test-fixture .clj files under lib/clojure/
;; (e.g. lib/clojure/test_clojure/, lib/clojure/core_test/) are not
;; bundled -- they exist on disk so the require/resolve test surface
;; can verify file-loading behaviour.
(def ^:private bundled-stdlib
  [["lib/clojure/string.clj"          "clojure.string"          "lib_clojure_string"]
   ["lib/clojure/set.clj"             "clojure.set"             "lib_clojure_set"]
   ["lib/clojure/walk.clj"            "clojure.walk"            "lib_clojure_walk"]
   ["lib/clojure/edn.clj"             "clojure.edn"             "lib_clojure_edn"]
   ["lib/clojure/pprint.clj"          "clojure.pprint"          "lib_clojure_pprint"]
   ["lib/clojure/zip.clj"             "clojure.zip"             "lib_clojure_zip"]
   ["lib/clojure/data.clj"            "clojure.data"            "lib_clojure_data"]
   ["lib/clojure/test.clj"            "clojure.test"            "lib_clojure_test"]
   ["lib/clojure/template.clj"        "clojure.template"        "lib_clojure_template"]
   ["lib/clojure/repl.clj"            "clojure.repl"            "lib_clojure_repl"]
   ["lib/clojure/stacktrace.clj"      "clojure.stacktrace"      "lib_clojure_stacktrace"]
   ["lib/clojure/datafy.clj"          "clojure.datafy"          "lib_clojure_datafy"]
   ["lib/clojure/core/protocols.clj"  "clojure.core.protocols"  "lib_clojure_core_protocols"]
   ["lib/clojure/instant.clj"         "clojure.instant"         "lib_clojure_instant"]
   ["lib/clojure/spec/alpha.clj"      "clojure.spec.alpha"      "lib_clojure_spec_alpha"]
   ["lib/clojure/core/specs/alpha.clj" "clojure.core.specs.alpha" "lib_clojure_core_specs_alpha"]
   ["lib/clojure/test/check/generators.clj" "clojure.test.check.generators" "lib_clojure_test_check_generators"]
   ["lib/clojure/test/check/properties.clj" "clojure.test.check.properties" "lib_clojure_test_check_properties"]
   ["lib/clojure/test/check.clj"      "clojure.test.check"      "lib_clojure_test_check"]
   ["lib/mino/deps.clj"               "mino.deps"               "lib_mino_deps"]
   ["lib/mino/tasks.clj"              "mino.tasks"              "lib_mino_tasks"]
   ["lib/mino/tasks/builtin.clj"      "mino.tasks.builtin"      "lib_mino_tasks_builtin"]])

(defn- regen-stdlib-header
  "Regenerates one bundled-stdlib header if its source is newer.
   Returns 1 when the header was rewritten, 0 otherwise."
  [[src-path ns-name c-symbol]]
  (let [out-path (str "src/" c-symbol ".h")]
    (if (stale? [src-path] out-path)
      (do (spit out-path
                (str "/* AUTO-GENERATED -- DO NOT EDIT.\n"
                     " *\n"
                     " * Produced by `gen-stdlib-headers` from " src-path ".\n"
                     " * Embeds the bundled mino-side " ns-name " namespace\n"
                     " * source as a C string literal so the runtime can\n"
                     " * register it without needing the file on disk.\n"
                     " *\n"
                     " * Edit " src-path ", then `./mino task build`\n"
                     " * (which regenerates this file). Gitignored.\n"
                     " */\n"
                     "static const char *" c-symbol "_src =\n    \""
                     (escape-source-as-c-string-literal (slurp src-path))
                     "\\n\"\n    ;\n"))
          1)
      0)))

(defn gen-stdlib-headers
  "Escape each lib/clojure/*.clj listed in bundled-stdlib into its
   own src/<symbol>.h header, parallel to gen-core-header. Each file
   defines a single static const char *<symbol>_src so the bundled
   per-namespace install hooks can register the source pointer
   without touching the disk."
  []
  (let [updated (reduce + 0 (map regen-stdlib-header bundled-stdlib))]
    (when (> updated 0)
      (println (str "gen-stdlib-headers: " updated " header(s) updated")))))

(defn build
  "Compile all .c sources and link the mino binary."
  []
  (let [compiled (atom 0)]
    ;; Compile stale .c -> .o (uses .d depfiles for header tracking)
    (doseq [src all-srcs]
      (let [obj  (src->obj src)
            dep  (src->dep src)
            deps (or (read-depfile dep) [src])]
        (when (stale? deps obj)
          (let [args (into [cc] (concat cflags ["-MMD" "-c" "-o" obj src]))]
            (println (str "  " (str/join " " args)))
            (apply sh! args)
            (swap! compiled inc)))))
    ;; Link if anything was compiled or the binary is missing.
    ;; On Windows, gcc -o mino produces mino.exe, so check both names.
    (let [objs      (mapv src->obj all-srcs)
          bin-exists (or (file-exists? "mino") (file-exists? "mino.exe"))
          need-link  (or (> @compiled 0) (not bin-exists))]
      (when need-link
        (let [args (into [cc] (concat cflags ldflags ["-o" "mino"] objs libs))]
          (println (str "  " (str/join " " args)))
          (apply sh! args)))
      (when (and (= @compiled 0) (not need-link))
        (println "  nothing to compile")))))

(defn clean
  "Remove object files, dep files, binary, and generated header."
  []
  (doseq [src all-srcs]
    (let [obj (src->obj src)
          dep (src->dep src)]
      (when (file-exists? obj) (rm-rf obj))
      (when (file-exists? dep) (rm-rf dep))))
  (when (file-exists? "mino") (rm-rf "mino"))
  (when (file-exists? "mino.exe") (rm-rf "mino.exe"))
  (when (file-exists? "mino_asan")  (rm-rf "mino_asan"))
  (when (file-exists? "mino_ubsan") (rm-rf "mino_ubsan"))
  (when (file-exists? "mino_tsan")  (rm-rf "mino_tsan"))
  (when (file-exists? "src/core_mino.h")
    (rm-rf "src/core_mino.h"))
  (doseq [[_ _ c-symbol] bundled-stdlib]
    (let [hpath (str "src/" c-symbol ".h")]
      (when (file-exists? hpath) (rm-rf hpath))))
  (println "  cleaned"))

;; ---- Sanitizer dev builds -----------------------------------------------
;;
;; Each sanitizer build links every source directly into a dedicated
;; binary. Sanitizer flags change code generation (redzones, shadow
;; memory, hooks) so we do not share objects with the regular build.
;; Binary names are suffixed so all three can coexist in the working
;; tree, e.g. ./mino_asan, ./mino_ubsan, ./mino_tsan.

(def ^:private san-base
  (into ["-g" "-O1" "-fno-omit-frame-pointer" "-std=c99"
         "-Wall" "-Wextra" "-Wpedantic"]
        (str/split include-flags " ")))

(defn- build-sanitized [label out-bin extra-flags]
  (gen-core-header)
  (let [args (into [cc]
                   (concat san-base
                           extra-flags
                           ldflags
                           ["-o" out-bin]
                           all-srcs
                           libs))]
    (println (str "  " (str/join " " args)))
    (apply sh! args)
    (println (str "  " label " build -> " out-bin))))

(defn build-asan
  "Build mino_asan with AddressSanitizer. Catches heap buffer overflows,
   use-after-free, stack overflows, and most memory errors. Slower than
   a release build; intended for development only."
  []
  (build-sanitized "asan" "mino_asan"
                   ["-fsanitize=address"]))

(defn build-ubsan
  "Build mino_ubsan with UndefinedBehaviorSanitizer. Catches signed
   overflow, misaligned loads, invalid shifts, and similar UB. The
   -fno-sanitize-recover switch makes the first finding fatal so it
   surfaces under a test runner that checks exit status."
  []
  (build-sanitized "ubsan" "mino_ubsan"
                   ["-fsanitize=undefined"
                    "-fno-sanitize-recover=undefined"]))

(defn build-tsan
  "Build mino_tsan with ThreadSanitizer. Only meaningful for the
   multi-state embedding tests; a single mino_state is single-threaded.
   Intended for running tests/embed_multi_state.c and similar harnesses."
  []
  (build-sanitized "tsan" "mino_tsan"
                   ["-fsanitize=thread"]))

(defn build-lean
  "Build mino-lean with -DMINO_CPJIT=1 stripped. Every call goes
   through the bytecode interpreter; the JIT pipeline compiles to a
   no-op stub. mino-lean is the distributable lean artifact: smaller
   static footprint, faster cold start, no JIT region allocation.
   Also drives test-jit-parity to confirm the inlined stencils
   produce results indistinguishable from the interpreter's matching
   OP_*_II / OP_*_IK / OP_INC_I / etc. handlers."
  []
  (gen-core-header)
  (gen-stdlib-headers)
  (let [lean-cflags (filterv #(not= % "-DMINO_CPJIT=1") cflags)
        args (into [cc]
                   (concat lean-cflags
                           ldflags
                           ["-o" "mino-lean"]
                           all-srcs
                           libs))]
    (println (str "  " (str/join " " args)))
    (apply sh! args)
    (println "  lean build -> mino-lean")))

(def ^:private jit-disabled-warning-prefix
  "mino: note: this build has the JIT compiled out")

(defn- strip-jit-disabled-warning [s]
  ;; mino prints a one-line stderr note when --jit=on or MINO_JIT=on is
  ;; requested on a build/host where the JIT isn't available (e.g. all
  ;; non-arm64-darwin matrix entries: arm64-linux, x86_64-linux,
  ;; x86_64-windows). sh captures stderr via 2>&1, so the note lands in
  ;; the variant's :out and breaks the byte-identity diff against
  ;; jit-auto (which doesn't trip the warning). The note is informational
  ;; and not part of the parity contract -- jit-parity is about the
  ;; runtime evaluator's stdout, not about which CLI flags this build
  ;; honors. Strip it before comparing.
  ;;
  ;; Implemented line-by-line because mino's clojure.string/replace
  ;; requires a string match; regex matching is unavailable on this path.
  (if (nil? s)
    s
    (str/join "\n"
              (remove (fn [line]
                        (str/starts-with? line jit-disabled-warning-prefix))
                      (str/split s "\n")))))

(defn- run-parity-variant [bin-args label parity-test]
  (let [result (apply sh (concat bin-args [parity-test]))]
    {:label label
     :out   (strip-jit-disabled-warning (get result :out))
     :exit  (get result :exit)}))

(defn test-jit-parity
  "Build ./mino and ./mino-lean, run tests/jit_parity_test.clj
   against four variants -- AUTO / ON / OFF on the full binary plus
   the lean binary -- and assert every variant's stdout bytes are
   byte-identical. The parity test pins ~40 literal-expected-value
   assertions covering range boundaries, tag-miss, comparison
   identity, and unary boundaries across the 16 inlined arith /
   cmp / unary stencils. Any divergence in either binary's output
   -- a different diagnostic, a different boxed-int representation,
   a missed coercion -- surfaces in the diff.

   The four variants are explicit so a regression in any mode is
   localised: AUTO is the warm-then-JIT path that release-gate
   exercises, ON forces eager compile (catches a JIT'd stencil
   that diverges from the interpreter), OFF inhibits compilation
   entirely (catches a runtime gating bug), and the lean binary
   has no JIT path at all (catches an embed-API drift between
   the two builds).

   Uses `sh` (not `sh!`) so a non-zero exit from any runner reports
   as a parity failure rather than crashing the task."
  []
  (build)
  (build-lean)
  (let [parity-test "tests/jit_parity_test.clj"
        variants    [(run-parity-variant [mino-bin "--jit=auto"]
                                          "jit-auto"  parity-test)
                     (run-parity-variant [mino-bin "--jit=on"]
                                          "jit-on"    parity-test)
                     (run-parity-variant [mino-bin "--jit=off"]
                                          "jit-off"   parity-test)
                     (run-parity-variant ["./mino-lean"]
                                          "lean"      parity-test)]
        baseline   (first variants)
        all-match? (every? (fn [v]
                             (and (= (:out v)  (:out  baseline))
                                  (= (:exit v) (:exit baseline))
                                  (= 0 (:exit v))))
                           variants)]
    (cond
      all-match?
      (do (println (:out baseline))
          (println (str "  jit-parity: OK -- stdout byte-identical across "
                        "jit-auto / jit-on / jit-off / lean, all exit 0")))

      :else
      (do (doseq [v variants]
            (spit (str "jit-parity-" (:label v) ".out") (:out v)))
          (println "  jit-parity: FAIL")
          (doseq [v variants]
            (println (str "    " (:label v) " exit=" (:exit v))))
          (println "    wrote jit-parity-<label>.out per variant")
          (doseq [v (rest variants)]
            (when (not= (:out v) (:out baseline))
              (println (str "  --- diff " (:label baseline)
                            " vs " (:label v) " ---"))
              (println (get (sh "diff"
                                 (str "jit-parity-" (:label baseline) ".out")
                                 (str "jit-parity-" (:label v) ".out"))
                            :out))))
          (throw (ex-info "jit-parity failure"
                          {:status   :differ
                           :variants (mapv (juxt :label :exit) variants)}))))))

;; ---- Release guardrails (G1 / G2 / G3 + composite release-gate) -----
;;
;; Three checks that fire before a tag-cutting commit lands. Each is
;; an independent task; release-gate composes them with fail-fast +
;; the standard test-suite / test-suite-asan / test-jit-parity steps
;; so the gate has one human entry point and a single binary outcome.
;;
;; The verbal "kept in sync" comments these guardrails replace are
;; still in the source for reader context, but the build-time
;; contracts now live in code paths a human can re-invoke and a CI
;; runner can hook.

(defn check-stencils-fresh
  "G1: re-run gen-stencils, then `git diff --exit-code` against the
   generated stencil header directory. Non-zero exit on stale
   committed bytes -- somebody edited a stencil source without
   regenerating the byte table. Failure mode is recoverable: run
   `./mino task gen-stencils`, commit the resulting diff."
  []
  (gen-stencils)
  (let [d (sh "git" "diff" "--exit-code"
              "--" "src/eval/bc/stencils/generated/")]
    (when-not (= 0 (get d :exit))
      (println (get d :out))
      (throw (ex-info "check-stencils-fresh: generated header is stale"
                      {:exit (get d :exit)
                       :fix  "run `./mino task gen-stencils` and commit"}))))
  (println "  check-stencils-fresh: OK"))

(defn check-stencil-registry
  "G2: cross-check the hardcoded stencil list in gen-stencils against
   the actual *.c files under src/eval/bc/stencils/. Catches drift
   in either direction:

     - a new stencil source landed without an entry in the registry
       (build still works for the existing entries but the new op
       has no byte table on the next gen-stencils run),
     - an entry references a source file that no longer exists
       (gen-stencils fails opaquely mid-loop).

   Also verifies each pair's symbol-name has the `stencil_op_<basename>`
   prefix, where <basename> is the source file's stem. Sources whose
   basename starts with `__proto_` are skipped on both sides -- that
   prefix is reserved for the op-fusion prototype branch's throwaway
   stencils."
  []
  (let [;; Mirror of the hardcoded list in gen-stencils. Kept in lock-
        ;; step with that list -- this is the very drift G2 detects.
        registry  [["return.c"           "stencil_op_return_arg0"]
                   ["return.c"           "stencil_op_return_imm"]
                   ["move.c"             "stencil_op_move"]
                   ["load_k.c"           "stencil_op_load_k"]
                   ["load_k_return.c"    "stencil_op_load_k_return"]
                   ["add_ii.c"           "stencil_op_add_ii"]
                   ["sub_ii.c"           "stencil_op_sub_ii"]
                   ["mul_ii.c"           "stencil_op_mul_ii"]
                   ["lt_ii.c"            "stencil_op_lt_ii"]
                   ["le_ii.c"            "stencil_op_le_ii"]
                   ["gt_ii.c"            "stencil_op_gt_ii"]
                   ["ge_ii.c"            "stencil_op_ge_ii"]
                   ["eq_ii.c"            "stencil_op_eq_ii"]
                   ["inc_i.c"            "stencil_op_inc_i"]
                   ["dec_i.c"            "stencil_op_dec_i"]
                   ["zero_int_p.c"       "stencil_op_zero_int_p"]
                   ["add_ik.c"           "stencil_op_add_ik"]
                   ["sub_ik.c"           "stencil_op_sub_ik"]
                   ["lt_ik.c"            "stencil_op_lt_ik"]
                   ["le_ik.c"            "stencil_op_le_ik"]
                   ["eq_ik.c"            "stencil_op_eq_ik"]
                   ["mod_ii.c"           "stencil_op_mod_ii"]
                   ["quot_ii.c"          "stencil_op_quot_ii"]
                   ["rem_ii.c"           "stencil_op_rem_ii"]
                   ["band_ii.c"          "stencil_op_band_ii"]
                   ["bor_ii.c"           "stencil_op_bor_ii"]
                   ["bxor_ii.c"          "stencil_op_bxor_ii"]
                   ["shl_ii.c"           "stencil_op_shl_ii"]
                   ["shr_ii.c"           "stencil_op_shr_ii"]
                   ["ushr_ii.c"          "stencil_op_ushr_ii"]
                   ["pos_p_i.c"          "stencil_op_pos_p_i"]
                   ["neg_p_i.c"          "stencil_op_neg_p_i"]
                   ["even_p_i.c"         "stencil_op_even_p_i"]
                   ["odd_p_i.c"          "stencil_op_odd_p_i"]
                   ["bnot_i.c"           "stencil_op_bnot_i"]
                   ["loop_int_lt.c"      "stencil_op_loop_int_lt"]
                   ["loop_int_dec.c"     "stencil_op_loop_int_dec"]
                   ["loop_int_lt_inc.c"  "stencil_op_loop_int_lt_inc"]
                   ["loop_int_dec_inc.c" "stencil_op_loop_int_dec_inc"]
                   ["loop_int_lt_acc.c"  "stencil_op_loop_int_lt_acc"]
                   ["loop_int_dec_acc.c" "stencil_op_loop_int_dec_acc"]
                   ["getglobal_cached.c" "stencil_op_getglobal_cached"]
                   ["call_cached.c"      "stencil_op_call_cached"]
                   ["protocol_call_cached.c"
                    "stencil_op_protocol_call_cached"]
                   ["protocol_tailcall_cached.c"
                    "stencil_op_protocol_tailcall_cached"]
                   ["call.c"             "stencil_op_call"]
                   ["tailcall.c"         "stencil_op_tailcall"]
                   ["closure.c"          "stencil_op_closure"]
                   ["make_lazy.c"        "stencil_op_make_lazy"]
                   ["push_env.c"         "stencil_op_push_env"]
                   ["pop_env.c"          "stencil_op_pop_env"]
                   ["env_bind.c"         "stencil_op_env_bind"]
                   ["nth_vec.c"          "stencil_op_nth_vec"]
                   ["first_vec.c"        "stencil_op_first_vec"]
                   ["count_vec.c"        "stencil_op_count_vec"]
                   ["empty_vec.c"        "stencil_op_empty_vec"]
                   ["get_kw_map.c"       "stencil_op_get_kw_map"]
                   ["conj_vec.c"         "stencil_op_conj_vec"]
                   ["assoc.c"            "stencil_op_assoc"]
                   ["assoc_bang.c"       "stencil_op_assoc_bang"]
                   ["conj_bang.c"        "stencil_op_conj_bang"]
                   ["dissoc_bang.c"      "stencil_op_dissoc_bang"]
                   ["disj_bang.c"        "stencil_op_disj_bang"]
                   ["dissoc.c"           "stencil_op_dissoc"]
                   ["deopt_to_interp.c"  "stencil_op_deopt_to_interp"]]
        stencil-dir "src/eval/bc/stencils"
        disk-files  (->> (file-seq stencil-dir)
                         (filterv #(str/ends-with? % ".c"))
                         (mapv #(subs % (inc (count stencil-dir))))
                         ;; Skip __proto_-prefixed sources (op-fusion
                         ;; prototype slot).
                         (filterv #(not (str/starts-with? % "__proto_")))
                         set)
        registry-files (set (map first registry))
        missing-from-disk (sort (set/difference registry-files
                                                       disk-files))
        missing-from-reg  (sort (set/difference disk-files
                                                       registry-files))
        bad-prefix        (filterv (fn [[file sym]]
                                     (let [base (subs file 0
                                                      (- (count file) 2))]
                                       (not (str/starts-with?
                                              sym
                                              (str "stencil_op_" base)))))
                                   registry)]
    (cond
      (seq missing-from-disk)
      (throw (ex-info (str "check-stencil-registry: registry references "
                           "missing source files: " missing-from-disk)
                      {:missing missing-from-disk}))

      (seq missing-from-reg)
      (throw (ex-info (str "check-stencil-registry: source files have "
                           "no registry entry: " missing-from-reg)
                      {:orphan missing-from-reg}))

      (seq bad-prefix)
      (throw (ex-info (str "check-stencil-registry: sym does not match "
                           "stencil_op_<basename> prefix: " bad-prefix)
                      {:offenders bad-prefix}))

      :else
      (println "  check-stencil-registry: OK"))))

(defn- parse-reloc-defines
  "Scan `path` for `#define MINO_STENCIL_RELOC_<NAME> <int>u` lines and
   return a map of name -> int. The runtime / extractor each carry
   their own copy of this enum; G3 cross-checks them by parsing
   both and asserting the maps match."
  [path]
  (let [pat #"#define\s+(MINO_STENCIL_RELOC_[A-Z_0-9]+)\s+(\d+)u?"]
    (reduce
      (fn [acc line]
        (if-let [m (re-find pat line)]
          (assoc acc (nth m 1) (parse-long (nth m 2)))
          acc))
      {}
      (str/split-lines (slurp path)))))

(defn check-reloc-mirror
  "G3: pin the MINO_STENCIL_RELOC_* enum mirror across the two files
   that carry it (src/eval/bc/jit/internal.h on the runtime side,
   tools/stencil_extract/core.h on the toolchain side), and run
   tools/stencil-extract --selftest for the extractor's internal
   consistency. The selftest catches drift within the extractor
   between its enum values and the per-format reloc-kind maps; the
   cross-file parse catches drift between the two enums independent
   of each file's internal consistency. Promotes the verbal `kept in
   sync` comment that lived in jit.c pre-v0.216 into a build-time
   contract."
  []
  (build-stencil-extract)
  (let [runtime-defs   (parse-reloc-defines "src/eval/bc/jit/internal.h")
        extractor-defs (parse-reloc-defines "tools/stencil_extract/core.h")
        ;; The extractor declares one extra symbol per non-ARM64 arch
        ;; (e.g., MINO_STENCIL_RELOC_X86_64_*) the runtime side does
        ;; not need to know until the corresponding stencil header
        ;; lands. Compare only the shared keys.
        shared-keys    (set/intersection (set (keys runtime-defs))
                                         (set (keys extractor-defs)))
        mismatches     (filterv (fn [k]
                                  (not= (get runtime-defs k)
                                        (get extractor-defs k)))
                                shared-keys)
        missing-rt     (sort (set/difference (set (keys extractor-defs))
                                             shared-keys))
        missing-tool   (sort (set/difference (set (keys runtime-defs))
                                             shared-keys))]
    (when (seq mismatches)
      (doseq [k mismatches]
        (println (str "    " k
                      "  runtime=" (get runtime-defs k)
                      "  extractor=" (get extractor-defs k))))
      (throw (ex-info "check-reloc-mirror: MINO_STENCIL_RELOC_* value mismatch"
                      {:mismatches mismatches})))
    (when (seq missing-tool)
      (throw (ex-info (str "check-reloc-mirror: runtime declares names "
                           "the extractor does not: " missing-tool)
                      {:missing-tool missing-tool}))))
  (let [r (sh "./tools/stencil-extract" "--selftest")]
    (when-not (= 0 (get r :exit))
      (println (get r :out))
      (throw (ex-info "check-reloc-mirror: stencil-extract selftest failed"
                      {:exit (get r :exit)}))))
  (println "  check-reloc-mirror: OK"))

(defn- mino-tests-adjacent
  "Path to a sibling mino-tests clone, or nil if not present.
   The release-gate composite uses this to chain into the satellite
   suite's smoke when both repos are checked out side-by-side."
  []
  (let [candidates ["../mino-tests" "../../mino-tests"]]
    (some (fn [p] (when (file-exists? (str p "/mino.edn")) p))
          candidates)))

(defn- mino-bench-adjacent
  "Path to a sibling mino-bench clone, or nil if not present.
   perf-gate chains into mino-bench's perf_gate.clj when the
   sibling is checked out side-by-side."
  []
  (let [candidates ["../mino-bench" "../../mino-bench"]]
    (some (fn [p]
            (when (file-exists? (str p "/benchmarks/perf_gate.clj"))
              p))
          candidates)))

(defn jit-blocker-report
  "Run a small benchmark corpus through `MINO_CPJIT_STATS=tracing`
   and write the bytes-blocked-by-op table verbatim to
   `.local/jit-blockers-latest.md`. Periodic snapshot artifact for
   the JIT cycles -- the diff between dashboards (rename the file
   after each run for date-stamped history) shows whether opcode
   coverage work is reducing the blocker surface area.

   No-op when mino-bench is not adjacent."
  []
  (if-let [mb (mino-bench-adjacent)]
    (let [out     ".local/jit-blockers-latest.md"
          tmp-err "/tmp/mino-jit-blockers.stderr"]
      (sh! "sh" "-c"
           (str "cd " mb " && "
                "MINO_CPJIT_STATS=tracing "
                (getenv "PWD") "/mino "
                "benchmarks/realistic_bench.clj "
                "2> " tmp-err " > /dev/null"))
      (let [stderr (try (slurp tmp-err) (catch _ ""))
            ;; Extract the bytes-blocked block: from the marker line
            ;; to the next blank line (or end of file).
            in-section (atom false)
            kept       (atom [])]
        (doseq [l (str/split-lines stderr)]
          (cond
            (str/includes? l "bytes-blocked by op") (reset! in-section true)
            (and @in-section (str/blank? l))         (reset! in-section false)
            @in-section                               (swap! kept conj l)))
        (let [body (apply str (mapv (fn [l] (str "  " l "\n")) @kept))]
          (spit out
                (str "# JIT blocker dashboard (latest)\n\n"
                     "Generated by `./mino task jit-blocker-report` against\n"
                     "`mino-bench/benchmarks/realistic_bench.clj`. Bytes-blocked\n"
                     "is the cumulative bytecode body size of fns rejected with\n"
                     "the named op as `first_unknown_op` at JIT-eligibility time.\n\n"
                     (if (empty? @kept)
                       "No fns rejected on this corpus (all ops covered).\n"
                       (str "## Top blockers\n\n```\n" body "```\n\n"))
                     "## Source\n\n"
                     "Re-run: `./mino task jit-blocker-report`\n"))
          (println (str "  jit-blocker-report: wrote " out)))))
    (println "  jit-blocker-report: mino-bench not adjacent -- skipped")))

(defn perf-gate
  "Run mino-bench/benchmarks/perf_gate.clj against the pinned
   baseline. Fails on any timing regression past the gate's
   threshold or any allocation drift.

   When mino-bench is not checked out side-by-side, exits with a
   warning rather than a failure: perf-gate is opt-in for
   developers who care about the eval floor; CI invokes it from
   the mino-bench repo directly.

   On a perf change with a known cost, set MINO_PERF_GATE_RECORD=1
   to rewrite the baseline in the same commit as the runtime change.

   Not wired into release-gate by default: the perf benches take
   tens of seconds to run, and release-gate is meant to be a
   sub-minute pre-tag check. A perf-conscious cycle close can
   chain to perf-gate explicitly."
  []
  (if-let [mb (mino-bench-adjacent)]
    (println (sh! "sh" "-c"
                  (str "cd " mb " && "
                       (getenv "PWD") "/mino "
                       "benchmarks/perf_gate.clj")))
    (println "  perf-gate: mino-bench not adjacent -- skipped")))

(defn release-gate
  "Composite pre-tag gate. Fails fast on the first non-OK check;
   the first failure is what the human reads, not a paragraph-long
   run-all summary. Order:

     1. check-reloc-mirror    -- G3, smallest scope
     2. check-stencil-registry -- G2, no I/O
     3. test-suite             -- bytecode + JIT path
     4. test-suite-asan        -- same suite, sanitiser-built
     5. test-jit-parity        -- byte-identical stdout vs no-JIT
     6. mino-tests adv-test    -- IF mino-tests is cloned adjacent;
                                   skipped with a warn otherwise.

   `check-stencils-fresh` is intentionally NOT part of the composite:
   it regenerates stencils with the host `cc`, which means the
   committed bytes have to byte-match whatever clang version the
   runner ships. CI matrix runners (Apple clang 15 on macos-14,
   gcc on ubuntu-24.04) diverge from dev (Apple clang 17), so the
   check is structurally incompatible with the matrix. Dev runs
   `./mino task check-stencils-fresh` before committing stencil
   source edits; CI correctness is gated by test-suite + ASan +
   4-way JIT parity, which catch the actual runtime impact of a
   stale stencil regardless of compiler version. See
   `.local/BUGS.md` for the architectural decision context.

   Exits 0 on a clean tree. Negative controls live in the cycle's
   .local/ status file -- this task is the positive control."
  []
  (check-reloc-mirror)
  (check-stencil-registry)
  (test-suite)
  (build-asan)
  (println (sh! "./mino_asan" "tests/run.clj"))
  (test-jit-parity)
  (if-let [mt (mino-tests-adjacent)]
    (do
      (println "  release-gate: chaining to mino-tests at" mt)
      ;; The probe runner uses relative paths (load-file "tests/...");
      ;; chdir via `sh -c` so it resolves from the mino-tests root.
      (println (sh! "sh" "-c"
                    (str "cd " mt " && "
                         "MINO_BIN=" (getenv "PWD") "/mino "
                         "MINO_LEAN_BIN=" (getenv "PWD") "/mino-lean "
                         (getenv "PWD") "/mino "
                         "tests/adv/runner.clj --seed 0 --mode smoke"))))
    (println "  release-gate: mino-tests not adjacent -- skipped satellite smoke"))
  (println "  release-gate: OK"))

(def ^:private ci-matrix-targets
  "Per-Docker-target build images. Each entry binds a docker tag
   (the image name) to its build context. Used by the ci-matrix
   task to drive end-to-end verification on every host that has a
   committed stencil header.

   Windows x86_64 is not represented here: Windows containers
   need a Windows host. GitHub Actions covers that target via the
   `windows-2022` runner in .github/workflows/ci.yml. The local
   `ci-matrix` driver is for the Linux pair only."
  [{:tag        "mino-ci-arm64-linux"
    :dockerfile "docker/arm64-linux.Dockerfile"
    :platform   "linux/arm64"}
   {:tag        "mino-ci-x86_64-linux"
    :dockerfile "docker/x86_64-linux.Dockerfile"
    :platform   "linux/amd64"}])

(defn ci-matrix
  "Build + run release-gate inside each Docker target image.
   On an Apple Silicon dev host: linux/arm64 runs natively via
   the macOS Virtualization framework; linux/amd64 runs via
   Rosetta 2 / qemu (slow but functional).

   For each target the driver:
     1. docker build -t <tag> -f docker/<arch>.Dockerfile .
     2. docker run --rm --platform <platform> -v $PWD:/mino:rw
                   <tag> sh -c 'make && ./mino task release-gate'
     3. Reports pass/fail per target; non-zero on any failure.

   Reproduces the CI matrix locally so failures surface before
   pushing. The GHA matrix in .github/workflows/ci.yml is the
   authoritative gate; this task is the local mirror."
  []
  (let [failures
        (reduce
          (fn [acc {:keys [tag dockerfile platform]}]
            (println (str "--- ci-matrix: " tag " (" platform ") ---"))
            (let [build (sh "docker" "build"
                            "--platform" platform
                            "-t" tag
                            "-f" dockerfile
                            ".")]
              (when-not (= 0 (get build :exit))
                (println (get build :out))
                (println (get build :err))))
            (let [run (sh "docker" "run" "--rm"
                          "--platform" platform
                          "-v" (str (System/getProperty "user.dir") ":/mino:rw")
                          tag
                          "sh" "-c"
                          "make && ./mino task release-gate")]
              (let [out (get run :out)
                    err (get run :err)
                    ok? (= 0 (get run :exit))]
                (when-not ok?
                  (println "--- last 60 lines of output ---")
                  (let [combined (str out err)
                        lines    (str/split-lines combined)
                        tail     (take-last 60 lines)]
                    (doseq [l tail] (println l))))
                (println (str "  ci-matrix " tag ": " (if ok? "OK" "FAIL")))
                (if ok? acc (conj acc tag)))))
          []
          ci-matrix-targets)]
    (if (seq failures)
      (throw (ex-info (str "ci-matrix: failed targets: "
                           (str/join ", " failures))
                      {:failed-targets failures}))
      (println "  ci-matrix: OK"))))

(defn build-alloc-profile
  "Build mino_prof with -DMINO_ALLOC_PROFILE=1. Wraps every gc_alloc_typed
   call with a per-callsite recorder; expose the data with the
   alloc-profile-dump! / alloc-profile-reset! primitives."
  []
  (gen-core-header)
  (let [args (into [cc]
                   (concat cflags
                           ["-DMINO_ALLOC_PROFILE=1"]
                           ldflags
                           ["-o" "mino_prof"]
                           all-srcs
                           libs))]
    (println (str "  " (str/join " " args)))
    (apply sh! args)
    (println (str "  alloc-profile build -> mino_prof"))))

(def ^:private stencil-extract-srcs
  "Source files compiled together into tools/stencil-extract. The
   format-agnostic core sits alongside the per-format parsers; each
   carve-out from the original monolith just adds another file here."
  ["tools/stencil_extract.c"
   "tools/stencil_extract/coff.c"
   "tools/stencil_extract/core.c"
   "tools/stencil_extract/elf.c"
   "tools/stencil_extract/macho.c"
   "tools/stencil_extract/selftest.c"])

(defn build-stencil-extract
  "Build tools/stencil-extract: the copy-and-patch stencil extractor
   used by the native tier build pipeline. The source splits across
   tools/stencil_extract.c (main + magic-sniff + aggregate selftest)
   and the per-format modules under tools/stencil_extract/. The
   binary keeps a hyphen because the directory takes the underscore
   name."
  []
  (let [args (into [cc "-std=c99" "-O2" "-Wall" "-Wpedantic"
                    "-o" "tools/stencil-extract"]
                   stencil-extract-srcs)]
    (println (str "  " (str/join " " args)))
    (apply sh! args)
    (println "  stencil-extract build -> tools/stencil-extract")))

(defn test-stencil-extract
  "Run the stencil-extract self-test. Verifies that the Mach-O struct
   layouts match the documented file format on the host."
  []
  (build-stencil-extract)
  (println (sh! "./tools/stencil-extract" "--selftest")))

(def ^:private stencil-list
  "Canonical mapping from stencil .c file to extracted symbol. The
   list is the single source of truth for both gen-stencils and the
   G2 registry check."
  [["return.c"        "stencil_op_return_arg0"]
   ["return.c"        "stencil_op_return_imm"]
   ["move.c"          "stencil_op_move"]
   ["load_k.c"        "stencil_op_load_k"]
   ["load_k_return.c" "stencil_op_load_k_return"]
   ["add_ii.c"        "stencil_op_add_ii"]
   ["sub_ii.c"        "stencil_op_sub_ii"]
   ["mul_ii.c"        "stencil_op_mul_ii"]
   ["lt_ii.c"         "stencil_op_lt_ii"]
   ["le_ii.c"         "stencil_op_le_ii"]
   ["gt_ii.c"         "stencil_op_gt_ii"]
   ["ge_ii.c"         "stencil_op_ge_ii"]
   ["eq_ii.c"         "stencil_op_eq_ii"]
   ["inc_i.c"         "stencil_op_inc_i"]
   ["dec_i.c"         "stencil_op_dec_i"]
   ["zero_int_p.c"    "stencil_op_zero_int_p"]
   ["add_ik.c"        "stencil_op_add_ik"]
   ["sub_ik.c"        "stencil_op_sub_ik"]
   ["lt_ik.c"         "stencil_op_lt_ik"]
   ["le_ik.c"         "stencil_op_le_ik"]
   ["eq_ik.c"         "stencil_op_eq_ik"]
   ["mod_ii.c"           "stencil_op_mod_ii"]
   ["quot_ii.c"          "stencil_op_quot_ii"]
   ["rem_ii.c"           "stencil_op_rem_ii"]
   ["band_ii.c"          "stencil_op_band_ii"]
   ["bor_ii.c"           "stencil_op_bor_ii"]
   ["bxor_ii.c"          "stencil_op_bxor_ii"]
   ["shl_ii.c"           "stencil_op_shl_ii"]
   ["shr_ii.c"           "stencil_op_shr_ii"]
   ["ushr_ii.c"          "stencil_op_ushr_ii"]
   ["pos_p_i.c"          "stencil_op_pos_p_i"]
   ["neg_p_i.c"          "stencil_op_neg_p_i"]
   ["even_p_i.c"         "stencil_op_even_p_i"]
   ["odd_p_i.c"          "stencil_op_odd_p_i"]
   ["bnot_i.c"           "stencil_op_bnot_i"]
   ["loop_int_lt.c"      "stencil_op_loop_int_lt"]
   ["loop_int_dec.c"     "stencil_op_loop_int_dec"]
   ["loop_int_lt_inc.c"  "stencil_op_loop_int_lt_inc"]
   ["loop_int_dec_inc.c" "stencil_op_loop_int_dec_inc"]
   ["loop_int_lt_acc.c"  "stencil_op_loop_int_lt_acc"]
   ["loop_int_dec_acc.c" "stencil_op_loop_int_dec_acc"]
   ["getglobal_cached.c" "stencil_op_getglobal_cached"]
   ["call_cached.c"      "stencil_op_call_cached"]
   ["protocol_call_cached.c"
                         "stencil_op_protocol_call_cached"]
   ["protocol_tailcall_cached.c"
                         "stencil_op_protocol_tailcall_cached"]
   ["call.c"             "stencil_op_call"]
   ["tailcall.c"         "stencil_op_tailcall"]
   ["closure.c"          "stencil_op_closure"]
   ["make_lazy.c"        "stencil_op_make_lazy"]
   ["push_env.c"         "stencil_op_push_env"]
   ["pop_env.c"          "stencil_op_pop_env"]
   ["env_bind.c"         "stencil_op_env_bind"]
   ["nth_vec.c"          "stencil_op_nth_vec"]
   ["first_vec.c"        "stencil_op_first_vec"]
   ["count_vec.c"        "stencil_op_count_vec"]
   ["empty_vec.c"        "stencil_op_empty_vec"]
   ["get_kw_map.c"       "stencil_op_get_kw_map"]
   ["conj_vec.c"         "stencil_op_conj_vec"]
   ["assoc.c"            "stencil_op_assoc"]
   ["assoc_bang.c"       "stencil_op_assoc_bang"]
   ["conj_bang.c"        "stencil_op_conj_bang"]
   ["dissoc_bang.c"      "stencil_op_dissoc_bang"]
   ["disj_bang.c"        "stencil_op_disj_bang"]
   ["dissoc.c"           "stencil_op_dissoc"]
   ["deopt_to_interp.c"  "stencil_op_deopt_to_interp"]])

(defn- gen-stencils-for
  "Compile every stencil source for the given target triple and
   extract bytes into src/eval/bc/stencils/generated/stencils_<triple>.h.
   target-cflags is a vector of extra cflags (e.g. cross-compile flag
   for non-host targets) and compiler is the cc/clang binary to use."
  [triple compiler target-cflags]
  (let [gen-dir   "src/eval/bc/stencils/generated"
        out-hdr   (str gen-dir "/stencils_" triple ".h")
        tmpdir    (str "/tmp/mino-stencils-" triple)
        base-args ["-std=c99" "-O2" "-fno-builtin"
                   "-fno-optimize-sibling-calls"]]
    (sh! "mkdir" "-p" gen-dir)
    (sh! "mkdir" "-p" tmpdir)
    ;; First stencil writes the preamble; subsequent ones append onto
    ;; the same header file. The compiler dedup-compiles each source
    ;; once per distinct file path.
    (loop [[[file sym] & rest] stencil-list
           first? true
           compiled #{}]
      (when file
        (let [src (str "src/eval/bc/stencils/" file)
              obj (str tmpdir "/" file ".o")]
          (when-not (compiled file)
            (apply sh! compiler (concat base-args target-cflags
                                        ["-c" src "-o" obj])))
          (if first?
            (sh! "./tools/stencil-extract" obj sym out-hdr)
            (sh! "./tools/stencil-extract" "--append" obj sym out-hdr))
          (recur rest false (conj compiled file)))))
    (println (str "  stencils -> " out-hdr))))

(defn gen-stencils
  "Compile every stencil source under src/eval/bc/stencils/ to an
   intermediate .o file and dispatch the extractor to write the byte
   tables into src/eval/bc/stencils/generated/<arch>_<os>.h. The
   runtime build includes the generated header; regenerate after
   touching any stencil source or after a toolchain change that
   would shift the emitted code.

   Defaults to the host triple (arm64_darwin on Apple Silicon).
   Other targets are produced via cross-compile and committed
   alongside the host header so non-host platforms can be built
   without their toolchain regenerating bytes."
  []
  (build-stencil-extract)
  ;; arch/os naming mirrors what platform releases extend.
  (gen-stencils-for "arm64_darwin" cc []))

(defn gen-stencils-arm64-linux
  "Cross-compile every stencil to aarch64-linux-gnu using clang's
   built-in cross-target support and write stencils_arm64_linux.h.
   The output header is checked into source so native Linux builds
   pick it up without needing to regenerate. Re-run when stencil
   sources change."
  []
  (build-stencil-extract)
  (gen-stencils-for "arm64_linux" "clang" ["--target=aarch64-linux-gnu"]))

(defn gen-stencils-x86-64-linux
  "Cross-compile every stencil to x86_64-linux-gnu using clang's
   built-in cross-target support and write stencils_x86_64_linux.h.
   Adds -mno-red-zone alongside the standard stencil flags so the
   JIT region doesn't end up reading aliased red-zone slots when a
   helper call returns. The output header is checked into source so
   native Linux builds pick it up without needing to regenerate."
  []
  (build-stencil-extract)
  (gen-stencils-for "x86_64_linux" "clang"
                    ["--target=x86_64-linux-gnu" "-mno-red-zone"]))

(defn gen-stencils-x86-64-darwin
  "Cross-compile every stencil to x86_64-apple-darwin using clang's
   built-in cross-target support and write stencils_x86_64_darwin.h.
   Adds -mno-red-zone alongside the standard stencil flags so the
   JIT region doesn't end up reading aliased red-zone slots when a
   helper call returns. The output header is checked into source so
   native x86_64 Darwin builds pick it up without needing to
   regenerate."
  []
  (build-stencil-extract)
  (gen-stencils-for "x86_64_darwin" "clang"
                    ["--target=x86_64-apple-darwin" "-mno-red-zone"]))

(defn gen-stencils-x86-64-windows
  "Cross-compile every stencil to x86_64-pc-windows-msvc (the COFF
   target) using clang's built-in cross-target support and write
   stencils_x86_64_windows.h. -mno-red-zone matches the Windows x64
   ABI which has no red zone. The output header is checked into
   source so native Windows builds pick it up without needing to
   regenerate."
  []
  (build-stencil-extract)
  (gen-stencils-for "x86_64_windows" "clang"
                    ["--target=x86_64-pc-windows-msvc" "-mno-red-zone"]))

(defn test-suite
  "Run the test suite."
  []
  (println (sh! mino-bin "tests/run.clj")))

(defn test-external
  "Run the external test runner."
  []
  (println (sh! mino-bin "tests/external_runner.clj")))

(defn- compile-and-run-embed-test
  "Compile a tests/embed_*.c harness against the lib srcs and run it."
  [src bin]
  (let [objs    (mapv src->obj lib-srcs)
        pthread (if windows? [] ["-pthread"])
        args    (into [cc] (concat cflags pthread ldflags
                                   ["-o" bin src]
                                   objs libs))]
    (println (str "  " (str/join " " args)))
    (apply sh! args)
    (println (sh! (str "./" bin)))))

(defn test-embed
  "Compile and run the embed_api_test smoke. Multi-state, STM, and
   capability embed tests live in the mino-tests satellite repo
   (test-embed-suite there); this smoke verifies the basic API
   surface in mino's own gate."
  []
  (compile-and-run-embed-test "tests/embed_api_test.c"
                              "embed_api_test"))

;; ---- Architecture quality gates ----

(def ^:private tu-limit 1100)
(def ^:private fn-limit 250)

;; Files allowed to exceed the TU size limit, with rationale.
(def ^:private tu-allowlist
  {"src/eval/read.c"            "lexer/parser -- inherently sequential, not decomposable"
   "src/prim/collections.c"     "14 domain primitives in one module, barely over limit"
   "src/prim/agent.c"           "agent subsystem -- worker thread, queue, prims kept together"
   "src/prim/bignum.c"          "imath wrapper + numeric tower coercions, barely over limit"
   "src/prim/module.c"          "ns / require / use / load surface kept together"
   "src/prim/ns.c"              "ns primitives -- intern / refer / alias / publics in one module"
   "src/prim/numeric.c"         "numeric tower -- arith + compare + math fns sharing coercion helpers"
   "src/prim/reflection.c"      "reflection / type / meta surface kept together"
   "src/prim/sequences.c"       "sequence primitives -- map / filter / take / etc share lazy-seq glue"
   "src/prim/stm.c"             "STM commit + retry + ref/alter/commute/dosync kept together"
   "src/prim/string.c"          "string primitives -- per-byte / per-char ops share parsing helpers"
   "src/values/val.c"           "value layer -- alloc / copy / hash / equality kept with type defs"
   "src/runtime/state.c"        "state lifecycle -- ctor/dtor/quiesce + lock impl kept together"
   "src/vendor/imath/imath.c"   "vendored bigint library -- not modified"})

;; Functions allowed to exceed the function size limit, keyed by file:signature prefix.
(def ^:private fn-allowlist
  #{"src/eval/special.c:eval_impl"     ;; main evaluator dispatch -- inherently large
    "src/eval/read.c:read_form"
    "src/values/val.c:int mino_eq"  ;; cross-type equality dispatch over every MINO_* tag
    "src/eval/print.c:void mino_print_to" ;; printer dispatch over every MINO_* tag
    "src/prim/module.c:mino_env_t *env" ;; load_ns_file -- multi-line signature; nested form-by-form loader
    "src/prim/module.c:mino_val_t *prim_require"}) ;; require -- spec parsing + loading + aliasing in one path

(defn- count-lines
  "Return the number of lines in a file."
  [path]
  (count (str/split (slurp path) "\n")))

(defn- find-large-functions
  "Find functions exceeding fn-limit lines.
   Uses column-0 brace tracking: { at col 0 opens a function body,
   } at col 0 closes it. Returns a vector of {:file :name :lines} maps."
  [path]
  (let [lines  (str/split (slurp path) "\n")
        nlines (count lines)]
    (loop [i 0 depth 0 fn-start nil fn-name nil results []]
      (if (>= i nlines)
        results
        (let [line (nth lines i)]
          (if (and (= depth 0) (str/starts-with? line "{"))
            ;; Opening brace at col 0: entering function body.
            ;; The function signature is on the preceding non-blank line.
            (let [sig (loop [j (- i 1)]
                        (if (< j 0)
                          "<unknown>"
                          (let [prev (str/trim (nth lines j))]
                            (if (= prev "")
                              (recur (- j 1))
                              prev))))]
              (recur (+ i 1) 1 i sig results))
            (if (and (> depth 0) (str/starts-with? line "}"))
              ;; Closing brace at col 0: leaving function body.
              (let [body-lines (+ (- i fn-start) 1)
                    new-results (if (> body-lines fn-limit)
                                  (conj results {:file path
                                                 :name fn-name
                                                 :lines body-lines})
                                  results)]
                (recur (+ i 1) 0 nil nil new-results))
              ;; Inside a function body, track nested braces.
              (recur (+ i 1) depth fn-start fn-name results))))))))

(defn- find-abort-sites
  "Find abort() call sites. Returns a vector of {:file :line :text :has-rationale} maps."
  [path]
  (let [lines  (str/split (slurp path) "\n")
        nlines (count lines)]
    (loop [i 0 results []]
      (if (>= i nlines)
        results
        (let [line (nth lines i)]
          (if (re-find #"abort\(\)" line)
            (let [;; Check if current line or preceding line has a comment.
                  has-comment (or (some? (re-find #"/\*" line))
                                  (some? (re-find #"//" line))
                                  (and (> i 0)
                                       (let [prev (nth lines (- i 1))]
                                         (or (some? (re-find #"/\*" prev))
                                             (some? (re-find #"//" prev))))))]
              (recur (+ i 1) (conj results {:file path
                                            :line (+ i 1)
                                            :text (str/trim line)
                                            :has-rationale has-comment})))
            (recur (+ i 1) results)))))))

(defn- check-tu-size
  "Prints one TU size line. Returns 1 on FAIL, 0 otherwise."
  [f]
  (let [n       (count-lines f)
        allowed (get tu-allowlist f)
        over    (> n tu-limit)]
    (cond
      (and over allowed)
      (do (println (str "  ALLOW  " f ": " n " LOC (" allowed ")")) 0)
      over
      (do (println (str "  FAIL   " f ": " n " LOC")) 1)
      :else
      (do (println (str "  ok     " f ": " n " LOC")) 0))))

(defn- check-large-fn
  "Prints one large-function line. Returns 1 on FAIL, 0 on ALLOW."
  [m]
  (let [file (:file m)
        sig  (:name m)
        allowed (some (fn [entry]
                        (let [parts (str/split entry ":")
                              af    (first parts)
                              afn   (first (rest parts))]
                          (and (= file af) (str/includes? sig afn))))
                      fn-allowlist)]
    (if allowed
      (do (println (str "  ALLOW  " file ": " sig " (" (:lines m) " LOC)")) 0)
      (do (println (str "  FAIL   " file ": " sig " (" (:lines m) " LOC)")) 1))))

(defn- run-tu-check [src-files]
  (println "== Translation unit size (limit:" tu-limit "LOC) ==")
  (reduce + 0 (map check-tu-size src-files)))

(defn- run-fn-check [src-files]
  (println)
  (println "== Function size (limit:" fn-limit "LOC) ==")
  (let [large (into [] (mapcat find-large-functions) src-files)]
    (if (empty? large)
      (do (println "  ok     no functions exceed limit") 0)
      (reduce + 0 (map check-large-fn large)))))

(defn- run-abort-check [src-files]
  (println)
  (println "== Abort inventory ==")
  (let [sites             (into [] (mapcat find-abort-sites) src-files)
        missing-rationale (filterv #(not (:has-rationale %)) sites)
        n-missing         (count missing-rationale)]
    (println (str "  " (count sites) " abort() sites across "
                  (count (into #{} (map :file) sites)) " files"))
    (doseq [s sites]
      (println (str "  " (if (:has-rationale s) "ok   " "FAIL ")
                    (:file s) ":" (:line s) " " (:text s))))
    (when (> n-missing 0)
      (println (str "  FAIL   " n-missing " sites missing rationale")))
    n-missing))

(defn qa-arch
  "Architecture quality gates: TU size, function size, abort inventory."
  []
  (let [src-files (sort (filterv #(str/ends-with? % ".c") (file-seq "src")))
        failures  (+ (run-tu-check src-files)
                     (run-fn-check src-files)
                     (run-abort-check src-files))]
    (println)
    (if (= failures 0)
      (println "qa-arch: PASS")
      (do (println (str "qa-arch: FAIL (" failures " issue(s))"))
          (throw (str "qa-arch failed with " failures " issue(s)"))))))
