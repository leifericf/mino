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

;; Stencil-regeneration toolchain. Stencils are committed as byte
;; headers, so this compiler is invoked only by maintainers running
;; gen-stencils-* / cross-build -- never by a normal build or an
;; embedder. It defaults to a pinned `zig cc`: one host then
;; cross-compiles every target to byte-identical output, which is
;; what lets the determinism gate be re-armed. STENCIL_CC=clang (or
;; any other argv) restores the legacy single-host path and opts out
;; of the version pin. Split on spaces so `apply sh!` splices the
;; vector (`zig cc` is two argv entries).
(def ^:private stencil-cc
  (str/split (or (getenv "STENCIL_CC") "zig cc") " "))

(def ^:private zig-version-pin
  "Pinned Zig version for stencil regeneration and cross-build. Zig
   is pre-1.0; a minor bump can shift the bundled LLVM and therefore
   the emitted stencil bytes, so byte-reproducibility requires an
   exact-version pin. Bump deliberately, then regenerate + commit the
   generated headers in the same change. Keep in lockstep with the
   `setup-zig` version in .github/workflows/ci.yml."
  "0.16.0")

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
   "src/collections/queue.c"
   "src/collections/bytes.c"
   "src/collections/rbtree.c"
   "src/collections/builders.c"
   "src/collections/gc_handlers.c"
   "src/collections/iter.c" "src/eval/read.c" "src/eval/print.c"
   "src/prim/prim.c" "src/prim/install.c" "src/prim/install_stdlib.c"
   "src/prim/numeric.c" "src/prim/numeric_math.c"
   "src/prim/numeric_bit.c" "src/prim/numeric_coerce.c"
   "src/prim/collections.c" "src/prim/collections_transient.c"
   "src/prim/bits.c"
   "src/prim/sequences.c" "src/prim/sequences_seq.c"
   "src/prim/lazy.c"
   "src/prim/string.c" "src/prim/io.c"
   "src/prim/reflection.c" "src/prim/meta.c" "src/prim/regex.c"
   "src/prim/stateful.c" "src/prim/stm.c" "src/prim/agent.c" "src/prim/module.c"
   "src/prim/ns.c"
   "src/prim/fs.c" "src/prim/proc.c"
   "src/prim/host.c" "src/prim/jvm_statics.c" "src/interop/syntax.c"
   "src/collections/clone.c" "src/regex/re.c" "src/collections/transient.c"
   "src/async/scheduler.c" "src/async/timer.c" "src/async/chan.c"
   "src/prim/async.c"
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
   ["lib/clojure/math.clj"            "clojure.math"            "lib_clojure_math"]
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
   ["lib/clojure/core/reducers.clj"   "clojure.core.reducers"   "lib_clojure_core_reducers"]
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

;; ---- Tier-2 cross-compile (optional) ----
;;
;; `zig cc` cross-compiles the Linux + Windows release binaries from
;; one host. This is an OPTIONAL maintainer path: `make` + `cc`
;; remains the canonical build, and embedders never touch it. Each
;; artifact mirrors the native `make` config (-DMINO_CPJIT=1, no
;; per-target JIT opt-in), so a cross-built binary matches what the
;; native release matrix produces for that platform -- a bytecode-
;; interpreter build. The committed Linux/Windows stencil headers stay
;; dormant here exactly as they do natively (only arm64-darwin enables
;; the JIT today, and darwin stays a native build -- Zig bundles no
;; macOS SDK to link libSystem against).

(def ^:private cross-cflags
  "Runtime CFLAGS shared by every cross target -- the Makefile
   bootstrap line minus the OS-specific LIBS/LDFLAGS, which each
   cross-targets entry supplies. -mno-red-zone is deliberately absent:
   it only affects stencil codegen, and these are no-JIT runtime
   builds."
  ["-std=c99" "-Wall" "-Wpedantic" "-Wextra" "-Werror"
   "-Wno-missing-field-initializers" "-Wno-unknown-warning-option"
   "-Wno-clobbered"
   ;; zig cc ships a newer clang than the native gcc/Apple-clang the
   ;; matrix uses, and it flags a set-but-unused `carry` in vendored
   ;; src/vendor/imath/imath.c that the native compilers do not. Same
   ;; class as -Wno-clobbered above: a toolchain-specific suppression
   ;; over third-party code, not a mino source defect.
   "-Wno-unused-but-set-variable"
   ;; The crash handler captures a backtrace via the compiler unwinder
   ;; (_Unwind_Backtrace, see src/runtime/crash_backtrace.h). The native
   ;; glibc/Apple-clang/mingw builds auto-link the unwinder (libgcc_s /
   ;; libSystem); zig cross targets do not, so emit unwind tables here
   ;; and link zig's libunwind explicitly (see cross-build-one).
   "-funwind-tables"
   "-O2" "-DMINO_CPJIT=1"])

(def ^:private cross-targets
  "Tier-2 cross-compile targets. macOS is intentionally absent: Zig
   bundles no macOS SDK, so the full runtime (which links libSystem)
   cannot cross-compile to darwin from Linux -- darwin stays a native
   release build.

   Linux ships in two flavors from the same host:
     * musl + -static (`:static true`, `:publish true`) -- the
       zero-dependency standalone artifact. A single file that runs on
       any Linux (glibc, musl/Alpine, old or new) with nothing to
       install. This is what releases publish. Unblocked by the
       portable crash handler (see src/runtime/crash_backtrace.h): musl
       ships no <execinfo.h>.
     * glibc (`:static false`, `:publish false`) -- kept build-only as
       a portability canary and for embedders who prefer a dynamically
       linked glibc binary. Not published; the musl build is the
       standalone download.

   Windows links via mingw WITHOUT -static: zig's mingw links the
   compiler runtime statically by default, so the PE has no
   libgcc_s_seh-1.dll / libwinpthread-1.dll dependency, retiring the
   Makefile's -static hack. Published.

   musl folds pthread into libc, so the musl targets need only -lm; the
   glibc targets link -lpthread explicitly."
  [{:platform "linux-amd64-musl" :triple "x86_64-linux-musl"
    :libs ["-lm"]                :exe "" :static true  :publish true}
   {:platform "linux-arm64-musl" :triple "aarch64-linux-musl"
    :libs ["-lm"]                :exe "" :static true  :publish true}
   {:platform "linux-amd64"      :triple "x86_64-linux-gnu"
    :libs ["-lm" "-lpthread"]    :exe "" :static false :publish false}
   {:platform "linux-arm64"      :triple "aarch64-linux-gnu"
    :libs ["-lm" "-lpthread"]    :exe "" :static false :publish false}
   {:platform "windows-amd64"    :triple "x86_64-windows-gnu"
    :libs ["-lm"]                :exe ".exe" :static false :publish true}])

(defn- cross-build-one
  "Cross-compile one target in a single `zig cc` invocation (compile +
   link all sources at once, like the Makefile bootstrap line). Output
   lands under dist-cross/ with a per-platform name, so cross objects
   never collide with the native build's src/*.o."
  [{:keys [platform triple libs exe static]}]
  (let [out-dir "dist-cross"
        bin     (str out-dir "/mino_" (str/replace platform "-" "_") exe)
        args    (concat stencil-cc
                        cross-cflags
                        (str/split include-flags " ")
                        [(str "--target=" triple)]
                        (if static ["-static"] [])
                        ["-o" bin]
                        all-srcs
                        libs
                        ;; zig's libunwind supplies _Unwind_Backtrace /
                        ;; _Unwind_GetIP for the portable crash handler.
                        ;; Uniform across every cross target (gnu Linux,
                        ;; musl static, arm64, mingw); the native make+cc
                        ;; build never needs it since the system unwinder
                        ;; is implicit.
                        ["-lunwind"])]
    (sh! "mkdir" "-p" out-dir)
    (println (str "  " (str/join " " args)))
    (apply sh! args)
    (println (str "  cross-build -> " bin))))

(defn- verify-cross-static
  "Assert each :static cross target produced a binary with no dynamic
   dependencies (no ELF NEEDED entries) -- the property that makes the
   musl artifact a true zero-dependency standalone download. readelf
   reads ELF headers without executing, so both the amd64 and arm64
   musl binaries are checked on any host arch. Skipped with a note when
   readelf is unavailable (e.g. a non-Linux maintainer host); CI's
   cross-build-validate runs on ubuntu where readelf is present."
  []
  (if-not (= 0 (get (sh "readelf" "--version") :exit))
    (println (str "  verify-cross-static: readelf unavailable -- "
                  "skipping static-link assertion"))
    (doseq [{:keys [platform exe static]} cross-targets
            :when static]
      (let [bin (str "dist-cross/mino_" (str/replace platform "-" "_") exe)
            out (str (get (sh "readelf" "-d" bin) :out))]
        (when (re-find #"\(NEEDED\)" out)
          (throw (ex-info (str "verify-cross-static: " bin " has dynamic "
                               "dependencies -- expected a fully static "
                               "musl binary")
                          {:platform platform})))
        (println (str "  verify-cross-static: " bin
                      " is fully static (0 NEEDED) OK"))))))

(defn cross-build
  "Cross-compile the mino release binaries from one host using the
   pinned `zig cc`. Produces, into dist-cross/:
     * Linux amd64/arm64 musl + -static -- the zero-dependency
       standalone artifacts releases publish;
     * Linux amd64/arm64 glibc -- build-only portability canaries;
     * Windows amd64 -- mingw PE with no runtime-DLL dependency.
   Optional maintainer/release task -- `make` + `cc` stays canonical
   and this is never required to build or embed mino. macOS is absent
   on purpose; see `cross-targets`. After building, asserts the static
   targets really are dependency-free. Run gen-core-header /
   gen-stdlib-headers first if the bundled-source headers are missing
   (the task deps handle this)."
  []
  (check-zig-version)
  (doseq [t cross-targets]
    (cross-build-one t))
  (verify-cross-static)
  (println "  cross-build: OK"))

(defn build-all
  "Developer convenience: from any host, build mino for every target in
   one command and smoke the native one. Wraps the canonical native
   build (cc -> ./mino) plus the pinned-zig cross-build (Linux musl
   amd64/arm64, glibc canaries, Windows amd64). Catches arch / libc /
   Windows breakage locally before a push, without waiting for the CI
   matrix. Cross artifacts land in dist-cross/; the native binary is
   ./mino. The native compiler stays cc -- only the cross targets need
   zig -- so the canonical path is exercised exactly as an embedder
   would build it."
  []
  (build)
  (let [out (str/trim (sh! mino-bin "-e" "(+ 1 2)"))]
    (when-not (= out "3")
      (throw (ex-info (str "build-all: native smoke failed -- (+ 1 2) gave "
                           (pr-str out))
                      {:got out})))
    (println (str "  build-all: native " mino-bin " smoke OK ((+ 1 2) => 3)")))
  (cross-build)
  (println "  build-all: native + all cross targets OK"))

(defn doctor
  "Report the state of the mino development toolchain and how to close
   any gaps. Two tiers, by design:

     * The C99 `cc` + make path is REQUIRED to build or embed mino and
       is the README promise -- zig is never needed for it.
     * The pinned `zig cc` is REQUIRED for the full maintainer workflow:
       stencil regeneration (gen-stencils-all), cross-build / release
       artifacts, and the reproducible QA lanes (sanitize-zig, lint-zig,
       analyze-zig, build-all). It is NEVER required for embedders.

   Advisory: prints a checklist and install guidance, exiting 0. The
   only hard failure is a missing C compiler -- without it nothing
   builds at all."
  []
  (println "mino toolchain doctor")
  (println "---------------------")
  ;; Tier 1: the C compiler -- required for everything, including embedders.
  (let [r (sh cc "--version")]
    (if (= 0 (get r :exit))
      (println (str "  [ok]   C compiler (" cc "): "
                    (first (str/split-lines (str (get r :out))))))
      (do
        (println (str "  [FAIL] C compiler (" cc ") not found -- mino cannot "
                      "build without a C99 compiler. Install gcc or clang "
                      "and/or set $CC."))
        (throw (ex-info "doctor: no C compiler" {:cc cc})))))
  ;; Tier 2: the pinned zig -- required for the maintainer workflow only.
  (let [r (sh "zig" "version")
        v (str/trim (str (get r :out)))]
    (cond
      (not= 0 (get r :exit))
      (do
        (println "  [warn] zig not found on PATH.")
        (println (str "         Required for: stencil regen, cross-build /"
                      " release, and the sanitize-zig / lint-zig /"
                      " analyze-zig / build-all lanes."))
        (println "         NOT required to build or embed mino (make + cc).")
        (println (str "         Install the pinned version " zig-version-pin
                      " -- see docs/MAINTAINER_TOOLCHAIN.md")))

      (not= v zig-version-pin)
      (do
        (println (str "  [warn] zig " v " on PATH, but the pinned version is "
                      zig-version-pin "."))
        (println (str "         The pin keeps stencil bytes + cross builds"
                      " reproducible; a mismatch can shift emitted code."))
        (println (str "         Install Zig " zig-version-pin
                      " (e.g. from https://ziglang.org/download/) and put it"
                      " ahead on PATH, or set STENCIL_CC to override (opts"
                      " out of byte reproducibility).")))

      :else
      (println (str "  [ok]   zig " v " (matches pinned " zig-version-pin ")"))))
  (println (str "  note: make + cc is the canonical build and embedder path;"
                " zig is a maintainer requirement only."))
  (println "  doctor: done"))

;; ---- Amalgamation ----

(def ^:private amalgam-search-paths
  ["src" "src/public" "src/runtime" "src/gc" "src/eval" "src/values"
   "src/collections" "src/prim" "src/async" "src/interop" "src/diag"
   "src/vendor/imath" "src/eval/bc" "src/eval/bc/jit" "src/eval/bc/stencils"
   "src/regex"])

(defn- amalgam-find-header
  "Resolve a project-local #include \"X\" string to an on-disk path.
   Walks `amalgam-search-paths` in order. Returns nil for headers that
   cannot be located (e.g. the system stencil header forwarded through
   the MINO_CPJIT_STENCILS_HEADER macro -- handled separately)."
  [name]
  (loop [paths amalgam-search-paths]
    (cond
      (empty? paths) nil
      (file-exists? (str (first paths) "/" name)) (str (first paths) "/" name)
      :else (recur (rest paths)))))

(defn- amalgam-strip-comments
  "Strip C-style line-comment trailers so the include-line parser
   doesn't trip on `#include \"X\" /* note */`. Keeps the rest of
   the line."
  [line]
  (str/replace line #"\s*/\*.*$" ""))

(defn- amalgam-block-comment-open?
  "Returns true when `line` opens a `/*` block comment that does NOT
   close with a matching `*/` on the same line. Naive scan: doesn't
   handle `/*` inside string literals, which mino's headers don't
   contain. Returning true means subsequent lines are inside the
   block comment until a `*/` is seen."
  [line]
  (let [opens   (count (re-seq #"/\*" line))
        closes  (count (re-seq #"\*/" line))]
    (> opens closes)))

(defn- amalgam-block-comment-close?
  "Returns true when `line` closes an open block comment (contains
   `*/`)."
  [line]
  (some? (re-find #"\*/" line)))

(defn- amalgam-expand
  "Recursively expand a source or header file into the amalgamation
   output stream. Drops project-local `#include \"X\"` lines (the
   referenced header is inlined here if not already seen); collects
   system `#include <X>` lines into the syshdrs atom; emits every
   other line verbatim, preceded by a `#line` directive so compile
   errors still resolve to the original source.

   When an `#include \"X\"` line carries a trailing `/* ... */` block
   comment that continues across subsequent lines, the continuation
   lines are also absorbed and emitted as a single comment block, so
   the comment doesn't leak as bare code after the inlined header.

   `seen` tracks absolute paths already inlined so cyclic or repeated
   includes become no-ops. `chunks` is the rope accumulating output;
   each appended string is one chunk."
  [path seen syshdrs chunks]
  (when-not (@seen path)
    (swap! seen conj path)
    (let [src   (slurp path)
          lines (str/split src "\n")
          nlines (count lines)]
      (swap! chunks conj (str "/* === " path " === */\n"))
      (swap! chunks conj (str "#line 1 \"" path "\"\n"))
      (loop [i 0]
        (when (< i nlines)
          (let [line  (nth lines i)
                stripped (amalgam-strip-comments line)
                proj  (re-find #"^\s*#\s*include\s+\"([^\"]+)\"" stripped)]
            (cond
              ;; Skip the file's own `#line` directives.
              (re-find #"^\s*#\s*line\b" line)
              (recur (+ i 1))

              ;; Project-local include: inline the referenced file in
              ;; place. If the original directive line opens a multi-
              ;; line block comment, absorb continuation lines until the
              ;; closing `*/`, emit them as a single comment block so
              ;; the comment doesn't leak as code after the inlined
              ;; header expansion.
              proj
              (let [hdr (nth proj 1)]
                (if-let [found (amalgam-find-header hdr)]
                  (let [;; Absorb the comment continuation if the
                        ;; directive line opens an unclosed `/*` so the
                        ;; trailing comment doesn't leak as code after
                        ;; the inlined expansion.
                        end-i (if (amalgam-block-comment-open? line)
                                (loop [j (+ i 1)]
                                  (cond
                                    (>= j nlines) j
                                    (amalgam-block-comment-close?
                                      (nth lines j)) j
                                    :else (recur (+ j 1))))
                                i)]
                    (swap! chunks conj
                           (str "/* AMALGAM-INLINED " hdr
                                " (absorbed " (+ 1 (- end-i i))
                                " line"
                                (if (= 0 (- end-i i)) "" "s")
                                ") */\n"))
                    (amalgam-expand found seen syshdrs chunks)
                    (swap! chunks conj
                           (str "#line " (+ end-i 2) " \"" path "\"\n"))
                    (recur (+ end-i 1)))
                  ;; Unfound: keep the directive verbatim.
                  (do (swap! chunks conj (str line "\n"))
                      (recur (+ i 1)))))

              ;; System include (`<...>`): leave in place. Conditional
              ;; platform blocks (#ifdef _WIN32 ... <process.h> ...) rely
              ;; on staying inside their guards; hoisting would break
              ;; cross-platform builds. System headers are idempotent
              ;; under repeated inclusion, so dup cost is negligible.
              :else
              (do (swap! chunks conj (str line "\n"))
                  (recur (+ i 1))))))))))

(defn amalgamate
  "Produce a single-file vendor distribution under dist/.

   Writes:
     dist/mino.h      -- copy of the public header
     dist/mino.c      -- unified TU of every lib src + transitively
                         referenced internal header, with project-
                         local includes pre-expanded inline and system
                         includes deduped at the top.
     dist/README.md   -- two-line build recipe.

   An embedder vendors the dist/ directory into their tree, compiles
   with `cc -std=c99 -c mino.c`, and links `mino.o app.o -lm -lpthread`.
   No -I paths beyond the dist directory; no transitive header
   dependencies; one TU."
  []
  (gen-core-header)
  (gen-stdlib-headers)
  (when-not (file-exists? "dist") (sh! "mkdir" "-p" "dist"))
  (let [seen    (atom #{})
        syshdrs (atom #{})
        chunks  (atom [])]
    ;; Recursively walk lib-srcs (excluding main.c -- the embedder
    ;; provides their own entrypoint).
    (doseq [src lib-srcs]
      (amalgam-expand src seen syshdrs chunks))
    (let [header  "/* mino single-file amalgamation. See dist/README.md for usage. */\n\n"
          body    (str/join "" @chunks)]
      (spit "dist/mino.c" (str header body))
      (sh! "cp" "src/mino.h" "dist/mino.h")
      (println (str "amalgamate: dist/mino.c (" (count body) " bytes body, "
                    (count @seen) " files inlined)"))))
  (spit "dist/README.md"
        (str "# mino amalgamation\n\n"
             "Single-file distribution of mino's embedding runtime.\n\n"
             "## Build\n\n"
             "```\n"
             "cc -std=c99 -O2 -c mino.c\n"
             "cc app.c mino.o -lm -lpthread -o app\n"
             "```\n\n"
             "## Files\n\n"
             "- `mino.h` -- public embedding API (the only header you include).\n"
             "- `mino.c` -- unified translation unit; one `.c` file builds the\n"
             "  entire runtime.\n\n"
             "## Versioning\n\n"
             "The amalgamation is bit-identical to the mino source tree at\n"
             "the tag whose CHANGELOG entry produced it. See `mino.h` for\n"
             "`MINO_VERSION_*` macros.\n")))

(defn clean-dist
  "Remove the dist/ amalgamation tree."
  []
  (when (file-exists? "dist") (rm-rf "dist"))
  (println "  cleaned dist/"))

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
  (when (file-exists? "mino_ubsan_zig") (rm-rf "mino_ubsan_zig"))
  (when (file-exists? "mino_tsan_zig")  (rm-rf "mino_tsan_zig"))
  (when (file-exists? "mino_zig")       (rm-rf "mino_zig"))
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
;;
;; Every sanitizer build is JIT-enabled for the host it runs on (see
;; jit-enable-flags): the JIT's C side -- emit, patcher, helpers,
;; invoke, safepoints -- is exactly the kind of pointer-heavy code the
;; sanitizers exist for, and it was historically their blind spot.
;; The JIT-emitted machine code itself is uninstrumented (no shadow
;; tracking inside a stencil's bytes; no false positives either); the
;; boundary is documented on the sanitize-zig task.

(defn- jit-enable-flags
  "The -D flag set that runtime-enables the CPJIT for this machine's
   native arch/OS: -DMINO_CPJIT=1 plus the per-host opt-in define for
   the dormant pipelines (none needed on arm64 darwin, where plain
   MINO_CPJIT=1 is runtime-enabled). On a host with no JIT pipeline
   the plain define still compiles the stub path, matching `make`."
  []
  (into ["-DMINO_CPJIT=1"]
        (if windows?
          ["-DMINO_CPJIT_X86_64_WINDOWS"]
          (let [os   (str/trim (str (get (sh "uname" "-s") :out)))
                arch (str/trim (str (get (sh "uname" "-m") :out)))]
            (cond
              (and (= os "Linux") (= arch "aarch64"))
              ["-DMINO_CPJIT_ARM64_LINUX"]

              (and (= os "Linux") (= arch "x86_64"))
              ["-DMINO_CPJIT_X86_64_LINUX"]

              (and (= os "Darwin") (= arch "x86_64"))
              ["-DMINO_CPJIT_X86_64_DARWIN"]

              :else [])))))

(defn- run-suite-with-test-bin
  "Run the full suite under `bin`, exporting MINO_TEST_BIN so
   subprocess-spawning tests target the binary under test rather than
   ./mino. POSIX-only env prefix; on Windows the suite runs without
   the export and those tests self-skip."
  [bin extra-args]
  (if windows?
    (println (apply sh! bin (concat extra-args ["tests/run.clj"])))
    (println (sh! "sh" "-c"
                  (str "MINO_TEST_BIN=" bin " " bin " "
                       (str/join " " extra-args)
                       (if (seq extra-args) " " "")
                       "tests/run.clj")))))

(def ^:private san-base
  (into ["-g" "-O1" "-fno-omit-frame-pointer" "-std=c99"
         "-Wall" "-Wextra" "-Wpedantic"]
        (str/split include-flags " ")))

(defn- build-sanitized [label out-bin extra-flags]
  (gen-core-header)
  (let [args (into [cc]
                   (concat san-base
                           (jit-enable-flags)
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

;; ---- Reproducible pinned-zig sanitizer lane ----
;;
;; The sanitizer builds above use the host `cc`, so the sanitizer
;; runtime is whatever libsanitizer the runner ships -- it drifts with
;; the image. These variants build with the pinned `zig cc` instead, so
;; the compiler-rt instrumentation is version-locked and reproducible
;; across machines. The lane is ADDITIVE: the host ASan build stays in
;; release-gate.
;;
;; Coverage is UBSan + TSan, not ASan: zig 0.16.0 bundles no prebuilt
;; AddressSanitizer runtime (linking -fsanitize=address fails with
;; `undefined symbol: __asan_init`), but it compiles the UBSan and TSan
;; compiler-rt from source. ASan reproducibility therefore stays with
;; the host toolchain's libasan; this lane covers the UB and data-race
;; classes the host gate does not otherwise run. -funwind-tables +
;; -lunwind satisfy the portable crash handler, as in cross-build.

(def ^:private san-base-zig
  (into ["-g" "-O1" "-fno-omit-frame-pointer" "-std=c99"
         "-Wall" "-Wextra" "-Wpedantic" "-funwind-tables"]
        (str/split include-flags " ")))

(defn- build-sanitized-zig [label out-bin extra-flags]
  (check-zig-version)
  (gen-core-header)
  (let [args (into (vec stencil-cc)
                   (concat san-base-zig
                           (jit-enable-flags)
                           extra-flags
                           ["-o" out-bin]
                           all-srcs
                           ;; zig's default target is musl, which folds
                           ;; pthread into libc; -lunwind supplies the
                           ;; crash handler's _Unwind_* symbols.
                           ["-lm" "-lunwind"]))]
    (println (str "  " (str/join " " args)))
    (apply sh! args)
    (println (str "  " label " (pinned zig) build -> " out-bin))))

(defn build-ubsan-zig
  "Build mino_ubsan_zig with UndefinedBehaviorSanitizer using the pinned
   `zig cc`. -fno-sanitize-recover makes the first finding fatal so it
   trips a test runner's exit status. mino is UBSan-clean today (the
   full suite runs with zero findings), so a regression surfaces here."
  []
  (build-sanitized-zig "ubsan" "mino_ubsan_zig"
                       ["-fsanitize=undefined"
                        "-fno-sanitize-recover=undefined"]))

(defn build-tsan-zig
  "Build mino_tsan_zig with ThreadSanitizer using the pinned `zig cc`.
   Exercises the async scheduler + cross-state mutex paths the suite
   spins up; the single-state evaluator is otherwise single-threaded."
  []
  (build-sanitized-zig "tsan" "mino_tsan_zig"
                       ["-fsanitize=thread"]))

(defn sanitize-zig
  "Reproducible pinned-zig sanitizer lane: build mino under UBSan and
   under TSan with the version-locked `zig cc`, and run the full test
   suite under each in BOTH JIT modes -- AUTO (warm-then-compile) and
   eager (--jit=on). Additive to the host ASan build in release-gate;
   covers the undefined-behavior and data-race classes with a
   reproducible compiler-rt. ASan is intentionally absent -- zig ships
   no ASan runtime; that coverage stays on the host toolchain.

   Instrumentation boundary: the sanitizers cover the JIT's whole C
   side (emit, patcher, helpers, invoke, safepoints, deopt) but NOT
   the JIT-emitted machine code itself -- stencil bytes carry no
   shadow-memory hooks, so a bug confined to emitted code is invisible
   here (and produces no false positives either). The four-way parity
   check is the net for emitted-code divergence."
  []
  (build-ubsan-zig)
  (run-suite-with-test-bin "./mino_ubsan_zig" [])
  (run-suite-with-test-bin "./mino_ubsan_zig" ["--jit=on"])
  (build-tsan-zig)
  (run-suite-with-test-bin "./mino_tsan_zig" [])
  (run-suite-with-test-bin "./mino_tsan_zig" ["--jit=on"])
  (println "  sanitize-zig: OK"))

;; ---- Curated strict warning lane (pinned zig cc) ----

(def ^:private lint-zig-warnings
  "Curated strict warning set for the pinned-zig lint lane -- high-signal
   warnings beyond the build's -Wall -Wextra -Wpedantic that mino's own
   code passes cleanly under zig's newer clang. The set is deliberately
   curated, NOT -Weverything; these categories are EXCLUDED on purpose:

     -Wmissing-field-initializers : `{0}` / partial-brace zero-init is
       correct C and the prim tables ({\"name\", fn, \"doc\"}, fn2 left
       zero) rely on it. The native build already suppresses this.
     -Wswitch-enum : the VM/dispatch switches carry an intentional
       `default:` arm; the valuable variant (-Wswitch, fires only when a
       switch has no default AND misses cases) stays on via -Wall.
     -Wcast-qual : audited 2026-06-06 and left OUT. ~90 sites, every
       one an intentional const-drop -- GC marking const-reachable
       objects, mutable-cache members (e.g. cached_hash memoization on
       an immutable value), numeric tier accumulators. Gating would
       force a uintptr_t-launder at each site across delicate GC /
       numeric code: high churn, near-zero bug-catching signal (a real
       accidental-mutation bug would drown in 90 known-intentional
       drops). -Wmissing-prototypes / -Wmissing-variable-declarations
       ARE gated below -- the over-export audit drove those to zero.
     -Wbad-function-cast : audited 2026-06-06 and left OUT. ~30 sites,
       all idiomatic function-return casts ((double)mino_val_int_get(v),
       (int)mino_type_of(v)); forcing intermediate variables is pure
       churn with no safety gain.
     -Wformat-nonliteral, -Wdouble-promotion : low signal for an IO /
       numeric library (computed format strings, deliberate float math).

   -O2 -DMINO_CPJIT=1 mirror the real build: zig cc enables UBSan-style
   safety checks by default at -O0, which flips __has_feature(...) and
   produces spurious unreachable-code findings in sanitizer-gated code."
  ["-Wno-missing-field-initializers"
   "-Wshadow" "-Wstrict-prototypes"
   "-Wmissing-prototypes" "-Wmissing-variable-declarations"
   "-Wpointer-arith" "-Wwrite-strings" "-Wundef" "-Wvla"
   "-Wimplicit-fallthrough" "-Wcomma" "-Wunreachable-code"
   "-Wnested-externs" "-Wredundant-decls"
   "-Wformat=2" "-Wno-format-nonliteral"])

(defn lint-zig
  "Strict-warning lint lane: compile every mino-authored TU with the
   pinned `zig cc` (a third compiler lens beyond the gcc/Apple-clang CI
   matrix) under -Werror over the curated `lint-zig-warnings` set.
   Vendored src/vendor/ is excluded -- upstream style is not mino's to
   gate. Compile-only (-c, output discarded); nothing is linked."
  []
  (check-zig-version)
  (let [own-srcs (remove #(str/includes? % "vendor/") all-srcs)
        null-obj (if windows? "NUL" "/dev/null")
        base     (concat stencil-cc
                         ["-std=c99" "-O2" "-DMINO_CPJIT=1"
                          "-Wall" "-Wextra" "-Wpedantic" "-Werror"]
                         lint-zig-warnings
                         (str/split include-flags " "))]
    (doseq [tu own-srcs]
      (apply sh! (concat base ["-c" "-o" null-obj tu])))
    (println (str "  lint-zig: " (count own-srcs)
                  " TUs clean under the curated strict set"))))

;; ---- clang static analyzer lane (pinned zig cc) ----

(defn analyze-zig
  "Run clang's static analyzer (via the pinned `zig cc`) over every
   mino-authored TU and report findings grouped by checker.

   ADVISORY, not a gate, for two structural reasons surfaced by the
   spike: (1) zig cc's driver always appends a link step that fails on
   the analyzer's plist output, so the process exit code is never 0 even
   on a clean analysis -- the findings are emitted before that and are
   what matter; (2) clang's analyzer has well-known cross-TU false
   positives (core.NullDereference especially, since it can't see
   invariants enforced in another TU). So this prints findings for a
   human to triage rather than failing the build. It is the discovery
   instrument for the C99 bloat / over-export audit: deadcode.DeadStores
   and unix.Malloc findings in particular are worth chasing. Exits 0.

   `sh` merges the analyzer's stderr into :out; we drop the benign link
   noise (ld.lld plist error, the spurious `-c` note) and keep the
   `warning: ... [checker.Name]` lines."
  []
  (let [findings (analyze-zig-findings)
        own-srcs (remove #(str/includes? % "vendor/") all-srcs)]
    (doseq [f findings]
      (println (str "  " (str/replace f #"^.*/mino/" ""))))
    (let [by-checker (->> findings
                          (keep #(second (re-find #"\[([a-z]+\.[A-Za-z]+)\]" %)))
                          frequencies
                          (sort-by (comp - val)))]
      (doseq [[checker n] by-checker]
        (println (str "  " checker ": " n)))
      (println (str "  analyze-zig: " (count findings)
                    " analyzer finding(s) across " (count own-srcs)
                    " TUs [advisory -- triage, not a gate]")))))

;; ---- analyze-zig baseline gate ----
;;
;; The raw analyzer output is noisy (clang cross-TU NullDereference
;; false positives dominate), so a "must be zero" gate is infeasible
;; without churning the source with suppressions. Instead the triaged
;; finding set is checked in at tools/analyze_baseline.txt and the
;; gate fails only on findings NOT in that baseline -- a genuinely new
;; analyzer hit. Entries are normalized to "<file>: warning: <msg>
;; [<checker>]" with line:col stripped, so edits above a finding don't
;; spuriously churn the baseline.

(def ^:private analyze-baseline-path "tools/analyze_baseline.txt")

(defn- analyze-zig-findings
  "Run the clang static analyzer (pinned zig cc) over every
   mino-authored TU and return the raw `warning: ... [checker.Name]`
   lines, project-relative."
  []
  (check-zig-version)
  (let [own-srcs (remove #(str/includes? % "vendor/") all-srcs)
        base     (concat stencil-cc
                         ["--analyze" "-Xclang" "-analyzer-output=text"
                          "-std=c99" "-DMINO_CPJIT=1"]
                         (str/split include-flags " "))
        finding? (fn [line] (re-find #"warning:.*\[[a-z]+\.[A-Za-z]+\]" line))]
    (->> own-srcs
         (mapcat
           (fn [tu]
             (->> (str/split-lines
                    (str (get (apply sh (concat base [tu])) :out)))
                  (filter finding?)
                  (map #(str/replace % #"^.*/mino/" "")))))
         vec)))

(defn- analyze-normalize
  "Strip the `:line:col:` span from a finding so the baseline is stable
   across edits that shift line numbers."
  [line]
  (str/replace line #":[0-9]+:[0-9]+:" ":"))

(defn- analyze-baseline-set []
  (if (file-exists? analyze-baseline-path)
    (->> (str/split-lines (slurp analyze-baseline-path))
         (remove #(or (str/blank? %) (str/starts-with? (str/trim %) "#")))
         set)
    #{}))

(defn gen-analyze-baseline
  "Regenerate tools/analyze_baseline.txt from the current analyzer
   output, preserving the triage header. Run after an intentional
   change that adds or removes a finding, and commit the diff."
  []
  (let [header (if (file-exists? analyze-baseline-path)
                 (->> (str/split-lines (slurp analyze-baseline-path))
                      (take-while #(or (str/blank? %)
                                       (str/starts-with? (str/trim %) "#")))
                      (str/join "\n"))
                 "# analyze-zig baseline -- triaged static-analyzer findings.")
        norm   (->> (analyze-zig-findings)
                    (map analyze-normalize)
                    distinct
                    sort)]
    (spit analyze-baseline-path
          (str header "\n\n" (str/join "\n" norm) "\n"))
    (println (str "  gen-analyze-baseline: " (count norm)
                  " finding(s) written to " analyze-baseline-path))))

(defn check-analyze-zig
  "Baseline gate: run the static analyzer and fail if any finding is
   absent from tools/analyze_baseline.txt -- i.e. the change introduced
   a new analyzer hit. New findings are printed; resolve them or, if
   triaged as benign, regenerate the baseline with
   `./mino task gen-analyze-baseline` and commit. Stale baseline
   entries (findings that no longer fire) are reported as a warning,
   not a failure -- they make the gate stricter, not weaker."
  []
  (let [baseline (analyze-baseline-set)
        current  (->> (analyze-zig-findings) (map analyze-normalize) set)
        new-findings  (sort (set/difference current baseline))
        gone          (sort (set/difference baseline current))]
    (when (seq gone)
      (println (str "  check-analyze-zig: " (count gone)
                    " baseline finding(s) no longer fire (consider"
                    " regenerating the baseline):"))
      (doseq [g gone] (println (str "    - " g))))
    (if (seq new-findings)
      (do
        (println (str "  check-analyze-zig: " (count new-findings)
                      " NEW analyzer finding(s) not in the baseline:"))
        (doseq [f new-findings] (println (str "    + " f)))
        (throw (ex-info "check-analyze-zig: new static-analyzer findings"
                        {:new (vec new-findings)
                         :fix (str "fix the finding, or if benign run "
                                   "`./mino task gen-analyze-baseline` "
                                   "and commit")})))
      (println (str "  check-analyze-zig: OK -- no new findings vs baseline ("
                    (count baseline) " accepted)")))))

;; ---- Hermetic pinned-zig build lane ----

(defn build-zig
  "Hermetic build: compile the native mino binary with the pinned
   `zig cc` (into ./mino_zig) instead of the host cc. The whole
   toolchain -- compiler, libc, linker -- is the version-locked zig, so
   a green build here does not depend on the runner image's gcc/clang
   version; it reproduces across machines. Builds for the host via zig's
   default target, which is musl and static-by-default, plus -static for
   an explicit zero-dependency guarantee; -lunwind supplies the portable
   crash handler's _Unwind_* symbols. Additive: the canonical cc build
   and the gcc/Apple-clang/mingw canaries are untouched."
  []
  (check-zig-version)
  (let [args (concat stencil-cc
                     ["-std=c99" "-O2" "-DMINO_CPJIT=1" "-funwind-tables"
                      "-static"]
                     (str/split include-flags " ")
                     ["-o" "mino_zig"]
                     all-srcs
                     ["-lm" "-lunwind"])]
    (println (str "  " (str/join " " args)))
    (apply sh! args)
    (println "  build-zig -> ./mino_zig")))

(defn test-zig
  "Hermetic compile+test lane: build mino with the pinned `zig cc`
   (build-zig) and run the full test suite against it. Proves the
   version-locked toolchain produces a correct mino independent of the
   runner's system compiler -- the additive reproducibility backstop
   behind the gcc/Apple-clang/mingw CI canaries."
  []
  (build-zig)
  (println (sh! "./mino_zig" "tests/run.clj")))

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

(defn- jit-parity-check
  "Four-way JIT parity core: run tests/jit_parity_test.clj as AUTO /
   ON / OFF on `full-bin` plus once on `lean-bin`, and assert every
   variant's stdout bytes are byte-identical with exit 0. Shared by
   test-jit-parity (canonical ./mino + ./mino-lean) and test-jit-host
   (the per-host canary twins). Uses `sh` (not `sh!`) so a non-zero
   exit from any runner reports as a parity failure rather than
   crashing the task."
  [full-bin lean-bin]
  (let [parity-test "tests/jit_parity_test.clj"
        variants    [(run-parity-variant [full-bin "--jit=auto"]
                                          "jit-auto"  parity-test)
                     (run-parity-variant [full-bin "--jit=on"]
                                          "jit-on"    parity-test)
                     (run-parity-variant [full-bin "--jit=off"]
                                          "jit-off"   parity-test)
                     (run-parity-variant [lean-bin]
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

(defn test-jit-parity
  "Build ./mino and ./mino-lean, then run the four-way parity check
   (see jit-parity-check). The parity test pins ~40 literal-expected-
   value assertions covering range boundaries, tag-miss, comparison
   identity, and unary boundaries across the 16 inlined arith / cmp /
   unary stencils. Any divergence in either binary's output -- a
   different diagnostic, a different boxed-int representation, a
   missed coercion -- surfaces in the diff.

   The four variants are explicit so a regression in any mode is
   localised: AUTO is the warm-then-JIT path that release-gate
   exercises, ON forces eager compile (catches a JIT'd stencil
   that diverges from the interpreter), OFF inhibits compilation
   entirely (catches a runtime gating bug), and the lean binary
   has no JIT path at all (catches an embed-API drift between
   the two builds)."
  []
  (build)
  (build-lean)
  (jit-parity-check mino-bin "./mino-lean"))

;; ---- Per-host JIT runtime canary --------------------------------------
;;
;; Four of the five committed JIT pipelines (ELF arm64/x86_64, COFF
;; x86_64, Mach-O x86_64) sit behind per-host opt-in defines that no
;; published artifact sets -- they ship interpreter-only until the
;; canary lanes hold a green streak. This task is the runtime exercise
;; for whichever dormant pipeline the current machine can execute:
;; build a JIT-enabled binary (and its lean twin) with the pinned
;; `zig cc`, then run the full suite in AUTO and eager (--jit=on) mode
;; plus the four-way parity check against it.
;;
;; Host resolution: on arm64 darwin the native pipeline is already
;; covered by `make` + darwin-zig-canary, so the canary cross-builds
;; the x86_64-darwin pipeline instead and lets Rosetta 2 execute it --
;; the one way to runtime-exercise a second Mach-O arch per machine.
;; Linux hosts build their native arch as static musl (matching the
;; published artifact's libc); Windows builds the COFF pipeline via
;; zig's bundled mingw.

(def ^:private jit-host-canary-cflags
  "Runtime CFLAGS for the canary twins. Mirrors the Makefile bootstrap
   line plus the zig-toolchain suppressions cross-cflags carries for
   vendored code (newer-clang -Wunused-but-set-variable in imath) and
   -funwind-tables + -lunwind for the portable crash handler on
   non-darwin targets (zig links no implicit unwinder)."
  ["-std=c99" "-Wall" "-Wpedantic" "-Wextra" "-Werror"
   "-Wno-missing-field-initializers" "-Wno-unknown-warning-option"
   "-Wno-clobbered" "-Wno-unused-but-set-variable"
   "-O2"])

(defn- jit-host-spec
  "Resolve the canary build spec for the current machine: zig target
   triple, JIT opt-in define, link shape. Returns nil on hosts with no
   runtime-executable dormant pipeline."
  []
  (if windows?
    {:triple "x86_64-windows-gnu" :define "MINO_CPJIT_X86_64_WINDOWS"
     :static? false :unwind? true :exe ".exe"}
    (let [os   (str/trim (str (get (sh "uname" "-s") :out)))
          arch (str/trim (str (get (sh "uname" "-m") :out)))]
      (cond
        (= os "Darwin")
        {:triple "x86_64-macos" :define "MINO_CPJIT_X86_64_DARWIN"
         :static? false :unwind? false :exe ""}

        (and (= os "Linux") (= arch "aarch64"))
        {:triple "aarch64-linux-musl" :define "MINO_CPJIT_ARM64_LINUX"
         :static? true :unwind? true :exe ""}

        (and (= os "Linux") (= arch "x86_64"))
        {:triple "x86_64-linux-musl" :define "MINO_CPJIT_X86_64_LINUX"
         :static? true :unwind? true :exe ""}

        :else nil))))

(defn- build-jit-host-one
  "Build one canary twin with the pinned zig cc. `defines` carries the
   -D list that distinguishes the JIT-enabled binary from its lean
   twin."
  [{:keys [triple static? unwind?]} out-bin defines]
  (let [args (concat stencil-cc
                     jit-host-canary-cflags
                     (when unwind? ["-funwind-tables"])
                     defines
                     (str/split include-flags " ")
                     [(str "--target=" triple)]
                     (when static? ["-static"])
                     ["-o" out-bin]
                     all-srcs
                     (when unwind? ["-lm" "-lunwind"]))]
    (println (str "  " (str/join " " args)))
    (apply sh! args)
    (println (str "  jit-host build -> " out-bin))))

(defn test-jit-host
  "Per-host JIT runtime canary: build the dormant JIT pipeline this
   machine can execute (see jit-host-spec) plus its lean twin, smoke
   it, run the full suite in AUTO and eager mode, and finish with the
   four-way parity check. Fails loudly on hosts with no resolvable
   pipeline -- CI should never schedule it there."
  []
  (check-zig-version)
  (gen-core-header)
  (gen-stdlib-headers)
  (let [spec (jit-host-spec)]
    (when (nil? spec)
      (throw (ex-info "test-jit-host: no runtime-executable JIT pipeline for this host"
                      {:fix "run on darwin (arm64/x86_64), linux (aarch64/x86_64), or windows x86_64"})))
    (let [bin  (str (if windows? "" "./") "mino_jit_host" (get spec :exe))
          lean (str (if windows? "" "./") "mino_jit_host_lean" (get spec :exe))]
      (println (str "  jit-host: triple=" (get spec :triple)
                    " define=" (get spec :define)))
      (build-jit-host-one spec bin ["-DMINO_CPJIT=1"
                                    (str "-D" (get spec :define))])
      (build-jit-host-one spec lean [])
      (let [out (str/trim (sh! bin "-e" "(+ 1 2)"))]
        (when-not (= out "3")
          (throw (ex-info (str "test-jit-host: smoke failed -- (+ 1 2) gave "
                               (pr-str out))
                          {:got out}))))
      (run-suite-with-test-bin bin [])
      (run-suite-with-test-bin bin ["--jit=on"])
      (jit-parity-check bin lean)
      (println "  test-jit-host: OK"))))

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

(defn check-stencils-fresh-all
  "Determinism gate (all targets). Regenerate every committed stencil
   header on the pinned `zig cc` via gen-stencils-all, then
   `git diff --exit-code` the generated dir. Non-zero exit means the
   committed bytes drifted from what the pinned toolchain emits --
   regenerate with `./mino task gen-stencils-all` and commit.

   Unlike the old per-host `check-stencils-fresh`, this is byte-stable
   across machines because it pins one cross-compiling toolchain, so
   it CAN run in CI. It must run on a SINGLE pinned-Zig host, never on
   the per-OS matrix: compiler-version skew across matrix runners is
   exactly what forced the original determinism job to be removed."
  []
  (gen-stencils-all)
  (let [d (sh "git" "diff" "--exit-code"
              "--" "src/eval/bc/stencils/generated/")]
    (when-not (= 0 (get d :exit))
      (println (get d :out))
      (throw (ex-info "check-stencils-fresh-all: generated headers are stale"
                      {:exit (get d :exit)
                       :fix  "run `./mino task gen-stencils-all` and commit"}))))
  (println "  check-stencils-fresh-all: OK"))

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
                   ["safepoint.c"        "stencil_op_safepoint"]
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
     6. examples               -- every examples/embed_*.c builds and
                                   runs against the lib srcs
     7. mino-tests adv-test    -- IF mino-tests is cloned adjacent;
                                   skipped with a warn otherwise.

   Neither stencil-freshness check is part of this composite, but for
   different reasons. The old per-host `check-stencils-fresh`
   regenerates with the host `cc`, so its committed bytes would have
   to byte-match whatever clang version the runner ships; CI matrix
   runners (Apple clang 15 on macos-14, gcc on ubuntu-24.04) diverge
   from dev, so it is structurally incompatible with the matrix. The
   newer `check-stencils-fresh-all` IS byte-stable across hosts (it
   pins one cross-compiling `zig cc`), but it belongs on a single
   dedicated pinned-Zig CI job (`stencil-determinism` in ci.yml), NOT
   inside a gate that runs on every matrix host -- putting a
   determinism check on the matrix is exactly what broke the original
   job. Either way, CI correctness here is gated by test-suite + ASan
   + 4-way JIT parity, which catch the actual runtime impact of a
   stale stencil regardless of compiler version. See `.local/BUGS.md`
   for the architectural decision context.

   Exits 0 on a clean tree. Negative controls live in the cycle's
   .local/ status file -- this task is the positive control."
  []
  (check-reloc-mirror)
  (check-stencil-registry)
  (test-suite)
  (build-asan)
  ;; ASan suite in both JIT modes: the JIT's C side (emit / patcher /
  ;; helpers / invoke) is enabled in the sanitizer build via
  ;; jit-enable-flags, so eager mode pushes every test through the
  ;; native tier under ASan. Emitted machine code itself stays
  ;; uninstrumented (see sanitize-zig's boundary note).
  (run-suite-with-test-bin "./mino_asan" [])
  (run-suite-with-test-bin "./mino_asan" ["--jit=on"])
  (test-jit-parity)
  (examples)
  (examples-amalgam)
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
   ["safepoint.c"        "stencil_op_safepoint"]
   ["deopt_to_interp.c"  "stencil_op_deopt_to_interp"]])

(def ^:private stencil-targets
  "Per-target table driving stencil regeneration. Each entry maps a
   committed header's <arch>_<os> name to its `--target=` triple and
   any extra cflags. All five cross-compile from one host under a
   single `zig cc`: stencil sources are hermetic (only project headers
   plus clang-resource <stdint.h>/<stddef.h>), so no target needs a
   platform SDK or libc. x86_64 adds -mno-red-zone so the JIT region
   never aliases red-zone slots across a helper-call return (the
   Windows x64 ABI has no red zone either).

   Windows uses the -gnu environment, not -msvc: stencil objects are
   compiled -c only and never linked, x86_64 PE/COFF symbol naming is
   ABI-agnostic (no leading underscore -- see
   tools/stencil_extract/coff.c), and -gnu stays consistent with the
   Tier-2 mingw link. The extractor is compiler-agnostic (magic-byte
   dispatch) and unchanged."
  [{:name "arm64_darwin"   :triple "aarch64-macos"      :cflags []}
   {:name "x86_64_darwin"  :triple "x86_64-macos"       :cflags ["-mno-red-zone"]}
   {:name "arm64_linux"    :triple "aarch64-linux-gnu"  :cflags []}
   {:name "x86_64_linux"   :triple "x86_64-linux-gnu"   :cflags ["-mno-red-zone"]}
   {:name "x86_64_windows" :triple "x86_64-windows-gnu" :cflags ["-mno-red-zone"]}])

(defn check-zig-version
  "Hard-fail unless the stencil compiler is the pinned `zig cc`
   version (`zig-version-pin`). This is what makes regenerated bytes
   reproducible across hosts. Skipped with a note when STENCIL_CC
   overrides zig -- the legacy clang path opts out of byte
   reproducibility on purpose."
  []
  (if-not (= ["zig" "cc"] stencil-cc)
    (println (str "  check-zig-version: STENCIL_CC override ("
                  (str/join " " stencil-cc)
                  ") -- skipping zig version pin"))
    (let [r (sh "zig" "version")
          v (str/trim (str (get r :out)))]
      (when-not (= 0 (get r :exit))
        (throw (ex-info (str "check-zig-version: `zig version` failed -- "
                             "is the pinned Zig installed and on PATH?")
                        {:exit (get r :exit)})))
      (when-not (= v zig-version-pin)
        (throw (ex-info (str "check-zig-version: zig " v
                             " != pinned " zig-version-pin)
                        {:found  v
                         :pinned zig-version-pin
                         :fix    (str "install Zig " zig-version-pin
                                      " or set STENCIL_CC to override")})))
      (println (str "  check-zig-version: zig " v " OK")))))

(defn- gen-stencils-for
  "Compile every stencil source for the given target and extract bytes
   into src/eval/bc/stencils/generated/stencils_<name>.h. `compiler`
   is the cc argv vector (e.g. [\"zig\" \"cc\"]); `target-cflags` are
   the extra flags for this target (--target=<triple> and any
   per-target additions such as -mno-red-zone)."
  [name compiler target-cflags]
  (let [gen-dir   "src/eval/bc/stencils/generated"
        out-hdr   (str gen-dir "/stencils_" name ".h")
        tmpdir    (str "/tmp/mino-stencils-" name)
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
            (apply sh! (concat compiler base-args target-cflags
                               ["-c" src "-o" obj])))
          (if first?
            (sh! "./tools/stencil-extract" obj sym out-hdr)
            (sh! "./tools/stencil-extract" "--append" obj sym out-hdr))
          (recur rest false (conj compiled file)))))
    (println (str "  stencils -> " out-hdr))))

(defn- gen-stencils-target
  "Regenerate one target's committed header by name, using the
   configured stencil compiler (`stencil-cc`, default pinned zig cc)
   to cross-compile it. Builds the extractor first. The named
   gen-stencils-* tasks are thin wrappers over this."
  [name]
  (build-stencil-extract)
  (let [t (some #(when (= name (:name %)) %) stencil-targets)]
    (when-not t
      (throw (ex-info (str "gen-stencils-target: unknown target " name)
                      {:known (mapv :name stencil-targets)})))
    (gen-stencils-for name stencil-cc
                      (into [(str "--target=" (:triple t))] (:cflags t)))))

(defn gen-stencils-all
  "Regenerate every committed stencil header from one host using the
   pinned `zig cc`. Verifies the Zig version first (byte
   reproducibility depends on it), builds the extractor once, then
   cross-compiles each target in `stencil-targets`. This is the
   maintainer entry point that replaces running the five per-target
   tasks by hand. Normal builds and embedders never invoke this --
   they consume the committed bytes."
  []
  (check-zig-version)
  (build-stencil-extract)
  (doseq [{:keys [name triple cflags]} stencil-targets]
    (gen-stencils-for name stencil-cc
                      (into [(str "--target=" triple)] cflags)))
  (println "  gen-stencils-all: OK"))

(defn gen-stencils
  "Regenerate the arm64_darwin stencil header. Historically the host
   task on Apple Silicon; it now cross-compiles to aarch64-macos via
   `stencil-cc` (default pinned zig cc) like every other target, so it
   produces the same bytes from any host. Prefer `gen-stencils-all` to
   regenerate every target at once."
  []
  (gen-stencils-target "arm64_darwin"))

(defn gen-stencils-arm64-linux
  "Regenerate stencils_arm64_linux.h (aarch64-linux-gnu). Thin wrapper
   over gen-stencils-target; see `stencil-targets`."
  []
  (gen-stencils-target "arm64_linux"))

(defn gen-stencils-x86-64-linux
  "Regenerate stencils_x86_64_linux.h (x86_64-linux-gnu, -mno-red-zone).
   Thin wrapper over gen-stencils-target; see `stencil-targets`."
  []
  (gen-stencils-target "x86_64_linux"))

(defn gen-stencils-x86-64-darwin
  "Regenerate stencils_x86_64_darwin.h (x86_64-macos, -mno-red-zone).
   Thin wrapper over gen-stencils-target; see `stencil-targets`."
  []
  (gen-stencils-target "x86_64_darwin"))

(defn gen-stencils-x86-64-windows
  "Regenerate stencils_x86_64_windows.h (x86_64-windows-gnu COFF,
   -mno-red-zone). Thin wrapper over gen-stencils-target; see
   `stencil-targets`."
  []
  (gen-stencils-target "x86_64_windows"))

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

(defn test-crash-handler
  "Compile and run the crash-handler backtrace smoke. Exercises the
   portable unwinder primitive in src/runtime/crash_backtrace.h that
   the CLI crash handler uses instead of glibc's <execinfo.h>. The test
   is standalone (no lib srcs): it only needs -Isrc/runtime and the
   compiler unwinder runtime the default link already provides. Running
   it under a static musl build is what proves the rework keeps working
   backtraces where <execinfo.h> is absent."
  []
  (let [bin     "crash_handler_test"
        args    (into [cc] (concat cflags ldflags
                                   ["-o" bin "tests/crash_handler_test.c"]
                                   libs))]
    (println (str "  " (str/join " " args)))
    (apply sh! args)
    (println (sh! (str "./" bin)))))

;; ---- Examples ----

(def ^:private embed-examples
  ;; Each example builds against the lib srcs as a tiny smoke; a stale
  ;; reference to the public surface fails the build, which catches the
  ;; v0.151-era cascade-rot class of bug for future renames.
  ["examples/embed_gc.c"
   "examples/embed_gc_stress.c"
   "examples/embed_record.c"
   "examples/embed_multi_tenant_threads.c"])

(defn- compile-and-run-example
  "Compile one examples/embed_*.c against the lib srcs and run it.
   Output binary lives next to the source so cleanup matches the
   harness pattern used by test-embed."
  [src]
  (let [slash   (str/last-index-of src "/")
        base    (if slash (subs src (+ 1 slash)) src)
        bin     (str "examples/" (subs base 0 (- (count base) 2)))
        objs    (mapv src->obj lib-srcs)
        pthread (if windows? [] ["-pthread"])
        args    (into [cc] (concat cflags pthread ldflags
                                   ["-o" bin src]
                                   objs libs))]
    (println (str "  " (str/join " " args)))
    (apply sh! args)
    (println (sh! (str "./" bin)))))

(defn examples
  "Build and run every examples/embed_*.c against the lib srcs.
   Acts as a smoke that the public mino.h surface stays sufficient
   and that every published example tracks the current API."
  []
  (build)
  (doseq [src embed-examples]
    (println (str "--- " src " ---"))
    (compile-and-run-example src)))

(defn- compile-and-run-example-amalgam
  "Compile one examples/embed_*.c against the amalgamation TU at
   dist/mino.o (built by `amalgamate`) and run it. Verifies that the
   single-file amalgamation distribution is sufficient for the public
   surface used by every published example."
  [src]
  (let [slash   (str/last-index-of src "/")
        base    (if slash (subs src (+ 1 slash)) src)
        bin     (str "examples/" (subs base 0 (- (count base) 2)) "_amalgam")
        pthread (if windows? [] ["-pthread"])
        args    (into [cc] (concat ["-std=c99" "-O2" "-Idist"]
                                   pthread ldflags
                                   ["-o" bin src "dist/mino.o"]
                                   libs))]
    (println (str "  " (str/join " " args)))
    (apply sh! args)
    (println (sh! (str "./" bin)))))

(defn examples-amalgam
  "Build and run every examples/embed_*.c against the single-file
   amalgamation (`dist/mino.c`). Enforces that the amalgamation
   distribution surface is sufficient for the public examples."
  []
  (amalgamate)
  ;; Compile the amalgamation once; reuse the .o across examples.
  (let [args (into [cc] ["-std=c99" "-O2" "-Idist" "-c" "dist/mino.c"
                         "-o" "dist/mino.o"])]
    (println (str "  " (str/join " " args)))
    (apply sh! args))
  (doseq [src embed-examples]
    (println (str "--- " src " (amalgam) ---"))
    (compile-and-run-example-amalgam src)))

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
    "src/prim/module.c:mino_env *env" ;; load_ns_file -- multi-line signature; nested form-by-form loader
    "src/prim/module.c:mino_val *prim_require"}) ;; require -- spec parsing + loading + aliasing in one path

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
