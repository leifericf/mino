(ns mino.cli
  "The babashka.cli option parser over a seq of argument strings.

  (require '[mino.cli :as cli])
  (cli/parse-opts [\"--port\" \"8080\"] {:spec {:port {:coerce :long}}})
  ;; => {:port 8080}
  (cli/parse-opts [\"-p\" \"8080\"] {:spec {:port {:coerce :long
                                                   :alias :p
                                                   :default 80}})
  ;; => {:port 8080}
  (cli/parse-opts [\"--verbose\"])            ; => {:verbose true}
  (cli/parse-opts [\"--no-colors\"])          ; => {:colors false}
  (cli/parse-opts [\"-abc\"])                 ; => {:a true :b true :c true}

  The spec maps option names to {:coerce :long | :double | :boolean |
  :keyword | :symbol | :edn | :string | fn, :alias :p, :default v}.
  --name v and --name=v both carry values; a flag standing alone (or
  one whose spec coerces :boolean) is true. Any option negates as
  --no-name. Short tokens try the exact alias first (so :vv beats
  per-char splitting), then expand per character. Unspec'd values
  auto-coerce: true/false, numbers, and :keywords become data, the
  rest stay strings. Parsing is open world: unknown options parse,
  leftover positionals ride on the result's metadata under
  :mino.cli/args, and a -- inside the args moves the rest there.

  A coercion that cannot succeed throws ex-info with :kind
  :cli/parse, :option, and :value in the data map.

  (cli/format-opts {:spec {:port {:alias :p :default 80
                                  :desc \"The port.\"}}})
  ;; => \"  -p, --port  The port. (default: 80)\""
  (:require [clojure.string :as str]))

;;;; Spec shape

(defn- spec-entries
  "Spec as a map or a vector of pairs, answered as [name opt-map]
  pairs; nil answers nil. A vector of pairs keeps its order (what
  format-opts prints)."
  [spec]
  (cond
    (nil? spec)    nil
    (map? spec)    (seq spec)
    (vector? spec) (seq spec)
    :else (throw (ex-info "mino.cli: :spec must be a map or a vector of pairs"
                          {:kind :cli/parse :spec spec}))))

(defn- alias->name
  "Alias keyword to option name, over the spec entries."
  [entries]
  (into {} (keep (fn [[k m]] (when (:alias m) [(:alias m) k])) entries)))

(defn- boolean-names
  "Names whose spec coerces :boolean: their flag never consumes the
  next token as a value."
  [entries]
  (into #{} (keep (fn [[k m]]
                    (when (contains? #{:boolean :bool} (:coerce m)) k))
                  entries)))

;;;; Coercion

(defn- throw-parse [msg option value]
  (throw (ex-info msg {:kind :cli/parse :option option :value value})))

(defn- auto-coerce [s]
  (cond
    (= "true" s)  true
    (= "false" s) false
    (re-matches #"[+-]?[\d].*" s) (read-string s)
    (str/starts-with? s ":") (keyword (subs s 1))
    :else s))

(defn- coerce-value
  "Applies coercion f to the string s. Non-strings pass through
  (flags already parsed as true/false)."
  [k f s]
  (cond
    (not (string? s)) s
    (nil? f)  (auto-coerce s)
    (fn? f)   (let [v (f s)]
                (when (nil? v)
                  (throw-parse (str "mino.cli: coercion of --" (name k)
                                    " returned nil for \"" s "\"")
                               k s))
                v)
    :else (case f
            :boolean (if (contains? #{"true" "false"} s)
                       (= "true" s)
                       (throw-parse (str "mino.cli: cannot coerce \"" s
                                         "\" for --" (name k) " to boolean")
                                    k s))
            :long   (if-let [v (parse-long s)]
                      v
                      (throw-parse (str "mino.cli: cannot coerce \"" s
                                        "\" for --" (name k) " to long")
                                   k s))
            :double (if-let [v (parse-double s)]
                      v
                      (throw-parse (str "mino.cli: cannot coerce \"" s
                                        "\" for --" (name k) " to double")
                                   k s))
            :keyword (keyword (if (str/starts-with? s ":") (subs s 1) s))
            :symbol  (symbol s)
            :string  s
            :edn     (try
                       (read-string s)
                       (catch err
                         (throw-parse (str "mino.cli: cannot read \"" s
                                           "\" for --" (name k) " as EDN")
                                      k s)))
            (throw-parse (str "mino.cli: unknown coercion " f
                              " for --" (name k))
                         k s))))

;; :bool and :int are the babashka synonyms of :boolean and :long.
(defn- coerce-dispatch [k f s]
  (coerce-value k (case f :bool :boolean, :int :long, f) s))

;;;; Token parsing

(defn- option-like? [s]
  (and (string? s) (str/starts-with? s "-") (> (count s) 1)))

(defn- split-flag-body
  "Splits a flag body at the first = into [name value?]."
  [body]
  (if-let [i (str/index-of body "=")]
    [(subs body 0 i) (subs body (inc i))]
    [body nil]))

(defn- value-for
  "The value a flag carries and how many extra tokens it consumed:
  an =value when given, else true when the option is
  boolean-coerced, else the next token when it exists and is not
  option-like."
  [eq-value k next-tokens boolean-name-set]
  (if (some? eq-value)
    [eq-value 0]
    (if (contains? boolean-name-set k)
      [true 0]
      (let [n (first next-tokens)]
        (if (and (some? n) (not (option-like? n)))
          [n 1]
          [true 0])))))

(defn- parse-long-flag
  "Long flag --name, --name=v, --no-name into {name [value used]}."
  [token next-tokens aliases boolean-name-set known?]
  (let [[body eq-value] (split-flag-body (subs token 2))]
    (if (and (nil? eq-value)
             (str/starts-with? body "no-")
             (not (known? (keyword body))))
      {(keyword (subs body 3)) [false 0]}
      (let [k (or (get aliases (keyword body)) (keyword body))
            [v used] (value-for eq-value k next-tokens boolean-name-set)]
        {k [v used]}))))

(defn- parse-short-flag
  "Short flag: the exact alias first (with optional =value), else
  per-character boolean expansion."
  [token next-tokens aliases boolean-name-set]
  (let [[body eq-value] (split-flag-body (subs token 1))
        direct          (get aliases (keyword body))]
    (if direct
      (let [[v used] (value-for eq-value direct next-tokens boolean-name-set)]
        {direct [v used]})
      (into {}
            (map (fn [c]
                   (let [one (str c)]
                     [(or (get aliases (keyword one)) (keyword one))
                      [true 0]]))
                 body)))))

;;;; format-opts

(defn- flag-column
  "The left column of one usage row: alias, long name, and ref."
  [k {:keys [alias ref negatable]}]
  (str (if alias (str "-" (name alias) ", ") "    ")
       "--" (if negatable "[no-]" "")
       (name k)
       (if ref (str " " ref) "")))

(defn- opt-desc
  "The right column: desc with the default appended in parentheses."
  [m]
  (let [dv (when (contains? m :default)
             (str "(default: "
                  (or (:default-desc m) (str (:default m))) ")"))]
    (cond
      (and (:desc m) dv) (str (:desc m) " " dv)
      (:desc m)          (:desc m)
      dv                 dv
      :else              "")))

(defn format-opts
  "Renders a spec into the babashka.cli usage block, one row per
  option: the alias and long-name column (with ref), two spaces,
  then the description with any default in parentheses. Takes
  {:spec spec} with optional {:order [names]} (selects and orders;
  names a missing option and throws) and {:indent n} (default 2).
  A vector-of-pairs spec keeps its declared order. No terminal
  wrapping.

  (cli/format-opts {:spec {:port {:alias :p :default 80
                                  :desc \"The port.\"}}})
  ;; => \"  -p, --port  The port. (default: 80)\""
  [{:keys [spec order indent] :or {indent 2}}]
  (let [entries   (or (spec-entries spec) [])
        by-name   (into {} entries)
        chosen    (if order
                    (map (fn [k]
                           (if (contains? by-name k)
                             [k (get by-name k)]
                             (throw-parse (str "mino.cli: :order names "
                                               (name k)
                                               " which is not in :spec")
                                          k nil)))
                         order)
                    entries)
        columns   (map (fn [[k m]] (flag-column k m)) chosen)
        width     (apply max 0 (map count columns))
        pad       (apply str (repeat indent " "))]
    (str/join "\n"
              (map (fn [[col m]]
                     (str pad col
                          (apply str (repeat (- width (count col)) " "))
                          "  " (opt-desc m)))
                   (map vector columns (map second chosen))))))

;;;; parse-opts

(defn parse-opts
  "Parses argument strings into an options map, babashka.cli shape.
  opts carries the spec under :spec; spec defaults fill absent keys.
  Leftover positional arguments ride on the result's metadata under
  :mino.cli/args. See the namespace docstring for the contract."
  ([args] (parse-opts args nil))
  ([args opts]
   (let [entries     (spec-entries (get opts :spec))
         aliases     (alias->name entries)
         bools       (boolean-names entries)
         names       (into #{} (map first) entries)
         known?      (fn [k] (or (contains? names k)
                                 (contains? aliases k)))
         coercions   (into {} (keep (fn [[k m]]
                                      (when (:coerce m) [k (:coerce m)]))
                                    entries))
         raw         (volatile! {})
         positionals (volatile! [])]
     (loop [tokens (seq args)]
       (when-let [t (first tokens)]
         (let [more (next tokens)]
           (cond
             (= "--" t)
             (vswap! positionals into (vec more))

             (option-like? t)
             (let [flags (if (str/starts-with? t "--")
                           (parse-long-flag t more aliases bools known?)
                           (parse-short-flag t more aliases bools))
                   used  (apply max 0 (map second (vals flags)))]
               (doseq [[k [v _]] flags]
                 (vswap! raw assoc k v))
               (recur (drop (inc used) tokens)))

             :else
             (do (vswap! positionals conj t)
                 (recur more))))))
     (let [coerced (reduce-kv (fn [m k v]
                                (assoc m k (coerce-dispatch k
                                                            (get coercions k)
                                                            v)))
                              {}
                              @raw)
           with-defaults (reduce (fn [m [k sm]]
                                   (if (or (contains? m k)
                                           (not (contains? sm :default)))
                                     m
                                     (assoc m k (:default sm))))
                                 coerced
                                 entries)]
        (with-meta with-defaults {:mino.cli/args @positionals})))))
