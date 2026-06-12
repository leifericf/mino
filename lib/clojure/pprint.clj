(ns clojure.pprint
  (:require [clojure.string :as str]))

;; clojure.pprint -- a compact pretty printer.
;;
;; The engine renders to a column-tracking string buffer and resolves
;; conditional newlines (:linear / :fill / :miser / :mandatory) against
;; *print-right-margin* and *print-miser-width*. Dispatch is a
;; rebindable function over a structural type table (simple-dispatch is
;; the default): maps break between pairs, collections fill and wrap,
;; depth indents. write exposes the Common-Lisp-style keyword interface;
;; pprint prints to *out*.
;;
;; A logical block decides whether its body fits on the current line by
;; rendering the body once in a measuring pass; if it does not fit (or a
;; mandatory newline forces it), conditional newlines in the block become
;; real newlines indented to the block. The measuring pass re-runs the
;; (side-effect-free) body thunk against a scratch buffer.

;; ---------------------------------------------------------------------------
;; Control variables
;; ---------------------------------------------------------------------------

(def ^:dynamic *print-pretty*
  "Bind to true if you want write to use pretty printing."
  true)

(def ^:dynamic *print-right-margin*
  "Pretty printing will try to avoid anything going beyond this column.
  Set it to nil to let lines be arbitrarily long."
  72)

(def ^:dynamic *print-miser-width*
  "The column at which to enter miser style, adding newlines in more
  places to keep lines short."
  40)

(def ^:dynamic *print-suppress-namespaces*
  "Don't print namespaces with symbols. Useful when pretty printing the
  results of macro expansions."
  nil)

(def ^:dynamic *print-radix*
  "Print a radix specifier in front of integers. If *print-base* is 2, 8
  or 16, the specifier is #b, #o or #x; otherwise it is #XXr where XX is
  the decimal value of *print-base*."
  nil)

(def ^:dynamic *print-base*
  "The base to use for printing integers."
  10)

;; ---------------------------------------------------------------------------
;; Engine state
;;
;; The engine writes to the ambient *out* with plain print (never
;; re-binding *out* mid-stream, which mino's string capture does not
;; tolerate). Callers that need a private sink bind *out* once at the top.
;;
;; *pp-state*   an atom holding the live engine state:
;;   :col       current output column (characters since the last newline)
;;   :blocks    a stack of logical-block frames, innermost last. Each
;;              frame is {:start-col C :indent C :suffix S :mode M}.
;;   :measure   when true, the engine is in a measuring pass: conditional
;;              newlines collapse to spaces and output goes nowhere real.
;;   :max-col   the widest column reached (used by the measuring pass).
;;   :saw-mandatory  set during a measuring pass when a mandatory newline
;;              is requested, forcing the enclosing block to break.
;; ---------------------------------------------------------------------------

(def ^:dynamic *pp-state* nil)

(defn- pp-engine?
  "True when the pretty-print engine is active."
  []
  (some? *pp-state*))

(defn- pp-emit
  "Append text s to the engine, updating the column counter. In a
  measuring pass output is discarded but columns still advance."
  [s]
  (when (pos? (count s))
    (let [st @*pp-state*
          nl (str/last-index-of s "\n")
          col (if nl
                (- (count s) (long nl) 1)
                (+ (:col st) (count s)))
          mx (max (:max-col st) col (:col st))]
      (reset! *pp-state* (assoc st :col col :max-col mx))
      (when-not (:measure st)
        (print s))))
  nil)

(defn pp-write-str
  "Write a raw string through the pretty-printing engine, advancing the
  column. Public so macros expanded in other namespaces (print-length-loop)
  can reach it."
  [s]
  (pp-emit s))

(defn- pp-indent
  "Current indentation column for newlines: the indent of the innermost
  logical block, or 0 at top level."
  []
  (let [blocks (:blocks @*pp-state*)]
    (if (seq blocks)
      (:indent (peek blocks))
      0)))

(defn- pp-newline-break
  "Emit a real newline followed by the current block indentation."
  []
  (let [n (pp-indent)]
    (pp-emit "\n")
    (pp-emit (apply str (repeat n " ")))))

;; ---------------------------------------------------------------------------
;; Number formatting honoring *print-base* / *print-radix*
;; ---------------------------------------------------------------------------

(def ^:private digit-chars "0123456789abcdefghijklmnopqrstuvwxyz")

(defn- int->base
  "Render the non-negative integer n in the given base as a string."
  [n base]
  (if (zero? n)
    "0"
    (loop [n n acc ""]
      (if (zero? n)
        acc
        (recur (quot n base)
               (str (nth digit-chars (mod n base)) acc))))))

(defn- radix-prefix
  "The reader prefix for the given base when *print-radix* is on."
  [base]
  (cond
    (= base 2) "#b"
    (= base 8) "#o"
    (= base 16) "#x"
    :else (str "#" base "r")))

(defn- format-integer
  "Format integer x subject to base and radix. Returns a string. Base 10
  with no radix prints the plain decimal."
  [x base radix]
  (let [neg (neg? x)
        mag (if neg (- x) x)
        body (int->base mag base)]
    (cond
      (and (= base 10) (not radix)) (str x)
      (and (= base 10) radix) (str (if neg "-" "") body ".")
      :else (str (if neg "-" "") (when radix (radix-prefix base)) body))))

(defn- format-simple-number
  "If x is an integer and either a non-default base or radix is in
  effect, return its formatted string; otherwise nil (caller uses pr)."
  [x]
  (when (and (integer? x)
             (or (not (= *print-base* 10)) *print-radix*))
    (format-integer x *print-base* *print-radix*)))

;; ---------------------------------------------------------------------------
;; The functional interface: logical blocks, conditional newlines, indents
;; ---------------------------------------------------------------------------

(def ^:dynamic *print-pprint-dispatch* nil)

(defn- check-enumerated [arg choices]
  (when-not (choices arg)
    (throw (str "Bad argument: " arg ". It must be one of " choices))))

(defn pprint-newline
  "Print a conditional newline to the pretty-printing engine. kind is
  :linear, :miser, :fill or :mandatory. Output goes to *out*, which must
  be a pretty-printing engine (true within pprint or a dispatch fn)."
  [kind]
  (check-enumerated kind #{:linear :miser :fill :mandatory})
  (let [st @*pp-state*
        frame (peek (:blocks st))
        mode (if frame (:mode frame) :inline)]
    (cond
      (:measure st)
      (if (= kind :mandatory)
        (do (reset! *pp-state* (assoc st :saw-mandatory true))
            (pp-emit "\n"))
        (pp-emit " "))
      (= kind :mandatory) (pp-newline-break)
      (= mode :break)
      (if (= kind :miser)
        (if (>= (:start-col frame)
                (- (or *print-right-margin* 1000000) *print-miser-width*))
          (pp-newline-break)
          (pp-emit " "))
        (pp-newline-break))
      :else nil))
  nil)

(defn pprint-indent
  "Set the indentation for following lines of the current logical block.
  relative-to is :block (offset from the block start) or :current (offset
  from the current column); n is the offset."
  [relative-to n]
  (check-enumerated relative-to #{:block :current})
  (let [st @*pp-state*
        blocks (:blocks st)]
    (when (seq blocks)
      (let [frame (peek blocks)
            base (if (= relative-to :block) (:start-col frame) (:col st))
            frame' (assoc frame :indent (+ base n))]
        (reset! *pp-state* (assoc st :blocks (conj (pop blocks) frame'))))))
  nil)

(defn pprint-tab
  "Tab in the pretty-printing engine. Not yet implemented."
  [kind colnum colinc]
  (check-enumerated kind #{:line :section :line-relative :section-relative})
  (throw "pprint-tab is not yet implemented"))

(defn- run-block
  "Open a logical block (prefix / suffix), run the body thunk under the
  given break mode, and close it."
  [prefix suffix per-line body-thunk mode]
  (let [st @*pp-state*
        start-col (:col st)
        pfx (or prefix "")
        frame {:start-col start-col
               :indent (+ start-col (count pfx))
               :suffix (or suffix "")
               :per-line per-line
               :mode mode}]
    (reset! *pp-state* (assoc st :blocks (conj (:blocks st) frame)))
    (when prefix (pp-emit prefix))
    (body-thunk)
    (when suffix (pp-emit suffix))
    (let [st2 @*pp-state*]
      (reset! *pp-state* (assoc st2 :blocks (pop (:blocks st2)))))))

(defn- block-fits-or-break?
  "True when the block should render inline (fits within margin and has
  no mandatory newline), false when it must break. Runs the body in a
  throwaway measuring pass over a fresh state cloned from the current
  one, then discards it."
  [prefix suffix body-thunk margin]
  (let [st @*pp-state*
        start-col (:col st)
        decision (atom :inline)]
    (binding [*pp-state* (atom (assoc st
                                      :measure true
                                      :max-col start-col
                                      :col start-col
                                      :saw-mandatory false))]
      (run-block prefix suffix nil body-thunk :inline)
      (let [mst @*pp-state*]
        (when (or (:saw-mandatory mst)
                  (> (:max-col mst) margin)
                  (> (:col mst) margin))
          (reset! decision :break))))
    (= @decision :inline)))

(defn pprint-logical-block*
  "Functional core of pprint-logical-block. prefix / per-line / suffix may
  be nil; body-thunk is a zero-argument function performing the block's
  writes. Decides break vs inline by measuring the body, then renders it
  for real."
  [prefix per-line suffix body-thunk]
  (if (:measure @*pp-state*)
    (run-block prefix suffix per-line body-thunk :inline)
    (let [margin (or *print-right-margin* 1000000)
          mode (if (block-fits-or-break? prefix suffix body-thunk margin)
                 :inline
                 :break)]
      (run-block prefix suffix per-line body-thunk mode)))
  nil)

(defmacro pprint-logical-block
  "Execute the body as a pretty-printing logical block on *out*, which
  must be a pretty-printing engine (true within pprint or a dispatch fn).
  Optional leading options: :prefix, :per-line-prefix, :suffix."
  [& args]
  (loop [opts {} body args]
    (if (and (#{:prefix :per-line-prefix :suffix} (first body))
             (next body))
      (recur (assoc opts (first body) (second body)) (drop 2 body))
      `(pprint-logical-block* ~(:prefix opts) ~(:per-line-prefix opts)
                              ~(:suffix opts)
                              (fn [] ~@body)))))

(defmacro print-length-loop
  "A loop that iterates at most *print-length* times, emitting \"...\"
  when the limit is reached. For use in dispatch functions."
  [bindings & body]
  (let [count-var (gensym "pll-count")
        walk (fn walk [form]
               (cond
                 (and (seq? form) (= 'recur (first form)))
                 (apply list 'recur (list 'inc count-var)
                        (map walk (rest form)))
                 (seq? form) (apply list (map walk form))
                 (vector? form) (vec (map walk form))
                 :else form))
        body* (doall (map walk body))]
    ;; *print-length* lives in clojure.core; emit the bare symbol (not
    ;; the syntax-quote-qualified clojure.pprint/* form) so it resolves
    ;; to the core var and honors a dynamic binding at the call site.
    `(loop ~(apply vector count-var 0 bindings)
       (if (or (not ~'*print-length*) (< ~count-var ~'*print-length*))
         (do ~@body*)
         (pp-write-str "...")))))

;; ---------------------------------------------------------------------------
;; write-out and the default dispatch
;; ---------------------------------------------------------------------------

(declare simple-dispatch)

(defn- pp-collection?
  "True when object is one of the aggregate kinds the dispatch table
  formats structurally (rather than printing atomically)."
  [object]
  (or (map? object) (vector? object) (set? object)
      (seq? object) (list? object)))

(defn- pp-pr
  "Print object the way pr would, but honoring *print-base* / *print-radix*
  for integers and *print-suppress-namespaces* for symbols. Routes through
  the engine so columns stay accurate."
  [object]
  (cond
    (and *print-suppress-namespaces* (symbol? object))
    (pp-emit (name object))
    :else
    (if-let [s (format-simple-number object)]
      (pp-emit s)
      (pp-emit (pr-str object)))))

(defn write-out
  "Write an object to *out* subject to the current printer control
  variables. Intended for use inside pretty-print dispatch functions,
  which run with the engine already established. Aggregates are handed to
  the dispatch function (so nested structures honor custom dispatch);
  atoms are printed directly, so a dispatch function written for a
  specific aggregate shape need not handle every leaf type."
  [object]
  (cond
    (not *print-pretty*) (pr object)
    (pp-collection? object) (*print-pprint-dispatch* object)
    :else (pp-pr object))
  nil)

(defn- pp-coll
  "Render a collection as a logical block: prefix, fill-wrapped elements
  separated by a space and a linear newline, suffix."
  [prefix suffix xs]
  (pprint-logical-block :prefix prefix :suffix suffix
    (print-length-loop [s (seq xs)]
      (when s
        (write-out (first s))
        (when (next s)
          (pp-emit " ")
          (pprint-newline :linear)
          (recur (next s)))))))

(defn- pp-map
  "Render a map as a logical block, one key/value pair per broken line."
  [amap]
  (pprint-logical-block :prefix "{" :suffix "}"
    (print-length-loop [s (seq amap)]
      (when s
        (let [pair (first s)]
          (pprint-logical-block
            (write-out (key pair))
            (pp-emit " ")
            (pprint-newline :linear)
            (write-out (val pair))))
        (when (next s)
          (pp-emit ", ")
          (pprint-newline :linear)
          (recur (next s)))))))

(defn- pp-write-top
  "Write the top-level object: hand it to the dispatch function (so a
  custom dispatch sees the whole object, even when it is an atom), or pr
  when pretty printing is off."
  [object]
  (if *print-pretty*
    (*print-pprint-dispatch* object)
    (pp-pr object)))

(defn simple-dispatch
  "The default pretty-print dispatch function for plain data."
  [object]
  (cond
    (nil? object) (pp-emit "nil")
    (map? object) (pp-map object)
    (vector? object) (pp-coll "[" "]" object)
    (set? object) (pp-coll "#{" "}" object)
    (seq? object) (pp-coll "(" ")" object)
    (list? object) (pp-coll "(" ")" object)
    :else (pp-pr object)))

(defn set-pprint-dispatch
  "Set the global pretty-print dispatch function."
  [function]
  (alter-var-root #'*print-pprint-dispatch* (constantly function))
  nil)

(set-pprint-dispatch simple-dispatch)

(defmacro with-pprint-dispatch
  "Execute body with the pretty-print dispatch function bound to function."
  [function & body]
  ;; Bind the bare var name (not the syntax-quote-qualified form) so the
  ;; dynamic binding established at the call site is visible to the
  ;; engine's own unqualified reads of *print-pprint-dispatch*.
  `(binding [~'*print-pprint-dispatch* ~function]
     ~@body))

;; ---------------------------------------------------------------------------
;; Column-aware writer helpers
;; ---------------------------------------------------------------------------

(defn get-pretty-writer
  "Returns a pretty-printing writer wrapping base-writer, or a default
  output sink when base-writer is nil. The result is usable as *out* for
  write-out."
  [base-writer]
  (or base-writer :mino/stdout))

(defn fresh-line
  "Emit a newline only when output is not already at the start of a line.
  Inside the engine the column is known exactly; otherwise, when *out* is
  a string-collecting atom, inspect the buffer; failing that, emit a
  newline."
  []
  (cond
    (pp-engine?)
    (when (pos? (:col @*pp-state*))
      (pp-emit "\n"))
    (atom? *out*)
    (let [s @*out*]
      (when (and (string? s) (pos? (count s)) (not (str/ends-with? s "\n")))
        (newline)))
    :else (newline))
  nil)

;; ---------------------------------------------------------------------------
;; Top-level entry points
;; ---------------------------------------------------------------------------

(defn- run-engine
  "Run body-fn with a fresh engine writing to the ambient *out*. start-col
  seeds the column. Returns nil."
  [start-col body-fn]
  (binding [*pp-state* (atom {:col start-col :max-col start-col
                              :blocks [] :measure false
                              :saw-mandatory false})]
    (body-fn))
  nil)

(defn write
  "Write object subject to the current printer control variables. Keyword
  options override individual variables for this call:

    :stream         writer for output, or nil to return a string
    :pretty         if true, pretty print
    :base           base for printing integers
    :radix          if true, prepend a radix specifier
    :right-margin   the right-margin column
    :miser-width    the column to enter miser style
    :length         maximum elements to show
    :suppress-namespaces  if true, no namespaces on symbols

  Returns the string result when :stream is nil, otherwise nil."
  [object & kw-args]
  (let [opts (apply hash-map kw-args)
        stream (if (contains? opts :stream) (:stream opts) true)]
    (binding [*print-pretty* (if (contains? opts :pretty)
                               (:pretty opts) *print-pretty*)
              *print-base* (get opts :base *print-base*)
              *print-radix* (get opts :radix *print-radix*)
              *print-right-margin* (get opts :right-margin *print-right-margin*)
              *print-miser-width* (get opts :miser-width *print-miser-width*)
              *print-length* (get opts :length *print-length*)
              *print-suppress-namespaces* (get opts :suppress-namespaces
                                               *print-suppress-namespaces*)
              *print-pprint-dispatch* (get opts :dispatch *print-pprint-dispatch*)]
      (if (nil? stream)
        (let [sink (atom "")]
          (binding [*out* sink]
            (if *print-pretty*
              (run-engine 0 #(pp-write-top object))
              (pr object)))
          @sink)
        (if (= stream true)
          ;; Write to the ambient *out*.
          (do (if *print-pretty*
                (run-engine 0 #(pp-write-top object))
                (pr object))
              nil)
          ;; Write to an explicit stream.
          (binding [*out* stream]
            (if *print-pretty*
              (run-engine 0 #(pp-write-top object))
              (pr object))
            nil))))))

(defn- out-mid-line?
  "True when the current engine column is non-zero, or -- for output a
  custom dispatch wrote with raw print/pr that the engine did not track
  -- when the *out* atom's buffer does not already end in a newline."
  []
  (or (pos? (:col @*pp-state*))
      (and (atom? *out*)
           (let [s @*out*]
             (and (string? s) (pos? (count s))
                  (not (str/ends-with? s "\n")))))))

(defn- pprint-body
  "Run the engine for object on the ambient *out*, finishing with a
  newline when output is mid-line."
  [object]
  (run-engine 0
              (fn []
                (pp-write-top object)
                (when (out-mid-line?)
                  (pp-emit "\n")))))

(defn pprint
  "Pretty print object to the optional writer, defaulting to *out*.
  Always finishes the output with a newline."
  ([object]
   ;; Write to the ambient *out*; never re-bind it to itself, which
   ;; mino's string capture does not tolerate.
   (binding [*print-pretty* true]
     (pprint-body object))
   nil)
  ([object writer]
   (binding [*print-pretty* true
             *out* writer]
     (pprint-body object))
   nil))

(defmacro pp
  "Pretty print the last REPL result (*1)."
  []
  `(pprint *1))

;; ---------------------------------------------------------------------------
;; cl-format: a Common-Lisp-style format function
;;
;; A format string is parsed once into a directive vector (literals plus
;; directive nodes with their colon/at flags and numeric parameters), then
;; interpreted against an argument navigator. formatter compiles a string
;; into a reusable closure over that directive vector; cl-format does the
;; parse and the run in one call. Output collects into a column-tracking
;; buffer so the column-aware directives (~& fresh-line, ~t tabulation)
;; see exact columns; the finished buffer is sent to the destination.
;; ---------------------------------------------------------------------------

;; --- The output sink: a string buffer that knows its current column. ---

(defn- clf-sink
  "A fresh format sink: an atom holding {:buf parts :col column}."
  []
  (atom {:buf [] :col 0}))

(defn- clf-out
  "Append string s to the sink, advancing the column (a newline resets the
  column to the count of characters following it)."
  [sink s]
  (when (pos? (count s))
    (let [nl (str/last-index-of s "\n")
          col (if nl
                (- (count s) (long nl) 1)
                (+ (:col @sink) (count s)))]
      (swap! sink (fn [st] (assoc st :buf (conj (:buf st) s) :col col)))))
  nil)

(defn- clf-result
  "The accumulated string in the sink."
  [sink]
  (apply str (:buf @sink)))

;; --- The argument navigator: a flat seq with a movable position. ---

(defn- clf-nav
  "A navigator over the argument seq, position 0."
  [args]
  (atom {:seq (vec args) :pos 0}))

(defn- clf-next
  "Take the next argument, advancing the position. Throws when the
  arguments are exhausted."
  [nav]
  (let [{:keys [seq pos]} @nav]
    (when (>= pos (count seq))
      (throw "Not enough arguments for format directive"))
    (swap! nav assoc :pos (inc pos))
    (nth seq pos)))

(defn- clf-peek
  "The argument at the current position without advancing, or nil when
  exhausted."
  [nav]
  (let [{:keys [seq pos]} @nav]
    (when (< pos (count seq)) (nth seq pos))))

(defn- clf-prev
  "The argument just consumed (the one before the current position)."
  [nav]
  (let [{:keys [seq pos]} @nav]
    (when (pos? pos) (nth seq (dec pos)))))

(defn- clf-remaining
  "How many arguments are left from the current position."
  [nav]
  (let [{:keys [seq pos]} @nav]
    (max 0 (- (count seq) pos))))

(defn- clf-skip
  "Move the position by delta (forward when positive), clamped."
  [nav delta]
  (let [{:keys [seq pos]} @nav]
    (swap! nav assoc :pos (max 0 (min (count seq) (+ pos delta)))))
  nil)

(defn- clf-goto
  "Set the position to an absolute index, clamped."
  [nav idx]
  (let [{:keys [seq]} @nav]
    (swap! nav assoc :pos (max 0 (min (count seq) idx))))
  nil)

;; --- Parsing a format string into a directive tree. ---

(defn- clf-split-params
  "Split a directive's raw parameter substring on commas, preserving empty
  slots. Returns a vector of token strings; \"\" marks an omitted slot.
  (mino string indexing yields single-character strings, so the parser
  works in terms of those rather than character objects.)"
  [s]
  (loop [i 0 cur "" out []]
    (if (>= i (count s))
      (conj out cur)
      (let [c (subs s i (inc i))]
        (cond
          (= c "'") (recur (+ i 2) (str cur c (subs s (inc i) (+ i 2))) out)
          (= c ",") (recur (inc i) "" (conj out cur))
          :else (recur (inc i) (str cur c) out))))))

(defn- clf-token->param
  "Turn one parameter token into its value: integer, character (from a
  'c quote), :v, :remaining, or nil for an omitted slot."
  [tok]
  (cond
    (= tok "") nil
    (= (subs tok 0 1) "'") (subs tok 1 2)
    (or (= tok "v") (= tok "V")) :v
    (= tok "#") :remaining
    :else (Long/parseLong tok)))

(def ^:private clf-digits "0123456789")

(defn- clf-digit?
  "True when the single-character string c is a decimal digit."
  [c]
  (and c (some? (str/index-of clf-digits c))))

(defn- clf-read-directive
  "Read one directive starting just after a ~ at index i. Returns
  {:params [...] :colon b :at b :char c :end j} where :char is the
  directive letter as a single-character string and j is the index past
  it."
  [fmt i]
  (let [n (count fmt)
        ;; collect the raw parameter/flag prefix up to the directive letter
        param-end (loop [j i]
                    (let [c (when (< j n) (subs fmt j (inc j)))]
                      (cond
                        (nil? c) j
                        (= c "'") (recur (+ j 2))
                        (or (= c ",") (= c "-") (= c ":") (= c "@")
                            (= c "v") (= c "V") (= c "#") (clf-digit? c))
                        (recur (inc j))
                        :else j)))
        raw (subs fmt i param-end)
        colon (str/includes? raw ":")
        at (str/includes? raw "@")
        ;; strip the flag characters, leaving the comma-separated params
        param-str (str/replace (str/replace raw ":" "") "@" "")
        params (if (= param-str "")
                 []
                 (mapv clf-token->param (clf-split-params param-str)))
        dir-char (when (< param-end n) (subs fmt param-end (inc param-end)))]
    {:params params :colon colon :at at :char dir-char :end (inc param-end)}))

(declare clf-parse-nested)

;; Directives that open a nested construct and the letter that closes it.
(def ^:private clf-closers {"{" "}", "[" "]", "<" ">"})

(defn- clf-parse-seq
  "Parse the format string from index i, stopping at end-of-string or at a
  closing directive whose letter is in stop-set. Returns
  [tokens next-index closer] where tokens is a vector of literal strings
  and directive nodes, and closer is the directive map that stopped the
  scan (or nil at end of string)."
  [fmt i stop-set]
  (let [n (count fmt)]
    (loop [i i tokens [] lit ""]
      (if (>= i n)
        [(if (= lit "") tokens (conj tokens lit)) i nil]
        (let [c (subs fmt i (inc i))]
          (if (= c "~")
            (let [d (clf-read-directive fmt (inc i))
                  tokens (if (= lit "") tokens (conj tokens lit))]
              (cond
                (and stop-set (stop-set (:char d)))
                [tokens (:end d) d]

                (contains? clf-closers (:char d))
                (let [[node next-i] (clf-parse-nested fmt (:end d) d)]
                  (recur next-i (conj tokens node) ""))

                :else
                (recur (:end d) (conj tokens (assoc d :type :directive)) "")))
            (recur (inc i) tokens (str lit c))))))))

(defn- clf-parse-nested
  "Parse the body of a block-opening directive (~{ ~[ ~<), splitting it on
  ~; into clauses and consuming the matching closer. Returns
  [node next-index]."
  [fmt i open]
  (let [closer (clf-closers (:char open))
        stop #{closer ";"}]
    (loop [i i clauses [] seps []]
      (let [[toks next-i d] (clf-parse-seq fmt i stop)]
        (cond
          (nil? d)
          (throw (str "Format directive ~" (:char open)
                      " is missing its closing ~" closer))
          (= (:char d) ";")
          (recur next-i (conj clauses {:type :clause :toks toks})
                 (conj seps {:colon (:colon d) :at (:at d)}))
          :else
          [{:type :block
            :open open
            :close d
            :clauses (conj clauses {:type :clause :toks toks})
            :seps seps}
           next-i])))))

(defn- clf-parse
  "Parse a whole format string into a directive vector."
  [fmt]
  (first (clf-parse-seq fmt 0 nil)))

;; --- English cardinal / ordinal number names (for ~r without a radix). ---

(def ^:private clf-card-units
  ["zero" "one" "two" "three" "four" "five" "six" "seven" "eight" "nine"
   "ten" "eleven" "twelve" "thirteen" "fourteen" "fifteen" "sixteen"
   "seventeen" "eighteen" "nineteen"])

(def ^:private clf-card-tens
  ["" "" "twenty" "thirty" "forty" "fifty" "sixty" "seventy" "eighty"
   "ninety"])

(def ^:private clf-ord-units
  ["zeroth" "first" "second" "third" "fourth" "fifth" "sixth" "seventh"
   "eighth" "ninth" "tenth" "eleventh" "twelfth" "thirteenth" "fourteenth"
   "fifteenth" "sixteenth" "seventeenth" "eighteenth" "nineteenth"])

(def ^:private clf-ord-tens
  ["" "" "twentieth" "thirtieth" "fortieth" "fiftieth" "sixtieth"
   "seventieth" "eightieth" "ninetieth"])

(def ^:private clf-scale-names
  ["" " thousand" " million" " billion" " trillion" " quadrillion"
   " quintillion" " sextillion" " septillion" " octillion" " nonillion"
   " decillion"])

(defn- clf-card-under-1000
  "Cardinal english for 0 < num < 1000 (no leading 'zero')."
  [num]
  (let [hundreds (quot num 100)
        tens (rem num 100)]
    (str
      (when (pos? hundreds) (str (nth clf-card-units hundreds) " hundred"))
      (when (and (pos? hundreds) (pos? tens)) " ")
      (when (pos? tens)
        (if (< tens 20)
          (nth clf-card-units tens)
          (let [t (quot tens 10) u (rem tens 10)]
            (str (nth clf-card-tens t)
                 (when (pos? u) (str "-" (nth clf-card-units u))))))))))

(defn- clf-group-1000
  "Split a non-negative integer into base-1000 groups, least significant
  first."
  [n]
  (loop [n n acc []]
    (if (zero? n)
      (if (empty? acc) [0] acc)
      (recur (quot n 1000) (conj acc (rem n 1000))))))

(defn- clf-cardinal-english
  "Cardinal english string for an integer."
  [n]
  (if (zero? n)
    "zero"
    (let [neg (neg? n)
          groups (clf-group-1000 (if neg (- n) n))
          parts (keep-indexed
                  (fn [idx g]
                    (when (pos? g)
                      (str (clf-card-under-1000 g) (nth clf-scale-names idx))))
                  groups)
          body (apply str (interpose " " (reverse parts)))]
      (str (when neg "minus ") body))))

(defn- clf-ord-under-1000
  "Ordinal english for 0 < num < 1000."
  [num]
  (let [hundreds (quot num 100)
        tens (rem num 100)]
    (str
      (when (pos? hundreds) (str (nth clf-card-units hundreds) " hundred"))
      (when (and (pos? hundreds) (pos? tens)) " ")
      (cond
        (pos? tens)
        (if (< tens 20)
          (nth clf-ord-units tens)
          (let [t (quot tens 10) u (rem tens 10)]
            (if (zero? u)
              (nth clf-ord-tens t)
              (str (nth clf-card-tens t) "-" (nth clf-ord-units u)))))
        (pos? hundreds) "th"
        :else ""))))

(defn- clf-ordinal-english
  "Ordinal english string for an integer."
  [n]
  (if (zero? n)
    "zeroth"
    (let [neg (neg? n)
          mag (if neg (- n) n)
          groups (clf-group-1000 mag)
          last-group (first groups)
          high-groups (next groups)
          head (when (and high-groups (some pos? high-groups))
                 (apply str
                        (interpose " "
                          (reverse
                            (keep-indexed
                              (fn [idx g]
                                (when (pos? g)
                                  (str (clf-card-under-1000 g)
                                       (nth clf-scale-names (inc idx)))))
                              high-groups)))))
          tail (if (zero? last-group) "th" (clf-ord-under-1000 last-group))]
      (str (when neg "minus ")
           (if (and head (not= head ""))
             (if (zero? last-group) (str head "th") (str head ", " tail))
             tail)))))

;; --- Integer formatting (~d ~x ~o ~b and the radix form of ~r). ---

(defn- clf-comma-group
  "Group the digits of a digit string into runs of `interval`, joined with
  `commachar`."
  [digits interval commachar]
  (let [len (count digits)]
    (loop [i len out []]
      (if (<= i 0)
        (apply str (interpose (str commachar) out))
        (let [start (max 0 (- i interval))]
          (recur start (cons (subs digits start i) out)))))))

(defn- clf-pad
  "Pad string s on the left (or right when right? is true) with padchar to
  at least mincol characters."
  [s mincol padchar right?]
  (if (< (count s) mincol)
    (let [fill (apply str (repeat (- mincol (count s)) padchar))]
      (if right? (str s fill) (str fill s)))
    s))

(defn- clf-format-integer
  "Format an integer for ~d/~x/~o/~b. base is the radix; colon adds comma
  grouping; at forces a leading + on non-negatives. params are
  [mincol padchar commachar commainterval]."
  [arg base {:keys [colon at params]}]
  (let [mincol (or (nth params 0 nil) 0)
        padchar (or (nth params 1 nil) \space)
        commachar (or (nth params 2 nil) \,)
        interval (or (nth params 3 nil) 3)
        neg (neg? arg)
        mag (if neg (- arg) arg)
        digits (int->base mag base)
        grouped (if colon (clf-comma-group digits interval commachar) digits)
        signed (cond neg (str "-" grouped)
                     at (str "+" grouped)
                     :else grouped)]
    (clf-pad signed mincol padchar false)))

;; --- Float formatting (~f ~e ~$). ---

(defn- clf-format-fixed
  "~f: fixed-point. params are [w d k overflowchar padchar]; w and d are
  honored, missing d prints full precision. at forces a leading +."
  [arg {:keys [at params]}]
  (let [w (nth params 0 nil)
        d (nth params 1 nil)
        body (cond
               (and w d) (format (str "%" w "." d "f") (double arg))
               d (format (str "%." d "f") (double arg))
               w (clf-pad (str (double arg)) w \space false)
               :else (str (double arg)))]
    (if (and at (not (neg? arg)))
      (let [t (str/triml body)
            lead (subs body 0 (- (count body) (count t)))]
        (str lead "+" t))
      body)))

(defn- clf-format-exp
  "~e: exponential notation. params are [w d e k ...]; w and d honored."
  [arg {:keys [params]}]
  (let [w (nth params 0 nil)
        d (nth params 1 nil)
        body (if d (format (str "%." d "e") (double arg)) (format "%e" (double arg)))]
    (if w (clf-pad body w \space false) body)))

(defn- clf-format-dollar
  "~$: monetary. params are [d n w padchar]; d defaults to 2, n to 1."
  [arg {:keys [at params]}]
  (let [d (or (nth params 0 nil) 2)
        n (or (nth params 1 nil) 1)
        w (or (nth params 2 nil) 0)
        padchar (or (nth params 3 nil) \space)
        neg (neg? arg)
        body (format (str "%." d "f") (Math/abs (double arg)))
        dot (str/index-of body ".")
        int-part (subs body 0 dot)
        int-part (if (< (count int-part) n)
                   (str (apply str (repeat (- n (count int-part)) "0")) int-part)
                   int-part)
        body (str int-part (subs body dot))
        sign (cond neg "-" at "+" :else "")]
    (clf-pad (str sign body) w padchar false)))

;; --- ~a and ~s padding. ---

(defn- clf-format-ascii
  "~a/~s: render arg with print-func and pad to mincol. params are
  [mincol colinc minpad padchar]; at right-aligns."
  [arg print-func {:keys [at params]}]
  (let [mincol (or (nth params 0 nil) 0)
        colinc (or (nth params 1 nil) 1)
        minpad (or (nth params 2 nil) 0)
        padchar (or (nth params 3 nil) \space)
        base (print-func arg)
        min-width (+ (count base) minpad)
        width (if (>= min-width mincol)
                min-width
                (+ min-width
                   (* (inc (quot (- mincol min-width 1) colinc)) colinc)))
        fill (apply str (repeat (- width (count base)) padchar))]
    (if at (str fill base) (str base fill))))

;; --- The interpreter. ---

(declare clf-run clf-run-block clf-run-conditional clf-run-iteration)

(defn- clf-realize-params
  "Resolve :v (take from the arg stream) and :remaining (the remaining
  count) parameters, returning the resolved params vector."
  [params nav]
  (mapv (fn [p]
          (cond
            (= p :v) (clf-next nav)
            (= p :remaining) (clf-remaining nav)
            :else p))
        params))

(defn- clf-pluralize
  "~p plural ending. colon re-checks the previous arg as the count; at
  yields y/ies, else \"\"/\"s\"."
  [{:keys [colon at]} nav]
  (let [n (if colon (clf-prev nav) (clf-next nav))]
    (if at
      (if (= n 1) "y" "ies")
      (if (= n 1) "" "s"))))

(defn- clf-tabulate
  "~t: pad with spaces to reach column colnum (at least one space when
  already past it). params are [colnum colinc]."
  [sink {:keys [params]}]
  (let [colnum (or (nth params 0 nil) 1)
        colinc (or (nth params 1 nil) 1)
        current (:col @sink)
        n (cond
            (< current colnum) (- colnum current)
            (zero? colinc) 1
            :else (- colinc (rem (- current colnum) colinc)))]
    (clf-out sink (apply str (repeat (max 1 n) " ")))))

(defn- clf-run-conditional
  "~[...~]: numeric select; ~:[ boolean; ~@[ optional (arg-guarded)."
  [sink node nav]
  (let [open (:open node)
        clauses (:clauses node)
        seps (:seps node)]
    (cond
      ;; ~@[clause~]: run the single clause only when the next arg is truthy
      (:at open)
      (let [arg (clf-peek nav)]
        (if arg
          (clf-run sink (:toks (first clauses)) nav)
          (clf-next nav)))
      ;; ~:[false~;true~]: pick on truthiness of the next arg
      (:colon open)
      (let [arg (clf-next nav)
            clause (if arg (second clauses) (first clauses))]
        (when clause (clf-run sink (:toks clause) nav)))
      ;; ~[a~;b~:;else~]: numeric index, with ~:; as the else clause
      :else
      (let [sel (if (pos? (count (:params open)))
                  (first (clf-realize-params (:params open) nav))
                  (clf-next nav))
            has-else (and (seq seps) (:colon (last seps)))
            n-clauses (count clauses)
            idx (long sel)]
        (cond
          (and (>= idx 0) (< idx (if has-else (dec n-clauses) n-clauses)))
          (clf-run sink (:toks (nth clauses idx)) nav)
          has-else (clf-run sink (:toks (last clauses)) nav)
          :else nil)))))

(defn- clf-run-iteration
  "~{...~}: iterate a clause over an arglist. : iterates over sublists; @
  consumes the remaining main args; ~^ inside the clause exits early."
  [sink node nav]
  (let [open (:open node)
        close (:close node)
        clause-toks (:toks (first (:clauses node)))
        max-iter (when (pos? (count (:params open)))
                   (first (clf-realize-params (:params open) nav)))
        colon (:colon open)
        at (:at open)
        force-one (:colon close)]
    (cond
      ;; ~@{...~}: iterate over the remaining main arguments
      (and at (not colon))
      (loop [count 0]
        (if (or (and max-iter (>= count max-iter))
                (and (zero? (clf-remaining nav))
                     (or (not force-one) (pos? count))))
          nil
          (let [r (clf-run sink clause-toks nav)]
            (if (clf-exit? r) nil (recur (inc count))))))
      ;; ~:{...~}: each main arg is a sublist; iterate per sublist. A plain
      ;; ~^ ends only the current sub-iteration; ~:^ ends the whole loop.
      colon
      (let [lists (clf-next nav)]
        (loop [ls (seq lists) count 0]
          (if (or (and max-iter (>= count max-iter))
                  (and (nil? ls) (or (not force-one) (pos? count))))
            nil
            (let [r (clf-run sink clause-toks (clf-nav (first ls)))]
              (if (= r :clf-colon-up-exit)
                nil
                (recur (next ls) (inc count)))))))
      ;; ~{...~}: the next arg is the whole arglist for the iterations
      :else
      (let [sub-nav (clf-nav (clf-next nav))]
        (loop [count 0]
          (if (or (and max-iter (>= count max-iter))
                  (and (zero? (clf-remaining sub-nav))
                       (or (not force-one) (pos? count))))
            nil
            (let [r (clf-run sink clause-toks sub-nav)]
              (if (clf-exit? r) nil (recur (inc count))))))))))

(defn- clf-run-block
  "Execute one block directive (~{ ~[). Dispatches on the opener's letter."
  [sink node nav]
  (let [ch (:char (:open node))]
    (cond
      (= ch "[") (clf-run-conditional sink node nav)
      (= ch "{") (clf-run-iteration sink node nav)
      :else (throw (str "Format directive ~" ch " is not supported")))))

(defn- clf-run-directive
  "Execute a single non-block directive node. Returns :clf-up-exit to
  signal a ~^ early termination, otherwise nil."
  [sink node nav]
  (let [flags (assoc node :params (clf-realize-params (:params node) nav))
        letter (str/lower-case (:char node))]
    (case letter
      "a" (clf-out sink (clf-format-ascii (clf-next nav) print-str flags))
      "s" (clf-out sink (clf-format-ascii (clf-next nav) pr-str flags))
      "d" (clf-out sink (clf-format-integer (clf-next nav) 10 flags))
      "x" (clf-out sink (clf-format-integer (clf-next nav) 16 flags))
      "o" (clf-out sink (clf-format-integer (clf-next nav) 8 flags))
      "b" (clf-out sink (clf-format-integer (clf-next nav) 2 flags))
      "r" (let [params (:params flags)]
            (if (pos? (count params))
              (clf-out sink (clf-format-integer (clf-next nav)
                                                (long (first params))
                                                (assoc flags :params
                                                       (vec (rest params)))))
              (clf-out sink (if (:colon flags)
                              (clf-ordinal-english (clf-next nav))
                              (clf-cardinal-english (clf-next nav))))))
      "f" (clf-out sink (clf-format-fixed (clf-next nav) flags))
      "e" (clf-out sink (clf-format-exp (clf-next nav) flags))
      "$" (clf-out sink (clf-format-dollar (clf-next nav) flags))
      "c" (clf-out sink (str (clf-next nav)))
      "p" (clf-out sink (clf-pluralize flags nav))
      "%" (clf-out sink (apply str (repeat (or (nth (:params flags) 0 nil) 1)
                                           "\n")))
      "&" (do (when (pos? (:col @sink)) (clf-out sink "\n")) nil)
      "~" (clf-out sink (apply str (repeat (or (nth (:params flags) 0 nil) 1)
                                           "~")))
      "t" (clf-tabulate sink flags)
      "*" (cond
            (:at flags) (clf-goto nav (or (nth (:params flags) 0 nil) 0))
            (:colon flags) (clf-skip nav (- (or (nth (:params flags) 0 nil) 1)))
            :else (clf-skip nav (or (nth (:params flags) 0 nil) 1)))
      "?" (let [sub-fmt (clf-next nav)
                sub-args (clf-next nav)]
            (clf-run sink (clf-parse sub-fmt) (clf-nav sub-args)))
      ;; ~^ aborts when no arguments remain. Plain ~^ ends the current
      ;; clause (and, in ~:{, only the current sub-iteration); ~:^ ends the
      ;; whole iteration. ~Nl^ aborts when the first parameter is zero.
      "^" (let [params (:params flags)
                fire (if (pos? (count params))
                       (= 0 (first params))
                       (zero? (clf-remaining nav)))]
            (when fire
              (if (:colon flags) :clf-colon-up-exit :clf-up-exit)))
      ;; an unrecognized directive emits nothing
      nil)))

(defn- clf-exit?
  "True when r is one of the ~^ early-exit tokens."
  [r]
  (or (= r :clf-up-exit) (= r :clf-colon-up-exit)))

(defn- clf-run
  "Walk a token vector, emitting to the sink. Returns :clf-up-exit or
  :clf-colon-up-exit when a ~^ requested an early exit, otherwise nil."
  [sink tokens nav]
  (loop [ts (seq tokens)]
    (if (nil? ts)
      nil
      (let [t (first ts)]
        (cond
          (string? t) (do (clf-out sink t) (recur (next ts)))
          (= (:type t) :block)
          (let [r (clf-run-block sink t nav)]
            (if (clf-exit? r) r (recur (next ts))))
          :else
          (let [r (clf-run-directive sink t nav)]
            (if (clf-exit? r) r (recur (next ts)))))))))

(defn- clf-execute
  "Run a parsed directive vector against args, returning the result
  string."
  [tokens args]
  (let [sink (clf-sink)]
    (clf-run sink tokens (clf-nav args))
    (clf-result sink)))

(defn- clf-send
  "Send the produced string to the cl-format destination: nil returns the
  string; true writes to *out*; any other value is treated as a writer and
  written to."
  [writer s]
  (cond
    (nil? writer) s
    (= writer true) (do (print s) nil)
    (atom? writer) (do (swap! writer str s) nil)
    :else (binding [*out* writer] (print s) nil)))

(defn cl-format
  "An implementation of a Common-Lisp-compatible format function.
  cl-format formats its arguments to an output destination or string based
  on the format control string given.

  writer is true to output to *out*, nil to return the formatted string, or
  a writer to write to it. format-in is the format control string (or a
  compiled formatter); the remaining arguments are the data to format.

  If writer is nil, cl-format returns the formatted result string;
  otherwise it returns nil."
  [writer format-in & args]
  (let [tokens (if (string? format-in) (clf-parse format-in) format-in)]
    (clf-send writer (clf-execute tokens args))))

(defn formatter
  "Makes a function which can directly run format-in. The returned function
  takes a writer (true, nil or a writer) followed by the format arguments,
  and returns nil or the formatted string the same way cl-format does."
  [format-in]
  (let [tokens (clf-parse format-in)]
    (fn [writer & args]
      (clf-send writer (clf-execute tokens args)))))

(defn formatter-out
  "Makes a function which can directly run format-in, writing to *out*. The
  returned function takes the format arguments and always writes to *out*,
  returning nil. Designed for use inside dispatch functions."
  [format-in]
  (let [tokens (clf-parse format-in)]
    (fn [& args]
      (clf-send true (clf-execute tokens args)))))

;; ---------------------------------------------------------------------------
;; code-dispatch: a dispatch function that pretty-prints Clojure code
;;
;; Like simple-dispatch, code-dispatch is a function over the structural
;; type of the object, selected only through with-pprint-dispatch or
;; set-pprint-dispatch (it registers nothing globally). Recognized heads
;; (defn, def, let, doseq, when, if, fn, ...) indent their body two columns
;; under the head, with let-style bindings paired; every other list falls
;; back to the simple-list layout, and non-lists defer to simple-dispatch.
;; ---------------------------------------------------------------------------

(declare code-dispatch)

(defn- pp-code-simple-list
  "Lay out a list as code: one element per broken line, the body indented
  one column past the open paren (block-relative indent 1)."
  [alis]
  (pprint-logical-block :prefix "(" :suffix ")"
    (pprint-indent :block 1)
    (print-length-loop [s (seq alis)]
      (when s
        (code-dispatch (first s))
        (when (next s)
          (pp-emit " ")
          (pprint-newline :linear)
          (recur (next s)))))))

(defn- pp-code-bindings
  "Lay out a let/loop/binding binding vector, keeping each name/value pair
  together and breaking between pairs."
  [binding-vec]
  (pprint-logical-block :prefix "[" :suffix "]"
    (print-length-loop [b (seq binding-vec)]
      (when b
        (pprint-logical-block
          (code-dispatch (first b))
          (when (next b)
            (pp-emit " ")
            (pprint-newline :miser)
            (code-dispatch (second b))))
        (when (next (rest b))
          (pp-emit " ")
          (pprint-newline :linear)
          (recur (next (rest b))))))))

(defn- pp-code-let
  "Lay out a let-like form: head, the binding vector, then body forms each
  on a broken line indented under the head."
  [alis]
  (if (and (next alis) (vector? (second alis)))
    (pprint-logical-block :prefix "(" :suffix ")"
      (pprint-indent :block 2)
      (code-dispatch (first alis))
      (pp-emit " ")
      (pp-code-bindings (second alis))
      (print-length-loop [s (seq (next (rest alis)))]
        (when s
          (pp-emit " ")
          (pprint-newline :linear)
          (code-dispatch (first s))
          (recur (next s)))))
    (pp-code-simple-list alis)))

(defn- pp-code-defn
  "Lay out a defn/defn-/defmacro/fn form: the head and name on the first
  line, then params and body forms each indented two columns under the
  head."
  [alis]
  (if (next alis)
    (pprint-logical-block :prefix "(" :suffix ")"
      (pprint-indent :block 2)
      (code-dispatch (first alis))
      (pp-emit " ")
      (code-dispatch (second alis))
      (print-length-loop [s (seq (next (rest alis)))]
        (when s
          (pp-emit " ")
          (pprint-newline :linear)
          (code-dispatch (first s))
          (recur (next s)))))
    (pp-code-simple-list alis)))

(defn- pp-code-if
  "Lay out an if/when-like form: head and test on the first line, the
  remaining forms broken and indented under the head."
  [alis]
  (pprint-logical-block :prefix "(" :suffix ")"
    (pprint-indent :block 2)
    (code-dispatch (first alis))
    (when (next alis)
      (pp-emit " ")
      (code-dispatch (second alis))
      (print-length-loop [s (seq (next (rest alis)))]
        (when s
          (pp-emit " ")
          (pprint-newline :linear)
          (code-dispatch (first s))
          (recur (next s)))))))

(defn- code-table-fn
  "The code layout function for a list whose head symbol selects special
  indentation, or nil when no special form applies."
  [head]
  (when (symbol? head)
    (let [n (name head)]
      (cond
        (#{"defn" "defn-" "defmacro" "fn" "def" "defonce"} n) pp-code-defn
        (#{"let" "loop" "binding" "doseq" "dotimes" "when-let" "if-let"
           "with-open" "with-local-vars" "when-first"} n) pp-code-let
        (#{"if" "if-not" "when" "when-not"} n) pp-code-if
        :else nil))))

(defn- pp-code-list
  "Dispatch a list as code: a recognized head gets its special layout, any
  other list gets the simple code-list layout."
  [alis]
  (if-let [f (and (seq alis) (code-table-fn (first alis)))]
    (f alis)
    (pp-code-simple-list alis)))

(defn code-dispatch
  "A pretty-print dispatch function for Clojure code. Select it with
  with-pprint-dispatch or set-pprint-dispatch; it registers nothing
  globally. Lists are laid out as code (recognized special forms indent
  their body under the head, let-style bindings stay paired); everything
  else falls back to the simple-dispatch layout."
  [object]
  (cond
    (nil? object) (pp-emit "nil")
    (seq? object) (pp-code-list object)
    (list? object) (pp-code-list object)
    (map? object) (pp-map object)
    (vector? object) (pp-coll "[" "]" object)
    (set? object) (pp-coll "#{" "}" object)
    :else (pp-pr object)))

(defn- pt-pad
  "Right-justify s within width columns by prepending spaces."
  [s width]
  (let [pad (- width (count s))]
    (if (pos? pad)
      (str (apply str (repeat pad " ")) s)
      s)))

(defn print-table
  "Prints a collection of maps as a textual table. Prints the heading row
  ks, a separator row, and one row per map, each cell drawn from the
  corresponding key. ks selects and orders the columns; it defaults to
  the keys of the first row. Columns are padded to the widest cell and
  delimited with pipes."
  ([rows] (print-table (keys (first rows)) rows))
  ([ks rows]
   (when (seq rows)
     (let [widths  (map (fn [k]
                          (apply max (count (str k))
                                 (map (fn [row] (count (str (get row k)))) rows)))
                        ks)
           spacers (map (fn [w] (apply str (repeat w "-"))) widths)
           fmt-row (fn [leader divider trailer row]
                     (str leader
                          (str/join divider
                                    (map (fn [k w] (pt-pad (str (get row k)) w))
                                         ks widths))
                          trailer))]
       (println)
       (println (fmt-row "| " " | " " |" (zipmap ks ks)))
       (println (fmt-row "|-" "-+-" "-|" (zipmap ks spacers)))
       (doseq [row rows]
         (println (fmt-row "| " " | " " |" row)))))))
