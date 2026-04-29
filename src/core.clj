;; mino core library
;; Compiled into the binary at build time via core_mino.h.

;; --- Macros ---

(defmacro when
  "Evaluates body when test is truthy. Returns nil otherwise."
  [c & body]
  `(if ~c (do ~@body)))

(defmacro cond
  "Takes pairs of test/expr. Returns the expr for the first truthy test."
  [& clauses]
  (if (< (count clauses) 2)
    nil
    `(if ~(first clauses)
         ~(first (rest clauses))
         (cond ~@(rest (rest clauses))))))

(defmacro and
  "Returns the first falsy value, or the last value if all are truthy."
  [& xs]
  (if (= 0 (count xs))
    true
    (if (= 1 (count xs))
      (first xs)
      (let [g (gensym)]
        `(let [~g ~(first xs)]
           (if ~g (and ~@(rest xs)) ~g))))))

(defmacro or
  "Returns the first truthy value, or the last value if none are truthy."
  [& xs]
  (if (= 0 (count xs))
    nil
    (if (= 1 (count xs))
      (first xs)
      (let [g (gensym)]
        `(let [~g ~(first xs)]
           (if ~g ~g (or ~@(rest xs))))))))

(defmacro ->
  "Thread-first. Inserts x as the second item in the first form, then
  inserts the result as the second item in the next form, and so on."
  [x & forms]
  (if (= 0 (count forms))
    x
    (let [step (first forms)]
      (if (cons? step)
        `(-> (~(first step) ~x ~@(rest step)) ~@(rest forms))
        `(-> (~step ~x) ~@(rest forms))))))

(defmacro ->>
  "Thread-last. Inserts x as the last item in the first form, then
  inserts the result as the last item in the next form, and so on."
  [x & forms]
  (if (= 0 (count forms))
    x
    (let [step (first forms)]
      (if (cons? step)
        `(->> (~(first step) ~@(rest step) ~x) ~@(rest forms))
        `(->> (~step ~x) ~@(rest forms))))))

;; --- Logic and utilities ---

;; not is a C primitive.
(def not=     "Returns true if the arguments are not equal." (fn
                ([x] false)
                ([x y] (not (= x y)))
                ([x y & more] (not (apply = x y more)))))
(def identity "Returns its argument." (fn [x] x))
;; list is a C primitive.
;; empty? is a C primitive.

;; >, <=, >= are C primitives (compare_chain) - see prim/numeric.c

;; --- Type predicates ---
;; Most primitive type predicates (nil?, cons?, string?, number?, keyword?,
;; symbol?, vector?, map?, fn?, set?, seq?) are defined as C primitives
;; for speed. ifn? is kept here since it combines multiple predicates.

(def ifn?     "Returns true if x can be called as a function." (fn [x] (or (fn? x) (keyword? x) (map? x) (vector? x) (set? x) (symbol? x) (var? x))))

;; --- Qualified symbol/keyword predicates ---

(def qualified-symbol?  "Returns true if x is a namespace-qualified symbol." (fn [x] (and (symbol? x) (some? (namespace x)))))
(def simple-symbol?     "Returns true if x is a symbol with no namespace." (fn [x] (and (symbol? x) (nil? (namespace x)))))
(def qualified-keyword? "Returns true if x is a namespace-qualified keyword." (fn [x] (and (keyword? x) (some? (namespace x)))))
(def simple-keyword?    "Returns true if x is a keyword with no namespace." (fn [x] (and (keyword? x) (nil? (namespace x)))))

;; --- Atoms ---

;; swap! is defined as a C primitive; no mino-level fallback needed.

;; --- Definitions ---

;; Walk a single fn arity (params-vec body...) and, if the first body
;; form is a {:pre [...] :post [...]} map, rewrite the arity so the
;; conditions run around the body. % in :post bodies refers to the
;; return value, matching Clojure.
(def ^:private fn-arity-with-prepost
  (fn [arity]
    (let [params (first arity)
          body   (rest arity)
          head   (first body)]
      (if (and (map? head) (or (contains? head :pre) (contains? head :post)))
        (let [pre   (get head :pre [])
              post  (get head :post [])
              rest-body (rest body)
              assert-pre (map (fn [p]
                                (list 'when-not p
                                      (list 'throw
                                            (list 'ex-info
                                                  (str "Pre-condition failed: " (pr-str p))
                                                  {:pre (list 'quote p)})))) pre)
              assert-post (map (fn [p]
                                 (list 'when-not p
                                       (list 'throw
                                             (list 'ex-info
                                                   (str "Post-condition failed: " (pr-str p))
                                                   {:post (list 'quote p)})))) post)
              wrapped (apply list
                             (concat assert-pre
                                     [(apply list 'let ['% (apply list 'do rest-body)]
                                             [(apply list 'do
                                                     (concat assert-post ['%]))])]))]
          (cons params wrapped))
        arity))))

(defmacro defn "Defines a named function. Supports docstrings, multi-arity, and :pre/:post conditions." [name & fdecl]
  (let [has-doc  (string? (first fdecl))
        doc      (if has-doc (first fdecl) nil)
        fdecl    (if has-doc (rest fdecl) fdecl)
        has-attr (map? (first fdecl))
        fdecl    (if has-attr (rest fdecl) fdecl)
        ;; If the first remaining form is a vector, this is single-arity
        ;; (params-vec body...). Otherwise it's a sequence of arity
        ;; lists (params-vec body...) (params-vec body...). Handle the
        ;; :pre/:post map either way.
        rewritten (if (vector? (first fdecl))
                    (fn-arity-with-prepost fdecl)
                    (mapv fn-arity-with-prepost fdecl))
        form     (if (vector? (first fdecl))
                   (cons 'fn rewritten)
                   (apply list 'fn rewritten))]
    (if doc
      `(def ~name ~doc ~form)
      `(def ~name ~form))))

(defmacro defn- "Same as defn, yielding a non-public def." [name & body]
  (apply list 'defn (vary-meta name assoc :private true) body))

(defmacro defonce "Defines name only if it has no root binding." [name expr]
  `(when-not (resolve '~name)
     (def ~name ~expr)))

;; --- Lazy sequence operations ---

(def ^:private map1 lazy-map-1)

(defn- all-some? [coll]
  (if (empty? coll)
    true
    (if (first coll)
      (all-some? (rest coll))
      false)))

(defn- map-n [f seqs]
  (lazy-seq
    (let [ss (map1 seq seqs)]
      (when (all-some? ss)
        (cons (apply f (map1 first ss))
              (map-n f (map1 rest ss)))))))

(defn map "Returns a lazy sequence of applying f to each item in coll. When called with multiple collections, maps f across them in parallel. When called with no collection, returns a transducer."
  ([f]
   (fn [rf]
     (fn ([] (rf))
         ([result] (rf result))
         ([result input] (rf result (f input)))
         ([result input & inputs] (rf result (apply f input inputs))))))
  ([f & colls]
   (if (= (count colls) 1)
     (map1 f (first colls))
     (map-n f colls))))

(defn filter "Returns a lazy sequence of items in coll for which pred returns truthy. When called with no collection, returns a transducer."
  ([pred]
   (fn [rf]
     (fn ([] (rf))
         ([result] (rf result))
         ([result input]
          (if (pred input)
            (rf result input)
            result)))))
  ([pred coll] (lazy-filter pred coll)))

(defn take "Returns a lazy sequence of the first n items in coll. When called with no collection, returns a transducer."
  ([n]
   (fn [rf]
     (let [remaining (volatile! n)]
       (fn ([] (rf))
           ([result] (rf result))
           ([result input]
            (let [r @remaining]
              (vswap! remaining dec)
              (if (> r 0)
                (let [ret (rf result input)]
                  (if (= (dec r) 0)
                    (reduced ret)
                    ret))
                result)))))))
  ([n coll] (lazy-take n coll)))

(defn drop "Returns a lazy sequence of all but the first n items in coll. When called with no collection, returns a transducer."
  ([n]
   (fn [rf]
     (let [remaining (volatile! n)]
       (fn ([] (rf))
           ([result] (rf result))
           ([result input]
            (if (> @remaining 0)
              (do (vswap! remaining dec) result)
              (rf result input)))))))
  ([n coll] (drop-seq n coll)))

(defn concat "Returns a lazy sequence of the concatenation of the given collections." [& colls]
  (lazy-seq
    (when (seq colls)
      (let [s (seq (first colls))]
        (if s
          (cons (first s) (apply concat (cons (rest s) (rest colls))))
          (apply concat (rest colls)))))))

;; range is a C primitive.

(defn repeat "Returns a lazy sequence of xs. With two args, returns n repetitions of x."
  ([x]
   (lazy-seq (cons x (repeat x))))
  ([n x]
   (lazy-seq
     (when (> n 0)
       (cons x (repeat (- n 1) x))))))

;; --- Collection utilities ---

(defn update "Updates the value at key k in map m by applying f to the old value and any args." [m k f & args]
  (assoc m k (apply f (get m k) args)))

;; --- Utility functions ---

(defn some "Returns the first truthy value of (pred x) for any x in coll, else nil." [pred coll]
  (when (not (empty? coll))
    (or (pred (first coll))
        (some pred (rest coll)))))

(defn every? "Returns true if (pred x) is truthy for every x in coll." [pred coll]
  (if (empty? coll)
    true
    (if (pred (first coll))
      (every? pred (rest coll))
      false)))

;; --- Higher-order functions ---

(defn comp "Returns a function that is the composition of the given functions."
  ([] identity)
  ([f] f)
  ([f g]
   (fn ([] (f (g)))
       ([x] (f (g x)))
       ([x y] (f (g x y)))
       ([x y z] (f (g x y z)))
       ([x y z & args] (f (apply g x y z args)))))
  ([f g & fs]
   (reduce comp (cons f (cons g fs)))))

(defn partial "Returns a function that applies f with the given arguments prepended."
  ([f] f)
  ([f arg1]
   (fn ([] (f arg1))
       ([x] (f arg1 x))
       ([x y] (f arg1 x y))
       ([x y z] (f arg1 x y z))
       ([x y z & args] (apply f arg1 x y z args))))
  ([f arg1 arg2]
   (fn ([] (f arg1 arg2))
       ([x] (f arg1 arg2 x))
       ([x y] (f arg1 arg2 x y))
       ([x y z] (f arg1 arg2 x y z))
       ([x y z & args] (apply f arg1 arg2 x y z args))))
  ([f arg1 arg2 arg3]
   (fn ([] (f arg1 arg2 arg3))
       ([x] (f arg1 arg2 arg3 x))
       ([x y] (f arg1 arg2 arg3 x y))
       ([x y z] (f arg1 arg2 arg3 x y z))
       ([x y z & args] (apply f arg1 arg2 arg3 x y z args))))
  ([f arg1 arg2 arg3 & more]
   (fn [& args] (apply f arg1 arg2 arg3 (concat more args)))))
(defn complement "Returns a function that returns the logical opposite of f." [f] (fn [& args] (not (apply f args))))

;; --- Trivial compositions ---

(defn second "Returns the second item in coll." [coll] (first (rest coll)))
(defn ffirst "Returns the first item of the first item in coll." [coll] (first (first coll)))
;; inc and dec are C primitives.
;; zero? is a C primitive.
(defn == "Returns true if nums are numerically equal, treating ints and floats uniformly."
                  ([x] true)
                  ([x y] (= (+ 0.0 x) (+ 0.0 y)))
                  ([x y & more]
                    (if (== x y)
                      (apply == y more)
                      false)))
;; pos?, neg?, even?, odd? are C primitives.
(defn abs "Returns the absolute value of x." [x] (if (< x 0) (- x) x))
(defn max "Returns the greatest of the given values."
                  ([a] a)
                  ([a b] (if (NaN? a) a (if (NaN? b) b (if (> a b) a b))))
                  ([a b & more] (reduce max (max a b) more)))
(defn min "Returns the least of the given values."
                  ([a] a)
                  ([a b] (if (NaN? a) a (if (NaN? b) b (if (< a b) a b))))
                  ([a b & more] (reduce min (min a b) more)))
(defn min-key "Returns the x for which (k x) is least."
                  ([k x] x)
                  ([k x y] (if (< (k x) (k y)) x y))
                  ([k x y & more]
                    (reduce (fn [best v] (min-key k best v))
                            (min-key k x y) more)))
(defn max-key "Returns the x for which (k x) is greatest."
                  ([k x] x)
                  ([k x y] (if (> (k x) (k y)) x y))
                  ([k x y & more]
                    (reduce (fn [best v] (max-key k best v))
                            (max-key k x y) more)))
(defn not-empty "Returns coll if it has items, nil otherwise." [coll] (if (seq coll) coll nil))
(defn constantly "Returns a function that always returns x." [x] (fn [& _] x))
(defn boolean "Coerces x to a boolean value." [x] (if x true false))
;; seq? is defined as a C primitive; no mino-level fallback needed.

;; --- Collection utilities ---

(defn merge "Returns a map that is the merge of the given maps." [& maps]
  (when (some identity maps)
    (reduce (fn [acc m]
      (if m
        (reduce (fn [a kv] (assoc a (first kv) (second kv)))
                (if acc acc (with-meta {} (meta m))) (seq m))
        acc))
      nil maps)))

(defn select-keys "Returns a map containing only the entries whose keys are in ks." [m ks]
  (reduce (fn [acc k]
    (if (contains? m k)
      (assoc acc k (get m k))
      acc))
    (with-meta {} (meta m)) ks))

(defn zipmap "Returns a map with keys mapped to corresponding vals." [ks vs]
  (letfn [(zm-impl [acc ks vs]
            (if (and (seq ks) (seq vs))
              (recur (assoc acc (first ks) (first vs))
                     (rest ks) (rest vs))
              acc))]
    (zm-impl {} ks vs)))

(defn frequencies "Returns a map from distinct items in coll to the number of times they appear." [coll]
  (persistent!
    (reduce (fn [acc x]
              (assoc! acc x (inc (get acc x 0))))
            (transient {}) coll)))

(defn group-by "Returns a map of the items in coll grouped by the result of f." [f coll]
  (persistent!
    (reduce (fn [acc x]
              (let [k (f x)]
                (assoc! acc k (conj (get acc k []) x))))
            (transient {}) coll)))

;; --- More higher-order ---

(defn juxt "Returns a function that returns a vector of applying each f to its args." [& fs]
  (fn [& args] (vec (map (fn [f] (apply f args)) fs))))

(defn mapcat "Returns the result of applying concat to the result of mapping f over coll. When called with no collection, returns a transducer."
  ([f] (comp (map f) cat))
  ([f coll]
   (let [cat-lazy (fn cat-lazy [s]
                    (lazy-seq
                      (when (seq s)
                        (let [inner (seq (f (first s)))]
                          (if inner
                            (concat inner (cat-lazy (rest s)))
                            (cat-lazy (rest s)))))))]
     (cat-lazy coll)))
  ([f c1 c2]
   (mapcat (fn [pair] (apply f pair))
           (map vector c1 c2)))
  ([f c1 c2 & colls]
   (mapcat (fn [args] (apply f args))
           (apply map vector c1 c2 colls))))

;; --- Lazy combinators ---

(defn take-while "Returns a lazy sequence of items from coll while pred returns truthy. When called with no collection, returns a transducer."
  ([pred]
   (fn [rf]
     (fn ([] (rf))
         ([result] (rf result))
         ([result input]
          (if (pred input)
            (rf result input)
            (reduced result))))))
  ([pred coll]
   (lazy-seq
     (let [s (seq coll)]
       (when (and s (pred (first s)))
         (cons (first s) (take-while pred (rest s))))))))

(defn drop-while "Returns a lazy sequence of items from coll after pred returns falsy. When called with no collection, returns a transducer."
  ([pred]
   (fn [rf]
     (let [dropping (volatile! true)]
       (fn ([] (rf))
           ([result] (rf result))
           ([result input]
            (if (and @dropping (pred input))
              result
              (do (vreset! dropping false)
                  (rf result input))))))))
  ([pred coll]
   (let [step (fn [pred coll]
                (let [s (seq coll)]
                  (if (and s (pred (first s)))
                    (recur pred (rest s))
                    s)))]
     (lazy-seq (step pred coll)))))

(defn take-nth "Returns a lazy sequence of every nth item in coll. When called with no collection, returns a transducer."
  ([n]
   (fn [rf]
     (let [i (volatile! -1)]
       (fn ([] (rf))
           ([result] (rf result))
           ([result input]
            (vswap! i inc)
            (if (= 0 (mod @i n))
              (rf result input)
              result))))))
  ([n coll]
   (lazy-seq
     (let [s (seq coll)]
       (when s
         (cons (first s) (take-nth n (drop n s))))))))

(defmacro lazy-cat "Expands to code that yields a lazy concatenation of the given collections." [& colls]
  (if (seq colls)
    `(lazy-seq (concat ~(first colls) (lazy-cat ~@(rest colls))))
    `(lazy-seq nil)))

(defn iterate "Returns a lazy sequence of x, (f x), (f (f x)), and so on." [f x]
  (lazy-seq (cons x (iterate f (f x)))))

(defn iteration
  "Creates a seqable via repeated calls to step, a function of some
   continuation token 'k'. The first call to step is passed initk,
   returning 'ret'. If (somef ret) is true, (vf ret) is included in
   the iteration; else iteration terminates and vf/kf are not called.
   If (kf ret) is non-nil it is passed to the next step call; else
   iteration terminates. Used to consume APIs that return paginated
   or batched data.

   step  - (possibly impure) fn of 'k' -> 'ret'
   :somef - fn of 'ret' -> truthy/falsy, default some?
   :vf    - fn of 'ret' -> 'v', default identity
   :kf    - fn of 'ret' -> 'next-k' or nil, default identity
   :initk - first value passed to step, default nil

   Step with non-initk is presumed unreproducible. The first step
   call is deferred until the result is realized.

   Divergence from canon: opts are passed as a single map (mino's
   destructuring does not yet support `& {:keys [...]}` keyword args)."
  ([step] (iteration step {}))
  ([step opts]
   (let [somef (get opts :somef some?)
         vf    (get opts :vf identity)
         kf    (get opts :kf identity)
         initk (get opts :initk nil)]
     (lazy-seq
       ((fn next-iter [ret]
          (when (somef ret)
            (cons (vf ret)
                  (when-some [k (kf ret)]
                    (lazy-seq (next-iter (step k)))))))
        (step initk))))))

(defn cycle "Returns a lazy infinite sequence of repetitions of the items in coll." [coll]
  (letfn [(cycle-impl [orig coll]
            (lazy-seq
              (let [s (seq coll)]
                (if s
                  (cons (first s) (cycle-impl orig (rest s)))
                  (when (seq orig)
                    (cycle-impl orig orig))))))]
    (cycle-impl coll coll)))

(defn repeatedly "Returns a lazy sequence of calls to f. With two args, returns n calls."
  ([f]   (lazy-seq (cons (f) (repeatedly f))))
  ([n f] (take n (repeatedly f))))

(def interleave "Returns a lazy sequence of the first item in each collection, then the second, and so on."
  (let [interleave2 (fn [c1 c2]
         (lazy-seq
           (let [s1 (seq c1) s2 (seq c2)]
             (when (and s1 s2)
               (cons (first s1) (cons (first s2)
                 (interleave2 (rest s1) (rest s2))))))))]
    (fn
      ([] ())
      ([c1] (lazy-seq (seq c1)))
      ([c1 c2] (interleave2 c1 c2))
      ([c1 c2 & colls]
       (lazy-seq
         (let [ss (map seq (cons c1 (cons c2 colls)))]
           (when (every? identity ss)
             (concat (map first ss)
                     (apply interleave (map rest ss))))))))))

(defn interpose "Returns a lazy sequence of the items in coll separated by sep. When called with no collection, returns a transducer."
  ([sep]
   (fn [rf]
     (let [started (volatile! false)]
       (fn ([] (rf))
           ([result] (rf result))
           ([result input]
            (if @started
              (let [r (rf result sep)]
                (if (reduced? r) r (rf r input)))
              (do (vreset! started true)
                  (rf result input))))))))
  ([sep coll]
   (lazy-seq
     (let [s (seq coll)]
       (when s
         (cons (first s)
           (mapcat (fn [x] (list sep x)) (rest s))))))))

(defn distinct "Returns a lazy sequence of the distinct items in coll. When called with no collection, returns a transducer."
  ([]
   (fn [rf]
     (let [seen (volatile! #{})]
       (fn ([] (rf))
           ([result] (rf result))
           ([result input]
            (if (contains? @seen input)
              result
              (do (vswap! seen conj input)
                  (rf result input))))))))
  ([coll]
   (let [step (fn step [xs seen]
                (lazy-seq
                  ((fn [xs seen]
                     (when-let [s (seq xs)]
                       (let [f (first s)]
                         (if (contains? seen f)
                           (recur (rest s) seen)
                           (cons f (step (rest s) (conj seen f)))))))
                   xs seen)))]
     (step coll #{}))))

(def partition "Returns a lazy sequence of lists of n items each, at offsets step apart. With pad, the final partition is filled from pad to reach n; if pad is shorter than needed, returns a partition with fewer than n items."
  (let [part-impl
        (fn part-impl [n step coll]
          (lazy-seq
            (when-let [s (seq coll)]
              (let [p (doall (take n s))]
                (when (= n (count p))
                  (cons p (part-impl n step (drop step s))))))))
        part-pad-impl
        (fn part-pad-impl [n step pad coll]
          (lazy-seq
            (when-let [s (seq coll)]
              (let [p (doall (take n s))]
                (if (= n (count p))
                  (cons p (part-pad-impl n step pad (drop step s)))
                  (list (take n (concat p pad))))))))]
    (fn
      ([n coll]            (part-impl n n coll))
      ([n step coll]       (part-impl n step coll))
      ([n step pad coll]   (part-pad-impl n step pad coll)))))

(defn partition-by "Splits coll into lazy sequences of consecutive items with the same (f item) value. When called with no collection, returns a transducer."
  ([f]
   (fn [rf]
     (let [buf (volatile! [])
           pval (volatile! ::none)]
       (fn ([] (rf))
           ([result]
            (let [b @buf]
              (if (empty? b)
                (rf result)
                (let [r (rf result b)]
                  (rf (unreduced r))))))
           ([result input]
            (let [v (f input)
                  p @pval]
              (vreset! pval v)
              (if (or (= p ::none) (= v p))
                (do (vswap! buf conj input) result)
                (let [b @buf]
                  (vreset! buf [input])
                  (rf result b)))))))))
  ([f coll]
   (lazy-seq
     (let [s (seq coll)]
       (when s
         (let [v (f (first s))
               run (cons (first s)
                     (take-while (fn [x] (= (f x) v)) (rest s)))
               remaining (drop (count run) s)]
           (cons run (partition-by f remaining))))))))

;; --- Forcing ---

;; doall and dorun are C primitives.

;; --- Type predicates and trivial compositions ---
;; true?, false?, boolean?, int?, float?, char? are C primitives.

(def integer?  "Returns true if x is an integer." int?)
(defn pos-int? "Returns true if x is a positive integer." [x] (and (int? x) (pos? x)))
(defn neg-int? "Returns true if x is a negative integer." [x] (and (int? x) (neg? x)))
(defn nat-int? "Returns true if x is a non-negative integer." [x] (and (int? x) (not (neg? x))))
(def double?   "Returns true if x is a float." float?)
;; ratio? / rational? / decimal? are C primitives that consult the real
;; numeric-tower types (MINO_RATIO, MINO_BIGDEC); registered in prim.c.
(def long      "Coerces x to an integer." int)
(def double    "Coerces x to a float." float)
(defn num "Returns x if it is a number, otherwise throws." [x] (if (number? x) x (throw "num expects a number")))
(defn coll? "Returns true if x is a collection." [x] (or (seq? x) (vector? x) (map? x) (set? x)))
;; some? is a C primitive.
(def list?     "Returns true if x is a list." cons?)
;; atom? is defined as a C primitive; no mino-level fallback needed.
(defn not-any? "Returns true if (pred x) is falsy for every x in coll." [pred coll] (not (some pred coll)))
(defn not-every? "Returns true if (pred x) is falsy for at least one x in coll." [pred coll] (not (every? pred coll)))
(defn distinct? "Returns true if no two of the arguments are equal." [& xs]
  (if (empty? xs)
    true
    (let [s (set xs)]
      (= (count s) (count xs)))))
(def array-map    "Creates a hash-map." hash-map)
(defn sorted? "Returns true if x is a sorted collection." [x] (let [t (type x)] (or (= t :sorted-map) (= t :sorted-set))))
(defn associative? "Returns true if x supports assoc (maps and vectors)." [x] (let [t (type x)] (or (= t :map) (= t :vector) (= t :sorted-map))))
(defn reversible? "Returns true if x supports rseq (vectors)." [x] (= (type x) :vector))
(defn any? "Returns true for any argument." [x] true)
(defn seqable? "Returns true if (seq x) is supported." [x] (or (nil? x) (coll? x) (string? x)))
(defn indexed? "Returns true if x supports nth in constant time (vectors)." [x] (vector? x))

;; --- Delay (lazy thunk) ---

(defn delay? "Returns true if x is a delay." [x] (and (map? x) (contains? x :delay/fn)))
(defmacro delay "Creates a delay that evaluates body on first deref." [& body]
  `(let [state# (atom {:status :pending})]
     {:delay/fn  (fn []
                   (let [s# @state#]
                     (if (= (:status s#) :done)
                       (:value s#)
                       (let [v# (do ~@body)]
                         (reset! state# {:status :done :value v#})
                         v#))))
      :delay/state state#}))
(defn deref-delay "Forces evaluation of a delay and returns its value." [d] ((:delay/fn d)))
(defn force "Forces evaluation of a delay. If x is not a delay, returns x." [x] (if (delay? x) (deref-delay x) x))
;; Override C realized? to also handle delays and futures
(let [c-realized? realized?]
  (def realized? "Returns true if a delay, lazy sequence, future, or promise has been realized." (fn [x]
    (cond
      (nil? x)    (throw "realized? requires a non-nil argument")
      (delay? x)  (= :done (:status @(:delay/state x)))
      (future? x) (future-done? x)
      :else       (c-realized? x)))))

;; --- Sequence navigation ---

(defn next "Returns a seq of the items after the first. Returns nil if no more items." [coll] (seq (rest coll)))
(defn nfirst "Same as (next (first coll))." [coll] (next (first coll)))
(defn fnext "Same as (first (next coll))." [coll] (first (next coll)))
(defn nnext "Same as (next (next coll))." [coll] (next (next coll)))

;; --- Map entry accessors ---

(defn key "Returns the key of a map entry." [entry] (first entry))
(defn val "Returns the value of a map entry." [entry] (second entry))

(defn counted? "Returns true if (count x) is a constant-time operation." [x]
  (let [t (type x)]
    (or (= t :vector) (= t :map) (= t :set) (= t :string)
        (= t :sorted-map) (= t :sorted-set))))

(defn bounded-count "Returns the count of coll, but stops counting at n." [n coll]
  (if (counted? coll)
    (count coll)
    (loop [i 0 s (seq coll)]
      (if (and s (< i n))
        (recur (inc i) (next s))
        i))))

;; --- Collection conversions ---

(defn remove "Returns a lazy sequence of items in coll for which pred returns falsy. When called with no collection, returns a transducer."
  ([pred] (filter (complement pred)))
  ([pred coll] (filter (complement pred) coll)))
(defn vec "Converts coll into a vector." [coll] (into [] coll))

;; --- Random utilities ---

(defn rand-int "Returns a random integer between 0 (inclusive) and n (exclusive)." [n] (int (* (rand) n)))
(defn rand-nth "Returns a random element from coll." [coll] (nth coll (rand-int (count coll))))
(defn random-sample "Returns items from coll with probability prob. When called with no collection, returns a transducer."
  ([prob]
   (filter (fn [_] (< (rand) prob))))
  ([prob coll]
   (filter (fn [_] (< (rand) prob)) coll)))

;; --- Side-effecting traversal ---

(defn run! "Applies f to each item in coll for side effects. Returns nil." [f coll]
  (let [go (fn [s] (when (seq s) (f (first s)) (go (rest s))))]
    (go coll)
    nil))

;; --- Control flow macros ---

(defmacro if-not "Evaluates then when test is falsy, else otherwise." [test then & else]
  (if (seq else)
    `(if (not ~test) ~then ~(first else))
    `(if (not ~test) ~then)))

(defmacro when-not "Evaluates body when test is falsy." [test & body]
  `(when (not ~test) ~@body))

(defmacro if-let "Binds the result of expr, evaluates then if truthy, else otherwise." [bindings then & else]
  (let [sym  (first bindings)
        expr (first (rest bindings))
        g    (gensym)]
    (if (seq else)
      `(let [~g ~expr]
         (if ~g (let [~sym ~g] ~then) ~(first else)))
      `(let [~g ~expr]
         (if ~g (let [~sym ~g] ~then))))))

(defmacro when-let "Binds the result of expr, evaluates body if truthy." [bindings & body]
  (let [sym  (first bindings)
        expr (first (rest bindings))
        g    (gensym)]
    `(let [~g ~expr]
       (when ~g (let [~sym ~g] ~@body)))))

(defmacro when-first "Binds the first element of a collection, evaluates body if the collection is non-empty." [[x coll] & body]
  `(when-let [s# (seq ~coll)]
     (let [~x (first s#)] ~@body)))

(defmacro letfn "Binds local functions. Each binding is (name [params] body...)."
  [bindings & body]
  (let [pairs (vec (mapcat (fn [b] [(first b) (apply list 'fn (first b) (rest b))]) bindings))]
    `(let [~@pairs] ~@body)))

(defmacro set! "No-op. JVM compiler directive, not applicable to mino." [& _] nil)

(defmacro comment "Ignores body, returns nil." [& body] nil)

(defmacro if-some "Binds the result of expr, evaluates then if non-nil, else otherwise." [bindings then & else]
  (let [sym  (first bindings)
        expr (first (rest bindings))
        g    (gensym)]
    (if (seq else)
      `(let [~g ~expr]
         (if (not (nil? ~g)) (let [~sym ~g] ~then) ~(first else)))
      `(let [~g ~expr]
         (if (not (nil? ~g)) (let [~sym ~g] ~then))))))

(defmacro when-some "Binds the result of expr, evaluates body if non-nil." [bindings & body]
  (let [sym  (first bindings)
        expr (first (rest bindings))
        g    (gensym)]
    `(let [~g ~expr]
       (when (not (nil? ~g)) (let [~sym ~g] ~@body)))))

;; --- Sequence functions ---

(defn last "Returns the last item in coll." [coll]
  (let [s (seq coll)]
    (if (next s)
      (last (next s))
      (first s))))

(defn butlast "Returns a seq of all but the last item in coll." [coll]
  (let [s (seq coll)]
    (when (next s)
      (cons (first s) (butlast (next s))))))

(defn nthrest "Returns the result of calling rest n times on coll." [coll n]
  (if (<= n 0) coll (nthrest (rest coll) (- n 1))))

(defn nthnext "Returns the result of calling next n times on coll." [coll n]
  (if (<= n 0) (seq coll) (nthnext (next coll) (- n 1))))

(defn take-last "Returns a seq of the last n items in coll." [n coll]
  (let [lead (drop n coll)
        step (fn [s lead]
               (if (seq lead)
                 (step (next s) (next lead))
                 s))]
    (step (seq coll) (seq lead))))

(defn drop-last "Returns a lazy sequence of all but the last n items in coll."
  ([coll]   (drop-last 1 coll))
  ([n coll] (map (fn [x _] x) coll (drop n coll))))

(defn split-at "Returns a vector of [(take n coll) (drop n coll)]." [n coll]
  (vector (take n coll) (drop n coll)))

(defn split-with "Returns a vector of [(take-while pred coll) (drop-while pred coll)]." [pred coll]
  (vector (take-while pred coll) (drop-while pred coll)))

;; mapv is defined as a C primitive; no mino-level fallback needed.
;; filterv is defined as a C primitive; no mino-level fallback needed.

(defn sort-by "Returns a sorted sequence of the items in coll, ordered by (keyfn item)." [keyfn & args]
  (let [cmp  (if (= (count args) 2) (first args) compare)
        coll (last args)]
    (sort (fn [a b] (cmp (keyfn a) (keyfn b))) coll)))

;; --- Collection utilities ---

(def get-in "Returns the value in a nested associative structure at the given key path."
  (let [step (fn step [m ks nf sentinel]
               (if ks
                 (let [v (get m (first ks) sentinel)]
                   (if (= v sentinel)
                     nf
                     (step v (next ks) nf sentinel)))
                 m))]
    (fn
      ([m ks]     (reduce get m ks))
      ([m ks nf]  (step m (seq ks) nf (gensym))))))

(defn assoc-in "Associates a value in a nested associative structure at the given key path." [m ks v]
  (let [k (first ks)]
    (if (next ks)
      (assoc m k (assoc-in (get m k) (rest ks) v))
      (assoc m k v))))

(defn update-in "Updates a value in a nested associative structure by applying f at the given key path." [m ks f & args]
  (let [k (first ks)]
    (if (next ks)
      (assoc m k (apply update-in (get m k) (rest ks) f args))
      (assoc m k (apply f (get m k) args)))))

(defn merge-with "Returns a map that is the merge of the given maps, using f to combine values at shared keys." [f & maps]
  (when (some identity maps)
    (reduce (fn [acc m]
      (if m
        (reduce (fn [a kv]
          (let [k (first kv)
                v (second kv)]
            (if (contains? a k)
              (assoc a k (f (get a k) v))
              (assoc a k v))))
          (if acc acc (with-meta {} (meta m))) (seq m))
        acc))
      nil maps)))

(defn reduce-kv "Reduces a map with f taking accumulator, key, and value." [f init m]
  (reduce (fn [acc kv] (f acc (first kv) (second kv)))
          init (seq m)))

(defn update-vals "Returns a map with f applied to each value." [m f]
  (reduce-kv (fn [acc k v] (assoc acc k (f v))) {} m))

(defn update-keys "Returns a map with f applied to each key." [m f]
  (reduce-kv (fn [acc k v] (assoc acc (f k) v)) {} m))

(defn replace "Returns a collection with items in coll replaced by entries in smap." [smap coll]
  (let [f (fn [x] (if-let [e (find smap x)] (val e) x))]
    (if (vector? coll)
      (with-meta (mapv f coll) (meta coll))
      (map f coll))))

;; str-replace is now a C primitive in prim/string.c

;; --- Bitwise compositions ---

(defn bit-and-not "Returns the bitwise AND of x and the complement of y." [x y] (bit-and x (bit-not y)))
(defn bit-test "Returns true if bit n of x is set." [x n] (not (= 0 (bit-and x (bit-shift-left 1 n)))))
(defn bit-set "Returns x with bit n set." [x n] (bit-or x (bit-shift-left 1 n)))
(defn bit-clear "Returns x with bit n cleared." [x n] (bit-and x (bit-not (bit-shift-left 1 n))))
(defn bit-flip "Returns x with bit n flipped." [x n] (bit-xor x (bit-shift-left 1 n)))

;; --- Misc utilities ---

(defn comparator "Returns a comparator function from a two-arg predicate." [pred]
  (fn [a b] (cond (pred a b) -1 (pred b a) 1 :else 0)))

;; --- Lazy combinators ---

(defn keep "Returns a lazy sequence of non-nil results of (f item). When called with no collection, returns a transducer."
  ([f]
   (fn [rf]
     (fn ([] (rf))
         ([result] (rf result))
         ([result input]
          (let [v (f input)]
            (if (nil? v)
              result
              (rf result v)))))))
  ([f coll]
   (lazy-seq
     (let [s (seq coll)]
       (when s
         (let [v (f (first s))]
           (if (nil? v)
             (keep f (rest s))
             (cons v (keep f (rest s))))))))))

(defn keep-indexed "Returns a lazy sequence of non-nil results of (f index item)." [f coll]
  (let [keepi (fn keepi [i coll]
                (lazy-seq
                  ((fn [i coll]
                     (when-let [s (seq coll)]
                       (let [v (f i (first s))]
                         (if (nil? v)
                           (recur (inc i) (rest s))
                           (cons v (keepi (inc i) (rest s)))))))
                   i coll)))]
    (keepi 0 coll)))

(defn map-indexed "Returns a lazy sequence of (f index item) for each item in coll. When called with no collection, returns a transducer."
  ([f]
   (fn [rf]
     (let [i (volatile! -1)]
       (fn ([] (rf))
           ([result] (rf result))
           ([result input]
            (rf result (f (vswap! i inc) input)))))))
  ([f coll]
   (let [step (fn step [i s]
                (lazy-seq
                  (when (seq s)
                    (cons (f i (first s))
                          (step (inc i) (rest s))))))]
     (step 0 coll))))

(defn partition-all "Like partition, but includes a final partial group if items remain."
  ([n]
   (fn [rf]
     (let [buf (volatile! [])]
       (fn ([] (rf))
           ([result]
            (let [b @buf]
              (if (empty? b)
                (rf result)
                (let [r (rf result (seq b))]
                  (rf (unreduced r))))))
           ([result input]
            (let [b (vswap! buf conj input)]
              (if (= (count b) n)
                (do (vreset! buf [])
                    (rf result (seq b)))
                result)))))))
  ([n coll] (partition-all n n coll))
  ([n step coll]
   (letfn [(pa-impl [n step coll]
             (lazy-seq
               (let [s (seq coll)]
                 (when s
                   (let [p (doall (take n s))]
                     (cons p (pa-impl n step (drop step s))))))))]
     (pa-impl n step coll))))

(defn reductions "Returns a lazy sequence of the intermediate values of a reduction." [f & args]
  (let [init (if (= (count args) 2) (first args) (first (first args)))
        coll (if (= (count args) 2) (second args) (rest (first args)))
        step (fn [acc s]
               (lazy-seq
                 (when (seq s)
                   (let [v (f acc (first s))]
                     (cons v (step v (rest s)))))))]
    (cons init (step init coll))))

(defn dedupe "Returns a lazy sequence removing consecutive duplicates. When called with no collection, returns a transducer."
  ([]
   (fn [rf]
     (let [prev (volatile! ::none)]
       (fn ([] (rf))
           ([result] (rf result))
           ([result input]
            (if (= @prev input)
              result
              (do (vreset! prev input)
                  (rf result input))))))))
  ([coll] (sequence (dedupe) coll)))

;; --- Higher-order combinators ---

(defn every-pred "Returns a function that returns true when all preds are satisfied by all its arguments."
  ([p]
   (fn ep1
     ([] true)
     ([x] (boolean (p x)))
     ([x y] (boolean (and (p x) (p y))))
     ([x y z] (boolean (and (p x) (p y) (p z))))
     ([x y z & args] (boolean (and (ep1 x y z) (every? p args))))))
  ([p1 p2]
   (fn ep2
     ([] true)
     ([x] (boolean (and (p1 x) (p2 x))))
     ([x y] (boolean (and (p1 x) (p1 y) (p2 x) (p2 y))))
     ([x y z] (boolean (and (p1 x) (p1 y) (p1 z) (p2 x) (p2 y) (p2 z))))
     ([x y z & args] (boolean (and (ep2 x y z)
                                   (every? (fn [a] (and (p1 a) (p2 a))) args))))))
  ([p1 p2 p3]
   (fn ep3
     ([] true)
     ([x] (boolean (and (p1 x) (p2 x) (p3 x))))
     ([x y] (boolean (and (p1 x) (p1 y) (p2 x) (p2 y) (p3 x) (p3 y))))
     ([x y z] (boolean (and (p1 x) (p1 y) (p1 z) (p2 x) (p2 y) (p2 z) (p3 x) (p3 y) (p3 z))))
     ([x y z & args] (boolean (and (ep3 x y z)
                                   (every? (fn [a] (and (p1 a) (p2 a) (p3 a))) args))))))
  ([p1 p2 p3 & ps]
   (let [ps (cons p1 (cons p2 (cons p3 ps)))]
     (fn epn
       ([] true)
       ([x] (every? (fn [p] (p x)) ps))
       ([x y] (every? (fn [p] (and (p x) (p y))) ps))
       ([x y z] (every? (fn [p] (and (p x) (p y) (p z))) ps))
       ([x y z & args] (boolean (and (epn x y z)
                                     (every? (fn [p] (every? p args)) ps))))))))

(defn some-fn "Returns a function that returns the first truthy value from any pred applied to any argument."
  ([p]
   (fn sp1
     ([] nil)
     ([x] (p x))
     ([x y] (or (p x) (p y)))
     ([x y z] (or (p x) (p y) (p z)))
     ([x y z & args] (or (sp1 x y z) (some p args)))))
  ([p1 p2]
   (fn sp2
     ([] nil)
     ([x] (or (p1 x) (p2 x)))
     ([x y] (or (p1 x) (p1 y) (p2 x) (p2 y)))
     ([x y z] (or (p1 x) (p1 y) (p1 z) (p2 x) (p2 y) (p2 z)))
     ([x y z & args] (or (sp2 x y z)
                         (some (fn [a] (or (p1 a) (p2 a))) args)))))
  ([p1 p2 p3]
   (fn sp3
     ([] nil)
     ([x] (or (p1 x) (p2 x) (p3 x)))
     ([x y] (or (p1 x) (p1 y) (p2 x) (p2 y) (p3 x) (p3 y)))
     ([x y z] (or (p1 x) (p1 y) (p1 z) (p2 x) (p2 y) (p2 z) (p3 x) (p3 y) (p3 z)))
     ([x y z & args] (or (sp3 x y z)
                         (some (fn [a] (or (p1 a) (p2 a) (p3 a))) args)))))
  ([p1 p2 p3 & ps]
   (let [ps (cons p1 (cons p2 (cons p3 ps)))]
     (fn spn
       ([] nil)
       ([x] (some (fn [p] (p x)) ps))
       ([x y] (some (fn [p] (or (p x) (p y))) ps))
       ([x y z] (some (fn [p] (or (p x) (p y) (p z))) ps))
       ([x y z & args] (or (spn x y z)
                           (some (fn [p] (some p args)) ps)))))))

(defn fnil "Returns a function like f, but replaces nil arguments with the given defaults."
  ([f d1]
   (fn [x & args]
     (apply f (if (nil? x) d1 x) args)))
  ([f d1 d2]
   (fn [x y & args]
     (apply f (if (nil? x) d1 x) (if (nil? y) d2 y) args)))
  ([f d1 d2 d3]
   (fn [x y z & args]
     (apply f (if (nil? x) d1 x) (if (nil? y) d2 y) (if (nil? z) d3 z) args))))

(defn memoize "Returns a memoized version of f that caches return values by arguments." [f]
  (let [cache (atom {})]
    (fn [& args]
      (let [e (find (deref cache) args)]
        (if e
          (val e)
          (let [v (apply f args)]
            (swap! cache assoc args v)
            v))))))

(defn trampoline "Calls f with args, then repeatedly calls the result if it is a function." [f & args]
  (let [result (apply f args)
        bounce (fn [r] (if (fn? r) (bounce (r)) r))]
    (bounce result)))

;; --- Threading macros ---

(defmacro as-> "Binds expr to sym, then threads it through each form where sym can appear anywhere." [expr sym & forms]
  (if (= 0 (count forms))
    expr
    `(let [~sym ~expr]
       (as-> ~(first forms) ~sym ~@(rest forms)))))

(defmacro cond-> "Thread-first through forms whose tests are truthy." [expr & clauses]
  (if (< (count clauses) 2)
    expr
    (let [g    (gensym)
          test (first clauses)
          step (first (rest clauses))]
      `(let [~g ~expr]
         (cond-> (if ~test
                   ~(if (cons? step)
                      `(~(first step) ~g ~@(rest step))
                      `(~step ~g))
                   ~g)
                 ~@(rest (rest clauses)))))))

(defmacro cond->> "Thread-last through forms whose tests are truthy." [expr & clauses]
  (if (< (count clauses) 2)
    expr
    (let [g    (gensym)
          test (first clauses)
          step (first (rest clauses))]
      `(let [~g ~expr]
         (cond->> (if ~test
                    ~(if (cons? step)
                       `(~(first step) ~@(rest step) ~g)
                       `(~step ~g))
                    ~g)
                  ~@(rest (rest clauses)))))))

(defmacro some-> "Thread-first through forms, short-circuiting on nil." [expr & forms]
  (if (= 0 (count forms))
    expr
    (let [g (gensym)]
      `(let [~g ~expr]
         (if (nil? ~g)
           nil
           (some-> (-> ~g ~(first forms)) ~@(rest forms)))))))

(defmacro some->> "Thread-last through forms, short-circuiting on nil." [expr & forms]
  (if (= 0 (count forms))
    expr
    (let [g (gensym)]
      `(let [~g ~expr]
         (if (nil? ~g)
           nil
           (some->> (->> ~g ~(first forms)) ~@(rest forms)))))))

;; --- Iteration macros ---

(defmacro doto "Evaluates x, then calls each form with x as the first argument. Returns x." [x & forms]
  (let [g       (gensym)
        stmts   (apply list (map (fn [f] (if (cons? f)
                                            `(~(first f) ~g ~@(rest f))
                                            `(~f ~g)))
                                  forms))]
    `(let [~g ~x]
       ~@stmts
       ~g)))

(defmacro dotimes "Evaluates body n times with sym bound to 0 through n-1." [bindings & body]
  (let [sym (first bindings)
        n   (first (rest bindings))
        gn  (gensym)
        go  (gensym)]
    `(let [~gn ~n
           ~go (fn [~sym]
                 (when (< ~sym ~gn)
                   ~@body
                   (~go (inc ~sym))))]
       (~go 0))))

(defmacro while "Repeatedly evaluates body while test is truthy." [test & body]
  (let [go (gensym)]
    `(let [~go (fn [] (when ~test ~@body (~go)))]
       (~go))))

(defmacro doseq "Iterates over collections for side effects. Supports nested bindings." [bindings & body]
  (if (<= (count bindings) 2)
    ;; Single binding: (doseq [x coll] body...)
    (let [sym  (first bindings)
          coll (first (rest bindings))
          gs   (gensym)
          go   (gensym)]
      `(let [~go (fn [~gs]
                   (when ~gs
                     (let [~sym (first ~gs)]
                       ~@body
                       (~go (next ~gs)))))]
         (~go (seq ~coll))))
    ;; Multiple bindings: nest inner doseq
    (let [sym  (first bindings)
          coll (first (rest bindings))
          rest-bindings (into [] (drop 2 bindings))
          gs   (gensym)
          go   (gensym)]
      `(let [~go (fn [~gs]
                   (when ~gs
                     (let [~sym (first ~gs)]
                       (doseq ~rest-bindings ~@body)
                       (~go (next ~gs)))))]
         (~go (seq ~coll))))))

;; --- Shuffle (Fisher-Yates) ---

(defn shuffle "Returns a randomly shuffled vector of the items in coll." [coll]
  (when (not (coll? coll)) (throw "shuffle requires a collection"))
  (let [v   (vec coll)
        n   (count v)
        step (fn [v i]
               (if (= i 0)
                 v
                 (let [j (rand-int (inc i))]
                   (step (assoc (assoc v i (nth v j)) j (nth v i))
                         (dec i)))))]
    (into [] (step v (dec n)))))

;; --- Timing ---

(defmacro time "Evaluates body, prints elapsed time, and returns the result." [& body]
  (let [start  (gensym)
        result (gensym)]
    `(let [~start  (time-ms)
           ~result (do ~@body)]
       (println (str "Elapsed time: " (- (time-ms) ~start) " ms"))
       ~result)))

;; --- Tree walking ---

(defn sequential? "Returns true if x is a sequential collection (list, vector, or lazy-seq)." [x]
  (or (cons? x) (vector? x) (seq? x)))

(defn flatten "Returns a lazy sequence of the non-sequential items from a nested structure." [x]
  (filter (complement sequential?)
          (rest (tree-seq sequential? seq x))))

(defn tree-seq "Returns a lazy depth-first sequence of nodes in a tree." [branch? children root]
  (let [walk (fn [node]
               (lazy-seq
                 (cons node
                   (when (branch? node)
                     (mapcat walk (children node))))))]
    (walk root)))

(defn walk "Traverses form, applying inner to each element and outer to the result." [inner outer form]
  (cond
    (cons?   form) (outer (apply list (map inner form)))
    (vector? form) (outer (mapv inner form))
    (map?    form) (outer (into (empty form) (map inner (seq form))))
    (set?    form) (outer (into (empty form) (map inner form)))
    true           (outer form)))

(defn postwalk "Walks form depth-first, applying f to each sub-form after its children." [f form] (walk (fn [x] (postwalk f x)) f form))
(defn prewalk "Walks form depth-first, applying f to each sub-form before its children." [f form] (walk (fn [x] (prewalk f x)) identity (f form)))

(defn postwalk-replace "Replaces items in form that appear as keys in smap, walking bottom-up." [smap form]
  (postwalk (fn [x] (if (contains? smap x) (get smap x) x)) form))

(defn prewalk-replace "Replaces items in form that appear as keys in smap, walking top-down." [smap form]
  (prewalk (fn [x] (if (contains? smap x) (get smap x) x)) form))

;; --- Regex ---

(def re-pattern "Returns pattern unchanged. Provided for compatibility." identity)

;; Capture the C primitives before the matcher-aware wrappers below
;; shadow them.
(def ^:private prim-re-find    re-find)
(def ^:private prim-re-matches re-matches)

(defn- match-whole [m]
  ;; The C primitives return either a string (no groups) or a vector
  ;; [whole g1 g2 ...] (groups present). Normalise to the whole match.
  (if (vector? m) (first m) m))

(defn re-seq
  "Returns a lazy sequence of all matches of pattern in string s. Each
   match is a string when the pattern has no groups, or a vector
   [whole g1 g2 ...] when it does."
  [pattern s]
  (letfn [(find-index [s sub i]
            (if (> (+ i (count sub)) (count s))
              nil
              (if (= (subs s i (+ i (count sub))) sub)
                i
                (recur s sub (inc i)))))]
    (lazy-seq
      (when-let [m (prim-re-find pattern s)]
        (let [whole (match-whole m)
              idx   (find-index s whole 0)]
          (when (not (nil? idx))
            (let [rest-s (subs s (+ idx (max (count whole) 1)))]
              (cons m (re-seq pattern rest-s)))))))))

;; re-matcher: a stateful matcher value backed by an atom holding
;; {:pattern :text :pos :last}. Each (re-find m) advances :pos past the
;; previous match's end and stores the latest result under :last.
(defn re-matcher
  "Returns a matcher value for repeated find/match operations on text
   using pattern. The resulting value is consumed by re-find, re-groups
   and so on."
  [pattern text]
  (atom {::matcher? true :pattern pattern :text text :pos 0 :last nil}))

(defn ^:private matcher? [m]
  (and (atom? m)
       (let [v (deref m)]
         (and (map? v) (::matcher? v)))))

(defn- substring-index [s sub]
  ;; Brute-force scan for the first index of sub in s. Used by
  ;; re-find-on-matcher to advance the matcher's :pos. Lives here
  ;; (rather than calling clojure.string/index-of) because core.clj
  ;; loads before clojure.string.
  (let [slen (count s)
        nlen (count sub)]
    (if (zero? nlen)
      0
      (loop [i 0]
        (cond
          (> (+ i nlen) slen) nil
          (= (subs s i (+ i nlen)) sub) i
          :else (recur (+ i 1)))))))

(defn- re-find-on-matcher [m]
  (let [state   @m
        pattern (:pattern state)
        text    (:text state)
        pos     (:pos state)
        rem     (subs text pos)
        result  (prim-re-find pattern rem)]
    (when (some? result)
      (let [whole  (match-whole result)
            offset (substring-index rem whole)]
        (when (some? offset)
          (let [end (+ pos offset (max (count whole) 1))]
            (swap! m assoc :pos end :last result)
            result))))))

(defn re-find
  "Find the first match. (re-find pattern text) returns a string (no
   groups) or [whole g1 g2 ...] (groups). (re-find m) advances a matcher."
  ([m]            (re-find-on-matcher m))
  ([pattern text] (prim-re-find pattern text)))

(defn re-matches
  "Like re-find but anchored to the whole string. Returns a string
   (no groups) or [whole g1 g2 ...] (groups), or nil."
  [pattern text]
  (prim-re-matches pattern text))

(defn re-groups
  "Returns the most recent match groups for matcher m: a vector
   [whole g1 g2 ...] when the pattern has groups, the whole-match string
   otherwise. Throws when the matcher has no recorded match yet."
  [m]
  (let [last-match (:last @m)]
    (when (nil? last-match)
      (throw (ex-info "No match has been performed by the matcher"
                      {:matcher m})))
    last-match))

;; --- Complex macros ---

(defmacro condp "Takes a binary predicate, an expression, and clauses. Returns the first clause value where (pred test-val expr) is truthy." [pred expr & clauses]
  (let [gexpr (gensym)]
    `(let [~gexpr ~expr]
       ~(let [build (fn [cls]
                      (if (< (count cls) 2)
                        (if (= (count cls) 1)
                          (first cls)
                          nil)
                        (let [test (first cls)
                              then (first (rest cls))
                              more (rest (rest cls))]
                          `(if (~pred ~test ~gexpr)
                             ~then
                             ~(build more)))))]
          (build clauses)))))

(defmacro case "Dispatches on the value of expr. Matches constants in pairs, with an optional default." [expr & clauses]
  (let [gexpr   (gensym)
        quote-c (fn [c]
                  (cond
                    (nil? c)     nil
                    (keyword? c) c
                    (number? c)  c
                    (string? c)  c
                    (= c true)  true
                    (= c false) false
                    :else        (list 'quote c)))
        match1  (fn [g c]
                  (if (cons? c)
                    ;; Multi-match list: (a b c) => (or (= g 'a) (= g 'b) ...)
                    (apply list 'or (map (fn [v] (list '= g (quote-c v))) c))
                    (list '= g (quote-c c))))
        build   (fn [cls]
                  (if (< (count cls) 2)
                    (if (= (count cls) 1)
                      (first cls)
                      (list 'throw (list 'ex-info "no matching case" {})))
                    (list 'if (match1 gexpr (first cls))
                          (first (rest cls))
                          (build (rest (rest cls))))))]
    `(let [~gexpr ~expr]
       ~(build clauses))))

(defmacro for "List comprehension. Takes binding vectors and body, returns a lazy sequence." [bindings & body]
  (let [sym  (first bindings)
        coll (first (rest bindings))
        rest-bindings (drop 2 bindings)]
    (if (empty? rest-bindings)
      ;; Last binding pair: produce elements
      `(map (fn [~sym] ~@body) ~coll)
      (let [modifier (first rest-bindings)]
        (cond
          ;; :when filter
          (= modifier :when)
          (let [pred (first (rest rest-bindings))
                remaining (into [] (drop 2 rest-bindings))]
            (if (empty? remaining)
              `(map (fn [~sym] ~@body)
                    (filter (fn [~sym] ~pred) ~coll))
              `(for ~(into [sym (list 'filter (list 'fn [sym] pred) coll)]
                           remaining)
                 ~@body)))
          ;; :while stop iteration
          (= modifier :while)
          (let [pred (first (rest rest-bindings))
                remaining (into [] (drop 2 rest-bindings))]
            (if (empty? remaining)
              `(map (fn [~sym] ~@body)
                    (take-while (fn [~sym] ~pred) ~coll))
              `(for ~(into [sym (list 'take-while (list 'fn [sym] pred) coll)]
                           remaining)
                 ~@body)))
          ;; :let local bindings
          (= modifier :let)
          (let [let-bindings (first (rest rest-bindings))
                after-let (into [] (drop 2 rest-bindings))
                ;; Collect :when/:while modifiers that follow the :let
                next-mod (first after-let)]
            (cond
              (empty? after-let)
              `(map (fn [~sym]
                      (let ~let-bindings ~@body))
                    ~coll)
              ;; :when after :let on same binding
              (= next-mod :when)
              (let [pred (first (rest after-let))
                    remaining (into [] (drop 2 after-let))]
                (if (empty? remaining)
                  `(map (fn [~sym]
                          (let ~let-bindings ~@body))
                        (filter (fn [~sym]
                                  (let ~let-bindings ~pred))
                                ~coll))
                  `(mapcat (fn [~sym]
                             (let ~let-bindings
                               (when ~pred
                                 (for ~remaining ~@body))))
                           ~coll)))
              ;; :while after :let on same binding
              (= next-mod :while)
              (let [pred (first (rest after-let))
                    remaining (into [] (drop 2 after-let))]
                (if (empty? remaining)
                  `(map (fn [~sym]
                          (let ~let-bindings ~@body))
                        (take-while (fn [~sym]
                                      (let ~let-bindings ~pred))
                                    ~coll))
                  `(mapcat (fn [~sym]
                             (let ~let-bindings
                               (for ~remaining ~@body)))
                           (take-while (fn [~sym]
                                         (let ~let-bindings ~pred))
                                       ~coll))))
              ;; Other bindings after :let
              :else
              `(mapcat (fn [~sym]
                         (let ~let-bindings
                           (for ~after-let ~@body)))
                       ~coll)))
          ;; Another binding pair: nested iteration
          :else
          `(mapcat (fn [~sym]
                     (for ~(into [] rest-bindings) ~@body))
                   ~coll))))))

;; ---------------------------------------------------------------------------
;; Agents: not supported on mino.
;;
;; Real Clojure agents have asynchronous send semantics, an error-mode
;; lifecycle, and a thread-pool dispatcher. Mino has none of those, so
;; rather than aliasing agent to atom and pretending the API works, the
;; functions throw with a clear message pointing at atoms or
;; core.async as the supported alternatives.
;; ---------------------------------------------------------------------------

;; ---------------------------------------------------------------------------
;; Exception info
;; ---------------------------------------------------------------------------

(defn ex-info
  "Create an exception map with a message and data map."
  [msg data]
  {:message msg :data data})

(defn ex-data
  "Extract the data map from an exception. Handles diagnostic maps
  (from catch), ex-info maps, and plain thrown values."
  [ex]
  (when (map? ex)
    (if (:mino/kind ex)
      ;; Diagnostic map: unwrap to the original thrown value.
      ;; If that value is an ex-info map, extract its :data.
      (let [orig (:mino/data ex)]
        (if (and (map? orig) (contains? orig :data))
          (:data orig)
          orig))
      (:data ex))))

(defn ex-message
  "Extract the message from an exception. Handles both diagnostic maps
  and ex-info maps."
  [ex]
  (when (map? ex)
    (if (:mino/kind ex)
      (:mino/message ex)
      (:message ex))))

(defn agent [& _]
  (throw (ex-info "agent is not supported on mino — use atoms for synchronous mutable state or core.async for async dispatch"
                  {:mino/unsupported :agent})))

(defn send-to [& _]
  (throw (ex-info "send-to is not supported on mino — see (atom) and core.async"
                  {:mino/unsupported :send-to})))

(defn agent-error [& _]
  (throw (ex-info "agent-error is not supported on mino"
                  {:mino/unsupported :agent-error})))

;; ---------------------------------------------------------------------------
;; Host threads.
;;
;; Real OS-thread futures and promises. Embedded mode starts at
;; thread_limit = 1 (single-threaded) and embedders raise it via
;; mino_set_thread_limit. Standalone `./mino` grants cpu_count
;; automatically so the REPL surface matches Clojure canon.
;;
;; When the host has not granted threads (limit <= 1), spawn entry
;; points throw :mino/unsupported with a message naming the grant
;; API. When the host has granted, the C primitives behind these
;; functions create real pthread/CreateThread workers.
;; ---------------------------------------------------------------------------

(defn ^:private mino-no-grant-msg
  "Build the :mino/unsupported message used when host threads are
  not granted in the current state."
  [name]
  (str name ": host threads are not granted in this state. "
       "Embedders call mino_set_thread_limit(S, n) with n > 1; the "
       "standalone `./mino` binary grants cpu_count automatically."))

(defmacro future
  "Takes a body of expressions and yields a future object that will
  evaluate the body in another thread, blocking on deref until the
  value is available. Throws :mino/unsupported when host threads are
  not granted; see mino-thread-limit."
  [& body]
  `(if (<= (mino-thread-limit) 1)
     (throw (ex-info (mino-no-grant-msg "future")
                     {:mino/unsupported :future
                      :mino/thread-limit (mino-thread-limit)}))
     (future-call (fn [] ~@body))))

(defmacro thread
  "Executes the body in another thread, returning a future-like value
  that can be deref'd. Shares the same worker pool as future. Throws
  :mino/unsupported when host threads are not granted."
  [& body]
  `(if (<= (mino-thread-limit) 1)
     (throw (ex-info (mino-no-grant-msg "thread")
                     {:mino/unsupported :thread
                      :mino/thread-limit (mino-thread-limit)}))
     (future-call (fn [] ~@body))))

;; ---------------------------------------------------------------------------
;; Protocols: polymorphic dispatch on the type of the first argument.
;;
;; (defprotocol Name
;;   (method1 [this])
;;   (method2 [this x]))
;;
;; (extend-type :string
;;   Name
;;   (method1 [this] ...)
;;   (method2 [this x] ...))
;;
;; (extend-protocol Name
;;   :string
;;   (method1 [this] ...)
;;   :vector
;;   (method1 [this] ...))
;;
;; (satisfies? Name x) returns true if x's type has implementations for Name.
;; ---------------------------------------------------------------------------

(defn protocol-dispatch [dispatch-atom mname & args]
  (let [target (first args)
        t (type target)
        dispatch-map @dispatch-atom
        impl (get dispatch-map t)]
    (if impl
      (apply impl args)
      (let [default (get dispatch-map :default)]
        (if default
          (apply default args)
          (throw (ex-info (str "No implementation of " mname " for type " (name t))
                          {:method mname :type t})))))))

(defmacro defprotocol "Defines a protocol with the given method signatures." [proto-name & methods]
  (let [methods (remove string? methods)
        methods (loop [ms methods result []]
                  (if (or (nil? ms) (empty? ms)) result
                    (if (keyword? (first ms))
                      (recur (drop 2 ms) result)
                      (recur (rest ms) (conj result (first ms))))))
        pname (name proto-name)
        method-info (into [] (map (fn [m]
                           (let [mname (first m)
                                 params (second m)]
                             {:mname mname
                              :params params
                              :dsym (symbol (str pname "--" (name mname)))}))
                         methods))
        atom-defs (into [] (map (fn [mi] (list 'def (:dsym mi) '(atom {})))
                       method-info))
        fn-defs (into [] (map (fn [mi]
                       (let [dispatch-call (apply list
                                             'protocol-dispatch
                                             (:dsym mi)
                                             (str (:mname mi))
                                             (:params mi))]
                         (list 'defn (:mname mi) (:params mi)
                               dispatch-call)))
                     method-info))
        proto-map (into {} (map (fn [mi]
                                  [(keyword (str (:mname mi))) (:dsym mi)])
                                method-info))
        proto-def (list 'def proto-name
                        {:name pname :methods proto-map})
        all-forms (concat atom-defs fn-defs (list proto-def))]
    (apply list 'do all-forms)))

(defmacro extend-type "Extends a protocol with method implementations for the given type." [type-kw & specs]
  (let [groups (loop [remaining specs result [] cur-proto nil cur-methods []]
                 (if (empty? remaining)
                   (if cur-proto (conj result [cur-proto cur-methods]) result)
                   (let [item (first remaining)]
                     (if (and (symbol? item) (not (list? item)))
                       (recur (rest remaining)
                              (if cur-proto (conj result [cur-proto cur-methods]) result)
                              item [])
                       (recur (rest remaining) result cur-proto
                              (conj cur-methods item))))))
        swaps (mapcat (fn [[proto methods]]
                (let [pname (name proto)
                      pns   (namespace proto)]
                  (map (fn [m]
                    (let [mname (first m)
                          params (second m)
                          body (drop 2 m)
                          dsym (symbol pns (str pname "--" (name mname)))]
                      (list 'swap! dsym 'assoc type-kw
                            (apply list 'fn params body))))
                   methods)))
              groups)]
    (apply list 'do (vec swaps))))

(defn- type-marker-key
  "Translates an extend-protocol type marker to the dispatch key
   extend-type understands. Keywords pass through (used for built-in
   types and ad-hoc tags via :type metadata). Symbols pass through
   too: at runtime they evaluate to a MINO_TYPE value so
   (extend-protocol P Point ...) places its impls under that type's
   pointer in the dispatch atom. Nil maps to :nil so a
   (extend-protocol P nil ...) clause registers against (type nil)."
  [marker]
  (cond
    (keyword? marker) marker
    (nil? marker)     :nil
    (symbol? marker)  marker
    :else (throw (ex-info (str "extend-protocol: unsupported type marker " (pr-str marker)
                               " — use a keyword, a record-type symbol, or nil")
                          {:marker marker
                           :mino/unsupported :extend-protocol-type-marker}))))

(defn- partition-protocol-specs [specs]
  (loop [remaining specs groups [] current nil]
    (if (empty? remaining)
      (if current (conj groups current) groups)
      (let [item (first remaining)]
        (if (or (cons? item) (list? item))
          (recur (rest remaining) groups (conj current item))
          (recur (rest remaining)
                 (if current (conj groups current) groups)
                 [(type-marker-key item)]))))))

(defmacro extend-protocol "Extends a protocol with implementations for multiple types." [proto & specs]
  (let [groups (partition-protocol-specs specs)
        forms (into [] (map (fn [group]
                (apply list 'extend-type (first group) proto (rest group)))
              groups))]
    (apply list 'do forms)))

(defn satisfies? "Returns true if x's type has implementations for all methods of proto." [proto x]
  (let [t (type x)
        methods (vals (:methods proto))]
    (every? (fn [dispatch-atom]
              (let [dm @dispatch-atom]
                (or (contains? dm t)
                    (contains? dm :default))))
            methods)))

;; ---------------------------------------------------------------------------
;; Core protocols: extension points wired into reduce / reduce-kv / datafy / nav.
;;
;; CollReduce  - per-type reduction strategy. The 3-arg form (coll-reduce
;;               coll f init) is the workhorse; user types extend this and
;;               reduce dispatches through it.
;; IKVReduce   - associative kv-reduce dispatch. Defaults walk seq pairs.
;; Datafiable  - per-type datafy hook; default returns the value unchanged.
;; Navigable   - per-coll/key/value nav hook; default returns v unchanged.
;;
;; Built-in collection types do not extend these; reduce / reduce-kv keep
;; the existing fast paths and consult the protocol only when a user has
;; registered a per-type override (or a :default catch-all).
;; ---------------------------------------------------------------------------

(def internal-reduce reduce)
(def internal-reduce-kv reduce-kv)

(defprotocol CollReduce
  "Protocol for collection types that can reduce themselves. User types
  extend coll-reduce to provide a custom traversal; mino's reduce
  consults the registry before falling back to its built-in seq-driven
  reduction."
  (coll-reduce [coll f init]))

(defprotocol IKVReduce
  "Protocol for associative collections that can kv-reduce directly,
  rather than walk a seq of [k v] pairs."
  (kv-reduce [coll f init]))

(defprotocol Datafiable
  "Protocol for things that can present a data view of themselves.
  The default impl is identity; user types may override."
  (datafy [o]))

(extend-type :default Datafiable (datafy [o] o))

(defprotocol Navigable
  "Protocol for navigating from a (k v) pair in coll into a related
  value. The default impl returns v unchanged."
  (nav [coll k v]))

(extend-type :default Navigable (nav [_coll _k v] v))

(defn reduce "Reduces coll using f. With 2 args, uses the first element as init.
  With 3 args, uses init explicitly. Consults CollReduce: a user
  type or :default override on coll-reduce takes precedence over the
  built-in seq-driven reduction."
    ([f coll]
     (let [s (seq coll)]
       (if (nil? s)
         (f)
         (let [init  (first s)
               rs    (rest s)
               table @CollReduce--coll-reduce
               impl  (or (get table (type rs))
                         (get table :default))]
           (if impl
             (impl rs f init)
             (internal-reduce f init rs))))))
    ([f init coll]
     (let [table @CollReduce--coll-reduce
           impl  (or (get table (type coll))
                     (get table :default))]
       (if impl
         (impl coll f init)
         (internal-reduce f init coll)))))

(defn reduce-kv "Reduces a map (or any associative source) with f taking accumulator,
  key, and value. Consults IKVReduce; falls back to walking the seq." [f init m]
    (let [table @IKVReduce--kv-reduce
          impl  (or (get table (type m))
                    (get table :default))]
      (if impl
        (impl m f init)
        (internal-reduce-kv f init m))))

;; ---------------------------------------------------------------------------
;; Hierarchies: immutable parent/child/ancestor/descendant relationships.
;;
;; A hierarchy is a map with :parents, :ancestors, and :descendants keys.
;; Values are maps from tag -> set of related tags.
;;
;; (make-hierarchy) returns an empty hierarchy.
;; (derive h child parent) adds a relationship (3-arg) or uses global (2-arg).
;; (underive h child parent) removes a relationship.
;; (parents h tag), (ancestors h tag), (descendants h tag) query relationships.
;; (isa? h child parent) checks if child derives from parent.
;; ---------------------------------------------------------------------------

(def ^:private global-hierarchy (atom {:parents {} :ancestors {} :descendants {}}))

;; Bumped on every mutation of global-hierarchy; multimethods compare it
;; against their cached version to invalidate stale dispatch entries when
;; derive / underive runs after methods have populated the cache.
(def ^:private hierarchy-version (atom 0))

(defn make-hierarchy "Returns an empty hierarchy." [] {:parents {} :ancestors {} :descendants {}})

(defn- tc-ancestors [parents-map node]
  (loop [frontier (get parents-map node #{})
         result #{}]
    (if (empty? frontier)
      result
      (let [p (first frontier)
            rst (disj frontier p)]
        (if (contains? result p)
          (recur rst result)
          (recur (into rst (get parents-map p #{}))
                 (conj result p)))))))

(defn- recompute-hierarchy [h]
  (let [pm (:parents h)
        all-nodes (into (set (keys pm))
                        (apply concat (vals pm)))
        ancestors-map (reduce (fn [m node]
                                (let [ancs (tc-ancestors pm node)]
                                  (if (empty? ancs) m (assoc m node ancs))))
                              {} all-nodes)
        descendants-map (reduce (fn [m node]
                                  (reduce (fn [m2 anc]
                                            (update m2 anc (fn [s] (conj (or s #{}) node))))
                                          m (get ancestors-map node #{})))
                                {} all-nodes)]
    (assoc h :ancestors ancestors-map :descendants descendants-map)))

(defn derive "Establishes a parent/child relationship between child and parent in a hierarchy."
  ([child parent]
   (swap! global-hierarchy derive child parent)
   (swap! hierarchy-version inc)
   nil)
  ([h child parent]
   (when (= child parent)
     (throw (ex-info "Cannot derive tag from itself" {:child child :parent parent})))
   (when (contains? (get (:ancestors h) parent #{}) child)
     (throw (ex-info "Cyclic derivation" {:child child :parent parent})))
   (let [new-parents (update (:parents h) child
                             (fn [s] (conj (or s #{}) parent)))]
     (recompute-hierarchy (assoc h :parents new-parents)))))

(defn- valid-hierarchy? [h]
  (and (map? h) (contains? h :parents) (contains? h :ancestors) (contains? h :descendants)))

(defn underive "Removes a parent/child relationship between child and parent."
  ([child parent]
   (swap! global-hierarchy underive child parent)
   (swap! hierarchy-version inc)
   nil)
  ([h child parent]
   (when-not (valid-hierarchy? h)
     (throw (ex-info "invalid hierarchy" {:h h})))
   (let [cur (get (:parents h) child #{})]
     (if (contains? cur parent)
       (let [new-set (disj cur parent)
             new-parents (if (empty? new-set)
                           (dissoc (:parents h) child)
                           (assoc (:parents h) child new-set))]
         (recompute-hierarchy (assoc h :parents new-parents)))
       h))))

(defn parents "Returns the immediate parents of tag in the hierarchy."
  ([tag] (parents @global-hierarchy tag))
  ([h tag]
   (let [p (get (:parents h) tag)]
     (if (and p (not (empty? p))) p nil))))

(defn ancestors "Returns all ancestors of tag in the hierarchy."
  ([tag] (ancestors @global-hierarchy tag))
  ([h tag]
   (let [a (get (:ancestors h) tag)]
     (if (and a (not (empty? a))) a nil))))

(defn descendants "Returns all descendants of tag in the hierarchy."
  ([tag] (descendants @global-hierarchy tag))
  ([h tag]
   (let [d (get (:descendants h) tag)]
     (if (and d (not (empty? d))) d nil))))

(defn isa? "Returns true if child is equal to or derives from parent."
  ([child parent] (isa? @global-hierarchy child parent))
  ([h child parent]
   (or (= child parent)
       (contains? (get (:ancestors h) child #{}) parent)
       (and (vector? child) (vector? parent)
            (= (count child) (count parent))
            (every? identity (map (fn [c p] (isa? h c p)) child parent))))))

;; ---------------------------------------------------------------------------
;; Multimethods: value-dispatched polymorphism.
;; ---------------------------------------------------------------------------

(defn- prefers? [x y prefer-table hierarchy]
  ;; True when x is preferred over y, considering transitive preferences
  ;; through hierarchy parents (matching Clojure's prefers? semantics).
  (or (contains? (get prefer-table x #{}) y)
      (boolean (some (fn [yp] (prefers? x yp prefer-table hierarchy))
                     (or (parents hierarchy y) #{})))
      (boolean (some (fn [xp] (prefers? xp y prefer-table hierarchy))
                     (or (parents hierarchy x) #{})))))

(defn- find-best-method [methods dval default-val prefer-table hierarchy]
  (let [exact (get methods dval)]
    (if exact
      exact
      (let [matches (reduce-kv
                      (fn [acc k v]
                        (if (and (not= k default-val) (isa? hierarchy dval k))
                          (conj acc [k v])
                          acc))
                      [] methods)]
        (cond
          (= 0 (count matches)) nil
          (= 1 (count matches)) (second (first matches))
          :else
          (let [preferred (filter
                            (fn [[k _]]
                              (every?
                                (fn [[k2 _]]
                                  (or (= k k2)
                                      (prefers? k k2 prefer-table hierarchy)))
                                matches))
                            matches)]
            (if (= 1 (count preferred))
              (second (first preferred))
              (throw (ex-info
                       (str "Multiple methods match dispatch value: " dval
                            ", none preferred")
                       {:dispatch-val dval
                        :matches (mapv first matches)})))))))))

(defn- create-multimethod [dispatch-fn default-val]
  (let [method-table   (atom {})
        prefer-table   (atom {})
        dispatch-cache (atom {})
        cache-version  (atom @hierarchy-version)
        mm (fn [& args]
             (let [hver @hierarchy-version]
               (when (not= hver @cache-version)
                 (reset! dispatch-cache {})
                 (reset! cache-version hver)))
             (let [dval    (apply dispatch-fn args)
                   cached  (get @dispatch-cache dval)]
               (if cached
                 (apply cached args)
                 (let [methods @method-table
                       impl    (or (get methods dval)
                                   (find-best-method methods dval default-val
                                                      @prefer-table
                                                      @global-hierarchy)
                                   (get methods default-val))]
                   (if impl
                     (do (swap! dispatch-cache assoc dval impl)
                         (apply impl args))
                     (throw (ex-info
                              (str "No method in multimethod for dispatch value: "
                                   (pr-str dval))
                              {:dispatch-val dval})))))))]
    (with-meta mm {:type           :multimethod
                   :method-table   method-table
                   :prefer-table   prefer-table
                   :dispatch-cache dispatch-cache
                   :cache-version  cache-version
                   :default        default-val})))

(defn- register-method [mm dispatch-val f]
  (swap! (:method-table (meta mm)) assoc dispatch-val f)
  (reset! (:dispatch-cache (meta mm)) {})
  mm)

(defmacro defmulti "Defines a multimethod with the given dispatch function." [mm-name & options]
  (let [options  (if (string? (first options)) (rest options) options)
        options  (if (map? (first options)) (rest options) options)
        dispatch-fn-form (first options)
        kw-opts  (apply hash-map (rest options))
        default-val (get kw-opts :default :default)]
    (list 'def mm-name
          (list 'create-multimethod dispatch-fn-form default-val))))

(defmacro defmethod "Defines a method for a multimethod." [mm-name dispatch-val & fn-tail]
  (list 'register-method mm-name dispatch-val
        (apply list 'fn fn-tail)))

(defn prefer-method "Prefers dispatch-val x over y in multimethod mm." [mm x y]
  (swap! (:prefer-table (meta mm)) update x (fn [s] (conj (or s #{}) y)))
  (reset! (:dispatch-cache (meta mm)) {})
  mm)

(defn remove-method "Removes the method for dispatch-val from multimethod mm." [mm dispatch-val]
  (swap! (:method-table (meta mm)) dissoc dispatch-val)
  (reset! (:dispatch-cache (meta mm)) {})
  mm)

(defn remove-all-methods "Removes all methods from multimethod mm." [mm]
  (reset! (:method-table (meta mm)) {})
  (reset! (:dispatch-cache (meta mm)) {})
  mm)

(defn methods "Returns the method table of multimethod mm." [mm]
  @(:method-table (meta mm)))

(defn get-method "Returns the method for dispatch-val, or nil." [mm dispatch-val]
  (get @(:method-table (meta mm)) dispatch-val))

(defn prefers "Returns the prefer-table of multimethod mm." [mm]
  @(:prefer-table (meta mm)))

;; ---------------------------------------------------------------------------
;; Extensible printer: print-method is a multimethod dispatched on (type x)
;; that routes pr / prn output. Users extend it for their own types via
;; (defmethod print-method :my-type [v] ...). The :default method delegates
;; to pr-builtin, which uses the C formatter and handles every built-in.
;;
;; (set-print-method! print-method) installs the late-binding hook in C so
;; pr / prn consult this multimethod. Before this line runs, pr / prn use
;; the permanently-safe C fallback (the Cortex Q5 invariant).
;; ---------------------------------------------------------------------------

(defmulti print-method
  "Multimethod for readable printing. Dispatched on (type x).
  Extend for user types via (defmethod print-method :my-type [v] ...).
  Methods write to stdout as a side effect."
  type)

(defmethod print-method :default [v] (pr-builtin v))

(set-print-method! print-method)

(defmacro with-out-str
  "Evaluates body with *out* bound to a fresh string-collecting atom,
  and returns the accumulated string."
  [& body]
  `(let [a# (atom "")]
     (binding [*out* a#]
       ~@body)
     (deref a#)))

(defmacro with-in-str
  "Evaluates body with *in* bound to a string-cursor atom holding
  s. read and read-line consume from the cursor as forms or lines
  are taken; the body's value is returned."
  [s & body]
  `(let [a# (atom ~s)]
     (binding [*in* a#]
       ~@body)))

(defn print-str
  "Returns the print-string of args, space-separated, no trailing newline."
  [& args]
  (with-out-str (apply print args)))

(defn prn-str
  "Returns the readable-string of args followed by a newline."
  [& args]
  (with-out-str (apply prn args)))

(defn println-str
  "Returns the print-string of args followed by a newline."
  [& args]
  (with-out-str (apply println args)))

(defmacro with-open "Binds resources, evaluates body, then closes each resource." [bindings & body]
  (if (empty? bindings)
    `(do ~@body)
    (let [name (first bindings)
          init (nth bindings 1)
          rest-bindings (drop 2 bindings)]
      `(let [~name ~init]
         (try
           (with-open ~(into [] rest-bindings) ~@body)
           (finally (close ~name)))))))

;; ---------------------------------------------------------------------------
;; Transducers: composable algorithmic transformations.
;;
;; A transducer is a function (rf -> rf) where rf is a reducing function
;; with three arities: ([] init), ([result] completion), ([result input] step).
;;
;; Use (transduce xf f coll) or (into to xf from) to apply.
;; ---------------------------------------------------------------------------

(defn cat "A transducer that concatenates the contents of each input." [rf]
    (fn ([] (rf))
        ([result] (rf result))
        ([result input]
         (reduce (fn [r v]
                   (let [ret (rf r v)]
                     (if (reduced? ret) (reduced ret) ret)))
                 result input))))

(defn completing "Returns a reducing function with a completion step."
  ([f] (completing f identity))
  ([f cf]
   (fn ([] (f))
       ([result] (cf result))
       ([result input] (f result input)))))

(defn unreduced "Unwraps a reduced value. If not reduced, returns x." [x]
  (if (reduced? x) (unreduced @x) x))

(defn ensure-reduced "Wraps x in reduced if it is not already reduced." [x]
  (if (reduced? x) x (reduced x)))

(defn transduce "Reduces coll using the transducer xf applied to the reducing function f."
  ([xf f coll]
   (transduce xf f (f) coll))
  ([xf f init coll]
   (let [xrf (xf (completing f))
         result (reduce xrf init coll)]
     (xrf (unreduced result)))))

(def ^:private prim-into into)
(defn into "Adds all items from from into to. With a transducer, transforms items first."
  ([] [])
  ([to] to)
  ([to from] (prim-into to from))
  ([to xf from] (transduce xf conj to from)))

(defn sequence "Returns a lazy sequence of applying transducer xf to coll, or to the pair-wise step of multiple collections. Parallel collections stop at the shortest."
  ([xf coll]
   (let [acc   (atom [])
         xrf   (xf (fn
                      ([] nil)
                      ([result] result)
                      ([_ input] (swap! acc conj input) nil)))]
     ((fn step [s]
        (lazy-seq
          (loop [s s]
            (if-let [s (seq s)]
              (do
                (let [ret (xrf nil (first s))]
                  (if (reduced? ret)
                    (do (xrf nil)
                        (let [items @acc]
                          (reset! acc [])
                          (when (seq items)
                            (concat items nil))))
                    (let [items @acc]
                      (if (seq items)
                        (do (reset! acc [])
                            (concat items (step (rest s))))
                        (recur (rest s)))))))
              (do (xrf nil)
                  (let [items @acc]
                    (reset! acc [])
                    (when (seq items) (seq items))))))))
      coll)))
  ([xf coll & more-colls]
   ;; Multi-coll variant: pull one element per collection per step and
   ;; pass them all as inputs to the transducer's reducer. The reducer
   ;; is expected to support (rf acc in1 in2 ...) for this to be
   ;; meaningful; map, filter, etc. implement that arity.
   (let [acc   (atom [])
         xrf   (xf (fn
                      ([] nil)
                      ([result] result)
                      ([_ input] (swap! acc conj input) nil)
                      ([_ input & more] (swap! acc conj (apply vector input more)) nil)))]
     ((fn step [ss]
        (lazy-seq
          (loop [ss ss]
            (let [seqs (map1 seq ss)]
              (if (all-some? seqs)
                (let [firsts (map1 first seqs)
                      ret   (apply xrf nil firsts)]
                  (if (reduced? ret)
                    (do (xrf nil)
                        (let [items @acc]
                          (reset! acc [])
                          (when (seq items) (concat items nil))))
                    (let [items @acc]
                      (if (seq items)
                        (do (reset! acc [])
                            (concat items (step (map1 rest seqs))))
                        (recur (map1 rest seqs))))))
                (do (xrf nil)
                    (let [items @acc]
                      (reset! acc [])
                      (when (seq items) (seq items)))))))))
      (cons coll more-colls)))))

(defn halt-when "Returns a transducer that halts reduction when pred is satisfied."
  ([pred] (halt-when pred nil))
  ([pred retf]
   (fn [rf]
     (fn ([] (rf))
         ([result]
          (if (and (map? result) (contains? result :__halt-when__))
            (get result :__halt-when__)
            (rf result)))
         ([result input]
          (if (pred input)
            (reduced {:__halt-when__
                      (if retf (retf (rf result) input) input)})
            (rf result input)))))))

(defn eduction "Returns a lazy sequence of applying the given transducers to coll." [& args]
  (let [coll (last args)
        xfs  (butlast args)]
    (if (= 1 (count xfs))
      (sequence (first xfs) coll)
      (sequence (apply comp xfs) coll))))

;; --- Array constructors (vectors are mino's array equivalent) ---

(defn object-array "Creates a vector from a size or collection."
  ([size-or-coll]
   (if (number? size-or-coll)
     (vec (repeat size-or-coll nil))
     (vec size-or-coll))))

(def to-array    "Converts a collection to a vector." vec)
(defn into-array "Converts a collection to a vector." [& args] (vec (last args)))
(def int-array   "Creates a vector from a size or collection." object-array)
(def long-array  "Creates a vector from a size or collection." object-array)
(def float-array "Creates a vector from a size or collection." object-array)
(def double-array "Creates a vector from a size or collection." object-array)
(def short-array "Creates a vector from a size or collection." object-array)
(def byte-array  "Creates a vector from a size or collection." object-array)
(def char-array  "Creates a vector from a size or collection." object-array)
(def boolean-array "Creates a vector from a size or collection." object-array)

;; --- Compatibility vars ---

(def *clojure-version* {:major 1 :minor 11 :incremental 0 :qualifier nil})
(defn clojure-version [] (str (:major *clojure-version*) "." (:minor *clojure-version*) "." (:incremental *clojure-version*)))

(defmacro assert
  ([x] (list 'when-not x (list 'throw "Assert failed")))
  ([x msg] (list 'when-not x (list 'throw msg))))

(def ^:dynamic *assert*
  "Controls assertion compilation. When false, `assert` is a no-op.
   Defaults to true."
  true)

;; --- Pure-Clojure surface ---

;; Identifier predicates.
(defn ident?
  "Returns true if x is a symbol or keyword."
  [x] (or (symbol? x) (keyword? x)))

(defn simple-ident?
  "Returns true if x is a non-namespace-qualified symbol or keyword."
  [x] (and (ident? x) (nil? (namespace x))))

(defn qualified-ident?
  "Returns true if x is a namespace-qualified symbol or keyword."
  [x] (and (ident? x) (some? (namespace x))))

(def ^:private special-symbols-set
  '#{if let do fn quote def set! var loop recur try throw new ns
     refer-clojure binding lazy-seq})

(defn special-symbol?
  "Returns true if x is a symbol that names a special form."
  [x] (contains? special-symbols-set x))

(defn map-entry?
  "Returns true if x is a map entry (mino represents entries as
   2-vectors)."
  [x] (and (vector? x) (= 2 (count x))))

;; Mino has no host byte-arrays, no Inst protocol, and no URI type.
;; These predicates exist for portability with portable-Clojure code
;; and always return false.
(defn bytes? [_] false)
(defn inst?  [_] false)
(defn uri?   [_] false)

(def ^:private uuid-hex-pattern #"[0-9a-fA-F]+")

(defn- uuid-string?
  "Validates RFC 4122 textual layout: 36 chars with dashes at the
   8/13/18/23 positions and hex digits everywhere else. Avoids the
   {n} quantifier mino's regex engine does not support."
  [s]
  (and (string? s)
       (= 36 (count s))
       (= "-" (nth s 8))
       (= "-" (nth s 13))
       (= "-" (nth s 18))
       (= "-" (nth s 23))
       (some? (re-matches uuid-hex-pattern
                          (str (subs s 0  8)
                               (subs s 9  13)
                               (subs s 14 18)
                               (subs s 19 23)
                               (subs s 24 36))))))

(defn uuid?
  "Returns true if x looks like a UUID string. Mino has no dedicated
   UUID type; UUIDs round-trip as their canonical string form."
  [x] (uuid-string? x))

(defn tagged-literal?
  "Returns true if x is a tagged-literal record produced by
   tagged-literal."
  [x] (boolean (some-> x meta :mino/tagged-literal)))

(defn reader-conditional?
  "Returns true if x is a reader-conditional record produced by
   reader-conditional."
  [x] (boolean (some-> x meta :mino/reader-conditional)))

;; Keyword interning probe. Mino interns every keyword on construction,
;; so any keyword we can construct already exists.
(defn find-keyword
  "Returns the keyword for the given string. In mino keywords are
   always interned, so this is equivalent to keyword for string input
   and nil for other input."
  ([s]      (when (string? s) (keyword s)))
  ([ns nm]  (when (and (string? ns) (string? nm)) (keyword ns nm))))

;; Parsing helpers (Clojure 1.11+).
(defn parse-boolean
  "Parses 'true' or 'false' (case-sensitive) and returns the boolean.
   Returns nil for any other input."
  [s]
  (when (string? s)
    (case s "true" true "false" false nil)))

(defn parse-uuid
  "Parses s as a UUID. Returns the canonical lowercase string (mino
   has no UUID type) or nil if s does not match the UUID format."
  [s]
  (when (uuid-string? s)
    (clojure.string/lower-case s)))

;; Eager-vector partitioning (Clojure 1.13+ surface, useful pre-1.13).
(defn partitionv
  "Like partition but returns a lazy seq of vectors instead of lists."
  ([n coll]          (map vec (partition n coll)))
  ([n step coll]     (map vec (partition n step coll)))
  ([n step pad coll] (map vec (partition n step pad coll))))

(defn partitionv-all
  "Like partition-all but returns a lazy seq of vectors instead of
   lists."
  ([n coll]      (map vec (partition-all n coll)))
  ([n step coll] (map vec (partition-all n step coll))))

(defn splitv-at
  "Returns a vector [(vec (take n coll)) (vec (drop n coll))]."
  [n coll] [(vec (take n coll)) (vec (drop n coll))])

(defn replicate
  "Returns a lazy seq of n copies of x. Deprecated alias for
   (take n (repeat x))."
  [n x] (take n (repeat x)))

(defn list*
  "Creates a new list containing the items prepended to the rest, the
   last of which will be treated as a sequence."
  ([args] (seq args))
  ([a args] (cons a args))
  ([a b args] (cons a (cons b args)))
  ([a b c args] (cons a (cons b (cons c args))))
  ([a b c d & more] (cons a (cons b (cons c (cons d (apply list* more)))))))

(defn reset-meta!
  "Atomically resets the metadata for a reference type to meta-map.
   Returns meta-map."
  [ref meta-map]
  (alter-meta! ref (constantly meta-map))
  meta-map)

;; Collection-hash helpers. Real Clojure mixes via Murmur3; mino uses
;; a simpler combiner that is consistent across runs but does not
;; match Clojure's exact bit pattern. Suitable for in-process equality
;; bookkeeping; not for cross-runtime hash compatibility.
(defn mix-collection-hash
  "Combines a hash-basis with the collection's count."
  [hash-basis cnt]
  (bit-xor (or hash-basis 0) (or cnt 0)))

(defn hash-ordered-coll
  "Computes a sequence-position-aware hash for an ordered collection."
  [coll]
  (mix-collection-hash
    (reduce (fn [h v] (bit-xor (* 31 h) (hash v))) 1 coll)
    (count coll)))

(defn hash-unordered-coll
  "Computes a position-independent hash for an unordered collection."
  [coll]
  (mix-collection-hash
    (reduce + 0 (map hash coll))
    (count coll)))

;; Exception cause (mino has no Throwable.getCause, so this reads from
;; ex-data or attached metadata).
(defn ex-cause
  "Returns the cause attached to the given exception, or nil."
  [ex]
  (or (some-> ex ex-data :cause)
      (some-> ex meta :cause)))

;; Inst protocol shape. Mino has no Inst type; calling inst-ms on any
;; value throws.
(defn inst-ms
  "Throws — mino has no Inst type."
  [_]
  (throw (ex-info "inst-ms is not supported on mino — there is no Inst type"
                  {:mino/unsupported :inst-ms})))

;; read: Clojure-compatible reader entry point.
;;   (read)        — reads one form from *in*. Atom-bound *in*
;;                   consumes from the head; the default stdin
;;                   sentinel raises an unsupported error (use
;;                   with-in-str or read-string instead).
;;   (read s)      — reads from the given string.
;;   (read opts s) — reads from the string with the given options.
(defn read
  ([]       (read*))
  ([s]      (read-string s))
  ([opts s] (read-string opts s)))

;; bound-fn / bound-fn*: capture the dynamic-binding context active at
;; capture time and replay it around every invocation of the wrapped fn.
;; Layered on the C primitives get-thread-bindings + with-bindings*.
(defn bound-fn*
  "Returns a function which installs the same bindings in effect as in
   the thread at the time bound-fn* was called and then invokes f."
  [f]
  (let [bindings (get-thread-bindings)]
    (fn [& args]
      (with-bindings* bindings (fn [] (apply f args))))))

(defmacro bound-fn
  "Returns a function defined by the given fntail, which will install
   the same bindings in effect as in the thread at the time bound-fn
   was called."
  [& fntail]
  `(bound-fn* (fn ~@fntail)))

;; with-redefs-fn: low-level redef (the macro variant lives above).
(defn with-redefs-fn
  "Temporarily rebinds the root values of vars to new-values while
   thunk runs, restoring originals afterward. bindings-map is a map
   of var -> new-value."
  [bindings-map thunk]
  (let [pairs (vec bindings-map)
        olds  (mapv (fn [pair] [(first pair) (deref (first pair))]) pairs)]
    (try
      (doseq [pair pairs]
        (alter-var-root (first pair) (constantly (second pair))))
      (thunk)
      (finally
        (doseq [pair olds]
          (alter-var-root (first pair) (constantly (second pair))))))))

;; Tap mechanism: a registry of fns invoked from tap>.
(def ^:private tap-fns (atom #{}))

(defn add-tap
  "Registers f as a tap target. Each call to tap> invokes every
   registered tap with the tapped value. Returns nil."
  [f]
  (swap! tap-fns conj f)
  nil)

(defn remove-tap
  "Unregisters f from the tap registry. Returns nil."
  [f]
  (swap! tap-fns disj f)
  nil)

(defn tap>
  "Sends x to every registered tap. Tap fns that throw are silently
   skipped so a misbehaving subscriber does not poison the stream.
   Returns true."
  [x]
  (doseq [f @tap-fns]
    (try (f x) (catch __e nil)))
  true)

;; Reader-extension records. tagged-literal and reader-conditional are
;; exposed as constructors so portable Clojure code can build round-
;; trippable values without depending on host-specific record types.
(defn tagged-literal
  "Builds a tagged-literal record with tag and form fields. Predicate
   tagged-literal? returns true on the result."
  [tag form]
  (with-meta {:tag tag :form form} {:mino/tagged-literal true}))

(defn reader-conditional
  "Builds a reader-conditional record with form and splicing? fields.
   Predicate reader-conditional? returns true on the result."
  [form splicing?]
  (with-meta {:form form :splicing? (boolean splicing?)}
             {:mino/reader-conditional true}))

;; --- Platform-specific forms (not supported on mino) ---
;;
;; Mino is neither a JVM nor a JavaScript runtime, so forms that
;; generate Java classes (defrecord, deftype, reify, proxy,
;; gen-class, definterface), import Java packages (import), or
;; perform Java instance-of checks (instance?) cannot be honored.
;; Each one throws an informative ex-info at expansion or call time
;; rather than silently no-op'ing or returning a fake value, so users
;; learn about the gap immediately and library failures point at the
;; real cause.

(defmacro defrecord
  "Defines a record type Name with the given fields and optional
   inline protocol specs. Establishes:
     Name       — the MINO_TYPE value (used by extend-type and
                  instance? as the dispatch key)
     ->Name     — positional constructor: (->Name f1 f2 ...) returns
                  a record value
     map->Name  — map constructor: (map->Name {:f1 v1 :f2 v2}) reads
                  declared fields from the map; non-field keys land
                  in ext.

   Fields must be a vector of symbols; they are stored as keywords
   on the type. Specs follow the same shape as extend-type:
   protocol-name followed by one or more (method [args] body) forms."
  [name fields & specs]
  (let [ns-str    (str (ns-name *ns*))
        name-str  (str name)
        ctor      (symbol (str "->" name))
        map-ctor  (symbol (str "map->" name))
        field-kws (mapv (fn [f] (keyword (str f))) fields)
        forms     [(list 'def name (list 'defrecord* ns-str name-str field-kws))
                   (list 'defn ctor fields
                         (list 'record* name (vec fields)))
                   (list 'defn map-ctor ['m]
                         (list 'record-from-map name 'm))]]
    (apply list 'do (if (seq specs)
                      (conj forms (apply list 'extend-type name specs))
                      forms))))

(defmacro deftype
  "Alias for defrecord. mino has no separate JVM-class layer to
   expose, so the deftype/defrecord distinction collapses; values
   created either way are real types with map-isomorphic behaviour."
  [name fields & specs]
  (apply list 'defrecord name fields specs))

(defmacro reify
  "Returns an instance of a fresh anonymous record type that
   satisfies the named protocols. Each reify form generates one
   type at expansion time; repeated invocations of the form share
   that type, so (= (type r1) (type r2)) is true for two values
   produced by the same reify form."
  [& specs]
  (let [ns-str   (str (ns-name *ns*))
        sym      (gensym "reify_T_")
        name-str (str sym)
        T        (gensym "T")]
    (list 'let [T (list 'defrecord* ns-str name-str [])]
          (apply list 'extend-type T specs)
          (list 'record* T []))))

(defmacro proxy [& _]
  (throw (ex-info "proxy is not supported on mino — there is no JVM to subclass"
                  {:mino/unsupported :proxy})))

(defmacro gen-class [& _]
  (throw (ex-info "gen-class is not supported on mino — there is no JVM to compile against"
                  {:mino/unsupported :gen-class})))

(defmacro definterface [& _]
  (throw (ex-info "definterface is not supported on mino — use defprotocol instead"
                  {:mino/unsupported :definterface})))

(defmacro import [& _]
  (throw (ex-info "Java import is not supported on mino — there are no Java classes to import"
                  {:mino/unsupported :import})))

(defn instance?
  "Returns true if x is an instance of t. For record types defined
   with defrecord, t is the type value and the test is type-pointer
   identity. For built-in types or ad-hoc :type-tagged values, t may
   be the keyword (type x) returns and the test is keyword equality."
  [t x]
  (= t (type x)))

(defmacro with-redefs
  "Temporarily rebinds the root bindings of vars while body executes, restoring
   them in a finally clause. Bindings is a vector of var-name/value pairs."
  [bindings & body]
  (let [pairs    (partition 2 bindings)
        var-syms (map first pairs)
        new-vals (map second pairs)
        olds     (map (fn [_] (gensym "old")) pairs)
        sets     (map (fn [v val]
                        (list 'alter-var-root (list 'var v)
                              (list 'fn ['_] val)))
                      var-syms new-vals)
        restores (map (fn [v old]
                        (list 'alter-var-root (list 'var v)
                              (list 'fn ['_] old)))
                      var-syms olds)
        finally-form (apply list 'finally restores)
        try-form     (apply list 'try (concat sets body (list finally-form)))]
    (list 'let
          (vec (mapcat (fn [old v] [old (list 'deref (list 'var v))])
                       olds var-syms))
          try-form)))

(defmacro with-local-vars
  "Binds names to fresh, lexically-scoped vars holding init values.
   Within body the names refer to vars: read with @name, mutate with
   (var-set name val). The vars are interned in the current namespace
   under gensym'd suffixes so they don't collide with named defs."
  [bindings & body]
  (let [pairs (partition 2 bindings)
        let-pairs (mapcat (fn [pair]
                            (let [n    (first pair)
                                  init (first (rest pair))]
                              [n (list 'intern '*ns*
                                       (list 'gensym (str (name n) "__lv__"))
                                       init)]))
                          pairs)]
    (apply list 'let (vec let-pairs) body)))

