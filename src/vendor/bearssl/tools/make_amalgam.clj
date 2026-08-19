;; Assemble the single-TU BearSSL TLS-client amalgam from the vendored
;; v0.6 tree at src/vendor/bearssl/.
;;
;; Usage: ./mino task bearssl-amalgam
;;   (or require this namespace and call run / generate-text)
;;
;; Concatenation order: public headers (bearssl.h include order, local
;; includes stripped, system includes retained in place), config.h,
;; inner.h, then the client-relevant .c units each preceded by a #line
;; marker and with file-local identifiers suffixed _u<idx> so the
;; independent TUs coexist in one translation unit.
;;
;; Deterministic: no timestamps, sorted directory walks, stable paste
;; order. Run from the repo root (paths are cwd-relative, like every
;; mino task). tests/bearssl_amalgam_test.clj pins the pure rename
;; core over a sample TU and gates byte-identical regeneration against
;; the committed bearssl_client.c.

(ns vendor.bearssl.tools.make-amalgam)

(require '[clojure.string :as str])

(def ^:private vendor-root "src/vendor/bearssl")
(def ^:private out-path "src/vendor/bearssl/bearssl_client.c")

;; Type and storage keywords never part of a declared name.
(def ^:private kw
  #{"static" "const" "unsigned" "signed" "int" "uint" "char"
    "void" "long" "short" "uint8_t" "uint16_t" "uint32_t"
    "uint64_t" "int8_t" "int16_t" "int32_t" "int64_t" "size_t"
    "struct" "union" "enum" "typedef" "inline" "register"})

;; Units gcc rejects under the strict bootstrap flags: both multiply
;; through unsigned __int128 behind BR_64 gates, a correct-vendored-
;; code pedwarn under -std=c99 -Wpedantic. Scoped per unit so every
;; other pasted unit stays under full pedantic checking; guarded so
;; MSVC never sees a GCC pragma.
(def ^:private pedantic-relief-units
  #{"src/int/i62_modpow2.c" "src/symcipher/poly1305_ctmulq.c"})

(def ^:private headers
  ["inc/bearssl.h"
   "inc/bearssl_hash.h" "inc/bearssl_hmac.h" "inc/bearssl_kdf.h"
   "inc/bearssl_block.h" "inc/bearssl_prf.h" "inc/bearssl_rand.h"
   "inc/bearssl_aead.h" "inc/bearssl_rsa.h" "inc/bearssl_ec.h"
   "inc/bearssl_x509.h" "inc/bearssl_ssl.h"])

(def ^:private codec
  ["ccopy" "dec16be" "dec16le" "dec32be" "dec32le" "dec64be"
   "dec64le" "enc16be" "enc16le" "enc32be" "enc32le" "enc64be"
   "enc64le"])

(def ^:private ssl
  ["prf" "prf_md5sha1" "prf_sha256" "prf_sha384"
   "ssl_ccert_single_ec" "ssl_ccert_single_rsa"
   "ssl_client" "ssl_client_default_rsapub" "ssl_client_full"
   "ssl_engine" "ssl_engine_default_aescbc" "ssl_engine_default_aesccm"
   "ssl_engine_default_aesgcm" "ssl_engine_default_chapol"
   "ssl_engine_default_descbc"
   "ssl_engine_default_ec" "ssl_engine_default_ecdsa"
   "ssl_engine_default_rsavrfy" "ssl_hashes" "ssl_hs_client"
   "ssl_io" "ssl_keyexport" "ssl_lru"
   "ssl_rec_cbc" "ssl_rec_ccm" "ssl_rec_chapol" "ssl_rec_gcm"])

(def ^:private x509
  ["x509_decoder" "x509_knownkey" "x509_minimal" "x509_minimal_full"])

;; ---- line plumbing ----------------------------------------------------

(defn- split-body
  "Split body into lines at \n boundaries. The -1 limit keeps the
  trailing empty lines mino's split would otherwise drop, so
  join-body round-trips a body byte-exactly (multi-newline file
  tails included)."
  [body]
  (str/split body "\n" -1))

(defn- join-body
  "Inverse of split-body."
  [lines]
  (str/join "\n" lines))

(defn drop-pragma-comment-lines
  "Remove `#pragma comment(...)` lines (MSVC linker directives, which
  are unknown-pragma noise under clang/gcc). Inert on the current
  tree: the only unit that carried one was sysrng.c, excluded from
  the file list."
  [body]
  (join-body
    (filterv #(not (re-matches #"\s*#pragma comment\(.*\)\s*" %))
             (split-body body))))

(defn local-include-line?
  "True for a `#include \"...\"` (project-local, collapsed into the
  amalgam) line as opposed to a system `#include <...>`."
  [line]
  (some? (re-find #"^\s*#\s*include\s*\"[^\"]+\"" line)))

;; ---- identifier detection ---------------------------------------------

(defn- typedef-decl
  "Scan body from `start` (just past the typedef keyword) up to the
  `;` closing the declaration at brace depth 0. Returns
  [decl-text saw-brace?]. Written as nested ifs rather than cond:
  recur through cond bodies compiles to the slow non-loop path in
  mino's bytecode compiler, and this scan runs per typedef."
  [body start]
  (let [n (count body)]
    (loop [j start depth 0 saw-brace false]
      (if (>= j n)
        [(subs body start j) saw-brace]
        (let [c (char-at body j)]
          (if (= c "{")
            (recur (+ j 1) (+ depth 1) true)
            (if (= c "}")
              (recur (+ j 1) (- depth 1) saw-brace)
              (if (and (= c ";") (zero? depth))
                [(subs body start j) saw-brace]
                (recur (+ j 1) depth saw-brace)))))))))

(defn- typedef-tail-names
  "Declared names in a typedef declaration: the text after the
  closing brace when the typedef defines a tagged/anonymous
  struct-union-enum, else the whole declaration, with the
  struct/union/enum keywords blanked and storage keywords filtered
  out."
  [decl saw-brace]
  (let [tail (if saw-brace
               (let [r (str/last-index-of decl "}")]
                 (subs decl (+ r 1)))
               decl)
        tail (str/replace tail #"\b(?:struct|union|enum)\b" " ")]
    (filterv #(not (contains? kw %)) (re-seq #"[A-Za-z_]\w*" tail))))

(defn- static-frag
  "Accumulate the declaration fragment starting at the static line
  (which may carry its name on a continuation line) until it contains
  a `;` `=` `{` or `[` terminator. Returns [frag last-line-index]."
  [lines i]
  (let [n (count lines)]
    (loop [j i frag (nth lines i)]
      (if (and (not (re-find #"[;={\[]" frag)) (< (+ j 1) n))
        (recur (+ j 1) (str frag " " (str/trim (nth lines (+ j 1)))))
        [frag j]))))

(defn unit-local-names
  "File-local identifiers defined by a unit: macros, typedefs,
  static functions/objects (prototypes and definitions). Struct/union/
  enum tags are handled separately by rename-unit's tag pass.

  All passes use loop + if with recur in branch tails: recur through
  cond / when-let bodies compiles to mino's slow non-loop path, and
  these scans run over every line of every unit."
  [body]
  (let [lines (str/split body "\n")
        nlines (count lines)
        offsets (loop [i 0 off 0 acc []]
                  (if (= i nlines)
                    acc
                    (recur (+ i 1)
                           (+ off (count (nth lines i)) 1)
                           (conj acc off))))
        names (atom #{})]
    ;; #define macros.
    (loop [i 0]
      (if (< i nlines)
        (do
          (let [m (re-find #"^\s*#\s*define\s+([A-Za-z_]\w*)" (nth lines i))]
            (if m (swap! names conj (nth m 1)) nil))
          (recur (+ i 1)))
        nil))
    ;; Typedefs: the declared names live after the struct/union/enum
    ;; body (or in the plain declarator).
    (loop [i 0]
      (if (< i nlines)
        (do
          (let [m (re-find #"^\s*typedef\b" (nth lines i))]
            (if m
              (let [start (+ (nth offsets i) (count m))
                    [decl saw-brace] (typedef-decl body start)
                    tail-names (typedef-tail-names decl saw-brace)]
                (loop [k 0]
                  (if (< k (count tail-names))
                    (do (swap! names conj (nth tail-names k))
                        (recur (+ k 1)))
                    nil)))
              nil))
          (recur (+ i 1)))
        nil))
    ;; Statics: the declared name is the first identifier followed by
    ;; `(` `[` `=` or `;` in the accumulated fragment.
    (loop [i 0]
      (if (< i nlines)
        (let [m (re-find #"^\s*static\b" (nth lines i))]
          (if m
            (let [[frag j] (static-frag lines i)
                  fm (re-find #"([A-Za-z_]\w*)\s*(\(|\[|=|;)" frag)]
              (if (and fm (not (contains? kw (nth fm 1))))
                (do (swap! names conj (nth fm 1)) (recur (+ j 1)))
                (recur (+ j 1))))
            (recur (+ i 1))))
        nil))
    @names))

(defn rename-unit
  "Suffix every file-local identifier (then every struct/union/enum
  tag the unit defines) with `_suffix` so independent TUs coexist in
  one translation unit; per-TU semantics guarantee every reference is
  intra-unit. Longer names rename first so a prefix name can never
  mangle an already-suffixed longer one. Returns [renamed-body
  renamed-name-count]; the count covers the identifier pass only,
  tags excluded."
  [body suffix]
  (let [names (unit-local-names body)
        desc-by-length (fn [coll] (reverse (sort-by count coll)))
        rename (fn [b n]
                 (str/replace b
                              (re-pattern (str "\\b" n "\\b"))
                              (str n "_" suffix)))
        body-1 (reduce rename body (desc-by-length names))
        tags (->> (re-seq #"\b(?:struct|union|enum)\s+([A-Za-z_]\w*)\s*\{" body-1)
                  (mapv #(nth % 1))
                  distinct)
        body-2 (reduce rename body-1 (desc-by-length tags))]
    [body-2 (count names)]))

(defn min-max-rename
  "Rename inner.h's MIN/MAX static inlines to br_MIN/br_MAX: hosts
  commonly define MIN/MAX macros (sys/param.h) which collide with
  inner.h's inlines, and BearSSL never calls them beyond this TU.
  Only call sites (identifier immediately followed by the paren)
  are rewritten."
  [text]
  (-> text
      (str/replace #"\bMIN\b\(" "br_MIN(")
      (str/replace #"\bMAX\b\(" "br_MAX(")))

;; ---- paste -------------------------------------------------------------

(defn- basename
  [p]
  (subs p (+ (str/last-index-of p "/") 1)))

(defn- dir-c-files
  "Sorted .c file names directly under vendor-root/dir."
  [dir]
  (->> (file-seq (str vendor-root "/" dir))
       (filterv #(str/ends-with? % ".c"))
       (mapv basename)
       sort
       vec))

(defn- cfile-list
  "The client-relevant unit list, in paste order. Directory walks are
  sorted; name exclusions follow the trim list (server side, key
  generation). sysrng.c is excluded: src/prim/tls.c provides
  br_prng_seeder_system (getentropy / BCryptGenRandom) so no Windows
  advapi32 link is needed."
  []
  (let [int-files (dir-c-files "src/int")
        rsa-files (->> (dir-c-files "src/rsa")
                       (filterv #(not (str/includes? % "keygen")))
                       (mapv #(subs % 0 (- (count %) 2))))
        ec-files (->> (dir-c-files "src/ec")
                      (filterv #(not= % "ec_keygen.c"))
                      (mapv #(subs % 0 (- (count %) 2))))
        symc-files (->> (dir-c-files "src/symcipher")
                        (mapv #(subs % 0 (- (count %) 2))))
        hash-files (->> (dir-c-files "src/hash")
                        (mapv #(subs % 0 (- (count %) 2))))]
    (vec (concat
           (mapv #(str "src/codec/" % ".c") codec)
           (mapv #(str "src/hash/" % ".c") hash-files)
           (mapv #(str "src/int/" %) int-files)
           ["src/mac/hmac.c" "src/mac/hmac_ct.c"]
           ["src/rand/hmac_drbg.c"]
           (mapv #(str "src/rsa/" % ".c") rsa-files)
           (mapv #(str "src/ec/" % ".c") ec-files)
           (mapv #(str "src/symcipher/" % ".c") symc-files)
           ["src/aead/gcm.c" "src/aead/ccm.c"]
           (mapv #(str "src/ssl/" % ".c") ssl)
           (mapv #(str "src/x509/" % ".c") x509)
           ["src/settings.c"]))))

(defn- emit
  "Paste one vendored file into the chunk accumulator: drop #pragma
  comment lines, rename file-local identifiers when a unit suffix is
  given, then emit the section marker, the #line directive, and every
  line except project-local includes (system includes stay in place
  inside their platform guards). Returns the renamed-name count."
  [chunks path suffix]
  (let [body (drop-pragma-comment-lines (slurp (str vendor-root "/" path)))
        [body nren] (if suffix
                      (rename-unit body suffix)
                      [body 0])]
    (swap! chunks conj (str "\n/* === " path " === */\n"))
    (swap! chunks conj (str "#line 1 \"" path "\"\n"))
    (swap! chunks conj
           (join-body (filterv #(not (local-include-line? %))
                               (split-body body))))
    nren))

(defn- pedantic-relief
  "Open or close the scoped -Wpedantic suppression around one unit
  (see pedantic-relief-units)."
  [chunks path opening]
  (when (contains? pedantic-relief-units path)
    (if opening
      (swap! chunks conj
             (str "#if defined(__GNUC__) || defined(__clang__)\n"
                  "#pragma GCC diagnostic push\n"
                  "#pragma GCC diagnostic ignored \"-Wpedantic\"\n"
                  "#endif\n"))
      (swap! chunks conj
             (str "#if defined(__GNUC__) || defined(__clang__)\n"
                  "#pragma GCC diagnostic pop\n"
                  "#endif\n")))))

(def ^:private banner
  (str
    "/* BearSSL v0.6 (commit 8ef7680) TLS-client amalgam, generated\n"
    " * by tools/make_amalgam.clj (mino task bearssl-amalgam) from\n"
    " * the vendored tree in this directory. Not an upstream file.\n"
    " * MIT (c) 2016 Thomas Pornin <pornin@bolet.org>; see LICENSE.\n"
    " *\n"
    " * Per-unit local identifiers (statics, macros, typedefs) carry\n"
    " * an _u<idx> suffix so the independent TUs coexist in one\n"
    " * translation unit.\n"
    " *\n"
    " * Units that use x86/POWER8 intrinsics set BR_ENABLE_INTRINSICS\n"
    " * before including inner.h; with collapsed includes the global\n"
    " * define below replaces the per-unit ones (inert on other\n"
    " * arches).\n"
    " *\n"
    " * WIN32_LEAN_AND_MEAN is defined up front: the _WIN32 branches of\n"
    " * ssl_engine.c and x509_minimal.c include <windows.h>, and the\n"
    " * lean flag keeps winsock.h out of it (it would redefine-clash\n"
    " * with the winsock2.h the net layer includes, under MSVC).\n"
    " *\n"
    " * MIN/MAX renamed to br_MIN/br_MAX: hosts commonly define MIN/MAX\n"
    " * macros (sys/param.h) which collide with inner.h's inlines.\n"
    " */\n"
    "#define BR_ENABLE_INTRINSICS 1\n"
    "/* WIN32_LEAN_AND_MEAN: see the file-top note. Inert on non-Windows\n"
    " * hosts (windows.h is never included there).\n"
    " */\n"
    "#define WIN32_LEAN_AND_MEAN\n"
    "/* Collapsing the headers into this TU makes their unused static\n"
    " * inline helpers visible as main-file functions; silence that for\n"
    " * the header block only, under compilers that understand the\n"
    " * GCC diagnostic pragmas (unknown pragmas are ignored per C99\n"
    " * 6.10.6; MSVC never reaches these lines).\n"
    " */\n"
    "#if defined(__GNUC__) || defined(__clang__)\n"
    "#pragma GCC diagnostic push\n"
    "#pragma GCC diagnostic ignored \"-Wunused-function\"\n"
    "#endif\n"))

(defn- header-pop
  "Close the header block's -Wunused-function suppression."
  []
  (str "#if defined(__GNUC__) || defined(__clang__)\n"
       "#pragma GCC diagnostic pop\n"
       "#endif\n"))

(defn generate-text
  "Assemble the full amalgam text from the vendored tree.
  Deterministic: fixed paste order, sorted directory walks, no
  timestamps. Pure with respect to the repo (reads the vendored tree,
  writes nothing)."
  []
  (let [chunks (atom [])]
    (swap! chunks conj banner)
    (doseq [h headers]
      (emit chunks h nil))
    (emit chunks "src/config.h" nil)
    (emit chunks "src/inner.h" nil)
    (swap! chunks conj (header-pop))
    (doseq [[idx c] (map-indexed vector (cfile-list))]
      (pedantic-relief chunks c true)
      (emit chunks c (str "u" idx))
      (pedantic-relief chunks c false))
    (min-max-rename (str/join "" @chunks))))

(defn run
  "Regenerate src/vendor/bearssl/bearssl_client.c from the vendored
  tree and print the paste report. The mino task entry point; run
  from the repo root."
  []
  (let [cfiles (cfile-list)
        text (generate-text)]
    (spit out-path text)
    (println (str "file list (" (count cfiles) " C files):"))
    (doseq [c cfiles]
      (println (format "  %-42s %7d"
                       c (count (slurp (str vendor-root "/" c))))))
    (println (format "amalgam: %s (%d chars)" out-path (count text)))))
