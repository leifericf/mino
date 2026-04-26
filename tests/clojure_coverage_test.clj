(require "tests/test")

;; Coverage test — measures how much of Clojure's core.* public
;; surface mino exposes.
;;
;; The manifest below pins the canonical Clojure 1.11 surface for
;; clojure.core, clojure.string, clojure.set, clojure.walk,
;; clojure.edn, and clojure.zip. For each name the test asks the
;; var registry whether mino has it. The JVM-only set names forms
;; that exist in Clojure but cannot apply on a runtime without Java
;; (defrecord, deftype, instance?, :import, etc.) — these throw
;; :mino/unsupported and are accounted as "expected divergence", not
;; "missing".

(def expected-clojure-core
  ;; Clojure 1.11 public surface in clojure.core, sorted, with the
  ;; mino-runnable subset filtered to what's portable. Names that
  ;; are deeply Java-coupled appear in jvm-only-clojure-core below.
  '#{*' *1 *2 *3 *' *agent* *clojure-version* *command-line-args*
     *compile-files* *compile-path* *compiler-options* *e *err* *file*
     *flush-on-newline* *fn-loader* *in* *math-context* *ns* *out*
     *print-dup* *print-length* *print-level* *print-meta*
     *print-namespace-maps* *print-readably* *read-eval* *reader-resolver*
     *source-path* *unchecked-math* *use-context-classloader*
     *verbose-defrecords* *warn-on-reflection*
     + +' - -' / < <= = == > >=
     NaN? PrintWriter-on
     abs accessor aclone add-classpath add-tap add-watch
     agent agent-error agent-errors aget alength alias all-ns alter
     alter-meta! alter-var-root amap ancestors and any? apply apropos
     areduce array-map aset aset-boolean aset-byte aset-char aset-double
     aset-float aset-int aset-long aset-short assert assoc assoc!
     assoc-in associative? atom await await-for await1
     bases bean bigdec bigint biginteger binding binding-conveyor-fn
     bit-and bit-and-not bit-clear bit-flip bit-not bit-or bit-set
     bit-shift-left bit-shift-right bit-test bit-xor boolean
     boolean-array boolean? booleans bound-fn bound-fn* bound? butlast
     byte byte-array bytes bytes?
     case cast cat catch char char-array char-escape-string
     char-name-string char? chars chunk chunk-append chunk-buffer
     chunk-cons chunk-first chunk-next chunk-rest chunked-seq?
     class class? clear-agent-errors clojure-version coll? comment
     commute comp comparator compare compare-and-set! compile complement
     completing concat cond cond-> cond->> condp conj conj!
     cons constantly construct-proxy contains? count counted? create-ns
     create-struct cycle
     dec dec' decimal? declare dedupe def defmacro defmethod defmulti
     defn defn- defonce defprotocol defrecord defstruct deftype delay
     delay? deliver denominator deref derive descendants destructure
     disj disj! dissoc dissoc! distinct distinct? do doall doc dorun
     doseq dosync dotimes doto double double-array double? doubles drop
     drop-last drop-while
     eduction empty empty? ensure ensure-reduced enumeration-seq error-handler
     error-mode eval even? every-pred every? ex-cause ex-data ex-info
     ex-message extend extend-protocol extend-type extends? extenders
     false? ffirst file-seq filter filterv finally find find-keyword
     find-ns find-protocol-impl find-protocol-method find-var first
     flatten float float-array float? floats flush fn fn? fnext fnil
     for force format frequencies future future-call future-cancel
     future-cancelled? future-done? future?
     gen-class gen-interface gensym get get-in get-method get-proxy-class
     get-thread-bindings get-validator group-by
     hash hash-combine hash-map hash-ordered-coll hash-set
     hash-unordered-coll
     ident? identical? identity if-let if-not if-some ifn? import
     in-ns inc inc' indexed? infinite? init-proxy inst-ms inst?
     instance? int int-array int? integer? interleave intern interpose
     into into-array ints io! isa? iterate iteration iterator-seq
     juxt
     keep keep-indexed key keys keyword keyword? keys
     last lazy-cat lazy-seq let letfn line-seq list list* list? load
     load-file load-reader load-string loaded-libs locking long
     long-array long? longs loop
     macroexpand macroexpand-1 make-array make-hierarchy map map-entry?
     map-indexed map? mapcat mapv max max-key memfn memoize merge
     merge-with meta methods min min-key mix-collection-hash mod
     monitor-enter monitor-exit
     name namespace namespace-munge nat-int? neg-int? neg? new newline
     next nfirst nil? nnext not not-any? not-empty not-every? not=
     ns ns-aliases ns-imports ns-interns ns-map ns-name ns-publics
     ns-refers ns-resolve ns-unalias ns-unmap nth nthnext nthrest num
     number? numerator
     object-array odd? or
     parents parse-boolean parse-double parse-long parse-uuid partial
     partition partition-all partition-by partitionv partitionv-all
     pcalls peek persistent! pmap pop pop! pop-thread-bindings pos-int?
     pos? pr pr-str prefer-method prefers print print-ctor
     print-dup print-method print-simple print-str printf println
     println-str prn prn-str promise proxy proxy-mappings
     proxy-name proxy-super push-thread-bindings pvalues
     qualified-ident? qualified-keyword? qualified-symbol? quot
     quote
     rand rand-int rand-nth random-sample range ratio? rational?
     rationalize re-find re-groups re-matcher re-matches re-pattern
     re-seq read read-line read-string reader-conditional
     reader-conditional? realized? record? recur reduce reduce-kv
     reduced reduced? reductions ref ref-history-count ref-max-history
     ref-min-history ref-set refer refer-clojure reify release-pending-sends
     rem remove remove-all-methods remove-method remove-ns remove-tap
     remove-watch repeat repeatedly replace replicate require
     requiring-resolve reset! reset-meta! reset-vals! resolve
     restart-agent rest reverse reversible? rseq rsubseq run!
     satisfies? second select-keys send send-off send-via seq seq?
     seqable? sequence sequential? set set? set-error-handler!
     set-error-mode! set-validator! short short-array shorts
     short-array shutdown-agents simple-ident? simple-keyword?
     simple-symbol? slurp some some-> some->> some-fn some? sort
     sort-by sorted-map sorted-map-by sorted-set sorted-set-by sorted?
     special-symbol? spit split-at split-with splitv-at str string?
     struct struct-map subs subseq subvec supers swap! swap-vals!
     symbol symbol? sync
     tagged-literal tagged-literal? take take-last take-nth take-while
     tap> test the-ns thread-bound? throw time to-array to-array-2d
     trampoline transduce transient transient? tree-seq true? try type
     unchecked-add unchecked-byte unchecked-char unchecked-dec
     unchecked-divide-int unchecked-double unchecked-float unchecked-inc
     unchecked-int unchecked-long unchecked-multiply unchecked-negate
     unchecked-short unchecked-subtract unreduced
     unsigned-bit-shift-right update update-in update-keys update-vals
     update-proxy uri? use uuid?
     val vals var var-get var-set var? vary-meta vec vector vector-of
     vector? volatile! volatile? vreset! vswap!
     when when-first when-let when-not when-some while
     with-bindings with-bindings* with-in-str with-loading-context
     with-local-vars with-meta with-open with-out-str with-precision
     with-redefs with-redefs-fn xml-seq
     zero? zipmap})

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
     defrecord deftype reify proxy gen-class gen-interface definterface
     instance? bean class supers ancestors-class
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
     load-string namespace-munge bound? get-thread-bindings
     pop-thread-bindings push-thread-bindings thread-bound?
     with-bindings with-bindings* with-loading-context
     ;; primitives that wrap Java types
     byte short char cast
     ;; futures / threading (JVM-coupled)
     future future-call future-cancel future-cancelled? future-done?
     future? promise deliver
     ;; stream + JVM IO
     enumeration-seq iterator-seq line-seq xml-seq file-seq slurp spit
     read-line print-method print-dup PrintWriter-on
     resultset-seq with-precision with-out-str with-in-str with-open
     io! locking monitor-enter monitor-exit
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
     ;; with-precision / numeric coercion to JVM types
     unchecked-add unchecked-byte unchecked-char unchecked-dec
     unchecked-divide-int unchecked-double unchecked-float unchecked-inc
     unchecked-int unchecked-long unchecked-multiply unchecked-negate
     unchecked-short unchecked-subtract unsigned-bit-shift-right
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
     char-escape-string char-name-string})

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

(run-tests)
