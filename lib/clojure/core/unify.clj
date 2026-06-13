;; clojure.core.unify — first-order syntactic unification.
;;
;; A small, pure unifier over ordinary data. Two terms are unified by
;; walking their structure in lockstep and collecting a binding map
;; that, when substituted into either term, makes both equal. Logic
;; variables are by default symbols whose name begins with `?` (`?x`,
;; `?tail`); everything else is ground data. A binding map flows through
;; the recursion as an accumulator, so a variable seen twice must agree
;; with the value it already carries or the whole unification fails.
;;
;; The default `unify` / `unifier` / `subst` entry points are built from
;; the factory functions `make-occurs-unify-fn`, `make-unify-fn`, and
;; `make-occurs-subst-fn`, each parameterized by a variable predicate so
;; callers can supply a different notion of "variable". The occurs check
;; is on by default: binding a variable to a term that contains the
;; variable itself (`?x` against `(f ?x)`) is rejected rather than
;; producing an infinite structure.

(ns clojure.core.unify
  "First-order syntactic unification over Clojure data. unify returns a
  substitution map that makes two terms equal, or nil when they cannot
  be made equal; unifier returns the unified term; subst applies a
  binding map to a term. Logic variables default to ?-prefixed
  symbols.")

;; --------------------------------------------------------------------
;; Variable and structure predicates
;; --------------------------------------------------------------------

(defn ignore-variable?
  "True for the wildcard variable `_`, which unifies with anything and
  never binds."
  [sym]
  (= sym '_))

(defn lvar?
  "Default logic-variable predicate: a symbol whose name starts with
  `?`. The wildcard `_` is treated as a variable as well."
  [x]
  (and (symbol? x)
       (or (ignore-variable? x)
           (= \? (first (name x))))))

(defn composite?
  "True for the compound terms the unifier descends into: lists, other
  seqs, vectors, and maps. Scalars and symbols are leaves."
  [x]
  (or (sequential? x)
      (map? x)))

;; --------------------------------------------------------------------
;; Occurs check
;; --------------------------------------------------------------------

(defn occurs?
  "True when variable `v` occurs anywhere inside `term` under the
  current bindings, following variable bindings transitively. Used to
  reject bindings that would build an infinite term."
  [variable? v term binds]
  (cond
    (= v term) true
    (and (variable? term) (contains? binds term))
    (occurs? variable? v (get binds term) binds)
    (composite? term)
    (cond
      (map? term)
      (some (fn [[k val]]
              (or (occurs? variable? v k binds)
                  (occurs? variable? v val binds)))
            term)
      :else
      (some #(occurs? variable? v % binds) (seq term)))
    :else false))

(defn bind-phase
  "Return the binding map extended with `variable` -> `expr`, unless
  `variable` is the wildcard `_`, in which case the binding is dropped
  (the wildcard unifies with anything and never records a value)."
  [binds variable expr]
  (if (ignore-variable? variable)
    binds
    (assoc binds variable expr)))

(defn- unify-variable
  "Unify a variable against a term. If the variable is already bound,
  recurse on its value; otherwise bind it to the term, running the
  occurs check first when `occurs-fn` is supplied. Returns the extended
  bindings or nil on failure."
  [variable? occurs-fn variable expr binds garner]
  (if (contains? binds variable)
    (garner (get binds variable) expr binds)
    (let [expr* (if (and (variable? expr) (contains? binds expr))
                  (get binds expr)
                  expr)]
      (cond
        (= variable expr*) binds
        (and occurs-fn (occurs-fn variable expr* binds)) nil
        :else (bind-phase binds variable expr*)))))

;; --------------------------------------------------------------------
;; Core unifier
;; --------------------------------------------------------------------

(defn make-garner-unifiers-fn
  "Build the recursive unifier `garner-unifiers` for a given variable
  predicate and occurs-check function. The returned fn takes two terms
  and a binding map and returns the extended bindings, or nil when the
  terms cannot be unified."
  [variable? occurs-fn]
  (letfn [(garner [x y binds]
            (cond
              (nil? binds) nil
              (variable? x) (unify-variable variable? occurs-fn x y binds garner)
              (variable? y) (unify-variable variable? occurs-fn y x binds garner)
              (and (map? x) (map? y))
              (if (= (set (keys x)) (set (keys y)))
                (reduce (fn [b k]
                          (if (nil? b)
                            nil
                            (garner (get x k) (get y k) b)))
                        binds
                        (keys x))
                nil)
              (and (sequential? x) (sequential? y))
              (cond
                (and (empty? x) (empty? y)) binds
                (or (empty? x) (empty? y)) nil
                :else
                (let [b (garner (first x) (first y) binds)]
                  (if (nil? b)
                    nil
                    (garner (rest x) (rest y) b))))
              (= x y) binds
              :else nil))]
    garner))

;; --------------------------------------------------------------------
;; Binding resolution
;; --------------------------------------------------------------------

(defn make-flatten-bindings-fn
  "Build `flatten-bindings` for a given variable predicate. The returned
  fn resolves every binding to its fully-determined value, chasing
  variable-to-variable chains (`?x` -> `?y` -> 3 collapses both `?x` and
  `?y` to 3)."
  [variable?]
  (letfn [(resolve* [v binds seen]
            (cond
              (and (variable? v) (contains? binds v))
              (if (contains? seen v)
                v
                (resolve* (get binds v) binds (conj seen v)))
              (composite? v)
              (cond
                (map? v) (into (empty v)
                               (map (fn [[k val]]
                                      [(resolve* k binds seen)
                                       (resolve* val binds seen)])
                                    v))
                (vector? v) (mapv #(resolve* % binds seen) v)
                :else (map #(resolve* % binds seen) v))
              :else v))]
    (fn [binds]
      (when binds
        (into {} (map (fn [[k v]] [k (resolve* v binds #{})]) binds))))))

;; --------------------------------------------------------------------
;; Substitution
;; --------------------------------------------------------------------

(defn make-occurs-subst-fn
  "Build `subst` for a given variable predicate. The returned fn walks a
  form and replaces every variable that has a binding with its bound
  value, descending into seqs, vectors, and maps."
  [variable?]
  (fn subst* [form binds]
    (cond
      (and (variable? form) (contains? binds form))
      (subst* (get binds form) binds)
      (composite? form)
      (cond
        (map? form) (into (empty form)
                          (map (fn [[k v]]
                                 [(subst* k binds) (subst* v binds)])
                               form))
        (vector? form) (mapv #(subst* % binds) form)
        :else (map #(subst* % binds) form))
      :else form)))

;; --------------------------------------------------------------------
;; Factory entry points
;; --------------------------------------------------------------------

(defn make-occurs-fn
  "Return an occurs-check function `(fn [variable term binds])` for the
  given variable predicate, or nil to disable the occurs check."
  [variable?]
  (fn [variable term binds]
    (occurs? variable? variable term binds)))

(defn make-unify-fn
  "Build a `unify`-style fn for the given variable predicate, with the
  occurs check disabled. The returned fn has arities `[x y]` and
  `[x y binds]` and yields the binding map (flattened) or nil."
  [variable?]
  (let [garner (make-garner-unifiers-fn variable? nil)
        flatten* (make-flatten-bindings-fn variable?)]
    (fn
      ([x y] (flatten* (garner x y {})))
      ([x y binds] (flatten* (garner x y binds))))))

(defn make-occurs-unify-fn
  "Build a `unify`-style fn for the given variable predicate, with the
  occurs check enabled. The returned fn has arities `[x y]` and
  `[x y binds]` and yields the binding map (flattened) or nil."
  [variable?]
  (let [garner (make-garner-unifiers-fn variable? (make-occurs-fn variable?))
        flatten* (make-flatten-bindings-fn variable?)]
    (fn
      ([x y] (flatten* (garner x y {})))
      ([x y binds] (flatten* (garner x y binds))))))

(defn make-unifier-fn
  "Build a `unifier`-style fn for the given variable predicate (occurs
  check enabled): unify x and y, then substitute the resulting bindings
  back into x, returning the unified term or nil on failure."
  [variable?]
  (let [unify* (make-occurs-unify-fn variable?)
        subst* (make-occurs-subst-fn variable?)]
    (fn
      ([x y]
       (let [binds (unify* x y)]
         (when binds (subst* x binds))))
      ([x y binds]
       (let [binds (unify* x y binds)]
         (when binds (subst* x binds)))))))

;; --------------------------------------------------------------------
;; Default surface (?-prefixed-symbol variables, occurs check on)
;; --------------------------------------------------------------------

(def garner-unifiers
  "The default recursive unifier: accumulates a binding map across
  composite terms (lists, vectors, maps, seqs) for ?-prefixed-symbol
  variables with the occurs check enabled. Returns extended bindings or
  nil. Call as (garner-unifiers x y binds)."
  (make-garner-unifiers-fn lvar? (make-occurs-fn lvar?)))

(def flatten-bindings
  "Resolve a binding map so each variable maps to its fully-determined
  value, chasing transitive variable-to-variable chains. Call as
  (flatten-bindings binds)."
  (make-flatten-bindings-fn lvar?))

(def subst
  "Substitute the values of a binding map into a form, walking nested
  seqs, vectors, and maps. Call as (subst form binds)."
  (make-occurs-subst-fn lvar?))

(def unify
  "Unify two terms and return a substitution map that makes them equal,
  or nil when they cannot be unified. Logic variables are ?-prefixed
  symbols; the occurs check is on. Arities: (unify x y) and
  (unify x y subst)."
  (make-occurs-unify-fn lvar?))

(def unifier
  "Unify two terms, then substitute the resulting bindings into the
  first term, returning the unified term (or nil on failure). Arities:
  (unifier x y) and (unifier x y subst)."
  (make-unifier-fn lvar?))

(def unifier-
  "The non-occurs-checking unifier constructor's default surface: like
  unify but with the occurs check disabled. Returns the binding map or
  nil. Provided for callers that knowingly want to skip the occurs
  check."
  (make-unify-fn lvar?))
