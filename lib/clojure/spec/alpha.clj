;; clojure.spec.alpha -- spec definitions and predicate composition.
;;
;; mino port of the canonical surface. Spec values are maps tagged with
;; ::kind and dispatched via multimethods (conform-impl, explain-impl).
;; The canonical Spec / Specize protocols are exposed as descriptor
;; values; their methods (conform* unform* explain* gen* with-gen*
;; describe* / specize*) are namespace fns dispatching on that tag.
;; Macros at the bottom of the file expand to calls of the corresponding
;; *-impl builder fn so the same surface works inside and outside macro
;; context. Generators are backed by the bundled clojure.test.check
;; (see the generator section below).
;;
;; File ordering: ns -> registry -> helpers -> defmulti decls ->
;; dispatch methods -> all macros at the bottom.  This ordering is
;; load-bearing because mino's syntax-quote auto-qualifies bare symbols
;; against the runtime current-ns env.  If `def`, `and`, `or`, `cat`,
;; `*`, `+`, `?`, `keys`, `nilable`, `coll-of`, `map-of`, `tuple`,
;; `assert`, `fdef`, `alt`, `every`, `every-kv`, `keys*`, `merge`, or
;; `multi-spec` are bound as macros mid-file, internal (defn ...) /
;; (def ...) calls expand to qualified forms that miss the special-form
;; dispatch and fire the macros instead, leaving the var unbound.
;; Names that collide with clojure.core fns (`merge`, `keys`, `*`, ...)
;; must be called via their clojure.core/ qualified form in every fn
;; body in this file, because bodies macroexpand at first call -- after
;; the whole file (macros included) has loaded.

(ns clojure.spec.alpha
  (:require [clojure.walk :as walk]))

;; ---------------------------------------------------------------------------
;; Dynamic tuning vars.  Defaults match the canonical library.  They are
;; declared up front so the conform / explain / gen machinery below can
;; consult them directly.
;; ---------------------------------------------------------------------------

(def ^:dynamic *recursion-limit*
  "A soft limit on how many times a branching spec (or / alt / * /
  opt-keys / multi-spec) can be recursed through during generation.
  After this a non-recursive branch is chosen."
  4)

(def ^:dynamic *fspec-iterations*
  "The number of times an anonymous fn specified by fspec is
  generatively sampled during conform."
  21)

(def ^:dynamic *coll-check-limit*
  "The number of elements validated in a collection spec'ed with
  every / every-kv."
  101)

(def ^:dynamic *coll-error-limit*
  "The number of errors reported by explain in a collection spec'ed
  with every / every-kv."
  20)

(def ^:dynamic *compile-asserts*
  "When true, s/assert is checked at runtime; when false it returns its
  value unevaluated.  Defaults to true."
  true)

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
(defmulti ^:private unform-impl
  (fn [spec _y] (::kind spec)))

(defn- spec-map?
  "True when x is a spec value of any kind -- regex ops included --
  i.e. a map carrying the ::kind tag.  Internal coercion helper; the
  public spec? excludes regex ops the way the canonical fn does."
  [x]
  (and (map? x) (contains? x ::kind)))

(defn spec?
  "Return x when x is a spec object, else nil.  Regex ops are not
  spec objects (see regex?)."
  [x]
  (when (and (map? x) (contains? x ::kind) (not (::op x)))
    x))

(defn regex?
  "Return x when x is a spec regex op (cat / * / + / ? / alt / &),
  else nil.  Specs that merely wrap a regex (see spec / regex-spec-impl)
  are spec objects, not regex ops."
  [x]
  (when (and (map? x) (::op x))
    x))

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
    (spec-map? x)          x
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
  explain-data / explain / explain-str. When the spec is registered
  (carries ::name), prepend that name to via so problems list the
  surrounding spec path the way Clojure's spec.alpha does."
  [spec path via in x]
  (let [via' (if-let [n (::name spec)]
               (conj (vec via) n)
               via)]
    (explain-impl spec path via' in x)))

(defn unform*
  "Internal dispatch for unform.  Public callers use unform."
  [spec y]
  (unform-impl spec y))

;; ---------------------------------------------------------------------------
;; Protocol surface.  The canonical library reifies specs over the Spec
;; and Specize protocols; mino specs are plain maps tagged with ::kind,
;; so the protocols are exposed as descriptor values and their methods
;; as namespace fns dispatching on that tag.
;; ---------------------------------------------------------------------------

(def Spec
  "Descriptor for the Spec protocol.  Spec values are ::kind-tagged
  maps; the protocol's methods are the namespace fns listed under
  ::methods, dispatching on that tag."
  {::protocol 'clojure.spec.alpha/Spec
   ::methods  '[conform* unform* explain* gen* with-gen* describe*]})

(def Specize
  "Descriptor for the Specize protocol.  Coercion to a spec value is
  the namespace fn specize*."
  {::protocol 'clojure.spec.alpha/Specize
   ::methods  '[specize*]})

(defn specize*
  "Coerce x to a spec value: spec maps pass through, registered
  keywords resolve, anything callable as a predicate is promoted to a
  pred spec.  The binary arity uses form as the promoted description."
  ([x] (as-spec x))
  ([x form]
   (cond
     (spec-map? x)          x
     (and (keyword? x)
          (get @registry-ref x))
                            (get @registry-ref x)
     (ifn? x)               (pred-spec form x)
     :else                  (throw (ex-info (str "Unable to coerce to spec: "
                                                 (pr-str x))
                                            {:spec x})))))

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

(defn unform
  "Return the destructuring inverse of conform.  Given a conformed
  value y produced by (conform spec x), unform returns a value that
  conforms to spec and conforms to it back to y.  For specs without
  an inverse (registered :clojure.spec.alpha/conformer specs without
  an :unform key), unform returns y unchanged."
  [spec y]
  (unform* (as-spec spec) y))

(defn explain-data*
  "Lower-level explain entry point: run explain* with the given path,
  via and in seeds and wrap the resulting problems in the canonical
  {::problems ::spec ::value} map, or return nil when x conforms.
  Public callers usually want explain-data."
  [spec path via in x]
  (let [problems (explain* (as-spec spec) path via in x)]
    (when (seq problems)
      {::problems problems
       ::spec     spec
       ::value    x})))

(defn explain-data
  "Return a problem map describing why x fails spec, or nil if it
  passes.  Shape: {::problems [{:path ... :pred ... :val ... :via ...
  :in ...}] ::spec ... ::value ...}."
  [spec x]
  (explain-data* spec [] [] [] x))

(declare abbrev)

(defn- problem-str
  "Format a single problem the way Clojure's spec.alpha does:
     <val> - failed: <pred> [in: <in>] [at: <path>] [spec: <name>]
   The leading val + ' - failed: <pred>' is the only mandatory chunk;
   the bracketed segments are emitted only when they carry data. The
   pred form is abbreviated (namespace-qualifiers dropped) to match
   Clojure's `explain-printer` output."
  [p]
  (str (pr-str (:val p))
       " - failed: "
       (pr-str (abbrev (:pred p)))
       (when (seq (:in p))   (str " in: " (pr-str (:in p))))
       (when (seq (:path p)) (str " at: " (pr-str (:path p))))
       (when (seq (:via p))  (str " spec: " (pr-str (last (:via p)))))))

(defn explain-printer
  "Default printer for explain-data: write a human-readable description
  of each problem to *out*.  nil indicates a successful validation and
  prints \"Success!\".  Problems are ordered longest-path first, the
  way Clojure's explain-printer sorts them, so the deepest failure
  reads first."
  [ed]
  (if ed
    (let [problems (->> (::problems ed)
                        (sort-by (fn [p] (- (count (:in p)))))
                        (sort-by (fn [p] (- (count (:path p))))))]
      (doseq [p problems]
        (print (problem-str p))
        (newline)))
    (println "Success!")))

(def ^:dynamic *explain-out*
  "The printer explain-out uses to render explain-data.  Defaults to
  explain-printer; rebind it to redirect explain output."
  explain-printer)

(defn explain-out
  "Print explanation data (per explain-data) to *out* using the printer
  in *explain-out*, by default explain-printer."
  [ed]
  (*explain-out* ed))

(defn explain-str
  "Return a string describing why x fails spec, or \"Success!\\n\" if
  it passes. Format follows clojure.spec.alpha: each problem on its
  own line terminated with \\n."
  [spec x]
  (with-out-str (explain-out (explain-data spec x))))

(defn explain
  "Print an explanation of why x fails spec to *out* via *explain-out*."
  [spec x]
  (explain-out (explain-data spec x)))

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

(defn describe*
  "Protocol-shaped describe: return the raw (unabbreviated) form of a
  spec.  Public callers usually want describe."
  [spec]
  (::form (as-spec spec)))

;; ---------------------------------------------------------------------------
;; pred -- bare predicates promoted to specs.
;; ---------------------------------------------------------------------------

(defmethod conform-impl ::pred [s x]
  (if ((::pred s) x) x ::invalid))

(defmethod explain-impl ::pred [s path via in x]
  (when-not ((::pred s) x)
    [{:path path :pred (::form s) :val x :via via :in in}]))

(defmethod unform-impl ::pred [_s y] y)

(defn regex-spec-impl
  "Wrap a regex op as a non-regex spec: the result conforms an element
  value as a whole sequence rather than splicing into the surrounding
  regex context.  gfn, when given, attaches a generator fn."
  [re gfn]
  (let [s {::kind ::wrap ::form (::form re) ::wrapped re}]
    (if gfn (assoc s ::gen gfn) s)))

(defn spec-impl
  "Build a spec value from a form and predicate.  When pred resolves to
  a regex spec (cat / * / + / ? / alt / a registered regex), the result
  wraps the regex (see regex-spec-impl)."
  [form pred]
  (if-let [reg (as-regex-spec pred)]
    (assoc (regex-spec-impl reg nil) ::form form)
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

(defmethod unform-impl ::wrap [s y]
  (let [inner (::wrapped s)]
    (if (regex-spec? inner)
      (unform-impl inner y)
      (unform-impl (as-spec inner) y))))

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
            (spec-map? spec) (assoc spec ::name k ::form form)
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

(defmethod unform-impl ::and [s y]
  ;; Chain unforms in reverse: the last spec produced the final value,
  ;; so unform through it first, then through each preceding spec.
  (reduce (fn [acc sp] (unform* (as-spec sp) acc))
          y
          (reverse (::specs s))))

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

(defn- spec-index-of
  "Find tag in the seq of keys; returns the 0-based index or -1."
  [keys tag]
  (loop [ks keys i 0]
    (cond
      (empty? ks)        -1
      (= (first ks) tag) i
      :else              (recur (rest ks) (inc i)))))

(defmethod unform-impl ::or [s y]
  ;; y is a [tag conformed-val] tuple produced by conform. Look up the
  ;; matching spec for the tag and unform through it.
  (let [[tag v] y
        idx     (spec-index-of (::keys s) tag)]
    (if (neg? idx)
      v
      (unform* (as-spec (nth (::specs s) idx)) v))))

;; ---------------------------------------------------------------------------
;; nilable -- spec-or-nil.
;; ---------------------------------------------------------------------------

(defn nilable-impl [form spec]
  {::kind ::nilable
   ::form (list 'clojure.spec.alpha/nilable form)
   ::inner-form form ::spec spec})

(defmethod conform-impl ::nilable [s x]
  (if (nil? x) nil (conform* (as-spec (::spec s)) x)))

(defmethod unform-impl ::nilable [s y]
  (if (nil? y) nil (unform* (as-spec (::spec s)) y)))

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

(defmethod unform-impl ::tuple [s y]
  ;; y is a vector of conformed values; unform each element through its
  ;; corresponding spec.
  (vec (map (fn [sp v] (unform* (as-spec sp) v))
            (::specs s) y)))

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
;; every / every-kv / coll-of / map-of via every-impl.  coll-of and
;; map-of conform every element (::conform-all); every and every-kv
;; only validate, by sampling at most coll-check-limit elements, and
;; conform to the input collection unchanged.
;; ---------------------------------------------------------------------------

(defn every-impl
  "Build an ::every spec value.  Recognized opts: :kind, :count,
  :min-count, :max-count, :distinct, :into, plus ::conform-all
  (conform each element instead of sampling) and ::kv (elements are
  [k v] entries; explain reports the key, not the index)."
  [form spec opts]
  (clojure.core/merge
    {::kind ::every ::form form ::spec spec
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
      (let [sp (as-spec (::spec s))]
        (if (::conform-all s)
          (let [ok? (every? (fn [v] (not= ::invalid (conform* sp v))) x)]
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
              ::invalid))
          ;; Sampling mode: validate up to *coll-check-limit* elements
          ;; and return the input unchanged -- elements are never
          ;; conformed.
          (loop [i 0 vs (seq x)]
            (cond
              (or (nil? vs) (>= i *coll-check-limit*)) x
              (= ::invalid (conform* sp (first vs))) ::invalid
              :else (recur (inc i) (next vs)))))))))

(defmethod unform-impl ::every [s y]
  ;; coll-of / map-of conform elements individually; unform each
  ;; through the element spec and reconstruct in the original
  ;; collection shape.  Sampling specs never conformed, so y is
  ;; already the original value.
  (if-not (::conform-all s)
    y
    (let [sp        (as-spec (::spec s))
          conformed (map (fn [v] (unform* sp v)) y)
          target    (::into s)]
      (cond
        (vector? target) (vec conformed)
        (set? target)    (set conformed)
        (list? target)   (apply list conformed)
        (vector? y)      (vec conformed)
        (set? y)         (set conformed)
        (map? y)         (into {} conformed)
        :else            (apply list conformed)))))

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
      (let [sp        (as-spec (::spec s))
            kv        (::kv s)
            ;; coll-of / map-of report every failing element; sampling
            ;; specs (every / every-kv) cap the report at
            ;; *coll-error-limit* problem groups, matching the canon.
            limit-fn  (if (::conform-all s)
                        identity
                        (fn [probs] (take *coll-error-limit* probs)))]
        (apply concat
               (limit-fn
                 (filter some?
                         (map-indexed
                           (fn [i v]
                             (let [r (conform* sp v)
                                   ;; kv entries report the entry key;
                                   ;; fall back to the index when the
                                   ;; element isn't a pair.
                                   k (if (and kv (sequential? v) (pos? (count v)))
                                       (nth v 0)
                                       i)]
                               (when (= ::invalid r)
                                 (explain* sp (conj path i) via (conj in k) v))))
                           x))))))))

(defn coll-of-impl [form spec opts]
  (every-impl (cons 'clojure.spec.alpha/coll-of (cons form (mapcat identity opts)))
              spec (assoc opts ::conform-all true)))

(defn map-of-impl [k-form v-form k-spec v-spec opts]
  (let [pair-spec (tuple-impl [k-form v-form] [k-spec v-spec])
        opts'     (assoc opts :kind map? ::conform-all true)]
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

(defmethod unform-impl ::keys [_s y]
  ;; keys-conformed values pass through identity; per-key values were
  ;; not transformed by conform.
  y)

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
;; map-spec-impl -- the canonical keys plumbing fn.  Driven entirely by
;; the argument map the canonical keys macro compiles: keys-pred gates
;; the map shape, then every entry whose spec name (req/opt key mapped
;; through req-specs/opt-specs, or the entry key itself) is registered
;; has its value conformed.  The keys macro in this file uses the
;; simpler keys-impl; map-spec-impl exists for callers of the canonical
;; plumbing signature.
;; ---------------------------------------------------------------------------

(defn map-spec-impl
  "Build a map spec from the canonical argument map.  See the comment
  above; callers normally use the keys macro."
  [{:keys [req opt req-un opt-un req-keys req-specs opt-keys opt-specs
           keys-pred pred-exprs pred-forms gfn]
    :as argm}]
  (let [k->s (zipmap (concat req-keys opt-keys)
                     (concat req-specs opt-specs))
        s    {::kind ::map-spec
              ::form (cons 'clojure.spec.alpha/keys
                           (concat (when req    [:req req])
                                   (when opt    [:opt opt])
                                   (when req-un [:req-un req-un])
                                   (when opt-un [:opt-un opt-un])))
              ::argm argm
              ::k->s k->s}]
    (if gfn (assoc s ::gen gfn) s)))

(defmethod conform-impl ::map-spec [s m]
  (if-not ((:keys-pred (::argm s)) m)
    ::invalid
    (let [reg  @registry-ref
          k->s (::k->s s)]
      (loop [ret m entries (seq m)]
        (if entries
          (let [[k v] (first entries)
                sp    (get reg (or (get k->s k) k))]
            (if sp
              (let [cv (conform* sp v)]
                (if (= ::invalid cv)
                  ::invalid
                  (recur (if (= cv v) ret (assoc ret k cv))
                         (next entries))))
              (recur ret (next entries))))
          ret)))))

(defmethod unform-impl ::map-spec [s m]
  (let [reg  @registry-ref
        k->s (::k->s s)]
    (loop [ret m entries (seq m)]
      (if entries
        (let [[k cv] (first entries)
              sp     (get reg (or (get k->s k) k))]
          (if sp
            (let [v (unform* sp cv)]
              (recur (if (= cv v) ret (assoc ret k v))
                     (next entries)))
            (recur ret (next entries))))
        ret))))

(defmethod explain-impl ::map-spec [s path via in x]
  (if-not (map? x)
    [{:path path :pred 'map? :val x :via via :in in}]
    (let [reg  @registry-ref
          k->s (::k->s s)
          {:keys [pred-exprs pred-forms]} (::argm s)
          pred-problems
          (map (fn [f] {:path path :pred f :val x :via via :in in})
               (filter some?
                       (map (fn [pred f] (when-not (pred x) f))
                            pred-exprs pred-forms)))
          key-problems
          (mapcat (fn [[k v]]
                    (when-let [sp (get reg (or (get k->s k) k))]
                      (when (= ::invalid (conform* sp v))
                        (explain* sp (conj path k) via (conj in k) v))))
                  (seq x))]
      (concat pred-problems key-problems))))

;; ---------------------------------------------------------------------------
;; merge -- conjunction of map specs.  x must conform to every branch
;; and the conformed maps are merged left to right; explain reports
;; only the failing branches.
;; ---------------------------------------------------------------------------

(defn merge-spec-impl
  "Build a merge spec from branch forms and branch specs.  Callers
  normally use the merge macro."
  [forms preds gfn]
  (let [s {::kind ::merge
           ::form (cons 'clojure.spec.alpha/merge forms)
           ::forms forms ::specs preds}]
    (if gfn (assoc s ::gen gfn) s)))

(defmethod conform-impl ::merge [s x]
  (loop [specs (seq (::specs s)) ms []]
    (if specs
      (let [r (conform* (as-spec (first specs)) x)]
        (if (= ::invalid r)
          ::invalid
          (recur (next specs) (conj ms r))))
      (apply clojure.core/merge ms))))

(defmethod unform-impl ::merge [s y]
  ;; Later branches win on conform, so unform them first and let
  ;; earlier branches' results overwrite.
  (apply clojure.core/merge
         (map (fn [sp] (unform* (as-spec sp) y))
              (reverse (::specs s)))))

(defmethod explain-impl ::merge [s path via in x]
  (apply concat
         (map (fn [sp] (explain* (as-spec sp) path via in x))
              (::specs s))))

;; ---------------------------------------------------------------------------
;; multi-spec -- open dispatch through a multimethod.  Each method
;; takes the value and returns the spec for its dispatch value, so new
;; branches are added with defmethod, without touching the spec.
;; ---------------------------------------------------------------------------

(defn- multi-spec-dval
  "Dispatch value for a multi-spec.  mino multimethods do not expose
  their dispatch fn, so the keyword retag (multi-spec's pervasive
  shape, where retag and the defmulti dispatch key coincide) doubles
  as the dispatch fn.  Fn retags are generation-only in the canonical
  library and cannot drive dispatch here."
  [retag x]
  (if (keyword? retag)
    (retag x)
    (throw (ex-info "multi-spec requires a keyword retag on mino"
                    {:retag retag}))))

(defn- multi-spec-method
  "Return mm's method fn for dispatch value dval, or nil: exact
  dispatch value first, then the default method.  Hierarchy-derived
  matches (isa? parents of dval) are not consulted."
  [mm dval]
  (or (get-method mm dval)
      (get-method mm (:default (meta mm)))))

(defn multi-spec-impl
  "Build a multi-spec value.  form names the multimethod, mmvar is the
  var holding it, retag is the tag key.  Callers normally use the
  multi-spec macro."
  ([form mmvar retag] (multi-spec-impl form mmvar retag nil))
  ([form mmvar retag gfn]
   (let [s {::kind ::multi
            ::form (list 'clojure.spec.alpha/multi-spec form retag)
            ::mm-form form ::mmvar mmvar ::retag retag}]
     (if gfn (assoc s ::gen gfn) s))))

(defmethod conform-impl ::multi [s x]
  (let [mm     @(::mmvar s)
        method (multi-spec-method mm (multi-spec-dval (::retag s) x))]
    (if method
      (conform* (as-spec (method x)) x)
      ::invalid)))

(defmethod unform-impl ::multi [s y]
  (let [mm     @(::mmvar s)
        dval   (multi-spec-dval (::retag s) y)
        method (multi-spec-method mm dval)]
    (if method
      (unform* (as-spec (method y)) y)
      (throw (ex-info (str "No method of: " (::mm-form s)
                           " for dispatch value: " (pr-str dval))
                      {:form (::mm-form s) :val y})))))

(defmethod explain-impl ::multi [s path via in x]
  (let [mm     @(::mmvar s)
        dval   (multi-spec-dval (::retag s) x)
        path   (conj path dval)
        method (multi-spec-method mm dval)]
    (if method
      (explain* (as-spec (method x)) path via in x)
      [{:path path :pred (::mm-form s) :val x
        :reason "no method" :via via :in in}])))

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

(defn rep+impl
  "Build a + regex spec: one or more occurrences of pred.  Callers
  normally use the + macro."
  [form pred]
  (rep-impl ::+ form pred))

(defn maybe-impl
  "Build a ? regex spec: zero or one occurrence of pred.  Callers
  normally use the ? macro.  Note the canonical argument order: pred
  first, form second."
  [pred form]
  (rep-impl ::? form pred))

(defn alt-impl [keys forms specs]
  {::kind ::regex ::regex true ::op ::alt
   ::form (cons 'clojure.spec.alpha/alt (interleave keys forms))
   ::keys keys ::forms forms ::specs specs})

(defn amp-impl
  "Build an ::amp regex spec: re-spec is consumed normally; the
  conformed sequence is then threaded through each pred as a spec
  (see the ::amp branch of re-consume).  Any ::invalid step yields
  ::invalid for the whole consume."
  [re-form pred-forms re-spec pred-fns]
  {::kind ::regex ::regex true ::op ::amp
   ::form (cons 'clojure.spec.alpha/& (cons re-form pred-forms))
   ::re-form    re-form
   ::pred-forms pred-forms
   ::re         re-spec
   ::preds      pred-fns})

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

(declare re-unform)

(defmethod unform-impl ::regex [s y] (re-unform s y))

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
                  [[(first keys) r] (rest xs)])))))))

    (= ::amp (::op spec))
    (let [re-sp (as-regex-spec (::re spec))
          [r rem] (if re-sp
                    (re-consume re-sp xs)
                    [::invalid xs])]
      (if (= ::invalid r)
        [::invalid xs]
        ;; Thread the conformed value through each pred as a spec:
        ;; plain predicates act as filters (the value passes through
        ;; unchanged), conformer / map specs transform it -- keys*
        ;; relies on the transformation.
        (let [r' (loop [v r ps (seq (::preds spec))]
                   (cond
                     (= ::invalid v) ::invalid
                     (nil? ps)       v
                     :else (recur (conform* (as-spec (first ps)) v)
                                  (next ps))))]
          (if (= ::invalid r')
            [::invalid xs]
            [r' rem]))))))

(defn- re-conform [spec xs]
  (let [[r rem] (re-consume spec xs)]
    (cond
      (= ::invalid r) ::invalid
      (seq rem)       ::invalid
      :else           r)))

(defn- re-unform
  "Reconstruct the input sequence that conformed to spec. Inverse of
  re-consume: cat unfolds a map of tagged values into the declared key
  order; *, +, ? unfold a collection or single value; alt unfolds the
  [tag val] pair via the matching branch."
  [spec y]
  (let [op (::op spec)]
    (cond
      (= ::cat op)
      ;; y is a map keyed by ::keys-listed tags. Walk in declared
      ;; order; if a tag was not produced (missing key) we drop that
      ;; position, matching JVM-Clojure's spec.alpha behaviour.
      (mapcat (fn [k sp]
                (let [present? (contains? y k)
                      v        (get y k)
                      rsp      (as-regex-spec sp)]
                  (cond
                    (not present?) ()
                    rsp            (let [sub (re-unform rsp v)]
                                     (if (sequential? sub) sub (list sub)))
                    :else          (list (unform* (as-spec sp) v)))))
              (::keys spec) (::specs spec))

      (or (= ::* op) (= ::+ op))
      (let [sp  (::spec spec)
            rsp (as-regex-spec sp)]
        (mapcat (fn [v]
                  (if rsp
                    (let [sub (re-unform rsp v)]
                      (if (sequential? sub) sub (list sub)))
                    (list (unform* (as-spec sp) v))))
                y))

      (= ::? op)
      (if (nil? y)
        ()
        (let [sp  (::spec spec)
              rsp (as-regex-spec sp)]
          (if rsp
            (let [sub (re-unform rsp y)]
              (if (sequential? sub) sub (list sub)))
            (list (unform* (as-spec sp) y)))))

      (= ::alt op)
      (let [[tag v] y
            idx     (spec-index-of (::keys spec) tag)]
        (if (neg? idx)
          ()
          (let [sp  (nth (::specs spec) idx)
                rsp (as-regex-spec sp)]
            (if rsp
              (re-unform rsp v)
              (list (unform* (as-spec sp) v))))))

      (= ::amp op)
      ;; Reverse the pred threading (conformers may have transformed
      ;; the value), then unform through the wrapped regex.
      (let [re-sp (as-regex-spec (::re spec))
            v     (reduce (fn [acc p] (unform* (as-spec p) acc))
                          y
                          (reverse (::preds spec)))]
        (if re-sp (re-unform re-sp v) v))

      :else y)))

(defn- re-explain [spec path via in xs]
  (let [r (re-conform spec xs)]
    (when (= ::invalid r)
      [{:path path :pred (::form spec) :val xs :via via :in in}])))

;; ---------------------------------------------------------------------------
;; fspec -- generative function spec.  conform / explain sample the
;; :args spec, apply the fn, and validate the :ret (and :fn) spec.  The
;; conformed value is always the fn itself.  fdef builds an fspec value
;; and registers it under the qualified symbol.
;; ---------------------------------------------------------------------------

(declare gen)

(defn- fspec-sample-args
  "Generate up to n argument vectors from an fspec :args spec, drawing
  growing-size samples through the generator path.  Returns a seq of
  arg sequences (a cat args-spec yields one value per declared slot)."
  [args-spec n]
  (let [g (gen args-spec)]
    (vec (for [i (range n)]
           (clojure.test.check.generators/generate
             g (mod (clojure.core/* (inc i) 5) 40))))))

(defn- fspec-call-valid?
  "Return true when applying f to one generated arg vector produces a
  :ret-conforming (and, when present, :fn-conforming) result."
  [f args-spec ret-spec fn-spec args]
  (let [cargs (conform* (as-spec args-spec) args)]
    (if (= ::invalid cargs)
      true
      (let [ret  (apply f args)
            cret (conform* (as-spec ret-spec) ret)]
        (clojure.core/and
          (not= ::invalid cret)
          (if fn-spec
            (not= ::invalid
                  (conform* (as-spec fn-spec) {:args cargs :ret cret}))
            true))))))

(defn- fspec-find-failure
  "Sample iters argument vectors and return the first that makes f
  violate :ret / :fn, or nil if all pass.  Sampling stops at the first
  failure."
  [f args-spec ret-spec fn-spec iters]
  (loop [samples (seq (fspec-sample-args args-spec iters))]
    (when samples
      (let [args (first samples)]
        (if (fspec-call-valid? f args-spec ret-spec fn-spec args)
          (recur (next samples))
          args)))))

(defn fspec-impl
  "Build an fspec value from the :args / :ret / :fn specs and their
  forms.  conform validates a fn by generatively sampling :args (up to
  *fspec-iterations*) and checking :ret (and :fn); the conformed value
  is the fn itself.  Callers normally use the fspec macro or fdef."
  [argspec aform retspec rform fnspec fform gfn]
  (let [s {::kind ::fspec
           ::form (list 'clojure.spec.alpha/fspec
                        :args aform :ret rform :fn fform)
           ::args argspec ::aform aform
           ::ret  retspec ::rform rform
           ::fn   fnspec  ::fform fform
           ;; Plain keys mirror the canonical ILookup surface so callers
           ;; can pull the sub-specs with (:args fspec) etc.
           :args argspec :ret retspec :fn fnspec}]
    (if gfn (assoc s ::gen gfn) s)))

(defmethod conform-impl ::fspec [s f]
  (let [argspec (::args s)]
    (cond
      (nil? argspec)
      (throw (ex-info "Can't conform fspec without args spec" {:spec s}))

      (not (ifn? f))
      ::invalid

      (fspec-find-failure f argspec (::ret s) (::fn s) *fspec-iterations*)
      ::invalid

      :else f)))

(defmethod unform-impl ::fspec [_s f] f)

(defmethod explain-impl ::fspec [s path via in f]
  (if-not (ifn? f)
    [{:path path :pred 'ifn? :val f :via via :in in}]
    (when-let [args (fspec-find-failure f (::args s) (::ret s) (::fn s)
                                        *fspec-iterations*)]
      (let [ret  (apply f args)
            cret (conform* (as-spec (::ret s)) ret)]
        (if (= ::invalid cret)
          (explain* (as-spec (::ret s)) (conj path :ret) via in ret)
          (let [cargs (conform* (as-spec (::args s)) args)]
            (explain* (as-spec (::fn s)) (conj path :fn) via in
                      {:args cargs :ret cret})))))))

;; ---------------------------------------------------------------------------
;; fdef runtime.  Macros at the bottom build the spec values; this helper
;; registers them.  The instrument / check tooling lives in
;; clojure.spec.test.alpha.
;; ---------------------------------------------------------------------------

(defn fdef-impl
  [sym args-form ret-form fn-form args-spec ret-spec fn-spec]
  (let [s (assoc (fspec-impl args-spec args-form ret-spec ret-form
                             fn-spec fn-form nil)
                 ::form (list 'clojure.spec.alpha/fdef sym
                              :args args-form :ret ret-form :fn fn-form)
                 ::name sym)]
    (swap! registry-ref assoc sym s)
    sym))

(def ^:private check-asserts-flag
  ;; Runtime toggle for s/assert.  Distinct from *compile-asserts*,
  ;; which gates compilation; this gates evaluation of compiled asserts.
  (atom true))

(defn check-asserts
  "Enable or disable runtime s/assert checking.  Returns the flag."
  [flag]
  (reset! check-asserts-flag (boolean flag)))

(defn check-asserts?
  "Return the current runtime s/assert checking flag."
  []
  @check-asserts-flag)

(defn assert*
  "Runtime worker behind the assert macro.  Returns x when it conforms
  to spec, else throws an ex-info carrying explain-data plus
  ::failure :assertion-failed.  Callers normally use the assert macro."
  [spec x]
  (if (valid? spec x)
    x
    (let [ed (assoc (explain-data* spec [] [] [] x)
                    ::failure :assertion-failed)]
      (throw (ex-info (str "Spec assertion failed\n"
                           (with-out-str (explain-out ed)))
                      ed)))))

;; ---------------------------------------------------------------------------
;; Generators: backed by the bundled clojure.test.check generators.
;; The generator surface is intentionally minimal -- shrinking is
;; deferred, and the predicate-to-generator map covers the common
;; primitive specs only. User-defined :gen-fn overrides take
;; precedence; pass them via the second argument to gen / exercise.
;; ---------------------------------------------------------------------------

(require '[clojure.test.check.generators :as gen-impl])

(def ^:private predicate-generators
  ;; Map a predicate symbol (qualified or bare) to a generator that
  ;; yields values satisfying it. Looked up by the symbolic form when
  ;; spec was created from a bare predicate (e.g. (s/def ::n int?));
  ;; spec stores the qualified form (clojure.core/int?) so both keys
  ;; appear here.
  (let [pairs {'int?     gen-impl/int
               'integer? gen-impl/int
               'nat-int? gen-impl/nat
               'pos-int? gen-impl/s-pos-int
               'neg-int? gen-impl/neg-int
               'string?  gen-impl/string
               'keyword? gen-impl/keyword
               'symbol?  gen-impl/symbol
               'boolean? gen-impl/boolean
               'double?  gen-impl/double
               'number?  gen-impl/double
               'nil?     (gen-impl/return nil)
               'any?     gen-impl/any}]
    (clojure.core/merge
      pairs
      (into {} (for [[k v] pairs]
                 [(symbol "clojure.core" (name k)) v])))))

(defn- spec-form-key [spec]
  ;; Spec values store the form under :clojure.spec.alpha/form. Fall
  ;; back to plain :form for any future shape that uses the bare key.
  (or (get spec :clojure.spec.alpha/form)
      (get spec :form)))

(defn- spec-form
  "Best-effort retrieval of a spec's :form key."
  [spec]
  (cond
    (map? spec)        (or (spec-form-key spec) spec)
    (symbol? spec)     spec
    (keyword? spec)    spec
    :else              spec))

(declare gen)

(defn- form->generator [form overrides]
  ;; Walk a spec form and produce a clojure.test.check generator.
  ;; Recognized shapes:
  ;;   - bare predicate symbol -> predicate-generators lookup
  ;;   - (s/coll-of pred) -> vector of pred
  ;;   - (s/tuple p1 p2 ...) -> tuple of preds
  ;;   - (s/and ...)    -> first form's generator filtered by rest
  ;;   - (s/or k1 p1 k2 p2 ...) -> one-of of [tag value]
  ;; Anything else falls through to predicate-generators or throws.
  (cond
    (and (symbol? form) (contains? predicate-generators form))
    (get predicate-generators form)

    (cons? form)
    (let [head (first form)
          tail (rest form)
          hd   (cond
                 (symbol? head) (name head)
                 :else          (str head))]
      (cond
        (or (= hd "coll-of")
            (and (= hd "spec") (cons? (first tail))
                 (= "coll-of" (name (first (first tail))))))
        (gen-impl/vector
          (form->generator (if (= hd "coll-of")
                             (first tail)
                             (second (first tail)))
                           overrides))

        (= hd "tuple")
        (apply gen-impl/tuple
               (map #(form->generator % overrides) tail))

        (= hd "nilable")
        (gen-impl/one-of
          [(form->generator (first tail) overrides)
           (gen-impl/return nil)])

        (= hd "and")
        ;; First form drives generation; later forms refine via such-that.
        (let [base (form->generator (first tail) overrides)
              rest-preds (rest tail)]
          (if (empty? rest-preds)
            base
            (gen-impl/such-that
              (fn [v]
                (every? (fn [p] (try ((eval p) v) (catch __e false)))
                        rest-preds))
              base)))

        (= hd "or")
        (let [pairs (partition 2 tail)]
          (gen-impl/one-of
            (map (fn [[k p]]
                   (gen-impl/fmap
                     (fn [v] [k v])
                     (form->generator p overrides)))
                 pairs)))

        ;; Regex ops generate sequences -- the value an fspec :args spec
        ;; (or a nested regex) consumes.  cat lays each declared pred's
        ;; value out in order; alt picks one branch; * / + / ? repeat.
        (= hd "cat")
        (let [preds (map second (partition 2 tail))
              gens  (map #(form->generator % overrides) preds)]
          (gen-impl/fmap (fn [vs] (apply list vs))
                         (apply gen-impl/tuple gens)))

        (= hd "alt")
        (let [preds (map second (partition 2 tail))]
          (gen-impl/one-of
            (map (fn [p]
                   (gen-impl/fmap (fn [v] (list v))
                                  (form->generator p overrides)))
                 preds)))

        (= hd "*")
        (gen-impl/fmap seq (gen-impl/vector
                             (form->generator (first tail) overrides)))

        (= hd "+")
        (gen-impl/fmap (fn [v] (seq (cons (first v) (second v))))
                       (gen-impl/tuple
                         (form->generator (first tail) overrides)
                         (gen-impl/vector
                           (form->generator (first tail) overrides))))

        (= hd "?")
        (gen-impl/one-of
          [(gen-impl/return ())
           (gen-impl/fmap (fn [v] (list v))
                          (form->generator (first tail) overrides))])

        :else
        (throw (ex-info (str "s/gen: don't know how to generate from form " form)
                        {:type :mino/unsupported :form form}))))

    :else
    (throw (ex-info (str "s/gen: don't know how to generate from spec " form)
                    {:type :mino/unsupported :spec form}))))

(defn gen
  "Build a clojure.test.check generator for `spec`. `overrides` is a
  map from spec-key keywords (or predicate symbols) to alternative
  generators. A generator attached with with-gen wins over the one
  derived from the spec's form."
  ([spec] (gen spec nil))
  ([spec overrides]
   (or (when overrides
         (or (get overrides spec)
             (when (keyword? spec)
               (get overrides spec))))
       (let [resolved (cond
                        (keyword? spec) (get @registry-ref spec)
                        :else            spec)]
         (if-let [gfn (and (map? resolved) (::gen resolved))]
           (gfn)
           (form->generator (spec-form resolved) (or overrides {})))))))

(defn gen*
  "Protocol-shaped generator hook: return the generator for spec,
  honoring an attached with-gen generator. path and rmap are accepted
  for the canonical signature; recursion limiting arrives with the
  dynamic-var wave."
  [spec overrides _path _rmap]
  (if-let [gfn (and (map? spec) (::gen spec))]
    (gfn)
    (gen spec overrides)))

(defn exercise
  "Generate `n` (default 10) sample/conformed pairs for `spec`."
  ([spec] (exercise spec 10))
  ([spec n] (exercise spec n nil))
  ([spec n overrides]
   (let [g (gen spec overrides)]
     (vec (for [i (range n)]
            (let [v (gen-impl/generate g (mod (clojure.core/* (inc i) 7) 50))]
              [v (conform spec v)]))))))

(defn exercise-fn
  "Exercise the fn named by sym by applying it to n (default 10)
  generated samples of its registered fdef :args spec.  Returns a
  sequence of [args ret] tuples.  The three-arity form takes an
  explicit fspec (or fn): sym-or-f may then be a fn directly."
  ([sym] (exercise-fn sym 10))
  ([sym n] (exercise-fn sym n (get-spec sym)))
  ([sym-or-f n fspec]
   (let [f        (if (symbol? sym-or-f) (resolve sym-or-f) sym-or-f)
         arg-spec (clojure.core/and fspec (::args fspec))]
     (if arg-spec
       (vec (for [i (range n)]
              (let [args (gen-impl/generate
                           (gen arg-spec)
                           (mod (clojure.core/* (inc i) 7) 50))]
                [args (apply f args)])))
       (throw (ex-info "No :args spec found, can't generate"
                       {:sym sym-or-f}))))))

;; ---------------------------------------------------------------------------
;; Range specs: int-in / double-in / inst-in and their standalone range
;; predicates.  The -range? fns are plain predicates; the -in builders
;; produce specs carrying a range-aware generator on ::gen so gen /
;; exercise sample only in range.
;; ---------------------------------------------------------------------------

(defn int-in-range?
  "Return true when val is a fixed-precision integer with
  start <= val < end."
  [start end val]
  (clojure.core/and (int? val)
                    (<= start val)
                    (< val end)))

(defn inst-in-range?
  "Return true when inst is at or after start and strictly before end."
  [start end inst]
  (clojure.core/and (inst? inst)
                    (let [t (inst-ms inst)]
                      (clojure.core/and (<= (inst-ms start) t)
                                        (< t (inst-ms end))))))

(defn- range-int-gen
  "Generator for integers in [lo, hi] inclusive, drawn by clamping an
  int sample into the range."
  [lo hi]
  (gen-impl/fmap
    (fn [n]
      (let [span (inc (- hi lo))]
        (if (<= span 0)
          lo
          (clojure.core/+ lo (mod (clojure.core/abs n) span)))))
    gen-impl/int))

(defn int-in-impl
  "Return a spec validating fixed-precision integers in the range from
  start (inclusive) to end (exclusive)."
  [start end]
  (let [sp (and-spec-impl
             (list 'clojure.core/int?
                   (list 'clojure.spec.alpha/int-in-range? start end '%))
             [int? (fn [x] (int-in-range? start end x))])
        sp (assoc sp ::form (list 'clojure.spec.alpha/int-in start end))]
    (assoc sp ::gen (fn [] (range-int-gen start (dec end))))))

;; Divergence from JVM clojure.spec.alpha: s/int-in is a function (first-class value) on the
;; JVM; mino uses a macro. This means int-in cannot be passed as a value in higher-order
;; contexts. Use int-in-impl directly for higher-order use.
(defmacro int-in
  "Return a spec validating fixed-precision integers in the range from
  start (inclusive) to end (exclusive)."
  [start end]
  `(clojure.spec.alpha/int-in-impl ~start ~end))

(defn double-in-impl
  "Return a spec for a 64-bit floating point number.  Options:
    :infinite? - whether +/- infinity is allowed (default false)
    :NaN?      - whether NaN is allowed (default false)
    :min       - inclusive minimum (default none)
    :max       - inclusive maximum (default none)

  Divergence: the canonical library defaults :infinite? and :NaN? to
  true; mino defaults both to false so a plain double-in rejects the
  non-finite values unless they are explicitly opted in -- the safer
  default for a range spec."
  [& opts]
  ;; The option keys :infinite? and :NaN? collide with the core
  ;; predicates of the same name; pull them through opts so the core
  ;; fns stay callable in the bound predicates.  A NaN passes any
  ;; numeric bound when NaN is allowed -- comparisons against NaN are
  ;; always false, so the bound preds short-circuit on it.
  (let [m         (apply hash-map opts)
        inf-ok?   (clojure.core/get m :infinite? false)
        nan-ok?   (clojure.core/get m :NaN? false)
        min       (:min m)
        max       (:max m)
        nan-pass? (fn [x] (clojure.core/and nan-ok? (NaN? x)))
        preds (cond-> [double?]
                (not inf-ok?) (conj (fn [x] (not (infinite? x))))
                (not nan-ok?) (conj (fn [x] (not (NaN? x))))
                max           (conj (fn [x] (clojure.core/or (nan-pass? x)
                                                            (<= x max))))
                min           (conj (fn [x] (clojure.core/or (nan-pass? x)
                                                            (<= min x)))))
        forms (cond-> ['clojure.core/double?]
                (not inf-ok?) (conj '(not (infinite? %)))
                (not nan-ok?) (conj '(not (NaN? %)))
                max           (conj (list '<= '% max))
                min           (conj (list '<= min '%)))
        sp    (and-spec-impl (apply list forms) preds)
        sp    (assoc sp ::form (list 'clojure.spec.alpha/double-in
                                     :min min :max max))
        lo    (clojure.core/double (clojure.core/or min -1000.0))
        hi    (clojure.core/double (clojure.core/or max 1000.0))]
    (assoc sp ::gen
           (fn []
             (gen-impl/fmap
               (fn [v]
                 (let [span (- hi lo)
                       r    (- v (clojure.core/double (long v)))]
                   (clojure.core/+ lo
                                   (clojure.core/* span (clojure.core/abs r)))))
               gen-impl/double)))))

(defmacro double-in
  "Return a spec for a 64-bit floating point number.  Options:
    :infinite? - whether +/- infinity is allowed (default false)
    :NaN?      - whether NaN is allowed (default false)
    :min       - inclusive minimum (default none)
    :max       - inclusive maximum (default none)

  Divergence: the canonical library defaults :infinite? and :NaN? to
  true; mino defaults both to false so a plain double-in rejects the
  non-finite values unless they are explicitly opted in -- the safer
  default for a range spec."
  [& opts]
  `(clojure.spec.alpha/double-in-impl ~@opts))

(defn- ms->inst
  "Build an inst component map (recognized by inst?) from epoch
  milliseconds.  Inverse of inst-ms for the UTC civil calendar."
  [ms]
  (let [days  (long (Math/floor (/ (clojure.core/double ms) 86400000.0)))
        rem-ms (- ms (clojure.core/* days 86400000))
        ;; Howard Hinnant's days-from-civil inverse.
        z     (clojure.core/+ days 719468)
        era   (quot (if (>= z 0) z (- z 146096)) 146097)
        doe   (- z (clojure.core/* era 146097))
        yoe   (quot (clojure.core/+ doe (- (quot doe 1460))
                       (quot doe 36524) (- (quot doe 146096)))
                    365)
        y     (clojure.core/+ yoe (clojure.core/* era 400))
        doy   (- doe (- (clojure.core/+ (clojure.core/* 365 yoe) (quot yoe 4))
                        (quot yoe 100)))
        mp    (quot (clojure.core/+ (clojure.core/* 5 doy) 2) 153)
        d     (clojure.core/+ (- doy (quot (clojure.core/+ (clojure.core/* 153 mp) 2) 5)) 1)
        m     (clojure.core/+ mp (if (< mp 10) 3 -9))
        yy    (if (<= m 2) (inc y) y)
        secs  (quot rem-ms 1000)
        h     (quot secs 3600)
        mi    (quot (mod secs 3600) 60)
        s     (mod secs 60)
        ns    (clojure.core/* (mod rem-ms 1000) 1000000)]
    (with-meta
      {:years yy :months m :days d :hours h :minutes mi :seconds s
       :nanoseconds ns :offset-sign 1 :offset-hours 0 :offset-minutes 0}
      {:mino/instant true})))

(defn inst-in-impl
  "Return a spec validating insts in the range from start (inclusive)
  to end (exclusive)."
  [start end]
  (let [st (inst-ms start)
        et (inst-ms end)
        sp (and-spec-impl
             (list 'clojure.core/inst?
                   (list 'clojure.spec.alpha/inst-in-range? start end '%))
             [inst? (fn [x] (inst-in-range? start end x))])
        sp (assoc sp ::form (list 'clojure.spec.alpha/inst-in start end))]
    (assoc sp ::gen
           (fn []
             (gen-impl/fmap
               (fn [ms-frac]
                 (let [span (- et st)
                       off  (long (clojure.core/* span ms-frac))]
                   (ms->inst (clojure.core/+ st (max 0 (min (dec span) off))))))
               (gen-impl/fmap
                 (fn [n] (clojure.core/abs
                           (- (clojure.core/double n)
                              (long (clojure.core/double n)))))
                 gen-impl/double))))))

;; Divergence from JVM clojure.spec.alpha: s/inst-in is a function (first-class value) on the
;; JVM; mino uses a macro. This means inst-in cannot be passed as a value in higher-order
;; contexts. Use inst-in-impl directly for higher-order use.
(defmacro inst-in
  "Return a spec validating insts in the range from start (inclusive)
  to end (exclusive)."
  [start end]
  `(clojure.spec.alpha/inst-in-impl ~start ~end))

;; ---------------------------------------------------------------------------
;; Macros at the bottom of the file.  Before this point, internal defns
;; resolve `def`, `and`, `or`, `cat`, `*`, `+`, `?`, `keys`, `nilable`,
;; `coll-of`, `map-of`, `tuple`, `assert`, `fdef`, `alt` to clojure.core
;; bindings (or special forms).  After this point, those names are
;; bound as macros in this namespace; users calling them as `s/and`,
;; `s/def`, etc. resolve correctly because qualified-symbol dispatch
;; bypasses the special-form table.
;; ---------------------------------------------------------------------------

;; ---------------------------------------------------------------------------
;; conformer -- a spec built from an arbitrary conform fn (and optional
;; unform fn). The conform fn must return ::invalid when the value does
;; not conform, otherwise the returned value is the conformed value.
;; ---------------------------------------------------------------------------

(defn conformer-impl
  "Return a spec that uses cfn as the conform path. cfn takes a value
  and returns the conformed value or :clojure.spec.alpha/invalid.
  Optionally takes an unform fn; if omitted, unform is identity."
  ([cfn]      (conformer-impl cfn identity))
  ([cfn ufn]
   {::kind ::conformer
    ::form (list 'clojure.spec.alpha/conformer cfn)
    ::conform-fn cfn
    ::unform-fn  ufn}))

(defmethod conform-impl ::conformer [s x]
  ((::conform-fn s) x))

(defmethod explain-impl ::conformer [s path via in x]
  (when (= ::invalid ((::conform-fn s) x))
    [{:path path :pred (::form s) :val x :via via :in in}]))

(defmethod unform-impl ::conformer [s y]
  ((::unform-fn s) y))

;; Divergence from JVM clojure.spec.alpha: s/conformer is a function (first-class value) on the
;; JVM; mino uses a macro. This means conformer cannot be passed as a value in higher-order
;; contexts. Use conformer-impl directly for higher-order use.
(defmacro conformer
  "Return a spec that uses cfn as the conform path. cfn takes a value
  and returns the conformed value or :clojure.spec.alpha/invalid.
  Optionally takes an unform fn; if omitted, unform is identity."
  [& args]
  `(clojure.spec.alpha/conformer-impl ~@args))

;; ---------------------------------------------------------------------------
;; nonconforming -- same acceptance as the wrapped spec, but conform
;; returns the original (not the conformed) value.
;; ---------------------------------------------------------------------------

(defn nonconforming
  "Return a spec with the same properties as spec, except conform
  returns the original (not the conformed) value."
  [spec]
  (let [sp (as-spec spec)]
    {::kind ::nonconforming
     ::form (list 'clojure.spec.alpha/nonconforming (::form sp))
     ::spec sp}))

(defmethod conform-impl ::nonconforming [s x]
  (let [r (conform* (::spec s) x)]
    (if (= ::invalid r) ::invalid x)))

(defmethod unform-impl ::nonconforming [_s y] y)

(defmethod explain-impl ::nonconforming [s path via in x]
  (explain* (::spec s) path via in x))

;; ---------------------------------------------------------------------------
;; keys* -- keys spec as a regex op.  An & wrapper consumes inline
;; keyword/value pairs via (* (cat ::k keyword? ::v any?)), the
;; kvs->map conformer turns the matched pairs into a map, and the keys
;; spec built from the same arguments validates that map.
;; ---------------------------------------------------------------------------

(def ^:private kvs->map-spec
  ;; Conformer threading keys* matches into a map; unform turns the
  ;; map back into the {::k k ::v v} pairs the regex unfolds.
  (conformer-impl
    (fn [kvs] (into {} (map (fn [kv] [(get kv ::k) (get kv ::v)]) kvs)))
    (fn [m] (map (fn [[k v]] {::k k ::v v}) m))))

(def ^:private keys*-kv-form
  '(clojure.spec.alpha/cat :clojure.spec.alpha/k clojure.core/keyword?
                           :clojure.spec.alpha/v clojure.core/any?))

(defn keys*-impl
  "Build the keys* regex op around mspec, a keys spec built from the
  same arguments.  Callers normally use the keys* macro."
  [mspec]
  (amp-impl (list 'clojure.spec.alpha/* keys*-kv-form)
            (list :clojure.spec.alpha/kvs->map (::form mspec))
            (rep-impl ::* keys*-kv-form
                      (cat-impl [::k ::v]
                                '[clojure.core/keyword? clojure.core/any?]
                                [keyword? any?]))
            [kvs->map-spec mspec]))

;; ---------------------------------------------------------------------------
;; with-gen -- attach an alternative generator to a spec. Generators
;; require clojure.test.check; the alternative is stored on the spec
;; map and surfaced through gen.
;; ---------------------------------------------------------------------------

(defn with-gen*
  "Protocol-shaped with-gen: return a copy of the spec value carrying
  gen-fn as its generator."
  [spec gen-fn]
  (assoc (as-spec spec) ::gen gen-fn))

(defn with-gen
  "Return a copy of spec that uses gen-fn as its generator. spec may
  be a registered keyword, a spec map, or any predicate-shaped value
  acceptable to as-spec."
  [spec gen-fn]
  (with-gen* spec gen-fn))

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
  `(clojure.spec.alpha/rep+impl '~(res pred) ~pred))

(defmacro ?
  "Zero or one occurrence of pred."
  [pred]
  `(clojure.spec.alpha/maybe-impl ~pred '~(res pred)))

(defmacro alt
  "Alternation: branches with named tags, single-element."
  [& key-pred-pairs]
  (let [pairs (partition 2 key-pred-pairs)
        keys  (mapv first pairs)
        forms (mapv second pairs)]
    `(clojure.spec.alpha/alt-impl '~keys '~(map res forms) ~forms)))

(defmacro &
  "Wrap a regex spec with additional predicates applied to the
  conformed sequence value. Each pred must return truthy."
  [re & preds]
  `(clojure.spec.alpha/amp-impl '~(res re) '~(mapv res preds)
                                ~re ~(vec preds)))

(defmacro fspec
  "Build a function spec from :args :ret and (optional) :fn preds.
  conform / explain take a fn and validate it via generative testing;
  the conformed value is always the fn itself.  :ret defaults to any?."
  [& opts]
  (let [opts-map (apply hash-map opts)
        args     (:args opts-map)
        ret      (clojure.core/or (:ret opts-map) `any?)
        fn-spec  (get opts-map :fn)]
    `(clojure.spec.alpha/fspec-impl
       ~args '~(res args)
       (clojure.spec.alpha/spec-impl '~(res ret) ~ret) '~(res ret)
       ~fn-spec '~(res fn-spec)
       nil)))

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
  "Spec-checking assert.  Returns x when it conforms to spec, else
  throws an ex-info carrying explain-data plus ::failure of
  :assertion-failed.  When *compile-asserts* is false at expansion the
  check compiles away to x; at runtime (check-asserts?) gates whether
  the compiled check evaluates."
  [spec x]
  (if clojure.spec.alpha/*compile-asserts*
    `(if (clojure.spec.alpha/check-asserts?)
       (clojure.spec.alpha/assert* ~spec ~x)
       ~x)
    x))

(defmacro every
  "Validate that every element of a collection satisfies pred.  Samples
  up to coll-check-limit (101) elements; does not conform elements; the
  input collection is returned unchanged on success.

  Opts: :kind pred, :count n, :min-count n, :max-count n, :distinct bool."
  [pred & opts]
  (let [opts-map (apply hash-map opts)]
    `(clojure.spec.alpha/every-impl
       '(clojure.spec.alpha/every ~(res pred) ~@opts)
       ~pred
       ~opts-map)))

(defmacro every-kv
  "Like every but validates associative entries as [kpred vpred] pairs.
  Works on maps and sequences of pairs."
  [kpred vpred & opts]
  (let [opts-map (apply hash-map opts)]
    `(clojure.spec.alpha/every-impl
       '(clojure.spec.alpha/every-kv ~(res kpred) ~(res vpred))
       (clojure.spec.alpha/tuple-impl '~[(res kpred) (res vpred)] [~kpred ~vpred])
       (assoc ~opts-map ::kv true))))

(defmacro keys*
  "Like keys but produces a regex spec that matches inline key/value
  pairs in a sequence.  Conforms to a map."
  [& opts]
  (let [opts-map (apply hash-map opts)]
    `(clojure.spec.alpha/keys*-impl
       (clojure.spec.alpha/keys-impl '~opts ~opts-map))))

(defmacro merge
  "Conjunction of map specs.  x must conform to every branch; the
  conformed maps are merged left to right."
  [& specs]
  `(clojure.spec.alpha/merge-spec-impl
     '~(mapv res specs)
     ~(vec specs)
     nil))

(defmacro multi-spec
  "Dispatch a spec through a multimethod.  mm is the multimethod
  symbol; retag is the keyword used to extract the dispatch value."
  [mm retag]
  `(clojure.spec.alpha/multi-spec-impl
     '~mm
     (resolve '~mm)
     ~retag))
