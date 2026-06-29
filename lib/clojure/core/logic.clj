;; clojure.core.logic -- relational (logic) programming for mino.
;;
;; The user-facing surface is identical to upstream core.logic: the
;; run / run* / fresh / conde / conda / condu macros, the == and !=
;; goals, the relation library (membero, appendo, conso, ...), defrel
;; facts, and (in the companion namespaces) CLP(FD) and nominal logic.
;; Programs and documentation written for core.logic transfer unchanged.
;;
;; The engine underneath is built natively for mino:
;;   - A logic variable is a defrecord carrying a unique id; the
;;     substitution is a persistent map keyed on those variables and the
;;     constraint store is a persistent vector -- no mutable cells, all
;;     persistent data.
;;   - A goal is a function from a package (substitution + constraints)
;;     to a stream of packages.  Streams are a trampolined thunk ADT and
;;     the disjunction operator interleaves them, so search is complete:
;;     an answer to the right of a divergent branch is still reached.
;;   - walk is loop-structured and the relation library is written so a
;;     relation's recursion advances by goal application rather than by
;;     deep C recursion. (Unifying or reifying a single very long ground
;;     list still recurses by its length, against mino's recursion guard.)
;;
;; A logical-cons cell (LCons) lets a list have an unbound tail, which is
;; what makes conso / appendo / membero relational in both directions.

(ns clojure.core.logic
  "Relational logic programming: run / run* drive a set of goals built
  from == (unify), != (disequality), fresh, conde, and a relation
  library, reifying the query variable's answers."
  (:refer-clojure :exclude [== walk run]))

;; --------------------------------------------------------------------
;; Logic variables and logical cons cells
;; --------------------------------------------------------------------

(defrecord LVar [id])
(defrecord LCons [car cdr])

(def ^:private lvar-id (atom 0))

(defn lvar
  "Return a fresh logic variable. The optional name is advisory; identity
  is by a unique id."
  ([] (->LVar (swap! lvar-id inc)))
  ([_name] (->LVar (swap! lvar-id inc))))

(defn lvar?
  "True when x is a logic variable."
  [x]
  (instance? LVar x))

(defn lcons
  "Construct a logical cons cell whose tail d may be a logic variable (an
  improper / partial list). When d is already a proper sequence the cell
  still unifies element-by-element like an ordinary cons."
  [a d]
  (->LCons a d))

(defn- lcons? [x] (instance? LCons x))

;; --------------------------------------------------------------------
;; The package carries four field slots:
;;   s    -- the substitution map
;;   cs   -- the disequality store (a vector of prefix maps, each a
;;           conjunction of bindings that must not all hold at once)
;;   doms -- the finite-domain map (lvar -> domain), used by the fd
;;           companion namespace
;;   fc   -- a vector of constraint functions (a -> a or nil); each is a
;;           propagator that the fd namespace installs and that run on
;;           every unification to a fixpoint
;; --------------------------------------------------------------------

(defrecord Subst [s cs doms fc])

(defn empty-s [] (->Subst {} [] {} []))

;; Extension hook for custom logic terms (the nominal namespace's Nom and
;; Tie). A type that participates in unification, deep-walking, and
;; reification implements ITerm. The hook is checked last on each path, so
;; a program that uses no custom terms pays only a single membership test
;; on the fall-through (non-collection, non-variable) terms. This relies
;; on records not being maps in mino: a Nom/Tie reaches the ITerm branch
;; only because (map? a-record) is false, so the map branch never
;; intercepts it.
(defprotocol ITerm
  (-unify-term [u v s]
    "Unify custom term u with v under substitution map s; return the
    extended map or nil.")
  (-walk-term [u s]
    "Deep-walk custom term u under s.")
  (-reify-term [u names]
    "Accumulate reification names from custom term u into names."))

;; --------------------------------------------------------------------
;; walk / walk* / unify
;; --------------------------------------------------------------------

(defn walk
  "Follow variable bindings in the substitution map s until reaching a
  non-variable or an unbound variable. Loop-structured."
  [u s]
  (loop [u u]
    (if (lvar? u)
      (let [v (get s u ::unbound)]
        (if (= v ::unbound) u (recur v)))
      u)))

(defn walk*
  "Resolve u against s through nested structure: variables, logical cons
  cells, sequentials, and maps are all walked recursively."
  [u s]
  (let [u (walk u s)]
    (cond
      (lvar? u) u
      (lcons? u)
      (let [a (walk* (:car u) s)
            d (walk* (:cdr u) s)]
        (cond
          (lvar? d)  (->LCons a d)
          (lcons? d) (->LCons a d)
          (nil? d)   (list a)
          (sequential? d) (cons a d)
          :else (->LCons a d)))
      (vector? u) (mapv #(walk* % s) u)
      (map? u)    (into {} (map (fn [[k v]] [(walk* k s) (walk* v s)]) u))
      (sequential? u) (map #(walk* % s) u)
      (satisfies? ITerm u) (-walk-term u s)
      :else u)))

(declare unify-lcons)

(defn unify
  "Unify terms u and v under substitution map s, returning the extended
  map or nil. No occurs check (matching core.logic's default ==)."
  [u v s]
  (let [u (walk u s)
        v (walk v s)]
    (cond
      (= u v) s
      (lvar? u) (assoc s u v)
      (lvar? v) (assoc s v u)
      (lcons? u) (unify-lcons u v s)
      (lcons? v) (unify-lcons v u s)
      (and (sequential? u) (sequential? v))
      (cond
        (and (empty? u) (empty? v)) s
        (or (empty? u) (empty? v)) nil
        :else (let [s (unify (first u) (first v) s)]
                (when s (unify (rest u) (rest v) s))))
      (and (map? u) (map? v))
      (if (= (set (keys u)) (set (keys v)))
        (reduce (fn [s k] (when s (unify (get u k) (get v k) s))) s (keys u))
        nil)
      (satisfies? ITerm u) (-unify-term u v s)
      (satisfies? ITerm v) (-unify-term v u s)
      :else nil)))

(defn- unify-lcons
  "Unify a logical cons cell l against a term v (a cons cell or a
  non-empty sequence)."
  [l v s]
  (cond
    (lcons? v)
    (let [s (unify (:car l) (:car v) s)]
      (when s (unify (:cdr l) (:cdr v) s)))
    (sequential? v)
    (if (empty? v)
      nil
      (let [s (unify (:car l) (first v) s)]
        (when s (unify (:cdr l) (rest v) s))))
    (lvar? v) (assoc s v l)
    :else nil))

;; --------------------------------------------------------------------
;; Disequality constraint store
;; --------------------------------------------------------------------

(defn- prefix-of
  "The entries of the extended map s' that are not already in s (the new
  bindings unify introduced)."
  [s' s]
  (into {} (filter (fn [[k _]] (not (contains? s k))) s')))

(defn- unify-prefix
  "Try to satisfy every binding of the prefix map c against s, returning
  the extended map, nil (the bindings can never all hold), or s unchanged
  (they already all hold)."
  [c s]
  (reduce (fn [s [u v]] (when s (unify u v s))) s (seq c)))

(defn verify-constraints
  "Re-check every stored disequality after a unification. Returns the
  package with its constraint store pruned, or nil when a constraint is
  violated (all of its bindings now hold). Any code that extends the
  substitution directly (finite-domain labeling, singleton binding) must
  pass the result through here so disequality stays sound."
  [a]
  (loop [cs (:cs a) kept []]
    (if (empty? cs)
      (assoc a :cs kept)
      (let [c (first cs)
            s' (unify-prefix c (:s a))]
        (cond
          (nil? s') (recur (rest cs) kept)          ;; can never hold -> satisfied
          (= s' (:s a)) nil                          ;; all hold -> violated
          :else (recur (rest cs)
                       (conj kept (prefix-of s' (:s a)))))))))

;; --------------------------------------------------------------------
;; Streams with an interleaving, trampolined disjunction for complete
;; search.
;;
;; A stream is one of three things:
;;   nil          -- the empty stream (mzero)
;;   [head tail]  -- a mature pair: a package and the rest of the stream
;;   (fn [] ...)  -- an immature thunk: one suspended step of work
;;
;; The thunk is what makes search complete and bounded: mplus, on meeting
;; a thunk, performs a single step and *swaps* the two streams, so a
;; divergent branch yields control after each step instead of looping to
;; the first (never-arriving) answer the way a lazy-seq's seq would.
;; --------------------------------------------------------------------

(def mzero nil)

(defn unit
  "A mature stream of exactly one package."
  [a] [a nil])

(defn- mplus
  "Interleave two streams. On a thunk, take one step and swap operands so
  neither branch can starve the other; on a mature pair, surface its head
  and continue with the rest."
  [s1 s2]
  (cond
    (nil? s1) s2
    (fn? s1)  (fn [] (mplus s2 (s1)))
    :else     [(nth s1 0) (mplus (nth s1 1) s2)]))

(defn bind
  "Feed every package in stream s to goal g, interleaving the results.
  Thunks defer; mature pairs run g on the head and bind the tail."
  [s g]
  (cond
    (nil? s) mzero
    (fn? s)  (fn [] (bind (s) g))
    :else    (mplus (g (nth s 0)) (bind (nth s 1) g))))

(defn mplus*
  "Interleave a list of streams."
  [streams]
  (if (seq streams)
    (mplus (first streams) (fn [] (mplus* (rest streams))))
    mzero))

(defn pull
  "Force a stream past its thunks to a mature pair or nil."
  [s]
  (loop [s s]
    (if (fn? s) (recur (s)) s)))

;; --------------------------------------------------------------------
;; Generic constraint store
;;
;; Each constraint in (:fc a) is a propagator (fn [a] -> a or nil). They
;; run to a fixpoint after every unification, so a domain narrowed by one
;; constraint feeds the others. Pure relational programs carry no
;; constraints, so this is a no-op for them.
;; --------------------------------------------------------------------

(defn run-constraints
  "Run the package's constraint propagators to a fixpoint. Returns the
  stabilized package, or nil when any propagator fails."
  [a]
  (loop [a a]
    (if (empty? (:fc a))
      a
      (let [a' (reduce (fn [acc c] (when acc (c acc))) a (:fc a))]
        (cond
          (nil? a') nil
          (and (= (:s a') (:s a)) (= (:doms a') (:doms a))) a'
          :else (recur a'))))))

;; --------------------------------------------------------------------
;; Core goals
;; --------------------------------------------------------------------

(defn ==
  "Unify goal: succeed when u and v can be made equal, extending the
  substitution, re-checking disequality constraints, and propagating any
  finite-domain constraints to a fixpoint."
  [u v]
  (fn [a]
    (let [s (unify u v (:s a))]
      (if s
        (let [a' (verify-constraints (assoc a :s s))]
          (if a'
            (let [a'' (run-constraints a')]
              (if a'' (unit a'') mzero))
            mzero))
        mzero))))

(defn !=
  "Disequality goal: succeed while u and v are kept from unifying. Adds a
  constraint that fails the moment a later unification would make them
  equal."
  [u v]
  (fn [a]
    (let [s' (unify u v (:s a))]
      (cond
        (nil? s') (unit a)              ;; can never be equal -> always holds
        (= s' (:s a)) mzero             ;; already equal -> fails now
        :else (unit (update a :cs conj (prefix-of s' (:s a))))))))

(defn succeed
  "Goal that always succeeds, leaving the package unchanged."
  [a]
  (unit a))

(defn fail
  "Goal that always fails."
  [a]
  mzero)

(def s# succeed)
(def u# fail)

(defn all
  "Conjoin goals: thread the stream through each in turn."
  [& goals]
  (fn [a]
    (reduce (fn [stream g] (bind stream g)) (unit a) goals)))

;; --------------------------------------------------------------------
;; Reification
;; --------------------------------------------------------------------

(defn reify-names
  "Walk a fully-resolved term and assign each remaining logic variable a
  reification name _0, _1, ... in first-appearance order."
  [v names]
  (cond
    (lvar? v) (if (contains? names v)
                names
                (assoc names v (symbol (str "_" (count names)))))
    (lcons? v) (reify-names (:cdr v) (reify-names (:car v) names))
    (vector? v) (reduce (fn [n x] (reify-names x n)) names v)
    (map? v) (reduce (fn [n [k val]] (reify-names val (reify-names k n))) names v)
    (sequential? v) (reduce (fn [n x] (reify-names x n)) names v)
    (satisfies? ITerm v) (-reify-term v names)
    :else names))

(defn reify-out
  "Resolve the query variable against a package and replace any remaining
  logic variables with their reification names."
  [v a]
  (let [v (walk* v (:s a))
        names (reify-names v {})]
    (walk* v names)))

;; --------------------------------------------------------------------
;; fresh / conde / run
;; --------------------------------------------------------------------

(defmacro fresh
  "Introduce fresh logic variables bound for the conjunction of goals."
  [vars & goals]
  (let [a (gensym "a")]
    `(fn [~a]
       (let [~@(mapcat (fn [v] [v `(lvar '~v)]) vars)]
         ((all ~@goals) ~a)))))

(defmacro conde
  "Disjunction of conjunctions: each clause is a vector of goals; the
  clauses are searched with interleaving so all are explored fairly."
  [& clauses]
  (let [a (gensym "a")]
    `(fn [~a]
       (mplus* (list ~@(map (fn [clause]
                              `(fn [] ((all ~@clause) ~a)))
                            clauses))))))

(defn take-stream
  "Realize up to n packages from a stream (all of them when n is false),
  driving the trampoline with pull."
  [n stream]
  (loop [n n s stream acc []]
    (if (and n (zero? n))
      (seq acc)
      (let [s (pull s)]
        (if (nil? s)
          (seq acc)
          (recur (and n (dec n)) (nth s 1) (conj acc (nth s 0))))))))

(defn force-doms
  "Labeling goal: bind every variable that still carries a finite domain
  to a value from it, branching over the choices and propagating after
  each assignment, so finite-domain queries enumerate their solutions.
  A no-op for packages without domains."
  [a]
  (if (empty? (:doms a))
    (unit a)
    ;; Label the lowest-id domain variable first, for a deterministic
    ;; answer order independent of map iteration order.
    (let [[v dom] (apply min-key (fn [[k _]] (:id k)) (seq (:doms a)))]
      (mplus*
       (map (fn [val]
              (fn []
                (let [a2 (assoc a :doms (dissoc (:doms a) v))
                      s  (unify v val (:s a2))]
                  (if s
                    (let [a3 (verify-constraints (assoc a2 :s s))]
                      (if a3
                        (let [a4 (run-constraints a3)]
                          (if a4 (force-doms a4) mzero))
                        mzero))
                    mzero))))
            dom)))))

(defmacro run
  "Search the goals and reify the first n answers for the query variable
  (n may be false for all answers; see run*)."
  [n bindings & goals]
  (let [q (first bindings)]
    `(let [~q (lvar '~q)]
       (map (fn [a#] (reify-out ~q a#))
            (take-stream ~n (bind ((all ~@goals) (empty-s)) force-doms))))))

(defmacro run*
  "Search the goals and reify every answer for the query variable."
  [bindings & goals]
  `(run false ~bindings ~@goals))

;; --------------------------------------------------------------------
;; Committed choice: conda (soft cut) and condu (once + soft cut)
;; --------------------------------------------------------------------

(defn ifa
  "Soft-cut combinator for conda: if the head stream has any answer,
  commit to running the body over it; otherwise fall through to the
  alternative thunk."
  [head body alt]
  (let [h (pull head)]
    (if (nil? h)
      (alt)
      (bind h body))))

(defn ifu
  "Like ifa, but the head contributes at most one answer (condu)."
  [head body alt]
  (let [h (pull head)]
    (if (nil? h)
      (alt)
      (bind (unit (nth h 0)) body))))

(defmacro conda
  "Committed choice: the first clause whose head goal succeeds is the
  only clause taken; its body runs over every head answer."
  [& clauses]
  (let [a (gensym "a")
        build (fn build [cls]
                (if (empty? cls)
                  `(fn [] mzero)
                  (let [[h & body] (first (vec cls))]
                    `(fn []
                       (ifa (~h ~a)
                            (all ~@body)
                            ~(build (rest cls)))))))]
    `(fn [~a] (~(build clauses)))))

(defmacro condu
  "Like conda, but each chosen clause's head contributes at most one
  answer."
  [& clauses]
  (let [a (gensym "a")
        build (fn build [cls]
                (if (empty? cls)
                  `(fn [] mzero)
                  (let [[h & body] (first (vec cls))]
                    `(fn []
                       (ifu (~h ~a)
                            (all ~@body)
                            ~(build (rest cls)))))))]
    `(fn [~a] (~(build clauses)))))

;; --------------------------------------------------------------------
;; project: bind the walked values of vars as ordinary locals
;; --------------------------------------------------------------------

(defmacro project
  "Bind the substitution's current values for vars as ordinary locals
  inside the goals, so ordinary non-relational code can inspect them."
  [vars & goals]
  (let [a (gensym "a")]
    `(fn [~a]
       (let [~@(mapcat (fn [v] [v `(walk* ~v (:s ~a))]) vars)]
         ((all ~@goals) ~a)))))

;; --------------------------------------------------------------------
;; The relation library
;; --------------------------------------------------------------------

(defn conso
  "Relate a list l to its head a and tail d (l = (a . d))."
  [a d l]
  (== (->LCons a d) l))

(defn firsto
  "Relate the first element of l to a."
  [l a]
  (fresh [d] (conso a d l)))

(defn resto
  "Relate the rest of l to d."
  [l d]
  (fresh [a] (conso a d l)))

(defn emptyo
  "Succeed when l is the empty list."
  [l]
  (== l ()))

(defn nexto
  "Relate adjacent elements x and y in list l (y immediately follows x)."
  [x y l]
  (fresh [t]
    (conde
      [(firsto l x) (resto l (lcons y t))]
      [(fresh [r] (resto l r) (nexto x y r))])))

(defn membero
  "Succeed once for each way x is a member of list l."
  [x l]
  (fresh [a d]
    (conso a d l)
    (conde
      [(== a x)]
      [(membero x d)])))

(defn rembero
  "Relate out to l with one occurrence of x removed."
  [x l out]
  (conde
    [(fresh [d] (conso x d l) (== d out))]
    [(fresh [a d res]
       (conso a d l)
       (conso a res out)
       (!= a x)
       (rembero x d res))]))

(defn appendo
  "Relate the concatenation of l1 and l2 to o (relational in all
  positions)."
  [l1 l2 o]
  (conde
    [(== l1 ()) (== l2 o)]
    [(fresh [a d res]
       (conso a d l1)
       (conso a res o)
       (appendo d l2 res))]))

(defn everyg
  "Goal that holds when (g elem) succeeds for every element of coll,
  where g is a one-argument goal constructor. coll is an ordinary,
  fully-realized collection (not a partial logic list)."
  [g coll]
  (if (empty? coll)
    succeed
    (all (g (first coll)) (everyg g (rest coll)))))

(defn distincto
  "Succeed when every element of l is pairwise distinct. l is an ordinary,
  fully-realized collection; the pairwise disequalities are posted up
  front."
  [l]
  (let [v (vec l)
        n (count v)
        pairs (for [i (range n) j (range (inc i) n)] [(nth v i) (nth v j)])]
    (reduce (fn [g [a b]] (all g (!= a b))) succeed pairs)))

(defn permuteo
  "Succeed when xl is a permutation of yl."
  [xl yl]
  (conde
    [(== xl ()) (== yl ())]
    [(fresh [a d res]
       (conso a d xl)
       (rembero a yl res)
       (permuteo d res))]))

(defn lvaro
  "Succeed when v is, at this point, still an unbound logic variable."
  [v]
  (fn [a]
    (if (lvar? (walk v (:s a))) (unit a) mzero)))

(defn nonlvaro
  "Succeed when v is, at this point, bound to a non-variable."
  [v]
  (fn [a]
    (if (lvar? (walk v (:s a))) mzero (unit a))))

;; --------------------------------------------------------------------
;; Pattern-matching relation sugar: matche / matcha / matchu and the
;; defne / fne / defna / defnu / defnu definers.  A clause head is a
;; vector of patterns matched positionally against the goal variables;
;; symbols become fresh logic variables, `_` is an anonymous fresh
;; variable, a quoted form is a literal, `[a b]` is a vector term, and a
;; `.` inside a vector/list pattern introduces a logical-cons tail.
;; --------------------------------------------------------------------

(defn- match-wildcard? [x] (= x '_))

(defn- rename-wildcards
  "Replace each `_` in a pattern with a distinct fresh symbol so every
  wildcard is its own logic variable."
  [pat]
  (cond
    (match-wildcard? pat) (gensym "_wld")
    (vector? pat) (mapv rename-wildcards pat)
    (and (seq? pat) (= (first pat) 'quote)) pat
    (seq? pat) (map rename-wildcards pat)
    :else pat))

(defn- pattern-symbols
  "Collect the distinct binding symbols of a pattern (excluding the
  symbols in `exclude`, the dot marker, and quoted literals)."
  [pat exclude]
  (let [acc (atom [])
        seen (atom #{})
        add! (fn [s] (when (and (symbol? s) (not= s '.)
                                (not (contains? exclude s))
                                (not (@seen s)))
                       (swap! seen conj s)
                       (swap! acc conj s)))
        go (fn go [p]
             (cond
               (symbol? p) (add! p)
               (vector? p) (doseq [x p] (go x))
               (and (seq? p) (= (first p) 'quote)) nil
               (seq? p) (doseq [x p] (go x))
               :else nil))]
    (go pat)
    @acc))

(declare pattern->term)

(defn- seq-pattern->term
  "Build a term from a sequence of element patterns, turning a `.` marker
  into a logical-cons tail."
  [elems]
  (let [v   (vec elems)
        dot (first (keep-indexed (fn [i x] (when (= x '.) i)) v))]
    (if dot
      (let [fixed (subvec v 0 dot)
            tail  (nth v (inc dot))]
        (reduce (fn [acc p] `(lcons ~(pattern->term p) ~acc))
                (pattern->term tail)
                (reverse fixed)))
      (reduce (fn [acc p] `(lcons ~(pattern->term p) ~acc))
              ()
              (reverse v)))))

(defn- pattern->term
  "Translate a pattern form into a term-constructing form: symbols stay
  as locals (the fresh logic variables), quoted forms and scalars are
  literals, vectors without a `.` are vector terms, and `.`-bearing
  vectors/lists become logical-cons chains."
  [pat]
  (cond
    (symbol? pat) pat
    (vector? pat) (if (some #(= % '.) pat)
                    (seq-pattern->term (seq pat))
                    (mapv pattern->term pat))
    (and (seq? pat) (= (first pat) 'quote)) pat
    (seq? pat) (seq-pattern->term pat)
    :else pat))

(defn- match-clause
  "Expand one matche/matcha/matchu clause into a goal vector: a fresh over
  the clause's new variables, the positional unifications, and the body
  goals."
  [vars exclude clause]
  (let [pats (mapv rename-wildcards (first clause))
        body (rest clause)
        fresh-syms (vec (mapcat #(pattern-symbols % exclude) pats))]
    `[(fresh [~@fresh-syms]
        ~@(map (fn [v p] `(== ~v ~(pattern->term p))) vars pats)
        ~@body)]))

(defmacro matche
  "conde with pattern-matching clause heads: each clause head is a vector
  of patterns unified positionally against vars."
  [vars & clauses]
  (let [vars (vec vars)
        exclude (set vars)]
    `(conde ~@(map #(match-clause vars exclude %) clauses))))

(defmacro matcha
  "conda with pattern-matching clause heads."
  [vars & clauses]
  (let [vars (vec vars)
        exclude (set vars)]
    `(conda ~@(map #(match-clause vars exclude %) clauses))))

(defmacro matchu
  "condu with pattern-matching clause heads."
  [vars & clauses]
  (let [vars (vec vars)
        exclude (set vars)]
    `(condu ~@(map #(match-clause vars exclude %) clauses))))

(defmacro defne
  "Define a relation by pattern-matching clauses (matche over its args)."
  [name args & clauses]
  `(defn ~name ~args (matche ~(vec args) ~@clauses)))

(defmacro fne
  "Anonymous defne: a relation value with matche clauses."
  [args & clauses]
  `(fn ~args (matche ~(vec args) ~@clauses)))

(defmacro defna
  "Define a relation by committed-choice pattern clauses (matcha)."
  [name args & clauses]
  `(defn ~name ~args (matcha ~(vec args) ~@clauses)))

(defmacro defnu
  "Define a relation by committed-choice once clauses (matchu)."
  [name args & clauses]
  `(defn ~name ~args (matchu ~(vec args) ~@clauses)))

;; --------------------------------------------------------------------
;; Facts database: defrel / fact / facts / retract.  A relation is backed
;; by an atom holding a set of argument tuples; querying it is a
;; disjunction over the stored tuples.
;; --------------------------------------------------------------------

(defn add-fact!
  "Add one tuple to a relation var's fact store."
  [rel-var tuple]
  (swap! (::facts (meta rel-var)) conj (vec tuple)))

(defmacro defrel
  "Define a relation backed by a fact set. (name a b ...) is a goal that
  succeeds once for every stored tuple unifying with the arguments."
  [name & args]
  `(let [store# (atom #{})]
     (defn ~name [~@args]
       (let [argv# [~@args]]
         (fn [a#]
           (mplus* (map (fn [tuple#] (fn [] ((== argv# tuple#) a#)))
                        @store#)))))
     (alter-meta! (var ~name) assoc ::facts store# ::arity ~(count args))
     (var ~name)))

(defmacro fact
  "Add a single fact tuple to relation rel."
  [rel & vs]
  `(add-fact! (var ~rel) [~@vs]))

(defmacro facts
  "Add many fact tuples (a sequence of tuples) to relation rel."
  [rel rows]
  `(doseq [row# ~rows] (add-fact! (var ~rel) row#)))

(defmacro retract
  "Remove a single fact tuple from relation rel."
  [rel & vs]
  `(swap! (::facts (meta (var ~rel))) disj [~@vs]))

(defn retractions
  "Return a function that retracts the given tuple from rel-var when
  called (the inverse of asserting it)."
  [rel-var tuple]
  (fn [] (swap! (::facts (meta rel-var)) disj (vec tuple))))

;; --------------------------------------------------------------------
;; Tabling: tabled memoizes a relation's answers so that recursion over a
;; cyclic relation (transitive closure, reachability) terminates and is
;; not recomputed.  A tabled relation keeps a per-call-key answer table;
;; the first (driver) call solves the table to a fixpoint -- repeatedly
;; evaluating the body, cutting cycles by yielding the answers found so
;; far, until a full pass discovers nothing new -- and then yields the
;; collected answers.  Re-entrant calls during the solve contribute and
;; read through the same table.
;; --------------------------------------------------------------------

(defn- refresh-term
  "Replace every distinct logic variable in a term with a fresh one,
  consistently. A stored tabled answer may contain variables bound only
  within the package that produced it; refreshing on retrieval gives each
  use its own fresh variables, so a non-ground answer reused elsewhere
  cannot collide by variable identity. Ground answers are returned
  unchanged."
  [t]
  (let [m (atom {})
        go (fn go [x]
             (cond
               (lvar? x) (or (get @m x)
                             (let [n (lvar)] (swap! m assoc x n) n))
               (lcons? x) (->LCons (go (:car x)) (go (:cdr x)))
               (vector? x) (mapv go x)
               (map? x) (into {} (map (fn [[k v]] [(go k) (go v)]) x))
               (sequential? x) (map go x)
               :else x))]
    (go t)))

(defn- answers-stream
  "Yield every stored answer for a table key, unified against the call
  arguments under package a (with fresh variables per retrieval)."
  [table key argv a]
  (mplus* (map (fn [ans] (fn [] ((== argv (refresh-term ans)) a)))
               (get @table key []))))

(defn- tabled-pass
  "One pass over a tabled call: when the key is already on the solving
  stack, yield the known answers (cutting the cycle); otherwise run the
  body, recording each newly discovered answer in the table."
  [table inprog changed key argv goal a]
  (if (contains? @inprog key)
    (answers-stream table key argv a)
    (do
      (swap! inprog conj key)
      (bind (goal a)
            (fn [a2]
              (let [ans (walk* argv (:s a2))]
                (when-not (some #(= % ans) (get @table key))
                  (swap! table update key (fn [v] (conj (or v []) ans)))
                  (reset! changed true))
                (unit a2)))))))

(defn tabled-rel
  "Build a tabled relation from body-fn, a function from the relation's
  arguments to its goal."
  [body-fn]
  (let [table   (atom {})
        inprog  (atom #{})
        changed (atom false)
        driving (atom false)]
    (fn [& args]
      (let [argv (vec args)]
        (fn [a]
          (let [key (walk* argv (:s a))]
            (if @driving
              (tabled-pass table inprog changed key argv (apply body-fn args) a)
              (do
                (reset! driving true)
                (loop []
                  (reset! inprog #{})
                  (reset! changed false)
                  (doall (take-stream false
                                      (tabled-pass table inprog changed key argv
                                                   (apply body-fn args) a)))
                  (when @changed (recur)))
                (reset! driving false)
                (answers-stream table key argv a)))))))))

(defmacro tabled
  "Define a tabled relation: like fn over args, but the relation's answers
  are memoized so recursion over a cyclic relation terminates."
  [args & body]
  `(tabled-rel (fn [~@args] (all ~@body))))
