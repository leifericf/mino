(ns mino.html.select
  "The hickory.select subset over hickory node maps (ADR 28; the
  html-xml campaign select contract, research section 6).

  (require '[mino.html :as html] '[mino.html.select :as sel])
  (def page (html/parse \"<body><ul id='nav'><li><a href='x'>one</a></li></ul></body>\"))
  (sel/select (sel/tag :a) page)
  => [{:type :element :tag :a :attrs {:href \"x\"} ...}]
  (map sel/text (sel/select (sel/child (sel/tag :li) (sel/tag :a)) page))
  => [\"one\"]

  A selector is a fn of one clojure.zip loc returning the loc on
  match and nil otherwise. select and select-locs ride xml-zip locs
  (xml-zip touches only :content, so the shared JVM clojure.xml
  element shape zips and selects identically for the :type-blind
  selectors). select returns the matching nodes as a vector;
  select-locs returns the live locs.

  Surface (v1): predicates tag, id, class, attr, any; combinators
  and, or, not, child, descendant; positions first-child,
  last-child, nth-child; entry points select, select-locs. Call
  shapes and observable semantics mirror hickory.select 0.7.7:
  name and value comparisons are case-insensitive, nth-child is
  1-based and counts element siblings only (leading or trailing
  text never disqualifies first-child and last-child), and
  (nth-child n c) is the stride form with hickory's exact an+b
  arithmetic (match distance n*k + c for non-negative k only).

  Divergences from hickory, each pinned by tests/html_select_test:
  sequential input (a parse-fragment vector) selects per top-level
  node with results concatenated in order; attr's predicate arity
  takes a fn, so regex matching rides (sel/attr :href #(re-find
  #\"^https:\" %)) because patterns are not callable in mino; and
  text is the local scraping extractor (hickory.select ships no
  text fn in 0.7.7). nth-of-type, n-moves-until, precede, follow,
  and any CSS string grammar are v1 exclusions."
  (:require [clojure.zip :as zip]
            [clojure.string :as str])
  (:refer-clojure :exclude [and or not class]))

;;; ---- element-only selectors (a walk fast path) ----

;; Base predicates and position selectors can only ever match
;; element nodes: tag/attr/id/class read :tag/:attrs (absent on
;; strings) and any/first-child/last-child/nth-child check
;; :type/parentage. select-locs reads this marker once and, when
;; set, skips string children in the walk -- observably identical
;; (those selectors return nil for strings either way), and it
;; saves the loc construction for the majority of nodes in a real
;; page. Combinators propagate the marker conservatively: and is
;; element-only when any arm is, or when all arms are, not never,
;; child/descendant when the final (loc-matching) selector is.
;; Custom selectors carry no marker and always get the full walk.
(def ^:private sel-elements-only {:mino.sel/elements-only true})

(defn- sel-elements-only?
  [selector]
  (if (get (meta selector) :mino.sel/elements-only)
    true
    false))

(defn- sel-mark
  [f marked?]
  (if marked?
    (with-meta f sel-elements-only)
    f))

;;; ---- predicates ----

(defn tag
  "Returns a selector that matches elements whose tag is tag. The
  tag argument can be a String or Named; the comparison is
  case-insensitive (hickory.select/tag)."
  [tag]
  (let [want (str/lower-case (name tag))]
    (sel-mark
      (fn [loc]
        (let [node (zip/node loc)]
          (if (map? node)
            (let [got (:tag node)]
              (if (nil? got)
                nil
                (if (= (str/lower-case (name got)) want)
                  loc))))))
      true)))

(defn attr
  "Returns a selector that matches nodes carrying the attribute
  attr-name, optionally with a value satisfying predicate. With one
  argument the attribute need only be present with any value; with
  two, predicate is called on the attribute's value and only when
  the attribute is present. The attribute name comparison is
  case-insensitive; the value passes to predicate as-is
  (hickory.select/attr). Patterns are not callable in mino, so the
  regex idiom is (sel/attr :href #(re-find #\"^https:\" %))."
  ([attr-name]
   (attr attr-name (fn [_] true)))
  ([attr-name predicate]
   (let [k (keyword (str/lower-case (name attr-name)))]
     (sel-mark
       (fn [loc]
         (let [attrs (:attrs (zip/node loc))]
           (if (contains? attrs k)
             (if (predicate (get attrs k))
               loc))))
       true))))

(defn id
  "Returns a selector that matches elements whose id attribute is
  id. The id argument can be a String or Named; the comparison is
  case-insensitive (hickory.select/id)."
  [id]
  (attr :id #(= (str/lower-case %) (str/lower-case (name id)))))

(def ^:private sel-class-split #"\s+")

(defn class
  "Returns a selector that matches elements whose class attribute
  contains class-name as one whitespace-separated token. The
  class-name argument can be a String or Named; the comparison is
  case-insensitive (hickory.select/class)."
  [class-name]
  (let [want (str/lower-case (name class-name))]
    (attr :class
          (fn [v]
            (if (some #(= % want)
                      (str/split (str/lower-case v) sel-class-split))
              true
              false)))))

(def any
  "The selector itself (it takes no arguments): matches every
  element node, corresponding to the CSS * selector. Text strings,
  comments, the document-type, and the document node do not match."
  (with-meta
    (fn [loc]
      (if (= :element (:type (zip/node loc)))
        loc))
    sel-elements-only))

;;; ---- combinators ----

(defn and
  "Takes any number of selectors and returns a selector that is
  true when all of the argument selectors are true
  (hickory.select/and)."
  [& selectors]
  (sel-mark
    (fn [loc]
      (if (every? (fn [s] (s loc)) selectors)
        loc))
    (boolean (some sel-elements-only? selectors))))

(defn or
  "Takes any number of selectors and returns a selector that is
  true when any of the argument selectors is true (hickory.select/
  or)."
  [& selectors]
  (sel-mark
    (fn [loc]
      (if (some (fn [s] (s loc)) selectors)
        loc))
    (boolean (every? sel-elements-only? selectors))))

(defn not
  "Takes a selector and returns a selector that is true when the
  underlying selector is false. This is hickory's raw not, not
  el-not: text nodes and comments match (not (tag :p))."
  [selector]
  (fn [loc]
    (if (clojure.core/not (selector loc))
      loc)))

(defn- sel-ordered-adjacent
  "loc must satisfy each selector in turn with one move-fn step
  between; a single failure fails the chain (hickory.select/
  ordered-adjacent over a reversed selector vector, index walk)."
  [move-fn rev-selectors]
  (let [n (count rev-selectors)]
    (fn [loc]
      (loop [curr loc idx 0]
        (if (>= idx n)
          loc
          (if (nil? curr)
            nil
            (let [next-loc ((nth rev-selectors idx) curr)]
              (if next-loc
                (recur (move-fn next-loc) (inc idx))))))))))

(defn- sel-ordered
  "Like sel-ordered-adjacent, but a failed selector does not stop
  the walk: the move continues and the same selector retries
  against later nodes (hickory.select/ordered)."
  [move-fn rev-selectors]
  (let [n (count rev-selectors)
        first-sel (nth rev-selectors 0)]
    (fn [loc]
      (if (first-sel loc)
        (loop [curr (move-fn loc) idx 1]
          (if (>= idx n)
            loc
            (if (nil? curr)
              nil
              (if ((nth rev-selectors idx) curr)
                (recur (move-fn curr) (inc idx))
                (recur (move-fn curr) idx)))))
        nil))))

(defn child
  "Takes any number of selectors and returns a selector that
  matches when the loc is at the end of a chain of direct child
  relationships: the last selector matches the loc itself and each
  earlier selector its parent (hickory.select/child).

  (sel/child (sel/tag :div) (sel/class :foo) (sel/attr :disabled))
  selects the input in
  <div><span class=\"foo\"><input disabled></input></span></div>
  but not in
  <div><span class=\"foo\"><b><input disabled></input></b></span></div>"
  [& selectors]
  (let [rev (vec (reverse selectors))]
    (sel-mark (sel-ordered-adjacent zip/up rev)
              (if (pos? (count rev))
                (sel-elements-only? (nth rev 0))
                false))))

(defn descendant
  "Takes any number of selectors and returns a selector that
  matches when the loc is at the end of a chain of descendant
  relationships: the last selector matches the loc, the earlier
  ones ancestors in the order given, skipping non-matching levels
  (hickory.select/descendant).

  (sel/descendant (sel/tag :div) (sel/class :foo)
                  (sel/attr :disabled))
  selects the input in both trees of the child docstring above."
  [& selectors]
  (let [rev (vec (reverse selectors))]
    (sel-mark (sel-ordered zip/up rev)
              (if (pos? (count rev))
                (sel-elements-only? (nth rev 0))
                false))))

;;; ---- position selectors ----

(defn- sel-element-node?
  [x]
  (if (map? x)
    (= :element (:type x))
    false))

(defn- sel-element-child-loc?
  "True when the loc's node is an element whose parent node is also
  an element (hickory.select/element-child as a boolean)."
  [loc]
  (if (sel-element-node? (zip/node loc))
    (let [up (zip/up loc)]
      (if up
        (sel-element-node? (zip/node up))
        false))
    false))

(defn- sel-nth-element
  "The n-moves-until core of hickory's nth-child family over element
  siblings: distance is the 1-based index of loc among its parent's
  element children counted from side-fn (zip/lefts or zip/rights);
  match when n is 0 and distance equals c, or when distance equals
  n*k + c for some non-negative integer k (hickory/CSS an+b, so the
  stride reaches only positions at or beyond the first match)."
  [side-fn n c]
  (fn [loc]
    (if (sel-element-child-loc? loc)
      (let [sides (side-fn loc)
            distance (if (nil? sides)
                       1
                       (inc (count (filter sel-element-node? sides))))]
        (if (zero? n)
          (if (= distance c)
            loc)
          (let [d (- distance c)]
            (if (and (zero? (rem d n)) (>= (quot d n) 0))
              loc)))))))

(defn nth-child
  "Returns a selector matching the nth element child of an element
  parent; the first child is 1 (hickory.select/nth-child). Text and
  comment siblings are not counted. c may be :odd or :even; the
  two-argument form is the stride arithmetic, hickory-exact."
  ([c]
   (cond (= :odd c) (nth-child 2 1)
         (= :even c) (nth-child 2 0)
         :else (nth-child 0 c)))
  ([n c]
   (sel-mark (sel-nth-element zip/lefts n c) true)))

(def first-child
  "The selector itself (it takes no arguments): matches an element
  that is the first element child of its element parent. Leading
  text does not disqualify; hickory counts elements, unlike CSS
  :first-child."
  (with-meta
    (fn [loc]
      (if (sel-element-child-loc? loc)
        (let [sides (zip/lefts loc)]
          (if (if (nil? sides)
                true
                (clojure.core/not (some sel-element-node? sides)))
            loc))))
    sel-elements-only))

(def last-child
  "The selector itself (it takes no arguments): matches an element
  that is the last element child of its element parent. Trailing
  text does not disqualify; hickory counts elements, unlike CSS
  :last-child."
  (with-meta
    (fn [loc]
      (if (sel-element-child-loc? loc)
        (let [sides (zip/rights loc)]
          (if (if (nil? sides)
                true
                (clojure.core/not (some sel-element-node? sides)))
            loc))))
    sel-elements-only))

;;; ---- entry points ----

(defn- sel-push-children
  "Pushes locs for each child of a node onto the walk stack in
  reverse order (LIFO pop yields document order). Sibling spans are
  subvec views; :r may be a subvec instead of zip's seq (seq-based
  readers like zip/rights and zip/right coerce with seq, verified
  by the loc pins). elements-only skips string children outright:
  the selector is one that can never match them."
  [st c p pnodes cn zmeta elements-only]
  (loop [i (dec cn)
         st st]
    (if (>= i 0)
      (let [ch (nth c i)]
        (recur (dec i)
               (if (if elements-only (map? ch) true)
                 (conj st
                       (with-meta
                         [ch
                          {:l (subvec c 0 i)
                           :r (if (< (inc i) cn)
                                (subvec c (inc i) cn)
                                nil)
                           :pnodes pnodes
                           :ppath p
                           :changed? false}]
                         zmeta))
                  st)))
      st)))

(defn- sel-locs-one
  "select-locs over one tree: a pre-order depth-first walk that
  builds each clojure.zip loc directly -- the node, the path map
  (l/r sibling spans, pnodes, ppath), and the zipper fn meta
  captured once from xml-zip -- instead of stepping zip/next. The
  visited sequence is the zip/next walk exactly (root loc first,
  children in order, strings and empty-content nodes as leaves);
  returned locs are ordinary xml-zip locs (up, edit, and root all
  work; the pins cover this). The html pipeline gate rides this
  walk: the direct build skips zip/next's branch/down/right/up
  probing per step, which the interpreter charges for at page
  scale. An elements-only selector (the marker on this namespace's
  own predicates, propagated conservatively through the
  combinators) additionally skips string children, which such
  selectors can never match."
  [selector tree]
  (let [zmeta (meta (zip/xml-zip tree))
        elements-only (sel-elements-only? selector)]
    (loop [st [(with-meta [tree nil] zmeta)]
           acc (transient [])]
      (if (pos? (count st))
        (let [loc (peek st)
              node (first loc)
              st (pop st)
              acc (if (selector loc)
                    (conj! acc loc)
                    acc)]
          (if (map? node)
            (let [c (:content node)]
              (if c
                (let [cv (if (vector? c) c (vec c))
                      cn (count cv)]
                  (if (pos? cn)
                    (let [p (second loc)]
                      (recur (sel-push-children
                               st cv p
                               (if p (conj (:pnodes p) node) [node])
                               cn zmeta elements-only)
                             acc))
                    (recur st acc)))
                (recur st acc)))
            (recur st acc)))
        (persistent! acc)))))

(defn select-locs
  "Given a selector and a hickory node, returns a vector of every
  matching zipper loc in document order; the root loc itself
  participates. sequential? input (a parse-fragment vector) selects
  per top-level node with the results concatenated in order, each
  fragment root its own zipper. Returned locs are live zipper locs:
  an edit through one surfaces at that loc's zip/root, and locs
  kept across a reshape go stale (hickory's caveat)."
  [selector tree]
  (if (sequential? tree)
    (into [] (mapcat #(sel-locs-one selector %)) tree)
    (sel-locs-one selector tree)))

(defn select
  "Given a selector and a hickory node (or a sequential collection
  of nodes, as from html/parse-fragment), returns a vector of every
  matching node in document order (hickory.select/select).

  (map sel/text (sel/select (sel/descendant (sel/tag :body)
                                            (sel/tag :a))
                            (html/parse \"<body><a href='x'>hi</a></body>\")))
  => [\"hi\"]"
  [selector tree]
  (mapv zip/node (select-locs selector tree)))

;;; ---- text extraction ----

(defn text
  "Concatenates every string descendant of node in document order:
  the scraping companion to select (hickory.select ships no text fn
  in 0.7.7; this is the local equivalent). Whitespace passes
  through verbatim, comment and document-type payloads are data and
  are skipped, and node may be a node map, a bare string, or a
  sequential collection of nodes.

  (map sel/text (sel/select (sel/tag :p) (html/parse doc)))"
  [node]
  (loop [st (if (sequential? node)
              (vec (reverse node))
              [node])
         acc (transient [])]
    (if (pos? (count st))
      (let [x (peek st)]
        (cond
          (string? x)
          (recur (pop st) (conj! acc x))

          (map? x)
          (let [typ (:type x)]
            (if (if (= :comment typ)
                  true
                  (= :document-type typ))
              (recur (pop st) acc)
              (let [c (:content x)]
                (if (vector? c)
                  (recur (into (pop st) (rseq c)) acc)
                  (if c
                    (recur (into (pop st) (reverse c)) acc)
                    (recur (pop st) acc))))))

          (sequential? x)
          (recur (into (pop st) (reverse x)) acc)

          :else (recur (pop st) acc)))
      (str/join (persistent! acc)))))
