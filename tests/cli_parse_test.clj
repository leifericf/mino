(require "tests/test")

;; mino.cli parse-opts: the babashka.cli option-parser shape over
;; plain strings and keyword spec maps. Open world: unknown options
;; parse, positionals ride along as metadata, defaults fill absent
;; keys, and every failed coercion throws a diagnostic carrying
;; :mino/kind :cli/parse.

(require '[mino.cli :as cli])

(def cli-spec
  {:port    {:coerce :long :alias :p}
   :ratio   {:coerce :double}
   :dry-run {:coerce :boolean}
   :from    {:coerce :keyword :alias :i}
   :mode    {:coerce :symbol}
   :cfg     {:coerce :edn}
   :label   {:coerce :string}})

(deftest long-option-takes-next-token-as-value
  (is (= {:port 1339} (cli/parse-opts ["--port" "1339"]
                                      {:spec {:port {:coerce :long}}}))))

(deftest long-option-equals-syntax
  (is (= {:foo "bar"} (cli/parse-opts ["--foo=bar"]))))

(deftest alias-resolves-to-the-option-name
  (is (= {:port 1339} (cli/parse-opts ["-p" "1339"] {:spec cli-spec}))))

(deftest standalone-flag-is-boolean-true-by-default
  (is (= {:verbose true} (cli/parse-opts ["--verbose"]))))

(deftest boolean-coercion-never-consumes-the-next-token
  (let [r (cli/parse-opts ["--dry-run" "file.txt"] {:spec cli-spec})]
    (is (= {:dry-run true} r))
    (is (= ["file.txt"] (:mino.cli/args (meta r))))))

(deftest unknown-option-consumes-its-value-open-world
  (is (= {:foo "bar"} (cli/parse-opts ["--foo" "bar"]))))

(deftest no-prefix-negates-any-option
  (is (= {:colors false} (cli/parse-opts ["--no-colors"])))
  (is (= {:colors false} (cli/parse-opts ["--no-colors"]
                                         {:spec {:colors {:default true}}}))))

(deftest grouped-short-flags-expand-to-booleans
  (is (= {:a true :b true :c true} (cli/parse-opts ["-abc"]))))

(deftest exact-alias-match-beats-per-char-splitting
  (let [spec {:verbose      {:alias :v}
              :very-verbose {:alias :vv}}]
    (is (= {:verbose true} (cli/parse-opts ["-v"] {:spec spec})))
    (is (= {:very-verbose true} (cli/parse-opts ["-vv"] {:spec spec})))))

(deftest defaults-fill-only-absent-keys
  (is (= {:port 80} (cli/parse-opts [] {:spec {:port {:default 80
                                                      :coerce :long}}})))
  (is (= {:port 8080} (cli/parse-opts ["--port" "8080"]
                                      {:spec {:port {:default 80
                                                     :coerce :long}}}))))

(deftest bare-dash-is-a-value-not-a-flag
  (is (= {:label "-"} (cli/parse-opts ["--label" "-"] {:spec cli-spec}))))

(deftest separator-moves-the-rest-to-args
  (let [r (cli/parse-opts ["--label" "x" "--" "--port" "1"]
                          {:spec cli-spec})]
    (is (= {:label "x"} r))
    (is (= ["--port" "1"] (:mino.cli/args (meta r))))))

(deftest positionals-ride-as-metadata-and-parsing-continues
  (let [r (cli/parse-opts ["foo" "--dry-run" "bar"] {:spec cli-spec})]
    (is (= {:dry-run true} r))
    (is (= ["foo" "bar"] (:mino.cli/args (meta r))))))

(deftest coercion-golden-vectors
  (is (= {:port 42} (cli/parse-opts ["--port" "42"] {:spec cli-spec})))
  (is (= {:ratio 1.5} (cli/parse-opts ["--ratio" "1.5"] {:spec cli-spec})))
  (is (= {:dry-run true} (cli/parse-opts ["--dry-run" "true"] {:spec cli-spec})))
  (is (= {:dry-run false} (cli/parse-opts ["--dry-run=false"] {:spec cli-spec})))
  (is (= {:from :edn} (cli/parse-opts ["--from" "edn"] {:spec cli-spec})))
  (is (= {:from :edn} (cli/parse-opts ["--from" ":edn"] {:spec cli-spec})))
  (is (= {:mode 'strict} (cli/parse-opts ["--mode" "strict"] {:spec cli-spec})))
  (is (= {:cfg [1 :a "s"]}
         (cli/parse-opts ["--cfg" "[1 :a \"s\"]"] {:spec cli-spec})))
  (is (= {:cfg :some-keyword}
         (cli/parse-opts ["--cfg" ":some-keyword"] {:spec cli-spec})))
  (is (= {:label "1339"} (cli/parse-opts ["--label" "1339"] {:spec cli-spec}))))

(deftest fn-coercion-is-called-with-the-string
  (is (= {:letter "a"}
         (cli/parse-opts ["--letter" "alpha"]
                          {:spec {:letter {:coerce (fn [s] (subs s 0 1))}}}))))

(deftest auto-coerce-covers-unknown-option-values
  (is (= {:num 1339 :kw :foo :bool false :str "bar"}
         (cli/parse-opts ["--num" "1339" "--kw" ":foo"
                          "--bool" "false" "--str" "bar"]))))

(deftest long-coercion-failure-throws-cli-parse
  (try
    (cli/parse-opts ["--port" "abc"] {:spec cli-spec})
    (is false "expected a throw")
    (catch e
      (is (= :cli/parse (:mino/kind e)))
      (is (= :port (:option (ex-data e))))
      (is (= "abc" (:value (ex-data e)))))))

(deftest long-coercion-failure-carries-classified-kind
  ;; ADR 37: the thrown diagnostic classifies as :cli/parse on
  ;; :mino/kind, the dispatch axis a classed catch reads.
  (is (= :cli/parse (try (cli/parse-opts ["--port" "abc"] {:spec cli-spec})
                         (catch e (:mino/kind e))))))

(deftest long-coercion-failure-dispatches-classed-catch
  ;; A classed catch on the promoted kind fires; a non-matching class
  ;; would let the throw escape.
  (is (= :caught (try (cli/parse-opts ["--port" "abc"] {:spec cli-spec})
                      (catch :cli/parse _ :caught)))))

(deftest boolean-coercion-failure-throws-cli-parse
  (try
    (cli/parse-opts ["--dry-run=maybe"] {:spec cli-spec})
    (is false "expected a throw")
    (catch e
      (is (= :cli/parse (:mino/kind e)))
      (is (= :dry-run (:option (ex-data e)))))))

(deftest edn-coercion-failure-throws-cli-parse
  (try
    (cli/parse-opts ["--cfg" "(unclosed"] {:spec cli-spec})
    (is false "expected a throw")
    (catch e
      (is (= :cli/parse (:mino/kind e)))
      (is (= :cfg (:option (ex-data e)))))))

(deftest bad-spec-shape-throws-cli-parse
  ;; spec-entries rejects a non-map, non-vector :spec with the same
  ;; :cli/parse class and the offending value under :spec in :mino/data.
  (is (= :cli/parse (try (cli/parse-opts ["--x" "1"] {:spec 42})
                         (catch e (:mino/kind e)))))
  (is (= 42 (try (cli/parse-opts ["--x" "1"] {:spec 42})
                 (catch e (:spec (ex-data e))))))
  (is (= :caught (try (cli/parse-opts ["--x" "1"] {:spec 42})
                      (catch :cli/parse _ :caught)))))

(deftest fn-coercion-returning-nil-throws-cli-parse
  (is (thrown? (cli/parse-opts ["--letter" "alpha"]
                               {:spec {:letter {:coerce (fn [_] nil)}}}))))

(deftest auto-coerce-leaves-non-numbers-as-strings
  ;; A digit-leading value that is not a valid long or double stays a
  ;; string; auto-coercion never evaluates the reader, so no arithmetic
  ;; or reader fault escapes for hostile input.
  (is (= {:x "1/0"}  (cli/parse-opts ["--x" "1/0"])))
  (is (= {:x "3abc"} (cli/parse-opts ["--x" "3abc"])))
  (is (= {:x "bar-1"} (cli/parse-opts ["--x" "bar-1"])))
  (is (= {:x 1339}   (cli/parse-opts ["--x" "1339"])))
  (is (= {:x 1.5}    (cli/parse-opts ["--x" "1.5"]))))

(deftest short-alias-carries-an-attached-value
  ;; -p8080 is the getopt attached-value form for the value-taking
  ;; alias :p, not three boolean flags.
  (is (= {:port 8080}
         (cli/parse-opts ["-p8080"] {:spec {:port {:alias :p :coerce :long}}})))
  (is (= {:from :edn}
         (cli/parse-opts ["-iedn"] {:spec cli-spec}))))

(run-tests-and-exit)
