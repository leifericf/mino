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
  "Return a pretty-printing writer wrapping base-writer. In mino the
  engine wraps *out* directly, so this returns a non-nil token usable as
  *out* for write-out."
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
;; cl-format placeholder (a later wave provides the implementation)
;; ---------------------------------------------------------------------------

(defn cl-format
  "A Common-Lisp-style format function. Not yet implemented in mino."
  [writer fmt & args]
  (throw (str "cl-format is not implemented in mino")))

(defn print-table
  "Print a collection of maps as a text table. ks selects and orders the
  columns; it defaults to the keys of the first row."
  ([rows] (print-table (keys (first rows)) rows))
  ([ks rows]
   (doseq [k ks] (print (str k "\t")))
   (println)
   (doseq [row rows]
     (doseq [k ks] (print (str (get row k) "\t")))
     (println))))
