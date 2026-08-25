(ns mino.template
  "Selmer-shaped string templating over plain data.

  (require '[mino.template :as tpl])
  (tpl/render \"Hello {{name}}\" {:name \"World\"})  ; => \"Hello World\"
  (tpl/render-file \"greet.txt\" {:name \"World\"})
  (tpl/render-many (tpl/compile t) ctx)   ; compile once, render many

  Supported v1: {{var}} interpolation with dotted lookup
  ({{a.b.c}}; keyword keys are tried before string keys), the blocks
  {{#each coll}} ... {{/each}} (vectors, seqs, and sets iterate their
  elements, a map iterates its vals; {{.}} inside is the current
  element and lookups resolve against the element first, then the
  enclosing scopes) and {{#if x}} ... {{else}} ... {{/if}} (nil,
  false, the empty string, and empty collections are falsy, Selmer's
  if semantics). Filters: upper, lower, and join, which takes an
  optional separator ({{xs|join:\", \"}}) and joins with the empty
  string without one, clojure.string/join's default. Filter chains
  apply left to right; a filter argument is everything after the
  filter's first colon with one layer of surrounding quotes stripped.

  Interpolation does not escape (Selmer does not escape by default),
  unknown variables render as the empty string, and non-string values
  interpolate through str.

  opts: {:tag-open \\{ :tag-close \\} :filter-tag \\|} override the
  delimiters, each as a character or a one-character string. A partial
  map overrides only the keys it names and an empty map keeps every
  default.

  compile parses a template into a plain-data AST (strings for literal
  text, [:var path filters] / [:if path then else] / [:each path body]
  nodes; no closures), render-many renders such an AST against a
  context map, and render composes the two. render-file reads its
  template with slurp.

  Errors are thrown ex-info: :kind :template/parse with :reason,
  1-based :location {:line :col}, and the offending :text for
  malformed templates; :kind :template/filter with :filter for an
  unknown filter or a bad filter call (checked at compile, except a
  join over a scalar, a render-time value error); :kind :template/each
  when each meets a non-collection value; :kind :template/opts for
  argument and opts validation. The parser walks character indices
  with no regex use, so the namespace needs no capability beyond the
  floor."
  (:require [clojure.string :as str]))

;;;; Opts and validation

(def ^:private default-delims
  {:tag-open \{ :tag-close \} :filter-tag \|})

(defn- throw-opts
  [msg arg]
  (throw (ex-info (str "mino.template: " msg)
                  {:kind :template/opts :arg arg})))

(defn- delimiter
  "The one-character string for delimiter key k, from opts or the
  defaults."
  [opts k]
  (let [v (if (contains? opts k) (get opts k) (get default-delims k))]
    (cond
      (char? v)                          (str v)
      (and (string? v) (= 1 (count v))) v
      :else (throw-opts (str ":" (name k) " must be a single character") v))))

(defn- check-opts-map
  [opts]
  (when (and (some? opts) (not (map? opts)))
    (throw-opts "opts must be a map" opts))
  (or opts {}))

(defn- check-template
  [tpl]
  (when-not (string? tpl)
    (throw-opts "template must be a string" tpl))
  tpl)

(defn- check-ctx
  [ctx]
  (when-not (or (nil? ctx) (map? ctx))
    (throw-opts "context must be a map" ctx))
  (or ctx {}))

;;;; Character scanning
;; char-at answers one-character strings; every delimiter and split
;; character here is held as one-character strings to match.

(def ^:private path-chars
  "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_-")

(defn- find-1
  "Index of the single character c at or after from, else nil."
  [s c from]
  (let [n (count s)]
    (loop [i from]
      (when (< i n)
        (if (= (char-at s i) c) i (recur (inc i)))))))

(defn- find-2
  "Index of the doubled character c (a tag's opener or closer is the
  delimiter twice) at or after from, else nil."
  [s c from]
  (let [n (count s)]
    (loop [i from]
      (when (< (+ i 1) n)
        (if (and (= (char-at s i) c) (= (char-at s (+ i 1)) c))
          i
          (recur (inc i)))))))

(defn- split-1
  "s split on the single character c, keeping empty parts."
  [s c]
  (loop [i 0, acc (transient [])]
    (if-let [j (find-1 s c i)]
      (recur (inc j) (conj! acc (subs s i j)))
      (persistent! (conj! acc (subs s i (count s)))))))

(defn- ws?
  [c]
  (or (= c " ") (= c "\t") (= c "\r") (= c "\n")))

(defn- skip-ws
  [s i]
  (let [n (count s)]
    (loop [i i]
      (if (and (< i n) (ws? (char-at s i)))
        (recur (inc i))
        i))))

(defn- split-ws
  "Whitespace-separated non-empty tokens of s."
  [s]
  (let [n (count s)]
    (loop [i (skip-ws s 0), toks (transient [])]
      (if (>= i n)
        (persistent! toks)
        (let [j (loop [j i]
                  (if (and (< j n) (not (ws? (char-at s j))))
                    (recur (inc j))
                    j))]
          (recur (skip-ws s j) (conj! toks (subs s i j))))))))

;;;; Parse errors

(defn- snippet
  "The source from idx, capped for error data."
  [s idx]
  (subs s idx (min (count s) (+ idx 40))))

(defn- line-col
  "1-based line and column of character index idx."
  [s idx]
  (loop [i 0, line 1, last-nl -1]
    (if (>= i idx)
      [line (if (< last-nl 0) (inc idx) (- idx last-nl))]
      (if (= (char-at s i) "\n")
        (recur (inc i) (inc line) i)
        (recur (inc i) line last-nl)))))

(defn- throw-parse
  [tpl reason idx]
  (let [[line col] (line-col tpl idx)]
    (throw (ex-info (str "mino.template: " (name reason)
                         " (line " line ", column " col ")")
                    {:kind :template/parse
                     :reason reason
                     :location {:line line :col col}
                     :text (snippet tpl idx)}))))

;;;; Tokenizer
;; Tokens: [:text s], [:var path filters idx], [:open name path idx],
;; [:close name idx], [:else idx], all plain data.

(defn- valid-seg?
  [seg]
  (if (= seg "")
    false
    (let [n (count seg)]
      (loop [i 0]
        (if (>= i n)
          true
          (if (some? (str/index-of path-chars (char-at seg i) 0))
            (recur (inc i))
            false))))))

(defn- parse-path
  "A dotted lookup path as a vector of segment strings; the single
  dot answers the current element."
  [tpl idx expr]
  (if (= expr ".")
    ["."]
    (let [segs (split-1 expr ".")]
      (if (every? valid-seg? segs)
        segs
        (throw-parse tpl :bad-path idx)))))

(def ^:private filter-max-args
  {"upper" 0 "lower" 0 "join" 1})

(defn- unquote-arg
  "One layer of matching surrounding quotes comes off a filter
  argument (the Django shape: {{xs|join:\", \"}} separates with a
  comma and space); anything else stays verbatim."
  [arg]
  (if (and (>= (count arg) 2)
           (or (and (str/starts-with? arg "\"")
                    (str/ends-with? arg "\""))
               (and (str/starts-with? arg "'")
                    (str/ends-with? arg "'"))))
    (subs arg 1 (dec (count arg)))
    arg))

(defn- parse-filter
  "One filter part after the filter tag: [name arg] where arg is the
  text after the first colon with one quote layer stripped, or nil.
  Filter names and arities are checked here, at compile."
  [part]
  (let [j (find-1 part ":" 0)
        name (str/trim (if j (subs part 0 j) part))
        arg (when j (unquote-arg (subs part (inc j))))]
    (let [max-args (get filter-max-args name)]
      (when (nil? max-args)
        (throw (ex-info (str "mino.template: unknown filter " name)
                        {:kind :template/filter
                         :filter name
                         :reason :unknown})))
      (when (and (some? arg) (zero? max-args))
        (throw (ex-info (str "mino.template: filter " name
                             " takes no argument")
                        {:kind :template/filter
                         :filter name
                         :reason :arg-not-expected})))
      [name arg])))

(defn- expr-token
  [tpl filter-c idx body]
  (let [parts (split-1 body filter-c)]
    [:var (parse-path tpl idx (str/trim (first parts)))
     (mapv parse-filter (rest parts))
     idx]))

(defn- open-token
  [tpl idx head]
  (let [parts (split-ws head)]
    (when (not= 2 (count parts))
      (throw-parse tpl :bad-block idx))
    (let [name (first parts)
          path (parse-path tpl idx (second parts))]
      (when-not (or (= name "if") (= name "each"))
        (throw-parse tpl :unknown-block idx))
      [:open name path idx])))

(defn- classify
  "The token for one tag body."
  [tpl filter-c idx body]
  (let [body (str/trim body)]
    (cond
      (= body "")       (throw-parse tpl :empty-tag idx)
      (= body "else")   [:else idx]
      :else (cond
              (= (char-at body 0) "/")
              (let [name (str/trim (subs body 1))]
                (when-not (or (= name "if") (= name "each"))
                  (throw-parse tpl :unknown-block idx))
                [:close name idx])

              (= (char-at body 0) "#") (open-token tpl idx (subs body 1))

              :else (expr-token tpl filter-c idx body)))))

(defn- conj-text
  "Literal text token, only when non-empty."
  [toks s from to]
  (if (< from to) (conj! toks [:text (subs s from to)]) toks))

(defn- tokenize
  "tpl -> the token vector. One pass with character-index scans; text
  between tags is cut once per tag, never per character. Every
  delimiter is resolved up front, so a bad opts value throws even
  when the template has no tags."
  [tpl opts]
  (let [open-c (delimiter opts :tag-open)
        close-c (delimiter opts :tag-close)
        filter-c (delimiter opts :filter-tag)]
    (loop [start 0, toks (transient [])]
      (let [i (find-2 tpl open-c start)]
        (if (nil? i)
          (persistent! (conj-text toks tpl start (count tpl)))
          (let [j (find-2 tpl close-c (+ i 2))]
            (when (nil? j)
              (throw-parse tpl :unterminated-tag i))
            (recur (+ j 2)
                   (-> toks
                       (conj-text tpl start i)
                       (conj! (classify tpl filter-c i (subs tpl (+ i 2) j)))))))))))

;;;; Parser: tokens -> plain-data AST
;; Nodes: literal strings, [:var path filters], [:if path then else],
;; [:each path body].

(defn- parse-run
  "toks from tpl -> [nodes stop stop-idx], where stop is the token
  that ended the run (a :close or :else, not consumed) or nil at the
  end of the token stream."
  [toks i tpl]
  (loop [i i, nodes (transient [])]
    (if (>= i (count toks))
      [(persistent! nodes) nil nil]
      (let [tok (nth toks i), op (nth tok 0)]
        (cond
          (= op :text)  (recur (inc i) (conj! nodes (nth tok 1)))
          (= op :var)   (recur (inc i) (conj! nodes (subvec tok 0 3)))
          (= op :else)  [(persistent! nodes) tok i]
          (= op :close) [(persistent! nodes) tok i]
          :else (let [[_ name path open-idx] tok
                      [body stop si] (parse-run toks (inc i) tpl)]
                  (cond
                    (nil? stop)
                    (throw-parse tpl :unclosed-block open-idx)

                    (and (= (nth stop 0) :else) (= name "if"))
                    (let [[els stop2 si2] (parse-run toks (inc si) tpl)]
                      (cond
                        (nil? stop2)
                        (throw-parse tpl :unclosed-block open-idx)

                        (= (nth stop2 0) :else)
                        (throw-parse tpl :duplicate-else (peek stop2))

                        (not= "if" (nth stop2 1))
                        (throw-parse tpl :mismatched-close (peek stop2))

                        :else (recur (inc si2)
                                     (conj! nodes [:if path body els]))))

                    (= (nth stop 0) :else)
                    (throw-parse tpl :else-not-allowed (peek stop))

                    (not= name (nth stop 1))
                    (throw-parse tpl :mismatched-close (peek stop))

                    :else (recur (inc si)
                                 (conj! nodes (if (= name "if")
                                                [:if path body []]
                                                [:each path body]))))))))))

(defn- parse-tokens
  [toks tpl]
  (let [[nodes stop] (parse-run toks 0 tpl)]
    (when stop
      (throw-parse tpl
                   (if (= (nth stop 0) :close)
                     :unmatched-close
                     :else-outside-if)
                   (peek stop)))
    nodes))

;;;; Render
;; scopes is a vector of maps/values, innermost last; each pushes the
;; current element. A nil resolution (missing or explicit) falls
;; through to the enclosing scope, a found false does not.

(defn- step-get
  [scope seg]
  (when (map? scope)
    (let [v (get scope (keyword seg))]
      (if (some? v) v (get scope seg)))))

(defn- resolve-path
  [scope path]
  (loop [scope scope, path path]
    (if (empty? path)
      scope
      (let [v (step-get scope (first path))]
        (when (some? v)
          (recur v (next path)))))))

(defn- lookup
  [scopes path]
  (if (= path ["."])
    (peek scopes)
    (loop [i (dec (count scopes))]
      (if (>= i 0)
        (let [v (resolve-path (nth scopes i) path)]
          (if (some? v)
            v
            (recur (dec i))))
        nil))))

(defn- truthy?
  "Selmer's if semantics: nil, false, the empty string, and empty
  collections are falsy; every other value, 0 included, is truthy."
  [v]
  (not (or (nil? v)
           (false? v)
           (and (string? v) (= "" v))
           (and (coll? v) (empty? v)))))

(defn- display
  [v]
  (cond (nil? v) "" (string? v) v :else (str v)))

(defn- apply-filter
  [[name arg] v]
  (cond
    (= name "upper") (str/upper-case (display v))
    (= name "lower") (str/lower-case (display v))
    :else (cond
            (nil? v) ""
            (coll? v) (if arg (str/join arg v) (str/join v))
            :else (throw (ex-info "mino.template: join needs a collection"
                                  {:kind :template/filter
                                   :filter "join"
                                   :reason :not-a-collection})))))

(declare render-node)

(defn- render-nodes
  [nodes scopes]
  (str/join (mapv #(render-node % scopes) nodes)))

(defn- render-each
  [path body scopes]
  (let [v (lookup scopes path)
        body-of (fn [el] (render-nodes body (conj scopes el)))]
    (cond
      (nil? v) ""
      (map? v) (str/join (mapv body-of (vals v)))
      (coll? v) (str/join (mapv body-of v))
      :else (throw (ex-info "mino.template: each over a non-collection"
                            {:kind :template/each :value v})))))

(defn- render-node
  [node scopes]
  (if (string? node)
    node
    (let [op (nth node 0)]
      (cond
        (= op :var) (reduce (fn [v f] (apply-filter f v))
                            (lookup scopes (nth node 1))
                            (nth node 2))
        (= op :if) (if (truthy? (lookup scopes (nth node 1)))
                     (render-nodes (nth node 2) scopes)
                     (render-nodes (nth node 3) scopes))
        :else (render-each (nth node 1) (nth node 2) scopes)))))

;;;; Public API

(defn compile
  "Parses a template into its plain-data AST, embedding the delimiter
  opts. render-many renders such an AST; render composes the two."
  ([tpl] (compile tpl nil))
  ([tpl opts]
   (check-template tpl)
   (let [opts (check-opts-map opts)]
     (parse-tokens (tokenize tpl opts) tpl))))

(defn render-many
  "Renders a compiled template AST against a context map. The cheap
  call for one template against many contexts."
  [compiled ctx]
  (when-not (vector? compiled)
    (throw-opts "render-many requires a compiled template" compiled))
  (render-nodes compiled [(check-ctx ctx)]))

(defn render
  "Renders a template string against a context map in the Selmer
  call shape; opts overrides the delimiters."
  ([tpl ctx] (render tpl ctx nil))
  ([tpl ctx opts]
   (render-many (compile tpl opts) ctx)))

(defn render-file
  "Reads the template with slurp and renders it; a missing file
  propagates the slurp error."
  ([path ctx] (render-file path ctx nil))
  ([path ctx opts]
   (render (slurp path) ctx opts)))
