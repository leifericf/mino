;; clojure.spec.alpha -- spec definitions and predicate composition.
;;
;; Mino port of the canonical surface. Spec values are maps tagged with
;; ::kind and dispatched via multimethods (conform-impl, explain-impl).
;; Macros at the bottom of the file expand to calls of the corresponding
;; *-impl builder fn so the same surface works inside and outside macro
;; context. Generators throw :mino/unsupported -- s/gen requires
;; clojure.test.check, deferred until a concrete user need lands.
;;
;; File ordering: ns -> registry -> helpers -> defmulti decls ->
;; dispatch methods -> all macros at the bottom.  This ordering is
;; load-bearing because mino's syntax-quote auto-qualifies bare symbols
;; against the runtime current-ns env.  If `def`, `and`, `or`, `cat`,
;; `*`, `+`, `?`, `keys`, `nilable`, `coll-of`, `map-of`, `tuple`,
;; `assert`, `fdef`, or `alt` are bound as macros mid-file, internal
;; (defn ...) / (def ...) calls expand to qualified forms that miss the
;; special-form dispatch and fire the macros instead, leaving the var
;; unbound.

(ns clojure.spec.alpha
  (:require [clojure.walk :as walk]))

(def ^:private registry-ref (atom {}))

(def invalid ::invalid)

(defn invalid? [v] (= v ::invalid))

(defn registry [] @registry-ref)

(defn get-spec
  "Return the spec registered under k, or nil."
  [k]
  (get @registry-ref k))

;; ---------------------------------------------------------------------------
;; Dispatch tables.
;; ---------------------------------------------------------------------------

(defmulti ^:private conform-impl
  (fn [spec _x] (::kind spec)))
(defmulti ^:private explain-impl
  (fn [spec _path _via _in _x] (::kind spec)))

(defn- spec? [x]
  (and (map? x) (contains? x ::kind)))

(defn- pred-spec
  "Wrap a predicate fn as a spec value."
  [form pred]
  {::kind ::pred ::form form ::pred pred})

(defn- as-spec
  "Resolve x to a spec map.  Registered keywords look up in the
  registry; map specs pass through; anything callable as a predicate
  (fn, set, map, vector, keyword used as fn) becomes a pred spec."
  [x]
  (cond
    (spec? x)              x
    (and (keyword? x)
         (get @registry-ref x))
                           (get @registry-ref x)
    (ifn? x)               (pred-spec x x)
    :else                  (throw (ex-info (str "Unable to coerce to spec: " (pr-str x))
                                           {:spec x}))))

(defn conform*
  "Internal dispatch for conform.  Public callers use conform."
  [spec x]
  (conform-impl spec x))

(defn explain*
  "Internal dispatch for explain-data.  Public callers use
  explain-data / explain / explain-str."
  [spec path via in x]
  (explain-impl spec path via in x))

;; ---------------------------------------------------------------------------
;; Public conform / valid? / explain entry points.
;; ---------------------------------------------------------------------------

(defn conform
  "Conform x against spec.  Returns the conformed value or
  :clojure.spec.alpha/invalid."
  [spec x]
  (conform* (as-spec spec) x))

(defn valid?
  "Return true iff x conforms to spec."
  [spec x]
  (not= ::invalid (conform spec x)))

(defn explain-data
  "Return a problem map describing why x fails spec, or nil if it
  passes.  Shape: {::problems [{:path ... :pred ... :val ... :via ...
  :in ...}] ::spec ... ::value ...}."
  [spec x]
  (let [s        (as-spec spec)
        problems (explain* s [] [] [] x)]
    (when (seq problems)
      {::problems problems
       ::spec     spec
       ::value    x})))

(defn- problem-str [p]
  (str "  val: "  (pr-str (:val p))
       " fails"
       (when (seq (:in p))   (str " in: " (pr-str (:in p))))
       (when (seq (:via p))  (str " spec: " (pr-str (last (:via p)))))
       (when (seq (:path p)) (str " at: " (pr-str (:path p))))
       " predicate: " (pr-str (:pred p))))

(defn explain-str
  "Return a string describing why x fails spec, or \"Success!\\n\" if
  it passes."
  [spec x]
  (if-let [data (explain-data spec x)]
    (apply str
           (interpose "\n"
                      (concat (map problem-str (::problems data))
                              [""])))
    "Success!\n"))

(defn explain
  "Print explain-str to *out*."
  [spec x]
  (print (explain-str spec x))
  (flush))

(defn form
  "Return the form of the spec, or :clojure.spec.alpha/unknown if x is
  not a registered spec."
  [x]
  (cond
    (keyword? x)             (when-let [s (get @registry-ref x)]
                               (or (::form s) ::unknown))
    (and (map? x) (::form x)) (::form x)
    :else                    ::unknown))

(defn abbrev
  "Return an abbreviated description of a spec form. Strips namespace
  qualifiers from symbols and shortens (fn [%] body) to body."
  [form]
  (cond
    (seq? form)
    (walk/postwalk
      (fn [f]
        (cond
          (and (symbol? f) (namespace f))
          (symbol (name f))

          (and (seq? f) (= 'fn (first f)) (= '[%] (second f)))
          (last f)

          :else f))
      form)

    (symbol? form)
    (symbol (name form))

    :else form))

(defn describe
  "Return an abbreviated description of the spec as data."
  [spec]
  (abbrev (form spec)))

;; ---------------------------------------------------------------------------
;; pred -- bare predicates promoted to specs.
;; ---------------------------------------------------------------------------

(defmethod conform-impl ::pred [s x]
  (if ((::pred s) x) x ::invalid))

(defmethod explain-impl ::pred [s path via in x]
  (when-not ((::pred s) x)
    [{:path path :pred (::form s) :val x :via via :in in}]))

(defn spec-impl
  "Build a spec value from a form and predicate.  When pred resolves to
  a regex spec (cat / * / + / ? / alt / a registered regex), the result
  wraps the regex so it conforms an element value as a whole sequence
  rather than splicing into the surrounding regex context."
  [form pred]
  (if-let [reg (as-regex-spec pred)]
    {::kind ::wrap ::form form ::wrapped reg}
    (pred-spec form pred)))

(defmethod conform-impl ::wrap [s x]
  (let [inner (::wrapped s)]
    (cond
      (and (sequential? x) (regex-spec? inner))
      (let [[r rem] (re-consume inner (seq x))]
        (cond
          (= ::invalid r) ::invalid
          (seq rem)       ::invalid
          :else           r))

      (regex-spec? inner)
      ::invalid

      :else
      (conform* (as-spec inner) x))))

(defmethod explain-impl ::wrap [s path via in x]
  (cond
    (and (sequential? x) (regex-spec? (::wrapped s)))
    (explain-impl (::wrapped s) path via in x)

    (regex-spec? (::wrapped s))
    [{:path path :pred 'sequential? :val x :via via :in in}]

    :else
    (explain-impl (as-spec (::wrapped s)) path via in x)))

;; ---------------------------------------------------------------------------
;; def-impl -- registry intern.
;; ---------------------------------------------------------------------------

(defn def-impl
  "Register the spec under k.  k must be a namespaced keyword or symbol."
  [k form spec]
  (when-not (or (and (keyword? k) (namespace k))
                (symbol? k))
    (throw (ex-info "k must be namespaced keyword or fully-qualified symbol"
                    {:k k})))
  (let [s (cond
            (spec? spec) (assoc spec ::name k ::form form)
            (fn? spec)   (assoc (pred-spec form spec) ::name k)
            (keyword? spec) (assoc (or (get @registry-ref spec)
                                       (throw (ex-info
                                               (str "Unable to resolve spec: " spec)
                                               {:spec spec})))
                                   ::name k ::form form)
            :else (throw (ex-info "spec must be a spec, predicate fn, or registered key"
                                  {:spec spec})))]
    (swap! registry-ref assoc k s)
    k))

;; ---------------------------------------------------------------------------
;; and -- chain conformers left to right.
;; ---------------------------------------------------------------------------

(defn and-spec-impl [forms specs]
  {::kind ::and ::forms forms ::specs specs
   ::form (cons 'clojure.spec.alpha/and forms)})

(defmethod conform-impl ::and [s x]
  (loop [v x specs (::specs s)]
    (if (empty? specs)
      v
      (let [r (conform* (as-spec (first specs)) v)]
        (if (= ::invalid r) ::invalid (recur r (rest specs)))))))

(defmethod explain-impl ::and [s path via in x]
  (loop [v x specs (::specs s) forms (::forms s)]
    (when (seq specs)
      (let [sp (as-spec (first specs))
            r  (conform* sp v)]
        (if (= ::invalid r)
          (explain* sp path via in v)
          (recur r (rest specs) (rest forms)))))))

;; ---------------------------------------------------------------------------
;; or -- branch with named tags.
;; ---------------------------------------------------------------------------

(defn or-spec-impl [keys forms specs]
  {::kind ::or ::keys keys ::forms forms ::specs specs
   ::form (cons 'clojure.spec.alpha/or (interleave keys forms))})

(defmethod conform-impl ::or [s x]
  (loop [keys (::keys s) specs (::specs s)]
    (if (empty? keys)
      ::invalid
      (let [r (conform* (as-spec (first specs)) x)]
        (if (= ::invalid r)
          (recur (rest keys) (rest specs))
          [(first keys) r])))))

(defmethod explain-impl ::or [s path via in x]
  (when (= ::invalid (conform-impl s x))
    (mapcat (fn [k sp f]
              (explain* (as-spec sp) (conj path k) via in x))
            (::keys s) (::specs s) (::forms s))))

;; ---------------------------------------------------------------------------
;; nilable -- spec-or-nil.
;; ---------------------------------------------------------------------------

(defn nilable-impl [form spec]
  {::kind ::nilable
   ::form (list 'clojure.spec.alpha/nilable form)
   ::inner-form form ::spec spec})

(defmethod conform-impl ::nilable [s x]
  (if (nil? x) nil (conform* (as-spec (::spec s)) x)))

(defmethod explain-impl ::nilable [s path via in x]
  (when-not (nil? x)
    (let [r (conform* (as-spec (::spec s)) x)]
      (when (= ::invalid r)
        (concat (explain* (as-spec (::spec s))
                          (conj path :clojure.spec.alpha/pred) via in x)
                [{:path (conj path :clojure.spec.alpha/nil)
                  :pred 'nil? :val x :via via :in in}])))))

;; ---------------------------------------------------------------------------
;; tuple -- fixed-length, position-indexed.
;; ---------------------------------------------------------------------------

(defn tuple-impl [forms specs]
  {::kind ::tuple ::forms forms ::specs specs
   ::form (cons 'clojure.spec.alpha/tuple forms)})

(defmethod conform-impl ::tuple [s x]
  (let [specs (::specs s)
        n     (count specs)]
    (if (and (vector? x) (= n (count x)))
      (loop [out [] i 0]
        (if (= i n)
          out
          (let [r (conform* (as-spec (nth specs i)) (nth x i))]
            (if (= ::invalid r)
              ::invalid
              (recur (conj out r) (inc i))))))
      ::invalid)))

(defmethod explain-impl ::tuple [s path via in x]
  (cond
    (not (vector? x))
    [{:path path :pred 'vector? :val x :via via :in in}]

    (not= (count (::specs s)) (count x))
    [{:path path
      :pred (list '= (count (::specs s)) '(count %))
      :val  x :via via :in in}]

    :else
    (apply concat
           (map-indexed
             (fn [i [sp f]]
               (let [v (nth x i)
                     r (conform* (as-spec sp) v)]
                 (when (= ::invalid r)
                   (explain* (as-spec sp) (conj path i) via (conj in i) v))))
             (map vector (::specs s) (::forms s))))))

;; ---------------------------------------------------------------------------
;; coll-of / map-of via every-impl.
;; ---------------------------------------------------------------------------

(defn every-impl
  [form spec opts]
  (merge {::kind ::every ::form form ::spec spec
          ::kind-pred (:kind opts)
          ::count (:count opts)
          ::min-count (:min-count opts)
          ::max-count (:max-count opts)
          ::distinct (:distinct opts)
          ::into (:into opts)}
         opts))

(defmethod conform-impl ::every [s x]
  (let [kp (::kind-pred s)
        c  (::count s)
        mn (::min-count s)
        mx (::max-count s)
        d  (::distinct s)]
    (cond
      (not (coll? x)) ::invalid
      (and kp (not (kp x))) ::invalid
      (and c (not= c (count x))) ::invalid
      (and mn (< (count x) mn)) ::invalid
      (and mx (> (count x) mx)) ::invalid
      (and d (not= (count x) (count (set x)))) ::invalid
      :else
      (let [sp  (as-spec (::spec s))
            ok? (every? (fn [v] (not= ::invalid (conform* sp v))) x)]
        (if ok?
          (let [conformed (map (fn [v] (conform* sp v)) x)
                target    (::into s)]
            (cond
              (vector? target) (vec conformed)
              (set? target)    (set conformed)
              (list? target)   (apply list conformed)
              (vector? x)      (vec conformed)
              (set? x)         (set conformed)
              :else            (apply list conformed)))
          ::invalid)))))

(defmethod explain-impl ::every [s path via in x]
  (let [kp (::kind-pred s)
        c  (::count s)
        mn (::min-count s)
        mx (::max-count s)]
    (cond
      (not (coll? x))
      [{:path path :pred 'coll? :val x :via via :in in}]

      (and kp (not (kp x)))
      [{:path path :pred (::form s) :val x :via via :in in}]

      (and c (not= c (count x)))
      [{:path path :pred (list '= c '(count %)) :val x :via via :in in}]

      (and mn (< (count x) mn))
      [{:path path :pred (list '<= mn '(count %)) :val x :via via :in in}]

      (and mx (> (count x) mx))
      [{:path path :pred (list '>= mx '(count %)) :val x :via via :in in}]

      :else
      (let [sp (as-spec (::spec s))]
        (apply concat
               (map-indexed
                 (fn [i v]
                   (let [r (conform* sp v)]
                     (when (= ::invalid r)
                       (explain* sp (conj path i) via (conj in i) v))))
                 x))))))

(defn coll-of-impl [form spec opts]
  (every-impl (cons 'clojure.spec.alpha/coll-of (cons form (mapcat identity opts)))
              spec opts))

(defn map-of-impl [k-form v-form k-spec v-spec opts]
  (let [pair-spec (tuple-impl [k-form v-form] [k-spec v-spec])
        opts'     (assoc opts :kind map?)]
    (every-impl (list 'clojure.spec.alpha/map-of k-form v-form)
                pair-spec opts')))

;; ---------------------------------------------------------------------------
;; keys -- map shape.  :req / :opt are namespaced keys (resolved via
;; the registry); :req-un / :opt-un are unqualified versions
;; (registered under qualified key, looked up by unqualified).
;; ---------------------------------------------------------------------------

(defn- unqualified-key
  "Return the unqualified keyword for a registered (qualified) key."
  [k]
  (keyword (name k)))

(defn keys-impl [forms opts]
  {::kind ::keys
   ::form (cons 'clojure.spec.alpha/keys
                (mapcat (fn [[k v]] [k v]) opts))
   ::req      (:req opts)
   ::opt      (:opt opts)
   ::req-un   (:req-un opts)
   ::opt-un   (:opt-un opts)})

(defmethod conform-impl ::keys [s x]
  (if-not (map? x)
    ::invalid
    (let [req    (::req s)
          req-un (::req-un s)
          opt    (::opt s)
          opt-un (::opt-un s)
          missing-req    (filter #(not (contains? x %)) req)
          missing-req-un (filter #(not (contains? x (unqualified-key %))) req-un)]
      (if (or (seq missing-req) (seq missing-req-un))
        ::invalid
        (let [check-fn
              (fn [key-q lookup-q]
                (let [v (get x lookup-q)]
                  (if (and (some? v) (get @registry-ref key-q))
                    (not= ::invalid (conform* (get @registry-ref key-q) v))
                    true)))
              all-ok?
              (and (every? #(check-fn % %)                          req)
                   (every? #(check-fn % (unqualified-key %))         req-un)
                   (every? #(if (contains? x %)
                              (check-fn % %) true)                   opt)
                   (every? #(if (contains? x (unqualified-key %))
                              (check-fn % (unqualified-key %)) true) opt-un))]
          (if all-ok? x ::invalid))))))

(defmethod explain-impl ::keys [s path via in x]
  (if-not (map? x)
    [{:path path :pred 'map? :val x :via via :in in}]
    (let [req    (::req s)
          req-un (::req-un s)
          opt    (::opt s)
          opt-un (::opt-un s)
          missing-req    (filter #(not (contains? x %)) req)
          missing-req-un (filter #(not (contains? x (unqualified-key %))) req-un)
          missing-problems
          (concat (map (fn [k]
                         {:path path
                          :pred (list 'contains? '% k)
                          :val  x :via via :in in})
                       missing-req)
                  (map (fn [k]
                         (let [uk (unqualified-key k)]
                           {:path path
                            :pred (list 'contains? '% uk)
                            :val  x :via via :in in}))
                       missing-req-un))
          per-key-problems
          (mapcat (fn [[key-q lookup-q]]
                    (when (contains? x lookup-q)
                      (when-let [sp (get @registry-ref key-q)]
                        (let [v (get x lookup-q)
                              r (conform* sp v)]
                          (when (= ::invalid r)
                            (explain* sp (conj path lookup-q)
                                      (conj via key-q)
                                      (conj in lookup-q) v))))))
                  (concat (map (fn [k] [k k])                  req)
                          (map (fn [k] [k (unqualified-key k)]) req-un)
                          (map (fn [k] [k k])                  opt)
                          (map (fn [k] [k (unqualified-key k)]) opt-un)))]
      (concat missing-problems per-key-problems))))

;; ---------------------------------------------------------------------------
;; Regex ops: cat, *, +, ?, alt.  These compose into a regex spec value
;; that walks an input sequence one element at a time.
;; ---------------------------------------------------------------------------

(defn- regex-spec? [m]
  (and (map? m) (::regex m)))

(defn- as-regex-spec
  "If sp resolves to a regex spec (cat / * / + / ? / alt), return it.
  Otherwise return nil."
  [sp]
  (cond
    (regex-spec? sp)        sp
    (and (keyword? sp)
         (regex-spec? (get @registry-ref sp)))
                            (get @registry-ref sp)
    :else                   nil))

(defn cat-impl [keys forms specs]
  {::kind ::regex ::regex true ::op ::cat
   ::form (cons 'clojure.spec.alpha/cat (interleave keys forms))
   ::keys keys ::forms forms ::specs specs})

(defn rep-impl [op form spec]
  {::kind ::regex ::regex true ::op op
   ::form (list (cond (= op ::*) 'clojure.spec.alpha/*
                      (= op ::+) 'clojure.spec.alpha/+
                      (= op ::?) 'clojure.spec.alpha/?) form)
   ::form-inner form ::spec spec})

(defn alt-impl [keys forms specs]
  {::kind ::regex ::regex true ::op ::alt
   ::form (cons 'clojure.spec.alpha/alt (interleave keys forms))
   ::keys keys ::forms forms ::specs specs})

(declare re-conform re-consume re-explain)

(defmethod conform-impl ::regex [s x]
  (cond
    (nil? x)        (re-conform s ())
    (sequential? x) (re-conform s (seq x))
    :else           ::invalid))

(defmethod explain-impl ::regex [s path via in x]
  (cond
    (nil? x)        (re-explain s path via in ())
    (sequential? x) (re-explain s path via in (seq x))
    :else           [{:path path :pred 'sequential? :val x :via via :in in}]))

(defn- re-conform-cat
  "Greedy cat: consume each declared element from xs.  Returns
  [conformed remaining-xs] on success, or [::invalid xs] on failure.
  Leftover elements in xs are returned in remaining-xs so callers in
  regex-repetition contexts can keep matching."
  [spec xs]
  (loop [keys  (::keys spec)
         specs (::specs spec)
         xs    xs
         out   {}]
    (cond
      (empty? specs)
      [out xs]

      :else
      (let [k  (first keys)
            sp (first specs)]
        (if-let [rsp (as-regex-spec sp)]
          (let [[r remainder] (re-consume rsp xs)]
            (if (= ::invalid r)
              [::invalid xs]
              (recur (rest keys) (rest specs) remainder (assoc out k r))))
          (if (empty? xs)
            [::invalid xs]
            (let [r (conform* (as-spec sp) (first xs))]
              (if (= ::invalid r)
                [::invalid xs]
                (recur (rest keys) (rest specs) (rest xs) (assoc out k r))))))))))

(defn re-consume
  "Greedy consume from xs against a regex spec.  Returns
  [conformed-result remaining-xs] or [::invalid xs]."
  [spec xs]
  (cond
    (= ::cat (::op spec))
    (re-conform-cat spec xs)

    (= ::* (::op spec))
    (loop [xs xs out []]
      (if (empty? xs)
        [out xs]
        (let [sp (::spec spec)]
          (if-let [rsp (as-regex-spec sp)]
            (let [[r rem] (re-consume rsp xs)]
              (if (= ::invalid r) [out xs] (recur rem (conj out r))))
            (let [r (conform* (as-spec sp) (first xs))]
              (if (= ::invalid r) [out xs] (recur (rest xs) (conj out r))))))))

    (= ::+ (::op spec))
    (let [sp (::spec spec)
          rsp (as-regex-spec sp)
          [first-r rem]
          (if rsp
            (re-consume rsp xs)
            (if (empty? xs)
              [::invalid xs]
              (let [r (conform* (as-spec sp) (first xs))]
                (if (= ::invalid r) [::invalid xs] [r (rest xs)]))))]
      (if (= ::invalid first-r)
        [::invalid xs]
        (let [[rest-out final-rem]
              (re-consume (assoc spec ::op ::*) rem)]
          [(into [first-r] rest-out) final-rem])))

    (= ::? (::op spec))
    (let [sp (::spec spec)]
      (if (empty? xs)
        [nil xs]
        (if-let [rsp (as-regex-spec sp)]
          (let [[r rem] (re-consume rsp xs)]
            (if (= ::invalid r) [nil xs] [r rem]))
          (let [r (conform* (as-spec sp) (first xs))]
            (if (= ::invalid r) [nil xs] [r (rest xs)])))))

    (= ::alt (::op spec))
    (loop [keys (::keys spec) specs (::specs spec)]
      (if (empty? keys)
        [::invalid xs]
        (let [sp (first specs)]
          (if-let [rsp (as-regex-spec sp)]
            (let [[r rem] (re-consume rsp xs)]
              (if (= ::invalid r)
                (recur (rest keys) (rest specs))
                [[(first keys) r] rem]))
            (if (empty? xs)
              (recur (rest keys) (rest specs))
              (let [r (conform* (as-spec sp) (first xs))]
                (if (= ::invalid r)
                  (recur (rest keys) (rest specs))
                  [[(first keys) r] (rest xs)])))))))))

(defn- re-conform [spec xs]
  (let [[r rem] (re-consume spec xs)]
    (cond
      (= ::invalid r) ::invalid
      (seq rem)       ::invalid
      :else           r)))

(defn- re-explain [spec path via in xs]
  (let [r (re-conform spec xs)]
    (when (= ::invalid r)
      [{:path path :pred (::form spec) :val xs :via via :in in}])))

;; ---------------------------------------------------------------------------
;; fdef / instrument runtime.  Macros at the bottom build the spec
;; values; these helpers run them.
;; ---------------------------------------------------------------------------

(defn fdef-impl
  [sym args-form ret-form fn-form args-spec ret-spec fn-spec]
  (let [s {::kind ::fspec
           ::form (list 'clojure.spec.alpha/fdef sym
                        :args args-form :ret ret-form :fn fn-form)
           ::args args-spec
           ::ret  ret-spec
           ::fn   fn-spec}]
    (swap! registry-ref assoc sym s)
    sym))

(def ^:private instrumented-vars (atom {}))

(defn- check-fn-args [sym args]
  (when-let [s (get @registry-ref sym)]
    (when-let [args-spec (::args s)]
      (when (= ::invalid (conform* args-spec args))
        (throw (ex-info (str "Call to " sym " did not conform to spec.")
                        {:sym  sym
                         :args args
                         :problems (::problems
                                    (explain-data args-spec args))}))))))

(defn instrument
  "Wrap the var named by sym so its args are validated against the
  registered fdef on every call.  Returns sym if instrumented, nil
  if no fdef is registered."
  [sym]
  (let [v (resolve sym)]
    (when (and v (get @registry-ref sym))
      (when-not (contains? @instrumented-vars sym)
        (let [orig @v]
          (swap! instrumented-vars assoc sym orig)
          (alter-var-root v
                          (fn [_]
                            (fn [& args]
                              (check-fn-args sym args)
                              (apply orig args))))))
      sym)))

(defn unstrument
  "Restore an instrumented var to its original value."
  [sym]
  (when-let [orig (get @instrumented-vars sym)]
    (let [v (resolve sym)]
      (when v
        (alter-var-root v (fn [_] orig))
        (swap! instrumented-vars dissoc sym)
        sym))))

(defn check-asserts
  "Toggle s/assert checking.  No-op stub; mino asserts always evaluate."
  [_v]
  true)

(defn check-asserts?
  "Whether s/assert is enabled.  Always true on mino."
  []
  true)

;; ---------------------------------------------------------------------------
;; Generators: deferred until clojure.test.check ports.
;; ---------------------------------------------------------------------------

(defn gen
  ([spec] (gen spec nil))
  ([spec _overrides]
   (throw (ex-info "s/gen requires clojure.test.check, which is not bundled in mino"
                   {:type :mino/unsupported :op 's/gen}))))

(defn exercise
  ([spec] (exercise spec 10))
  ([spec n] (exercise spec n nil))
  ([spec _n _overrides]
   (throw (ex-info "s/exercise requires clojure.test.check, which is not bundled in mino"
                   {:type :mino/unsupported :op 's/exercise}))))

;; ---------------------------------------------------------------------------
;; Macros at the bottom of the file.  Before this point, internal defns
;; resolve `def`, `and`, `or`, `cat`, `*`, `+`, `?`, `keys`, `nilable`,
;; `coll-of`, `map-of`, `tuple`, `assert`, `fdef`, `alt` to clojure.core
;; bindings (or special forms).  After this point, those names are
;; bound as macros in this namespace; users calling them as `s/and`,
;; `s/def`, etc. resolve correctly because qualified-symbol dispatch
;; bypasses the special-form table.
;; ---------------------------------------------------------------------------

(defn- res
  "Resolve a symbol to its qualified form for stable :form data."
  [form]
  (cond
    (keyword? form) form
    (symbol? form)  (or (when-let [v (resolve form)]
                          (let [{:keys [name ns]} (meta v)]
                            (when (and name ns)
                              (symbol (str ns) (str name)))))
                        form)
    :else form))

(defmacro spec
  "Promote a predicate form to a spec value."
  [pred]
  `(clojure.spec.alpha/spec-impl '~(res pred) ~pred))

(defmacro def
  "Register a spec under k.  k is a namespaced keyword."
  [k spec-form]
  `(clojure.spec.alpha/def-impl '~k '~(res spec-form) ~spec-form))

(defmacro and
  "Compose specs left to right; each receives the conformed value of
  the previous."
  [& forms]
  `(clojure.spec.alpha/and-spec-impl '~(map res forms) ~(vec forms)))

(defmacro or
  "Branches with named tags; the first match wins."
  [& key-pred-pairs]
  (let [pairs (partition 2 key-pred-pairs)
        keys  (mapv first pairs)
        forms (mapv second pairs)]
    `(clojure.spec.alpha/or-spec-impl '~keys '~(map res forms) ~forms)))

(defmacro keys
  "Validate a map shape.  Required-key presence is enforced; per-key
  values are conformed against specs registered under the qualified
  key."
  [& opts]
  (let [opts-map (apply hash-map opts)]
    `(clojure.spec.alpha/keys-impl '~opts ~opts-map)))

(defmacro coll-of
  "Validate that x is a collection whose elements all conform to spec."
  [pred & opts]
  (let [opts-map (apply hash-map opts)]
    `(clojure.spec.alpha/coll-of-impl '~(res pred) ~pred ~opts-map)))

(defmacro map-of
  "Validate a map of key-spec to val-spec."
  [k-pred v-pred & opts]
  (let [opts-map (apply hash-map opts)]
    `(clojure.spec.alpha/map-of-impl '~(res k-pred) '~(res v-pred)
                                     ~k-pred ~v-pred ~opts-map)))

(defmacro tuple
  "Validate a fixed-length, position-indexed vector."
  [& preds]
  `(clojure.spec.alpha/tuple-impl '~(map res preds) ~(vec preds)))

(defmacro nilable
  "Spec that allows nil OR a value matching pred."
  [pred]
  `(clojure.spec.alpha/nilable-impl '~(res pred) ~pred))

(defmacro cat
  "Sequence of named tagged elements."
  [& key-pred-pairs]
  (let [pairs (partition 2 key-pred-pairs)
        keys  (mapv first pairs)
        forms (mapv second pairs)]
    `(clojure.spec.alpha/cat-impl '~keys '~(map res forms) ~forms)))

(defmacro *
  "Zero or more occurrences of pred."
  [pred]
  `(clojure.spec.alpha/rep-impl :clojure.spec.alpha/* '~(res pred) ~pred))

(defmacro +
  "One or more occurrences of pred."
  [pred]
  `(clojure.spec.alpha/rep-impl :clojure.spec.alpha/+ '~(res pred) ~pred))

(defmacro ?
  "Zero or one occurrence of pred."
  [pred]
  `(clojure.spec.alpha/rep-impl :clojure.spec.alpha/? '~(res pred) ~pred))

(defmacro alt
  "Alternation: branches with named tags, single-element."
  [& key-pred-pairs]
  (let [pairs (partition 2 key-pred-pairs)
        keys  (mapv first pairs)
        forms (mapv second pairs)]
    `(clojure.spec.alpha/alt-impl '~keys '~(map res forms) ~forms)))

(defmacro fdef
  "Register a function spec under the qualified symbol of fn-sym.
  Options: :args, :ret, :fn (each a spec form)."
  [fn-sym & opts]
  (let [opts-map (apply hash-map opts)
        args     (:args opts-map)
        ret      (:ret  opts-map)
        fn-spec  (get opts-map :fn)
        qsym     (symbol (str (or (some-> (resolve fn-sym) meta :ns)
                                  *ns*))
                         (str (if (symbol? fn-sym) (name fn-sym) fn-sym)))]
    `(clojure.spec.alpha/fdef-impl '~qsym '~args '~ret '~fn-spec
                                   ~args ~ret ~fn-spec)))

(defmacro assert
  "Throw on spec violation.  Returns the value if it conforms."
  [spec x]
  `(let [x# ~x]
     (if (clojure.spec.alpha/valid? ~spec x#)
       x#
       (throw (ex-info (str "Spec assertion failed: " (pr-str ~spec))
                       (or (clojure.spec.alpha/explain-data ~spec x#) {}))))))
