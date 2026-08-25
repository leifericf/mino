;; Test runner: loads all test files and runs them.
;; Usage: ./mino tests/run.clj
;;
;; suite-mode is set to true around the require chain so each test
;; file's bottom (run-tests-and-exit) call is a no-op. The final
;; (run-tests-and-exit) runs the accumulated registry once,
;; end-to-end, and exits with 0 on success or 1 on any failure.
;;
;; Sharding (CI): the suite's resident set on glibc Linux grows
;; with every file loaded in the process (churn-driven old-gen
;; growth; tracked as the suite-RSS issue). On a memory-constrained
;; runner the full single-process run can exhaust the host, so the
;; CI lanes set MINO_TEST_SHARD=k/n and execute the partitions as
;; separate sequential processes; each partition's peak stays low
;; and the totals across partitions reconstruct the full suite.
;; Cut points are memory-balance-derived AND order-preserving:
;; shard 1 keeps every file through require_extended_test, which
;; depends on state from earlier loads. Unset (local runs): the
;; whole suite in one process, exactly as before.

(require "tests/test")
(require '[clojure.string :as str])

(def ^:private suite-files
  [   "tests/test"
   "tests/compat_test"
   "tests/arithmetic_test"
   "tests/binding_test"
   "tests/control_test"
   "tests/function_test"
   "tests/arity_strict_test"
   "tests/collection_test"
   "tests/string_test"
   "tests/sequence_test"
   "tests/lazy_test"
   "tests/lazy_seq_ns_scope_test"
   "tests/jvm_statics_test"
   "tests/clojure_version_test"
   "tests/inst_test"
   "tests/bytes_test"
   "tests/bits_test"
   "tests/macro_test"
   "tests/error_test"
   "tests/atom_test"
   "tests/stm_test"
   "tests/predicate_test"
   "tests/io_test"
   "tests/reflection_test"
   "tests/repl_test"
   "tests/repl_doc_smoke_test"
   "tests/gc_test"
   "tests/regression_bc_clause_params_gc"
   "tests/math_test"
   "tests/hash_compare_test"
   "tests/regex_test"
   "tests/tco_test"
   "tests/core_extra_test"
   "tests/core_misc_test"
   "tests/destructuring_test"
   "tests/reader_macros_test"
   "tests/phase3_test"
   "tests/finally_test"
   "tests/protocol_test"
   "tests/core_protocols_test"
   "tests/iteration_test"
   "tests/metadata_test"
   "tests/transducer_test"
   "tests/dialect_test"
   "tests/clj_sequences_test"
   "tests/clj_predicates_test"
   "tests/clj_control_test"
   "tests/clj_math_test"
   "tests/clj_higher_order_test"
   "tests/clj_transducer_test"
   "tests/clj_metadata_test"
   "tests/empty_list_test"
   "tests/error_path_test"
   "tests/bc_try_catch_test"
   "tests/bc_error_quality_test"
   "tests/jit_parity_test"
   "tests/jit_invalidation_test"
   "tests/bc_binding_test"
   "tests/bc_destructure_test"
   "tests/bc_queue_into_test"
   "tests/bc_closure_test"
   "tests/reduce_perf_test"
   "tests/bc_let_fold_test"
   "tests/bc_bitwise_test"
   "tests/ifn_test"
   "tests/stack_test"
   "tests/sorted_test"
   "tests/transient_test"
   "tests/print_method_test"
   "tests/pprint_test"
   "tests/cl_format_test"
   "tests/pprint_macro_test"
   "tests/print_dynvars_test"
   "tests/conformance_test"
   "tests/var_quote_test"
   "tests/reader_cond_test"
   "tests/ns_test"
   "tests/require_extended_test"
   "tests/var_test"
   "tests/ns_resolution_test"
   "tests/are_test"
   "tests/literal_test"
   "tests/char_test"
   "tests/numeric_tower_test"
   "tests/numeric_edges_test"
   "tests/collections_semantics_test"
   "tests/stateful_test"
   "tests/hierarchy_test"
   "tests/multimethod_test"
   "tests/clojure_string_test"
   "tests/tasks_test"
   "tests/records_test"
   "tests/values_safety_test"
   "tests/instant_template_test"
   "tests/data_test"
   "tests/walk_demo_test"
   "tests/data_readers_test"
   "tests/reader_conditional_test"
   "tests/queue_test"
   "tests/spec_test"
   "tests/spec_census_test"
   "tests/spec_fspec_test"
   "tests/spec_gen_alpha_test"
   "tests/spec_test_alpha_test"
   "tests/reducers_test"
   "tests/core_unify_test"
   "tests/core_cache_test"
   "tests/core_memoize_test"
   "tests/core_match_test"
   "tests/core_logic_test"
   "tests/core_logic_fd_test"
   "tests/core_logic_nominal_test"
   "tests/store_test"
   "tests/test_tap_junit_test"
   "tests/clojure_test_port_test"
   "tests/test_lib_flags_test"
   "tests/capability_metadata_test"
   "tests/async_smoke_test"
   "tests/parallel_calls_test"
   "tests/parallel_fold_test"
   "tests/test_check_shrinking_test"
   "tests/string_perf_test"
   "tests/regex_perf_test"
   "tests/pin_pressure_test"
   "tests/fs_test"
   "tests/path_test"
   "tests/path_match_test"
   "tests/path_glob_test"
   "tests/path_ns_test"
    "tests/json_test"
    "tests/json_perf_test"
     "tests/csv_test"
     "tests/csv_perf_test"
     "tests/toml_perf_test"
   "tests/proc_test"
   "tests/deps_test"
   "tests/introspection_test"
   "tests/census_surface_test"
   "tests/qualified_special_form_test"
   "tests/image_test"
   "tests/require_env_test"
   "tests/url_encode_test"
   "tests/url_parse_test"
    "tests/codec_test"
    "tests/digest_test"
    "tests/digest_hex_test"
    "tests/time_civil_test"
   "tests/time_parse_test"
   "tests/time_format_test"
   "tests/time_arith_test"
   "tests/time_ns_test"
   "tests/http_codec_test"
   "tests/redirect_test"
   "tests/gzip_test"
   "tests/net_test"
   "tests/tls_test"
   "tests/pool_test"
   "tests/http_request_test"
   "tests/http_ns_test"
    "tests/ca_roots_test"
    "tests/bearssl_amalgam_test"
    "tests/http_integration_test"
    "tests/cli_test"
    "tests/cli_parse_test"
    "tests/cli_format_test"
      "tests/env_test"
      "tests/terminal_test"
      "tests/term_test"
      "tests/term_progress_test"
       "tests/log_test"
        "tests/toml_test"
        "tests/yaml_test"
        "tests/yaml_perf_test"
        "tests/time_zone_test"])

(def ^:private shard-cuts
  "File-count prefix boundaries for MINO_TEST_SHARD partitions:
   shard k covers files [cuts[k-1], cuts[k]). Measured peaks on
   glibc (2026-08-21): 4.6GB / 4.0GB / 2.3GB. Rebalance when the
   tail grows past ~5GB. The final entry must equal the file count."
  [0 103 129 174])

(defn- parse-shard-int [s]
  (when (and (string? s) (re-find #"^\d+$" s))
    (read-string s)))

(defn- excluded-files []
  "MINO_TEST_EXCLUDE is a comma list of suite file basenames (e.g.
   net_test,tls_test) dropped from this run. Used by sanitizer
   lanes whose allocator/gc timing exposes the known parked-future
   teardown unsoundness (the tracker's forced-GC/futures issue) in
   exactly the tests that park socket futures; those files stay
   covered on every non-sanitized lane and under UBSan."
  (if-let [spec (getenv "MINO_TEST_EXCLUDE")]
    (set (filter #(not= % "")
                 (map str/trim (str/split spec #","))))
    #{}))

(defn- shard-files []
  (when (not= (peek shard-cuts) (count suite-files))
    (throw (ex-info "shard-cuts tail must equal the file count"
                    {:cuts (peek shard-cuts)
                     :files (count suite-files)})))
  (if-let [spec (getenv "MINO_TEST_SHARD")]
    (let [parts (str/split spec #"/")
          n (dec (count shard-cuts))]
      (when (not= 2 (count parts))
        (throw (ex-info "MINO_TEST_SHARD must look like 2/3"
                        {:got spec})))
      (let [k (parse-shard-int (first parts))
            total (parse-shard-int (second parts))]
        (when (or (nil? k) (nil? total) (not= total n)
                  (< k 1) (> k total))
          (throw (ex-info (str "MINO_TEST_SHARD out of range; this "
                               "runner partitions into " n)
                          {:got spec})))
        (subvec suite-files (nth shard-cuts (dec k))
                (nth shard-cuts k))))
    suite-files))

(reset! clojure.test/suite-mode true)

(let [skip (excluded-files)]
  (doseq [f (shard-files)]
    (let [base (last (str/split f #"/"))]
      (when-not (contains? skip base)
        (require f)))))

(reset! clojure.test/suite-mode false)

(run-tests-and-exit)
