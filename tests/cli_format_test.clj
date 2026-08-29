(require "tests/test")
(require '[clojure.string :as str])

;; mino.cli format-opts: the babashka.cli usage block rendered from
;; the spec. Alias, name, ref, and description columns align; the
;; default rides the description in parentheses. The exact output is
;; pinned as data.

(require '[mino.cli :as cli])

(def fmt-spec
  {:from   {:ref "<format>" :desc "The input format."
            :coerce :keyword :alias :i
            :default :edn :default-desc "edn"}
   :to     {:ref "<format>" :desc "The output format."
            :coerce :keyword :alias :o
            :default :json :default-desc "json"}
   :pretty {:desc "Pretty-print output." :alias :p}
   :paths  {:desc "Paths of files to transform."
            :default ["src" "test"] :default-desc "src test"}})

(def canonical-lines
  ["  -i, --from <format>  The input format. (default: edn)"
   "  -o, --to <format>    The output format. (default: json)"
   "      --paths          Paths of files to transform. (default: src test)"
   "  -p, --pretty         Pretty-print output."])

(deftest renders-the-canonical-usage-block
  (is (= (str/join "\n" canonical-lines)
         (cli/format-opts {:spec fmt-spec :order [:from :to :paths :pretty]}))))

(deftest map-spec-renders-the-same-lines-in-any-order
  (is (= (set canonical-lines)
         (set (str/split (cli/format-opts {:spec fmt-spec}) #"\n")))))

(deftest vector-spec-keeps-its-declared-order
  (is (= (str/join "\n" [(nth canonical-lines 3) (nth canonical-lines 0)])
         (cli/format-opts {:spec [[:pretty (:pretty fmt-spec)]
                                  [:from (:from fmt-spec)]]}))))

(deftest default-without-desc-prints-the-value
  (is (= "  -p, --port  (default: 80)"
         (cli/format-opts {:spec {:port {:alias :p :default 80}}}))))

(deftest negatable-renders-the-bracketed-name
  (is (= "      --[no-]colors  Colorize output."
         (cli/format-opts {:spec {:colors {:negatable true
                                           :desc "Colorize output."}}}))))

(deftest empty-spec-renders-the-empty-string
  (is (= "" (cli/format-opts {:spec {}}))))

(deftest order-naming-an-unknown-option-throws
  (try
    (cli/format-opts {:spec {:port {:default 80}} :order [:port :bogus]})
    (is false "expected a throw")
    (catch e
      (is (= :cli/parse (:mino/kind e)))
      (is (= :bogus (:option (ex-data e)))))))

(deftest order-naming-an-unknown-option-dispatches-classed-catch
  ;; ADR 37: format-opts routes :order misses through the same
  ;; :cli/parse class, so a classed catch fires.
  (is (= :caught (try (cli/format-opts {:spec {:port {:default 80}}
                                        :order [:port :bogus]})
                      (catch :cli/parse _ :caught)))))

(run-tests-and-exit)
