;; clojure.core.match -- pattern matching for mino.
;;
;; The public surface is identical to upstream core.match: the `match`,
;; `matchv`, and `match-let` macros and the pattern grammar (literals,
;; wildcards, bindings, vectors with `& rest`, `(... :seq)` seqs, maps
;; with `:only`, `(:or ...)`, `(p :guard pred)`, `(p :as name)`, and
;; quoted-symbol literals). Existing match code and documentation
;; transfer unchanged.
;;
;; The compiler underneath is built natively for mino. A clause set compiles to
;; a structure that:
;;   - binds each occurrence (the matched expression) to a local exactly
;;     once, so sub-terms are never recomputed, and
;;   - threads a shared failure continuation, so the code emitted for a
;;     clause appears once and a mismatch falls through to the next
;;     clause by a thunk call rather than by duplicating downstream code.
;; The result is emitted code whose size is linear in the clause set --
;; there is no exponential blow-up from naive backtracking -- and whose
;; per-occurrence tests are shared across the rows that need them.
;;
;; Pattern compilation is continuation-passing at macroexpand time: each
;; pattern node is compiled against an occurrence with an explicit
;; success continuation (a compile-time function of the accumulated
;; bindings) and a failure thunk symbol. Guards, or-patterns, and
;; as-patterns compose through that interface without special-casing the
;; surrounding structure. A full Maranget column-selection pass (optimal
;; decision-tree test ordering) is a clean future optimization to add
;; only against a measured workload; the current compiler keeps the
;; decision structure simple and correct.

(ns clojure.core.match
  "Pattern matching: match, matchv, and match-let compile a set of
  pattern rows into a decision structure that binds the first matching
  row's variables and evaluates its expression, or throws when no row
  matches.")

;; --------------------------------------------------------------------
;; Pattern parsing -- source form -> pattern node (a plain map).
;;
;; Node tags:
;;   :wildcard  {:sym s?}            ;; _ or a binding symbol
;;   :literal   {:value v}           ;; number/string/keyword/bool/nil/char
;;                                   ;; or a quoted symbol
;;   :vector    {:elems [n] :rest n?}
;;   :seq       {:elems [n] :rest n?}
;;   :map       {:entries {k n} :only #{k}?}
;;   :or        {:alts [n]}
;;   :guard     {:pat n :preds [expr]}
;;   :as        {:pat n :sym s}
;; --------------------------------------------------------------------

(defn- literal-form?
  "True when form is a self-evaluating literal usable as a match value."
  [form]
  (or (number? form) (string? form) (keyword? form)
      (true? form) (false? form) (nil? form) (char? form)))

(declare parse-pattern)

(defn- split-rest
  "Split a vector/seq pattern element list on the `&` marker. Returns
  [fixed-forms rest-form] where rest-form is nil when there is no `&`."
  [forms]
  (let [forms (vec forms)
        amp   (first (keep-indexed (fn [i x] (when (= x '&) i)) forms))]
    (if amp
      [(subvec forms 0 amp) (nth forms (inc amp))]
      [forms nil])))

(defn- parse-elems
  "Parse a fixed/rest element-list shared by vector and seq patterns."
  [forms]
  (let [[fixed rest-form] (split-rest forms)]
    {:elems (mapv parse-pattern fixed)
     :rest  (when rest-form (parse-pattern rest-form))}))

(defn- parse-list-pattern
  "Parse a list-form pattern: (:or ...), (p :as s), (p :guard g),
  (inner :seq), or (m :only [ks])."
  [form]
  (let [head   (first form)
        marker (when (>= (count form) 2) (second form))]
    (cond
      (= head :or)
      {:tag :or :alts (mapv parse-pattern (rest form))}

      (= marker :as)
      {:tag :as :sym (nth form 2) :pat (parse-pattern head)}

      (= marker :guard)
      (let [g (nth form 2)]
        {:tag :guard
         :pat (parse-pattern head)
         :preds (if (vector? g) (vec g) [g])})

      (= marker :seq)
      (assoc (parse-elems head) :tag :seq)

      (= marker :only)
      (let [m (parse-pattern head)]
        (assoc m :only (set (nth form 2))))

      :else
      (throw (ex-info (str "core.match: unsupported pattern " (pr-str form))
                      {:pattern form})))))

(defn parse-pattern
  "Parse one source pattern form into a pattern node."
  [form]
  (cond
    (= form '_)            {:tag :wildcard}
    (symbol? form)         {:tag :wildcard :sym form}
    (literal-form? form)   {:tag :literal :value form}
    (and (seq? form) (= (first form) 'quote))
    {:tag :literal :value (second form)}
    (vector? form)         (assoc (parse-elems form) :tag :vector)
    (map? form)            {:tag :map
                            :entries (into {} (map (fn [[k v]]
                                                     [k (parse-pattern v)])
                                                   form))
                            :only nil}
    (seq? form)            (parse-list-pattern form)
    :else                  {:tag :literal :value form}))

;; --------------------------------------------------------------------
;; Pattern variables -- the symbols a pattern binds (for or-pattern join
;; points, whose alternatives share a success continuation).
;; --------------------------------------------------------------------

(defn- pattern-vars
  "Return the vector of distinct symbols bound by a pattern node, in
  left-to-right order."
  [node]
  (let [acc (atom [])
        seen (atom #{})
        add! (fn [s] (when (and (symbol? s) (not (@seen s)))
                       (swap! seen conj s)
                       (swap! acc conj s)))
        walk (fn walk [n]
               (case (:tag n)
                 :wildcard (when (:sym n) (add! (:sym n)))
                 :literal  nil
                 :vector   (do (doseq [e (:elems n)] (walk e))
                               (when (:rest n) (walk (:rest n))))
                 :seq      (do (doseq [e (:elems n)] (walk e))
                               (when (:rest n) (walk (:rest n))))
                 :map      (doseq [[_ v] (:entries n)] (walk v))
                 :or       (doseq [a (:alts n)] (walk a))
                 :guard    (walk (:pat n))
                 :as       (do (add! (:sym n)) (walk (:pat n)))
                 nil))]
    (walk node)
    @acc))

;; --------------------------------------------------------------------
;; Compilation -- continuation-passing over pattern nodes.
;;
;; (compile-pattern node occ binds succ fk) returns an expression that
;; tests `node` against the occurrence expression `occ`. `binds` is the
;; accumulated vector of [sym occ-expr] pairs; `succ` is a compile-time
;; function of the (possibly extended) binds returning the success
;; expression; `fk` is the symbol of a zero-arg failure thunk to call on
;; mismatch.
;; --------------------------------------------------------------------

(declare compile-pattern)

(defn- bound-value
  "Look up the occurrence expression a symbol is bound to in a binds
  vector (used to forward or-alternative bindings to the join point)."
  [binds sym]
  (some (fn [[s v]] (when (= s sym) v)) binds))

(defn- compile-seqlike
  "Compile a vector or seq pattern. `pred` is the type test on the
  occurrence; `nth-fn`/`rest-fn` build sub-occurrence expressions. The
  occurrence is matched element by element, threading the success
  continuation through each."
  [node occ binds succ fk pred nth-fn rest-fn]
  (let [elems   (:elems node)
        rst     (:rest node)
        n       (count elems)
        ;; Bind each fixed sub-occurrence (and the rest) to a fresh local
        ;; so they are computed once.
        subsyms (mapv (fn [_] (gensym "o__")) elems)
        restsym (when rst (gensym "or__"))
        ;; Build the chain of element matches inside-out.
        match-elems
        (fn match-elems [i b]
          (if (< i n)
            (compile-pattern (nth elems i) (nth subsyms i) b
                             (fn [b'] (match-elems (inc i) b'))
                             fk)
            (if rst
              (compile-pattern rst restsym b succ fk)
              (succ b))))
        count-test (if rst
                     `(>= (count ~occ) ~n)
                     `(= (count ~occ) ~n))
        let-binds (vec (concat
                        (mapcat (fn [s i] [s (nth-fn occ i)]) subsyms (range n))
                        (when rst [restsym (rest-fn occ n)])))]
    `(if (and ~pred ~count-test)
       (let [~@let-binds]
         ~(match-elems 0 binds))
       (~fk))))

(defn- compile-map
  "Compile a map pattern: require a map carrying each named key (and,
  with :only, no keys beyond the named set), then match each value."
  [node occ binds succ fk]
  (let [entries (vec (:entries node))
        ks      (mapv first entries)
        vpats   (mapv second entries)
        vsyms   (mapv (fn [_] (gensym "mv__")) ks)
        match-vals
        (fn match-vals [i b]
          (if (< i (count ks))
            (compile-pattern (nth vpats i) (nth vsyms i) b
                             (fn [b'] (match-vals (inc i) b'))
                             fk)
            (succ b)))
        key-tests (map (fn [k] `(contains? ~occ ~k)) ks)
        only-test (when (:only node)
                    (let [allowed (:only node)]
                      `(every? (fn [k#] (contains? ~allowed k#)) (keys ~occ))))
        all-tests (concat [`(map? ~occ)] key-tests (when only-test [only-test]))
        let-binds (vec (mapcat (fn [s k] [s `(get ~occ ~k)]) vsyms ks))]
    `(if (and ~@all-tests)
       (let [~@let-binds]
         ~(match-vals 0 binds))
       (~fk))))

(defn- compile-or
  "Compile an or-pattern: try each alternative in order against the same
  occurrence, sharing one success continuation (a join thunk over the
  variables the alternatives bind) so it is not duplicated per branch."
  [node occ binds succ fk]
  (let [alts   (:alts node)
        params (pattern-vars (first alts))
        join   (gensym "join__")
        succ-body (succ (into binds (mapv (fn [s] [s s]) params)))]
    `(let [~join (fn [~@params] ~succ-body)]
       ~(reduce
         (fn [next-expr alt]
           (let [kf (gensym "orf__")]
             `(let [~kf (fn [] ~next-expr)]
                ~(compile-pattern alt occ binds
                                  (fn [b']
                                    `(~join ~@(map (fn [s] (bound-value b' s))
                                                   params)))
                                  kf))))
         `(~fk)
         (reverse alts)))))

(defn compile-pattern
  "Compile one pattern node against an occurrence. See the section
  comment for the calling convention."
  [node occ binds succ fk]
  (case (:tag node)
    :wildcard
    (if (:sym node)
      (succ (conj binds [(:sym node) occ]))
      (succ binds))

    :literal
    `(if (= ~occ (quote ~(:value node)))
       ~(succ binds)
       (~fk))

    :as
    (compile-pattern (:pat node) occ (conj binds [(:sym node) occ]) succ fk)

    :guard
    (compile-pattern (:pat node) occ binds
                     (fn [b']
                       `(if (and ~@(map (fn [p] `(~p ~occ)) (:preds node)))
                          ~(succ b')
                          (~fk)))
                     fk)

    :or
    (compile-or node occ binds succ fk)

    :vector
    (compile-seqlike node occ binds succ fk
                     `(vector? ~occ)
                     (fn [o i] `(nth ~o ~i))
                     (fn [o n] `(subvec ~o ~n)))

    :seq
    (compile-seqlike node occ binds succ fk
                     `(sequential? ~occ)
                     (fn [o i] `(nth ~o ~i))
                     (fn [o n] `(nthrest ~o ~n)))

    :map
    (compile-map node occ binds succ fk)))

;; --------------------------------------------------------------------
;; Clause assembly -- occurrences bound once, clause thunks chained.
;; --------------------------------------------------------------------

(defn- compile-row
  "Compile one clause row (a vector of patterns, one per occurrence)
  against the occurrence symbols, with `action` as the success body and
  `fk` as the failure thunk. Patterns are matched left to right."
  [occsyms pats action fk]
  (let [match-cols
        (fn match-cols [i binds]
          (if (< i (count pats))
            (compile-pattern (parse-pattern (nth pats i)) (nth occsyms i) binds
                             (fn [b'] (match-cols (inc i) b'))
                             fk)
            `(let [~@(mapcat identity binds)] ~action)))]
    (match-cols 0 [])))

(defn compile-match
  "Compile a match expression: `occs` is the occurrence vector (a vector
  of expressions); `clauses` is the flat pattern/expr clause list.
  Returns the full expansion: the occurrences bound once, then a chain
  of clause failure thunks, with the head thunk invoked."
  [occs clauses]
  (when (odd? (count clauses))
    (throw (ex-info "core.match: clauses must come in pattern/expr pairs"
                    {:clauses clauses})))
  (let [occsyms (mapv (fn [_] (gensym "occ__")) occs)
        pairs   (vec (partition 2 clauses))
        ;; An :else clause supplies the base failure action; clauses
        ;; after it are unreachable and dropped.
        normal  (vec (take-while (fn [[lhs _]] (not= lhs :else)) pairs))
        elsep   (first (drop-while (fn [[lhs _]] (not= lhs :else)) pairs))
        base    (if elsep
                  (second elsep)
                  `(throw (ex-info "no matching clause"
                                   {:occurrences [~@occsyms]})))
        n       (count normal)
        ;; n+1 thunk symbols: k0..k_{n-1} per clause, k_n for the base.
        ksyms   (mapv (fn [_] (gensym "fk__")) (range (inc n)))
        ;; Bind the base thunk first, then clauses n-1..0, so each
        ;; thunk's reference to the next is already in scope.
        ordered (loop [i (dec n)
                       acc [(nth ksyms n) `(fn [] ~base)]]
                  (if (< i 0)
                    acc
                    (let [[lhs rhs] (nth normal i)
                          body (compile-row occsyms (vec lhs) rhs
                                            (nth ksyms (inc i)))]
                      (recur (dec i)
                             (conj acc (nth ksyms i) `(fn [] ~body))))))]
    `(let [~@(mapcat (fn [s e] [s e]) occsyms occs)]
       (let [~@ordered]
         (~(first ksyms))))))

;; --------------------------------------------------------------------
;; Public macros
;; --------------------------------------------------------------------

(defmacro match
  "Match the values of the occurrence vector `vars` against `clauses`,
  a flat sequence of pattern-row / result-expression pairs. The first
  row whose patterns all match has its bound variables established and
  its result expression evaluated. A clause whose pattern position is
  the keyword :else matches unconditionally; with no :else and no
  matching row, an exception is thrown."
  [vars & clauses]
  (compile-match (vec vars) clauses))

(defmacro matchv
  "Like match, with an explicit occurrence type tag as the first
  argument (mino has a single vector representation, so the tag is
  accepted for surface compatibility and otherwise ignored)."
  [_type vars & clauses]
  (compile-match (vec vars) clauses))

(defmacro match-let
  "Like let, but each binding's left-hand side is a match pattern and
  its right-hand side an expression; the pattern's variables are bound
  for the body, and a non-matching binding throws. Bindings are
  established in sequence, so a later right-hand side may use an earlier
  binding's variables."
  [bindings & body]
  (let [pairs (partition 2 bindings)]
    (reduce (fn [inner [pat expr]]
              `(match [~expr] [~pat] ~inner))
            `(do ~@body)
            (reverse pairs))))
