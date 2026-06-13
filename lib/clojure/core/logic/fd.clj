;; clojure.core.logic.fd -- finite-domain (CLP(FD)) constraints for
;; core.logic.  The public surface matches upstream: in / interval /
;; domain for declaring domains, the arithmetic constraints +, -, *,
;; quot, the relational constraints ==, !=, <, <=, >, >=, the global
;; distinct, and the eq sugar for arithmetic expressions.
;;
;; A domain is represented as a sorted set of integers and the
;; propagation engine narrows domains to a fixpoint: each constraint is a
;; function installed in the package's constraint store (see
;; clojure.core.logic/run-constraints) that removes domain values it can
;; prove impossible, binds a variable once its domain is a singleton, and
;; fails when any domain becomes empty.  Small finite domains keep this
;; enumerate-and-filter engine simple and exact.

(ns clojure.core.logic.fd
  (:refer-clojure :exclude [== + - * < <= > >= quot distinct])
  (:require [clojure.core.logic :as l]))

;; --------------------------------------------------------------------
;; Domains
;; --------------------------------------------------------------------

(defn interval
  "The domain of all integers from lb to ub inclusive."
  [lb ub]
  (apply sorted-set (range lb (clojure.core/+ ub 1))))

(defn domain
  "The domain consisting of exactly the given integer values."
  [& vs]
  (apply sorted-set vs))

(defn- intersect
  "Intersection of two domains (sorted sets)."
  [d1 d2]
  (into (sorted-set) (filter #(contains? d2 %) d1)))

(defn- get-dom
  "The current domain of v: a singleton when v is bound to an integer,
  its stored domain when v is an unbound variable, or nil when v has no
  domain yet."
  [a v]
  (let [w (l/walk v (:s a))]
    (cond
      (integer? w) (sorted-set w)
      (l/lvar? w)  (get (:doms a) w)
      :else        nil)))

(defn- set-dom
  "Narrow v's domain to dom. Binds v directly when dom is a singleton,
  fails (nil) when dom is empty or excludes v's bound value."
  [a v dom]
  (let [w (l/walk v (:s a))]
    (cond
      (empty? dom) nil
      (integer? w) (if (contains? dom w) a nil)
      (l/lvar? w)
      (if (= (count dom) 1)
        ;; Binding the variable extends the substitution directly, so the
        ;; disequality store must be re-verified (returns nil on conflict).
        (l/verify-constraints
         (assoc a :s    (assoc (:s a) w (first dom))
                  :doms (dissoc (:doms a) w)))
        (assoc a :doms (assoc (:doms a) w dom)))
      :else nil)))

;; --------------------------------------------------------------------
;; in: declare a domain for one or more variables
;; --------------------------------------------------------------------

(defn in
  "Constrain each of the leading variables to the trailing domain."
  [& args]
  (let [vars (butlast args)
        dom  (last args)]
    (fn [a]
      (let [a' (reduce (fn [acc v]
                         (when acc
                           (let [cur (get-dom acc v)]
                             (set-dom acc v (if cur (intersect cur dom) dom)))))
                       a vars)]
        (if a'
          (let [a'' (l/run-constraints a')]
            (if a'' (l/unit a'') l/mzero))
          l/mzero)))))

;; --------------------------------------------------------------------
;; Arithmetic propagators (z = x op y), narrowed in every direction
;; --------------------------------------------------------------------

(defn- add-prop
  "Propagator for z = x + y over enumerated domains."
  [x y z]
  (fn [a]
    (let [dx (get-dom a x) dy (get-dom a y) dz (get-dom a z)]
      (if (and dx dy)
        (let [sums (into (sorted-set) (for [i dx j dy] (clojure.core/+ i j)))
              dz'  (if dz (intersect dz sums) sums)]
          (if (empty? dz')
            nil
            (let [dx' (into (sorted-set)
                            (filter (fn [i] (some (fn [j] (contains? dz' (clojure.core/+ i j))) dy)) dx))
                  dy' (into (sorted-set)
                            (filter (fn [j] (some (fn [i] (contains? dz' (clojure.core/+ i j))) dx)) dy))]
              (some-> a (set-dom z dz') (set-dom x dx') (set-dom y dy')))))
        a))))

(defn- mul-prop
  "Propagator for z = x * y over enumerated domains."
  [x y z]
  (fn [a]
    (let [dx (get-dom a x) dy (get-dom a y) dz (get-dom a z)]
      (if (and dx dy)
        (let [prods (into (sorted-set) (for [i dx j dy] (clojure.core/* i j)))
              dz'   (if dz (intersect dz prods) prods)]
          (if (empty? dz')
            nil
            (let [dx' (into (sorted-set)
                            (filter (fn [i] (some (fn [j] (contains? dz' (clojure.core/* i j))) dy)) dx))
                  dy' (into (sorted-set)
                            (filter (fn [j] (some (fn [i] (contains? dz' (clojure.core/* i j))) dx)) dy))]
              (some-> a (set-dom z dz') (set-dom x dx') (set-dom y dy')))))
        a))))

(defn- install
  "Add a propagator to the package, run all constraints to a fixpoint,
  and yield the result as a stream."
  [a c]
  (let [a' (l/run-constraints (update a :fc conj c))]
    (if a' (l/unit a') l/mzero)))

(defn +
  "Constraint z = x + y."
  [x y z]
  (fn [a] (install a (add-prop x y z))))

(defn -
  "Constraint z = x - y (that is, x = y + z)."
  [x y z]
  (fn [a] (install a (add-prop y z x))))

(defn *
  "Constraint z = x * y."
  [x y z]
  (fn [a] (install a (mul-prop x y z))))

(defn- quot-prop
  "Propagator for z = (quot x y), truncating toward zero, over enumerated
  domains. Zero is removed from y's domain (division by zero is excluded)."
  [x y z]
  (fn [a]
    (let [dx (get-dom a x) dy (get-dom a y)]
      (if (and dx dy)
        (let [dy*   (disj dy 0)
              dz    (get-dom a z)
              quots (into (sorted-set) (for [i dx j dy*] (clojure.core/quot i j)))
              dz'   (if dz (intersect dz quots) quots)]
          (if (or (empty? dy*) (empty? dz'))
            nil
            (let [dx' (into (sorted-set)
                            (filter (fn [i] (some (fn [j] (contains? dz' (clojure.core/quot i j))) dy*)) dx))
                  dy' (into (sorted-set)
                            (filter (fn [j] (some (fn [i] (contains? dz' (clojure.core/quot i j))) dx)) dy*))]
              (some-> a (set-dom z dz') (set-dom x dx') (set-dom y dy')))))
        a))))

(defn quot
  "Constraint z = (quot x y), truncating integer division toward zero."
  [x y z]
  (fn [a] (install a (quot-prop x y z))))

;; --------------------------------------------------------------------
;; Relational constraints
;; --------------------------------------------------------------------

(defn- eq-prop
  [x y]
  (fn [a]
    (let [dx (get-dom a x) dy (get-dom a y)]
      (if (and dx dy)
        (let [d (intersect dx dy)]
          (some-> a (set-dom x d) (set-dom y d)))
        a))))

(defn ==
  "Constraint x = y, intersecting their domains."
  [x y]
  (fn [a]
    (let [s (l/unify x y (:s a))]
      (if s
        (install (assoc a :s s) (eq-prop x y))
        l/mzero))))

(defn- neq-prop
  [x y]
  (fn [a]
    (let [dx (get-dom a x) dy (get-dom a y)]
      (cond
        (and dx dy (= 1 (count dx)) (= 1 (count dy)))
        (if (= (first dx) (first dy)) nil a)
        (and dx (= 1 (count dx)) dy) (set-dom a y (disj dy (first dx)))
        (and dy (= 1 (count dy)) dx) (set-dom a x (disj dx (first dy)))
        :else a))))

(defn !=
  "Constraint x != y."
  [x y]
  (fn [a] (install a (neq-prop x y))))

(defn- lt-prop
  [x y]
  (fn [a]
    (let [dx (get-dom a x) dy (get-dom a y)]
      (if (and dx dy)
        ;; x < y: drop from x anything not below y's max, and from y
        ;; anything not above x's min.
        (let [xmin (first dx) ymax (last dy)
              dx'  (into (sorted-set) (filter #(clojure.core/< % ymax) dx))
              dy'  (into (sorted-set) (filter #(clojure.core/> % xmin) dy))]
          (some-> a (set-dom x dx') (set-dom y dy')))
        a))))

(defn <
  "Constraint x < y."
  [x y]
  (fn [a] (install a (lt-prop x y))))

(defn >
  "Constraint x > y."
  [x y]
  (fn [a] (install a (lt-prop y x))))

(defn- le-prop
  [x y]
  (fn [a]
    (let [dx (get-dom a x) dy (get-dom a y)]
      (if (and dx dy)
        ;; x <= y: drop from x anything above y's max, and from y
        ;; anything below x's min.
        (let [xmin (first dx) ymax (last dy)
              dx'  (into (sorted-set) (filter #(clojure.core/<= % ymax) dx))
              dy'  (into (sorted-set) (filter #(clojure.core/>= % xmin) dy))]
          (some-> a (set-dom x dx') (set-dom y dy')))
        a))))

(defn <=
  "Constraint x <= y."
  [x y]
  (fn [a] (install a (le-prop x y))))

(defn >=
  "Constraint x >= y."
  [x y]
  (fn [a] (install a (le-prop y x))))

;; --------------------------------------------------------------------
;; distinct: all variables pairwise different
;; --------------------------------------------------------------------

(defn distinct
  "Constraint that every variable in the collection takes a different
  value. Pruning is forward-checking (a value is removed from the others
  once a variable becomes a singleton); labeling completes the search."
  [vars]
  (let [v (vec vars)
        n (count v)]
    (fn [a]
      (let [a' (reduce (fn [acc c] (l/run-constraints (update acc :fc conj c)))
                       a
                       (for [i (range n) j (range (clojure.core/+ i 1) n)]
                         (neq-prop (nth v i) (nth v j))))]
        (if a' (l/unit a') l/mzero)))))

;; --------------------------------------------------------------------
;; eq: write arithmetic relations with ordinary operators
;; --------------------------------------------------------------------

(defn- arith-op? [op]
  (contains? '#{+ - *} op))

(declare compile-expr)

(defn- compile-expr
  "Compile an arithmetic expression form into [value-form pre-goals],
  introducing fresh fd variables for sub-expressions."
  [form]
  (if (and (seq? form) (arith-op? (first form)))
    (let [[op a b] form
          [va ga]  (compile-expr a)
          [vb gb]  (compile-expr b)
          r        (gensym "fd_r")
          op-goal  (case op
                     + `(clojure.core.logic.fd/+ ~va ~vb ~r)
                     - `(clojure.core.logic.fd/- ~va ~vb ~r)
                     * `(clojure.core.logic.fd/* ~va ~vb ~r))]
      [r (concat ga gb [[r] op-goal])])
    [form nil]))

(defn- compile-rel
  "Compile one (rel-op lhs rhs) relation form into the goals that realize
  it via fd constraints."
  [form]
  (let [[op l r] form
        [vl gl] (compile-expr l)
        [vr gr] (compile-expr r)
        rel (case op
              = `(clojure.core.logic.fd/== ~vl ~vr)
              < `(clojure.core.logic.fd/< ~vl ~vr)
              <= `(clojure.core.logic.fd/<= ~vl ~vr)
              > `(clojure.core.logic.fd/> ~vl ~vr)
              >= `(clojure.core.logic.fd/>= ~vl ~vr)
              not= `(clojure.core.logic.fd/!= ~vl ~vr))]
    (concat gl gr [rel])))

(defmacro eq
  "Write finite-domain relations using ordinary arithmetic: each form is
  a relation like (= (+ x y) z) or (< x y); sub-expressions are compiled
  to fd constraints with fresh intermediate variables."
  [& forms]
  (let [pieces (mapcat compile-rel forms)
        ;; pieces alternate between [fresh-var] markers and goal forms.
        fresh-syms (vec (mapcat (fn [p] (when (vector? p) p)) pieces))
        goals (remove vector? pieces)]
    `(l/fresh [~@fresh-syms] ~@goals)))
