(require "tests/test")
(require '[clojure.string :as str])
(require '[vendor.bearssl.tools.make-amalgam :as ma])

;; Pure core of the BearSSL amalgam generator
;; (src/vendor/bearssl/tools/make_amalgam.clj), pinned over a tiny
;; committed sample TU (tests/fixtures/bearssl/sample_unit.c) plus a
;; reproducibility gate: regenerating the amalgam in memory must be
;; byte-identical with the committed bearssl_client.c, so the vendored
;; tree can never drift from the committed amalgam silently (the
;; ca_roots_test.clj drift-gate pattern).

(def ^:private sample-path "tests/fixtures/bearssl/sample_unit.c")
(def ^:private sample (slurp sample-path))
(def ^:private amalgam-path "src/vendor/bearssl/bearssl_client.c")

(deftest bearssl-amalgam-sample-file-exists
  (is (file-exists? sample-path) "the committed sample TU is checked in"))

(deftest bearssl-amalgam-unit-local-names
  (let [names (ma/unit-local-names sample)]
    (doseq [n ["ROTR32" "TLEN"                        ; macro defines
               "point_u" "point_ptr" "spair_t" "byte_t" ; typedefs
               "P" "add32" "table"]]                    ; statics
      (is (contains? names n) (str "detected " n)))
    (doseq [n ["sample_public"                          ; external linkage
               "tag_spair"                              ; tag names come
               "MIN" "MAX"                              ; via the tag pass
               "uint32_t" "unsigned" "char" "const" "static" "struct"
               "pt" "x" "y" "s"]]                       ; members/locals
      (is (not (contains? names n)) (str "not detected " n)))))

(deftest bearssl-amalgam-rename-unit
  (let [[renamed nren] (ma/rename-unit sample "u0")]
    (is (= 9 nren) "nine file-local names renamed (macros+typedefs+statics)")
    ;; statics, macros, typedef names: every reference intra-unit
    (doseq [tok ["add32_u0(" "table_u0[" "ROTR32_u0(" "P_u0["
                 "point_u_u0" "point_ptr_u0" "spair_t_u0" "byte_t_u0"
                 "TLEN_u0"]]
      (is (str/includes? renamed tok) (str "suffixed " tok)))
    ;; tag names renamed even though they are not in the name set
    (is (str/includes? renamed "struct tag_spair_u0 {")
        "struct tag suffixed by the tag pass")
    ;; keyword type names and externals untouched
    (doseq [tok ["uint32_t" "unsigned char" "sample_public(" "MIN(" "MAX("]]
      (is (str/includes? renamed tok) (str "left alone: " tok)))
    (is (not (str/includes? renamed "uint32_t_u0"))
        "keyword type names never suffixed")))

(deftest bearssl-amalgam-drop-pragma-comment-lines
  (let [out (ma/drop-pragma-comment-lines sample)]
    (is (not (str/includes? out "#pragma comment"))
        "MSVC #pragma comment linker directives are dropped")
    (is (str/includes? out "#define TLEN")
        "every other line survives the pragma filter")))

(deftest bearssl-amalgam-local-include-line
  (is (ma/local-include-line? "#include \"inner.h\"")
      "quoted include is project-local")
  (is (ma/local-include-line? "  #  include \"inner.h\"")
      "spaced include directive is still project-local")
  (is (not (ma/local-include-line? "#include <stdint.h>"))
      "angled include is system, kept in place")
  (is (not (ma/local-include-line? "int x;")) "not an include at all"))

(deftest bearssl-amalgam-min-max-rename
  (let [out (ma/min-max-rename "a = MIN(x, y) + MAX(z) MINIMUM MIN (q) xMAX(r);")]
    (is (str/includes? out "br_MIN(x, y)") "call sites become br_MIN")
    (is (str/includes? out "br_MAX(z)") "call sites become br_MAX")
    (is (str/includes? out "MINIMUM") "longer identifiers untouched")
    (is (str/includes? out "MIN (q)") "space before paren is not a call")
    (is (str/includes? out "xMAX(r)") "embedded MAX untouched")))

(deftest bearssl-amalgam-regenerates-byte-identical
  (let [committed (slurp amalgam-path)
        generated (ma/generate-text)]
    (is (= (count committed) (count generated))
        "amalgam size unchanged by regeneration")
    (is (= committed generated)
        "committed bearssl_client.c is byte-identical with a fresh regeneration")))

(run-tests-and-exit)
