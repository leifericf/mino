(require "tests/test")

;; Coverage test — measures how much of Clojure's core.* public
;; surface mino exposes.
;;
;; The manifest pins the canonical Clojure 1.12 surface for
;; clojure.core, clojure.string, clojure.set, clojure.walk,
;; clojure.edn, and clojure.zip. For each name the test asks the
;; var registry whether mino has it. The JVM-only set names forms
;; that exist in Clojure but cannot apply on a runtime without Java
;; (defrecord, deftype, instance?, :import, etc.) — these throw
;; :mino/unsupported and are accounted as "expected divergence", not
;; "missing".
;;
;; Regenerate the six expected-* sets from the canonical reference
;; surface that clojure-census captures (clojure/1.12.4-surface.edn):
;;
;;   bb tools/gen_coverage_manifest.bb [path/to/1.12.4-surface.edn]
;;
;; See docs/adr/19-census-as-source-of-truth.md.

(def expected-clojure-core
  '#{
     * *' *1 *2 *3 *agent* *allow-unresolved-vars* *assert* *clojure-version* *command-line-args* *compile-files* *compile-path*
     *compiler-options* *data-readers* *default-data-reader-fn* *e *err* *file* *flush-on-newline* *fn-loader* *in* *math-context* *ns* *out*
     *print-dup* *print-length* *print-level* *print-meta* *print-namespace-maps* *print-readably* *read-eval* *reader-resolver* *repl* *source-path* *suppress-read* *unchecked-math*
     *use-context-classloader* *verbose-defrecords* *warn-on-reflection* + +' - -' -> ->> ->ArrayChunk ->Eduction ->Vec
     ->VecNode ->VecSeq -cache-protocol-fn -reset-methods .. / < <= = == > >=
     EMPTY-NODE Inst NaN? PrintWriter-on StackTraceElement->vec Throwable->map abs accessor aclone add-classpath add-tap add-watch
     agent agent-error agent-errors aget alength alias all-ns alter alter-meta! alter-var-root amap ancestors
     and any? apply areduce array-map as-> aset aset-boolean aset-byte aset-char aset-double aset-float
     aset-int aset-long aset-short assert assoc assoc! assoc-in associative? atom await await-for await1
     bases bean bigdec bigint biginteger binding bit-and bit-and-not bit-clear bit-flip bit-not bit-or
     bit-set bit-shift-left bit-shift-right bit-test bit-xor boolean boolean-array boolean? booleans bound-fn bound-fn* bound?
     bounded-count butlast byte byte-array bytes bytes? case cast cat char char-array char-escape-string
     char-name-string char? chars chunk chunk-append chunk-buffer chunk-cons chunk-first chunk-next chunk-rest chunked-seq? class
     class? clear-agent-errors clojure-version coll? comment commute comp comparator compare compare-and-set! compile complement
     completing concat cond cond-> cond->> condp conj conj! cons constantly construct-proxy contains?
     count counted? create-ns create-struct cycle dec dec' decimal? declare dedupe default-data-readers definline
     definterface defmacro defmethod defmulti defn defn- defonce defprotocol defrecord defstruct deftype delay
     delay? deliver denominator deref derive descendants destructure disj disj! dissoc dissoc! distinct
     distinct? doall dorun doseq dosync dotimes doto double double-array double? doubles drop
     drop-last drop-while eduction empty empty? ensure ensure-reduced enumeration-seq error-handler error-mode eval even?
     every-pred every? ex-cause ex-data ex-info ex-message extend extend-protocol extend-type extenders extends? false?
     ffirst file-seq filter filterv find find-keyword find-ns find-protocol-impl find-protocol-method find-var first flatten
     float float-array float? floats flush fn fn? fnext fnil for force format
     frequencies future future-call future-cancel future-cancelled? future-done? future? gen-class gen-interface gensym get get-in
     get-method get-proxy-class get-thread-bindings get-validator group-by halt-when hash hash-combine hash-map hash-ordered-coll hash-set hash-unordered-coll
     ident? identical? identity if-let if-not if-some ifn? import in-ns inc inc' indexed?
     infinite? init-proxy inst-ms inst-ms* inst? instance? int int-array int? integer? interleave intern
     interpose into into-array ints io! isa? iterate iteration iterator-seq juxt keep keep-indexed
     key keys keyword keyword? last lazy-cat lazy-seq let letfn line-seq list list*
     list? load load-file load-reader load-string loaded-libs locking long long-array longs loop macroexpand
     macroexpand-1 make-array make-hierarchy map map-entry? map-indexed map? mapcat mapv max max-key memfn
     memoize merge merge-with meta method-sig methods min min-key mix-collection-hash mod munge name
     namespace namespace-munge nat-int? neg-int? neg? newline next nfirst nil? nnext not not-any?
     not-empty not-every? not= ns ns-aliases ns-imports ns-interns ns-map ns-name ns-publics ns-refers ns-resolve
     ns-unalias ns-unmap nth nthnext nthrest num number? numerator object-array odd? or parents
     parse-boolean parse-double parse-long parse-uuid partial partition partition-all partition-by partitionv partitionv-all pcalls peek
     persistent! pmap pop pop! pop-thread-bindings pos-int? pos? pr pr-str prefer-method prefers primitives-classnames
     print print-ctor print-dup print-method print-simple print-str printf println println-str prn prn-str promise
     proxy proxy-call-with-super proxy-mappings proxy-name proxy-super push-thread-bindings pvalues qualified-ident? qualified-keyword? qualified-symbol? quot rand
     rand-int rand-nth random-sample random-uuid range ratio? rational? rationalize re-find re-groups re-matcher re-matches
     re-pattern re-seq read read+string read-line read-string reader-conditional reader-conditional? realized? record? reduce reduce-kv
     reduced reduced? reductions ref ref-history-count ref-max-history ref-min-history ref-set refer refer-clojure reify release-pending-sends
     rem remove remove-all-methods remove-method remove-ns remove-tap remove-watch repeat repeatedly replace replicate require
     requiring-resolve reset! reset-meta! reset-vals! resolve rest restart-agent resultset-seq reverse reversible? rseq rsubseq
     run! satisfies? second select-keys send send-off send-via seq seq-to-map-for-destructuring seq? seqable? seque
     sequence sequential? set set-agent-send-executor! set-agent-send-off-executor! set-error-handler! set-error-mode! set-validator! set? short short-array shorts
     shuffle shutdown-agents simple-ident? simple-keyword? simple-symbol? slurp some some-> some->> some-fn some? sort
     sort-by sorted-map sorted-map-by sorted-set sorted-set-by sorted? special-symbol? spit split-at split-with splitv-at str
     stream-into! stream-reduce! stream-seq! stream-transduce! string? struct struct-map subs subseq subvec supers swap!
     swap-vals! symbol symbol? sync tagged-literal tagged-literal? take take-last take-nth take-while tap> test
     the-ns thread-bound? time to-array to-array-2d trampoline transduce transient tree-seq true? type unchecked-add
     unchecked-add-int unchecked-byte unchecked-char unchecked-dec unchecked-dec-int unchecked-divide-int unchecked-double unchecked-float unchecked-inc unchecked-inc-int unchecked-int unchecked-long
     unchecked-multiply unchecked-multiply-int unchecked-negate unchecked-negate-int unchecked-remainder-int unchecked-short unchecked-subtract unchecked-subtract-int underive unquote unquote-splicing unreduced
     unsigned-bit-shift-right update update-in update-keys update-proxy update-vals uri? use uuid? val vals var-get
     var-set var? vary-meta vec vector vector-of vector? volatile! volatile? vreset! vswap! when
     when-first when-let when-not when-some while with-bindings with-bindings* with-in-str with-loading-context with-local-vars with-meta with-open
     with-out-str with-precision with-redefs with-redefs-fn xml-seq zero? zipmap
     })

(def expected-clojure-string
  '#{blank? capitalize ends-with? escape includes? index-of join
     last-index-of lower-case re-quote-replacement replace replace-first
     reverse split split-lines starts-with? trim trim-newline triml
     trimr upper-case})

(def expected-clojure-set
  '#{difference index intersection join map-invert project rename
     rename-keys select subset? superset? union})

(def expected-clojure-walk
  '#{keywordize-keys macroexpand-all postwalk postwalk-demo
     postwalk-replace prewalk prewalk-demo prewalk-replace
     stringify-keys walk})

(def expected-clojure-edn
  '#{read read-string})

(def expected-clojure-zip
  '#{append-child branch? children down edit end? insert-child
     insert-left insert-right left lefts make-node next node path
     prev remove replace right rights root rightmost leftmost seq-zip
     up vector-zip xml-zip zipper})

;; Names that exist in Clojure but cannot be honored on a runtime
;; without a Java host. mino throws :mino/unsupported on each.
(def jvm-only
  '#{;; class generation / object system
     proxy gen-class gen-interface definterface
     ancestors-class
     ;; Java-import surface
     import construct-proxy proxy-mappings proxy-name proxy-super
     update-proxy init-proxy get-proxy-class
     ;; agents (thread pool, JVM-coupled)
     agent agent-error agent-errors await await-for await1 send
     send-off send-via shutdown-agents release-pending-sends
     restart-agent set-agent-send-executor! set-agent-send-off-executor!
     set-error-handler! set-error-mode! error-handler error-mode
     binding-conveyor-fn clear-agent-errors
     ;; refs (STM, JVM-coupled)
     ref ref-history-count ref-max-history ref-min-history ref-set
     alter commute dosync ensure io! sync
     ;; Java arrays
     aget aset alength aclone amap areduce make-array to-array-2d
     aset-boolean aset-byte aset-char aset-double aset-float aset-int
     aset-long aset-short ints longs floats doubles shorts bytes booleans chars
     ;; classloader / JVM compile
     add-classpath load load-file load-reader compile compile-files
     load-string namespace-munge
     with-loading-context
     ;; primitives that wrap Java types
     byte short char cast
     ;; futures / threading (JVM-coupled)
     future future-call future-cancel future-cancelled? future-done?
     future? promise deliver
     ;; stream + JVM IO
     enumeration-seq iterator-seq line-seq xml-seq file-seq slurp spit
     read-line PrintWriter-on
     resultset-seq with-precision with-out-str with-in-str with-open
     locking monitor-enter monitor-exit
     ;; struct (deprecated, JVM-coupled)
     create-struct defstruct struct struct-map accessor
     ;; bigdec/biginteger (Java types)
     biginteger BigInt BigDecimal
     ;; bean / introspection
     bean class? supers
     ;; dynamic vars + dynvar utilities
     *agent* *command-line-args* *compile-files* *compile-path*
     *compiler-options* *err* *file* *fn-loader* *math-context* *out*
     *in* *print-length* *print-level* *print-meta*
     *print-namespace-maps* *reader-resolver* *source-path*
     *unchecked-math* *use-context-classloader* *verbose-defrecords*
     *warn-on-reflection* *flush-on-newline* *print-dup* *print-readably*
     *read-eval* *e *1 *2 *3
     ;; deprecated / JVM-only var getters and special-form interop
     find-protocol-impl find-protocol-method extends? extenders extend
     extend-protocol extend-type satisfies? memfn vector-of test
     mk-bound-fn comment quote-form
     ;; print machinery (depends on *out*)
     pr prn pr-str print print-str println println-str prn-str
     print-ctor print-simple print-dup print-method newline flush printf
     ;; numeric coercion to JVM types (unchecked-byte, unchecked-int,
     ;; etc.) — these only matter on a runtime that distinguishes
     ;; primitive int from long. mino has unchecked-add / unchecked-
     ;; subtract / unchecked-multiply / unchecked-inc / unchecked-dec
     ;; / unchecked-negate as the int64-wraparound family.
     unchecked-byte unchecked-char unchecked-divide-int
     unchecked-double unchecked-float unchecked-int unchecked-long
     unchecked-short unsigned-bit-shift-right
     ;; chunked seqs (JVM-only optimization)
     chunk chunk-append chunk-buffer chunk-cons chunk-first chunk-next
     chunk-rest chunked-seq?
     ;; Volatile (mino has volatile! but not the JVM-coupled internals)
     hash-combine
     ;; misc less-portable
     pcalls pmap pvalues iteration
     ;; JVM class introspection / name surface
     bases ns-imports record? long?
     ;; char-name / char-escape lookup tables (Java-side, low-priority)
     char-escape-string char-name-string
     ;; Auto-promoting +' -' *' inc' dec' siblings: in mino the plain
     ;; +/-/*/inc/dec auto-promote on long overflow, so a separate
     ;; quote-suffix variant is redundant and was removed in v0.78.0.
      ;; The fast int64 wraparound path is unchecked-+ / unchecked--
      ;; / unchecked-* / unchecked-inc / unchecked-dec.
      +' -' *' inc' dec'
      ;; Clojure 1.12 surface the canon dump surfaces that is
      ;; JVM-coupled: record constructors for the internal vector
      ;; and chunk types, internal protocol machinery, Java Stream
      ;; reducers, executor-pool config, primitive-classnames, JVM
      ;; interop threading (..) and proxy call support.
      ->ArrayChunk ->Vec ->VecNode ->VecSeq -cache-protocol-fn
      -reset-methods EMPTY-NODE method-sig primitives-classnames
      StackTraceElement->vec stream-into! stream-reduce!
      stream-seq! stream-transduce! *allow-unresolved-vars*
      *suppress-read* .. proxy-call-with-super})

;; Names that are special forms in Clojure (and mino). They are not
;; interned as vars in either runtime — find-var will not see them —
;; but every Clojure program can use them. Excluded from coverage so
;; the percentage reflects the var-shaped surface specifically.
(def special-forms
  ;; Names handled by mino's special-form dispatch rather than as
  ;; vars (a strict superset of Clojure's special forms — mino chose
  ;; to inline declare and defmacro because the macro system itself
  ;; depends on them).
  '#{def do if let fn quote var recur throw try catch finally new
     set! loop binding lazy-seq ns refer-clojure declare defmacro})

(defn- has-name? [ns-sym name-sym]
  (some? (find-var (symbol (str ns-sym) (str name-sym)))))

(defn coverage-report-_ [ns-sym expected jvm]
  (let [excluded (clojure.set/union jvm special-forms)
        portable (sort (vec (clojure.set/difference expected excluded)))
        present  (filterv (fn [n] (has-name? ns-sym n)) portable)
        missing  (filterv (fn [n] (not (has-name? ns-sym n))) portable)
        special  (clojure.set/intersection expected special-forms)
        jvmcount (count (clojure.set/intersection expected jvm))]
    {:ns         ns-sym
     :expected   (count expected)
     :portable   (count portable)
     :present    (count present)
     :missing    missing
     :jvm-only   jvmcount
     :specials   (count special)}))

(require '[clojure.set])

(defn print-coverage-_ [{:keys [ns portable present missing jvm-only specials]}]
  (let [pct (if (zero? portable) 0 (long (/ (* 100 present) portable)))]
    (println
      (str ns ": " present "/" portable " portable names (" pct "%)"
           "; " jvm-only " JVM-only excluded"
           ", " specials " special forms excluded"))
    (when (seq missing)
      (println (str "  missing: " missing)))))

(deftest clojure-core-coverage
  (let [report (coverage-report-_ 'clojure.core
                                  expected-clojure-core
                                  jvm-only)]
    (print-coverage-_ report)
    (testing "clojure.core portable surface ≥ 80% coverage"
      (let [pct (long (/ (* 100 (:present report)) (:portable report)))]
        (is (>= pct 80))))))

(deftest clojure-string-coverage
  (require '[clojure.string])
  (let [report (coverage-report-_ 'clojure.string
                                  expected-clojure-string
                                  #{})]
    (print-coverage-_ report)
    (testing "clojure.string portable surface ≥ 70% coverage"
      (let [pct (long (/ (* 100 (:present report)) (:portable report)))]
        (is (>= pct 70))))))

(deftest clojure-set-coverage
  (require '[clojure.set])
  (let [report (coverage-report-_ 'clojure.set
                                  expected-clojure-set
                                  #{})]
    (print-coverage-_ report)
    (testing "clojure.set portable surface ≥ 90% coverage"
      (let [pct (long (/ (* 100 (:present report)) (:portable report)))]
        (is (>= pct 90))))))

(deftest clojure-walk-coverage
  (require '[clojure.walk])
  (let [report (coverage-report-_ 'clojure.walk
                                  expected-clojure-walk
                                  '#{postwalk-demo prewalk-demo})]
    (print-coverage-_ report)
    (testing "clojure.walk portable surface ≥ 60% coverage"
      (let [pct (long (/ (* 100 (:present report)) (:portable report)))]
        (is (>= pct 60))))))

(deftest clojure-edn-coverage
  (require '[clojure.edn])
  (let [report (coverage-report-_ 'clojure.edn
                                  expected-clojure-edn
                                  #{})]
    (print-coverage-_ report)
    (testing "clojure.edn portable surface ≥ 50% coverage"
      (let [pct (long (/ (* 100 (:present report)) (:portable report)))]
        (is (>= pct 50))))))

(deftest clojure-zip-coverage
  (require '[clojure.zip])
  (let [report (coverage-report-_ 'clojure.zip
                                  expected-clojure-zip
                                  #{})]
    (print-coverage-_ report)
    (testing "clojure.zip portable surface ≥ 50% coverage"
      (let [pct (long (/ (* 100 (:present report)) (:portable report)))]
        (is (>= pct 50))))))

(run-tests-and-exit)
