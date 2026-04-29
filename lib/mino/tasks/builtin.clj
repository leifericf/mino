(ns mino.tasks.builtin)

(require '[clojure.string :as str])

;; Build configuration -- matches Makefile defaults.

(def ^:private cc      (or (getenv "CC") "cc"))
(def ^:private include-flags
  (str "-Isrc -Isrc/public -Isrc/runtime -Isrc/gc -Isrc/eval"
       " -Isrc/collections -Isrc/prim -Isrc/async -Isrc/interop"
       " -Isrc/diag -Isrc/vendor/imath"))
(def ^:private cflags  (str/split (or (getenv "CFLAGS")
                                  (str "-std=c99 -Wall -Wpedantic -Wextra -O2 "
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
   "src/runtime/state.c" "src/runtime/var.c"
   "src/runtime/error.c" "src/runtime/env.c"
   "src/runtime/ns_env.c"
   "src/runtime/path_buf.c"
   "src/runtime/host_threads.c"
   "src/gc/driver.c" "src/gc/roots.c" "src/gc/major.c"
   "src/gc/barrier.c" "src/gc/minor.c"
   "src/gc/trace.c" "src/runtime/module.c"
   "src/public/gc.c" "src/public/embed.c"
   "src/collections/val.c" "src/collections/vec.c" "src/collections/map.c"
   "src/collections/rbtree.c" "src/eval/read.c" "src/eval/print.c"
   "src/prim/prim.c" "src/prim/install.c" "src/prim/install_stdlib.c"
   "src/prim/numeric.c" "src/prim/collections.c"
   "src/prim/sequences.c" "src/prim/lazy.c"
   "src/prim/string.c" "src/prim/io.c"
   "src/prim/reflection.c" "src/prim/meta.c" "src/prim/regex.c"
   "src/prim/stateful.c" "src/prim/module.c"
   "src/prim/ns.c"
   "src/prim/fs.c" "src/prim/proc.c"
   "src/prim/host.c" "src/interop/syntax.c"
   "src/collections/clone.c" "src/regex/re.c" "src/collections/transient.c"
   "src/async/scheduler.c" "src/async/timer.c" "src/prim/async.c"
   "src/prim/bignum.c" "src/vendor/imath/imath.c"])

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

(defn test-suite
  "Run the test suite."
  []
  (println (sh! mino-bin "tests/run.clj")))

(defn test-external
  "Run the external test runner."
  []
  (println (sh! mino-bin "tests/external_runner.clj")))

(defn test-fault-inject
  "Run fault injection tests (simulated OOM)."
  []
  (println (sh! mino-bin "tests/fault_inject_runner.clj")))

(defn test-gc-stress
  "Run GC stability tests under MINO_GC_STRESS=1 (collect on every allocation)."
  []
  (println (sh! "env" "MINO_GC_STRESS=1" mino-bin "tests/gc_stress_runner.clj")))

(defn test-embed
  "Compile and run the C embedding stress tests (embed_multi_state).
   Exercises 16 mino_state_t * 16 pthreads to assert the embedding API
   is safe under the documented one-state-per-thread contract."
  []
  (let [src     "tests/embed_multi_state.c"
        bin     "embed_multi_state"
        objs    (mapv src->obj lib-srcs)
        pthread (if windows? [] ["-pthread"])
        args    (into [cc] (concat cflags pthread ldflags
                                   ["-o" bin src]
                                   objs libs))]
    (println (str "  " (str/join " " args)))
    (apply sh! args)
    (println (sh! (str "./" bin)))))

;; ---- Architecture quality gates ----

(def ^:private tu-limit 1100)
(def ^:private fn-limit 250)

;; Files allowed to exceed the TU size limit, with rationale.
(def ^:private tu-allowlist
  {"src/eval/read.c"            "lexer/parser -- inherently sequential, not decomposable"
   "src/prim/collections.c"     "14 domain primitives in one module, barely over limit"})

;; Functions allowed to exceed the function size limit, keyed by file:signature prefix.
(def ^:private fn-allowlist
  #{"src/eval/special.c:eval_impl"     ;; main evaluator dispatch -- inherently large
    "src/eval/read.c:read_form"})

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
                          (and (= file af) (includes? sig afn))))
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
