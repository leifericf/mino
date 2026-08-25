(require "tests/test")
(require '[clojure.string :as str])
(require '[clojure.edn :as edn])
(require '[mino.html :as html])
(require '[mino.html.select :as sel])
(require '[clojure.zip :as zip])

;; mino.html.select pins (html-xml campaign p4t1). The contract of
;; record is research section 6's select row: the hickory.select
;; subset over clojure.zip locs -- predicates tag/id/class/attr/any,
;; combinators and/or/not, child/descendant, first-child/last-child/
;; nth-child, entry points select and select-locs -- riding xml-zip
;; over the hickory node maps (xml-zip touches only :content, p3t3
;; pins). Call shapes and observable semantics mirror hickory.select
;; 0.7.7 exactly (source fetched and read this campaign); the
;; documented mino divergences each carry a named pin below:
;; sequential (fragment-vector) input selects per top-level node,
;; and attr's predicate arity takes a fn because patterns are not
;; IFn in mino. nth-of-type, n-moves-until, precede, follow, and any
;; CSS string grammar are v1 exclusions pinned by the surface-cap
;; test. The scraping idiom rides sel/text (hickory.select ships no
;; text fn; the local equivalent).

;;; ---- helpers ----

(defn- html-sel-el
  ([tag] {:type :element :tag tag :attrs {} :content []})
  ([tag content] {:type :element :tag tag :attrs {} :content content})
  ([tag attrs content] {:type :element :tag tag :attrs attrs
                        :content content}))

(defn- html-sel-tags
  "The :tags of selected nodes, strings rendered as [:text s]."
  [nodes]
  (mapv #(if (map? %) (:tag %) [:text %]) nodes))

(defn- html-sel-attr-of
  [k nodes]
  (mapv #(get-in % [:attrs k]) nodes))

(defn- html-sel-count-elements
  "Independent structural element count (explicit stack, no zipper,
  no selectors): the oracle for cross-checking select."
  [roots]
  (loop [stack (vec roots) acc 0]
    (if (pos? (count stack))
      (let [n (peek stack)]
        (if (map? n)
          (recur (into (pop stack) (or (:content n) []))
                 (if (= :element (:type n)) (inc acc) acc))
          (recur (pop stack) acc)))
      acc)))

(defn- html-sel-count-tag
  "Independent structural count of elements whose tag is tag."
  [tag roots]
  (loop [stack (vec roots) acc 0]
    (if (pos? (count stack))
      (let [n (peek stack)]
        (if (map? n)
          (recur (into (pop stack) (or (:content n) []))
                 (if (= tag (:tag n)) (inc acc) acc))
          (recur (pop stack) acc)))
      acc)))

(def ^:private html-sel-page-src
  (str/join
    ["<!DOCTYPE html>"
     "<html lang=\"en\"><head><meta charset=\"utf-8\">"
     "<title>Demo</title></head>"
     "<body id=\"main\" class=\"page demo\">"
     "<h1>Links</h1>"
     "<ul id=\"nav\">"
     "<li class=\"item first\"><a href=\"https://a.example/1\">one</a></li>"
     "<li class=\"item\"><a href=\"https://a.example/2\" target=\"_blank\">two</a></li>"
     "<li class=\"item last\"><a href=\"http://b.example/3\">three</a></li>"
     "</ul>"
     "<div class=\"content\"><p>para <b>bold</b> tail</p>"
     "<p class=\"note\">second</p>"
     "<table id=\"t1\"><tr><td>c1</td><td>c2</td></tr></table></div>"
     "</body></html>"]))

(def ^:private html-sel-page (html/parse html-sel-page-src))

;;; ---- predicates ----

(deftest html-select-tag-predicate
  (is (= [:a :a :a] (html-sel-tags (sel/select (sel/tag :a) html-sel-page))))
  (is (= [:table] (html-sel-tags (sel/select (sel/tag :table) html-sel-page))))
  (is (= [] (html-sel-tags (sel/select (sel/tag :section) html-sel-page))))
  ;; text nodes, comments, doctype never match tag
  (is (= [:p :p]
         (html-sel-tags (sel/select (sel/tag :p)
                                    (html/parse-fragment
                                      "<!--c--><p>a</p>txt<p>b</p>")))))
  ;; select returns a vector (hickory contract: "returns a vector")
  (is (vector? (sel/select (sel/tag :a) html-sel-page))))

(deftest html-select-tag-case-insensitive
  ;; hickory: "The tag name comparison is done case-insensitively";
  ;; String or Named argument
  (is (= 3 (count (sel/select (sel/tag "A") html-sel-page))))
  (is (= 3 (count (sel/select (sel/tag :A) html-sel-page))))
  (is (= 1 (count (sel/select (sel/tag "BODY") html-sel-page)))))

(deftest html-select-id-predicate
  (is (= [:ul] (html-sel-tags (sel/select (sel/id "nav") html-sel-page))))
  (is (= [:body] (html-sel-tags (sel/select (sel/id :main) html-sel-page))))
  ;; value comparison is case-insensitive too (rides attr)
  (is (= [:ul] (html-sel-tags (sel/select (sel/id "NAV") html-sel-page))))
  (is (= [] (html-sel-tags (sel/select (sel/id "nope") html-sel-page))))
  ;; an element whose id is a prefix of the ask does not match
  (is (= [] (html-sel-tags (sel/select (sel/id "na") html-sel-page)))))

(deftest html-select-class-predicate
  (is (= [:li :li :li]
         (html-sel-tags (sel/select (sel/class :item) html-sel-page))))
  (is (= [:li] (html-sel-tags (sel/select (sel/class "first") html-sel-page))))
  (is (= [:body] (html-sel-tags (sel/select (sel/class "demo") html-sel-page))))
  ;; case-insensitive name comparison
  (is (= 3 (count (sel/select (sel/class "ITEM") html-sel-page))))
  ;; whitespace-split membership, not substring
  (is (= [] (html-sel-tags (sel/select (sel/class "fir") html-sel-page))))
  (is (= [:li]
         (html-sel-tags
           (sel/select (sel/class :first)
                       (first (html/parse-fragment
                                "<p class=\"first\tlast\">x</p>")))))))

(deftest html-select-attr-presence
  (is (= [:a] (html-sel-tags (sel/select (sel/attr :target) html-sel-page))))
  (is (= [:html] (html-sel-tags (sel/select (sel/attr :lang) html-sel-page))))
  (is (= ["_blank"] (html-sel-attr-of :target
                                      (sel/select (sel/attr :target)
                                                  html-sel-page))))
  (is (= [] (html-sel-tags (sel/select (sel/attr "TARGETX") html-sel-page))))
  ;; valueless attributes normalize to "" (tier rule 6) so presence holds
  (is (= [:input]
         (html-sel-tags
           (sel/select (sel/attr :disabled)
                       (html/parse-fragment "<input disabled>"))))))

(deftest html-select-attr-predicate
  ;; the 0.7.7 two-arity shape (attr attr-name predicate); patterns
  ;; are not IFn in mino, so the regex rides a fn (the idiom the
  ;; docstrings show)
  (is (= ["https://a.example/1" "https://a.example/2"]
         (html-sel-attr-of :href
                           (sel/select (sel/attr :href
                                                 #(re-find #"^https:" %))
                                       html-sel-page))))
  (is (= 3 (count (sel/select (sel/attr :href string?) html-sel-page))))
  ;; the predicate only runs when the attribute is present
  (is (= [] (html-sel-tags (sel/select (sel/attr :colspan #(= % "2"))
                                       html-sel-page)))))

(deftest html-select-any
  ;; any is the selector itself (no call): every element, nothing else
  (let [nodes (sel/select sel/any html-sel-page)
        els (filter map? nodes)]
    (is (= (html-sel-count-elements [html-sel-page]) (count nodes)))
    (is (= (count nodes) (count els))))
  (is (= [] (sel/select sel/any (html/parse-fragment "just text"))))
  (is (sel/any (zip/xml-zip (html-sel-el :p ["x"])))
      "any on a loc directly")
  (is (nil? (sel/any (zip/down (zip/xml-zip (html-sel-el :p ["x"])))))))

;;; ---- combinators ----

(deftest html-select-and
  (is (= [:li]
         (html-sel-tags (sel/select (sel/and (sel/tag :li)
                                             (sel/class "first"))
                                    html-sel-page))))
  (is (= [:body]
         (html-sel-tags (sel/select (sel/and (sel/tag :body)
                                             (sel/class :page))
                                    html-sel-page))))
  (is (= [] (html-sel-tags (sel/select (sel/and (sel/tag :li)
                                                (sel/class :note))
                                       html-sel-page))))
  ;; three-way composition
  (is (= [:a]
         (html-sel-tags (sel/select (sel/and (sel/tag :a)
                                             (sel/attr :href)
                                             (sel/attr :target))
                                    html-sel-page)))))

(deftest html-select-or
  (is (= [:h1 :table]
         (html-sel-tags (sel/select (sel/or (sel/tag :h1)
                                            (sel/tag :table))
                                    html-sel-page))))
  (is (= 2 (count (sel/select (sel/or (sel/class "first")
                                      (sel/class "last"))
                              html-sel-page)))
      "exactly li.first and li.last"))

(deftest html-select-not
  ;; not is hickory's raw not, not el-not: non-elements match too
  (let [sel-out (sel/select (sel/not (sel/tag :div)) html-sel-page)]
    (is (some string? sel-out) "text nodes match (not (tag ...))")
    (is (some #(= :comment (:type %)) sel-out) "comments match")
    (is (= [] (filter #(= :div (:tag %)) sel-out)))
    (is (= [(html-sel-el :div)]
           (sel/select (sel/not (sel/tag :span))
                       (html-sel-el :div))))))
  (is (= [:p :p :b]
         (html-sel-tags
           (filter map?)
           (sel/select (sel/not (sel/class :note))
                       (first (html/parse-fragment
                                "<p>a</p><p class=\"note\">n</p><b>c</b>")))))))

(deftest html-select-child
  ;; the hickory docstring example, verbatim shape: direct chain
  (let [direct (html/parse-fragment
                 "<div><span class=\"foo\"><input disabled></input></span></div>")
        nested (html/parse-fragment
                 "<div><span class=\"foo\"><b><input disabled></input></b></span></div>")]
    (is (= [:input]
           (html-sel-tags (sel/select (sel/child (sel/tag :div)
                                                 (sel/class :foo)
                                                 (sel/attr :disabled))
                                      direct)))
    (is (= [] (html-sel-tags (sel/select (sel/child (sel/tag :div)
                                                    (sel/class :foo)
                                                    (sel/attr :disabled))
                                          nested)))))
  ;; curated page: p's parent is the div, not body
  (is (= [] (html-sel-tags (sel/select (sel/child (sel/tag :body)
                                                  (sel/tag :p))
                                       html-sel-page))))
  (is (= [:p :p] (html-sel-tags (sel/select (sel/child (sel/tag :div)
                                                       (sel/tag :p))
                                            html-sel-page))))
  (is (= [:a :a :a] (html-sel-tags (sel/select (sel/child (sel/tag :li)
                                                          (sel/tag :a))
                                               html-sel-page)))))

(deftest html-select-descendant
  ;; the hickory docstring example: descendant matches through the
  ;; intervening b
  (let [direct (html/parse-fragment
                 "<div><span class=\"foo\"><input disabled></input></span></div>")
        nested (html/parse-fragment
                 "<div><span class=\"foo\"><b><input disabled></input></b></span></div>")]
    (is (= [:input]
           (html-sel-tags (sel/select (sel/descendant (sel/tag :div)
                                                      (sel/class :foo)
                                                      (sel/attr :disabled))
                                      direct)))
    (is (= [:input]
           (html-sel-tags (sel/select (sel/descendant (sel/tag :div)
                                                      (sel/class :foo)
                                                      (sel/attr :disabled))
                                      nested)))))
  (is (= [:a :a :a] (html-sel-tags (sel/select (sel/descendant (sel/tag :body)
                                                               (sel/tag :a))
                                               html-sel-page))))
  (is (= [:td :td] (html-sel-tags (sel/select (sel/descendant (sel/tag :body)
                                                              (sel/tag :td))
                                              html-sel-page))))
  ;; ancestor order is load-bearing: a descendant of a inside body
  ;; never matches (descendant (tag :a) (tag :body))
  (is (= [] (html-sel-tags (sel/select (sel/descendant (sel/tag :a)
                                                       (sel/tag :body))
                                       html-sel-page)))))

;;; ---- position selectors ----

(deftest html-select-first-child
  (let [frag (html/parse-fragment "<ul><li>a</li><li>b</li><li>c</li></ul>")]
    (is (= ["a"] (map sel/text (sel/select (sel/and (sel/tag :li)
                                                    sel/first-child)
                                           frag)))))
  ;; hickory counts element children only: leading text does not
  ;; disqualify (differs from CSS :first-child; hickory-exact)
  (is (= [:span]
         (html-sel-tags (sel/select (sel/and (sel/tag :span)
                                             sel/first-child)
                                    (html/parse-fragment
                                      "<p>lead<span>x</span></p>")))))
  ;; the top html element's parent is the document, not an element
  (is (= [] (html-sel-tags (sel/select sel/first-child html-sel-page)))))

(deftest html-select-last-child
  (let [frag (html/parse-fragment "<ul><li>a</li><li>b</li><li>c</li></ul>")]
    (is (= ["c"] (map sel/text (sel/select (sel/and (sel/tag :li)
                                                    sel/last-child)
                                           frag)))))
  ;; trailing text does not disqualify the last element child
  (is (= [:b]
         (html-sel-tags (sel/select (sel/and (sel/tag :b)
                                             sel/last-child)
                                    (html/parse-fragment
                                      "<p><b>y</b>tail</p>")))))
  (is (= [] (html-sel-tags (sel/select (sel/and (sel/tag :b)
                                                sel/last-child)
                                       (html/parse-fragment
                                         "<p>head<b>y</b>tail<b>z</b></p>"))))))

(deftest html-select-nth-child
  (let [frag (html/parse-fragment "<ul><li>1</li><li>2</li><li>3</li><li>4</li></ul>")]
    ;; 1-based
    (is (= ["2"] (map sel/text (sel/select (sel/and (sel/tag :li)
                                                    (sel/nth-child 2))
                                           frag))))
    (is (= ["1"] (map sel/text (sel/select (sel/and (sel/tag :li)
                                                    (sel/nth-child 1))
                                           frag))))
    (is (= ["1" "3"] (map sel/text (sel/select (sel/and (sel/tag :li)
                                                        (sel/nth-child :odd))
                                               frag))))
    (is (= ["2" "4"] (map sel/text (sel/select (sel/and (sel/tag :li)
                                                        (sel/nth-child :even))
                                               frag))))
    ;; (nth-child n c) is the stride form; (nth-child 2 1) is :odd
    (is (= ["1" "3"] (map sel/text (sel/select (sel/and (sel/tag :li)
                                                        (sel/nth-child 2 1))
                                               frag))))
    ;; hickory's stride arithmetic is (rem (- distance c) n) with no
    ;; k >= 0 bound, so distance 1 satisfies c=3 n=2 (CSS an+b would
    ;; say 3 only); pinned hickory-exact
    (is (= ["1" "3"] (map sel/text (sel/select (sel/and (sel/tag :li)
                                                        (sel/nth-child 2 3))
                                               frag))))
    ;; no 5th child
    (is (= [] (sel/select (sel/nth-child 5) frag))))
  ;; text siblings are skipped by the element count
  (let [frag (html/parse-fragment "<ul>x<li>a</li>y<li>b</li></ul>")]
    (is (= ["a"] (map sel/text (sel/select (sel/and (sel/tag :li)
                                                    (sel/nth-child 1))
                                           frag))))
    (is (= ["b"] (map sel/text (sel/select (sel/and (sel/tag :li)
                                                    (sel/nth-child 2))
                                           frag))))))

;;; ---- entry points ----

(deftest html-select-select-locs
  (let [locs (sel/select-locs (sel/tag :a) html-sel-page)]
    (is (vector? locs))
    (is (= [:a :a :a] (html-sel-tags (mapv zip/node locs))))
    ;; locs are live zipper locs: up walks to the parent li
    (is (= :li (:tag (zip/node (zip/up (first locs))))))
    ;; an edit through a returned loc surfaces at that loc's root;
    ;; the original tree is untouched (immutability)
    (let [edited (zip/edit (first locs) assoc :attrs {:href "x"})]
      (is (= "x" (get-in (zip/root edited) [:attrs :href])))
      (is (= "https://a.example/1"
             (get-in (first (sel/select (sel/tag :a) html-sel-page))
                     [:attrs :href])))))
  ;; select-locs starts at the root loc: a selector matching the
  ;; root node itself returns it
  (let [div (first (html/parse-fragment "<div><p>x</p></div>"))
        locs (sel/select-locs (sel/tag :div) div)]
    (is (= 1 (count locs)))
    (is (= div (zip/node (first locs))))))

(deftest html-select-document-entry
  ;; over a parsed document: wrappers participate like any node
  (is (= [:html] (html-sel-tags (sel/select (sel/tag :html) html-sel-page))))
  (is (= 1 (count (sel/select (sel/tag :html) html-sel-page))))
  (is (= [:body] (html-sel-tags (sel/select (sel/id "main") html-sel-page))))
  ;; the document node itself is walkable by a custom selector
  (is (= [:document]
         (mapv :type (sel/select (fn [loc] (when (= :document
                                                   (:type (zip/node loc)))
                                             loc))
                                 html-sel-page))))
  ;; selecting over the html element directly: the root participates
  (let [html-el (first (sel/select (sel/tag :html) html-sel-page))]
    (is (= [html-el] (sel/select (sel/tag :html) html-el)))))

(deftest html-select-fragment-entry
  ;; divergence pin: sequential input selects per top-level node,
  ;; results concatenated in order (parse-fragment has no wrapper)
  (let [frag (html/parse-fragment "<p>a</p>text<p>b</p>")]
    (is (= [:p :p] (html-sel-tags (sel/select (sel/tag :p) frag))))
    (is (= [[:text "text"]]
           (html-sel-tags (sel/select (fn [loc] (when (string? (zip/node loc))
                                                  loc))
                                      frag))))
    (is (= [] (sel/select sel/any frag))
        "no elements among bare strings")))
  ;; each fragment root is its own zipper: first-child applies per root
  (let [frag (html/parse-fragment "<p>a</p><p>b</p>")]
    (is (= [:p :p]
           (html-sel-tags (sel/select (sel/and (sel/tag :p)
                                               sel/first-child)
                                      frag)))))
  (is (= [] (sel/select (sel/tag :p) (html/parse-fragment "no tags")))))

(deftest html-select-jvm-shared-shape
  ;; tag/attr are :type-blind, so the JVM clojure.xml element shape
  ;; selects too (the shared shape, research section 6); any and the
  ;; position selectors are HTML-shaped and stay inert on it
  (let [root {:tag :root :attrs {}
              :content [{:tag :a :attrs {:x "1"} :content ["va"]}
                        " tail"]}]}
    (is (= [:a] (html-sel-tags (sel/select (sel/tag :a) root))))
    (is (= ["1"] (html-sel-attr-of :x (sel/select (sel/attr :x) root))))
    (is (= [] (sel/select sel/any root)))))

;;; ---- text extraction (the scraping idiom) ----

(deftest html-select-text
  ;; deep concatenation of string descendants, document order
  (is (= "para bold tail"
         (sel/text (first (sel/select (sel/and (sel/tag :p)
                                               (sel/not (sel/class :note)))
                                      html-sel-page)))))
  (is (= "second" (sel/text (first (sel/select (sel/class :note)
                                               html-sel-page)))))
  ;; whitespace runs pass through verbatim (no normalization)
  (is (= "a\n  b"
         (sel/text (first (html/parse-fragment "<p>a\n  <b>b</b></p>")))))
  ;; comment and doctype payloads are data, not text
  (is (= "x"
         (sel/text (first (html/parse-fragment
                            "<p>x<!--secret--></p>")))))
  ;; the idiom the docstrings show
  (is (= ["one" "two" "three"]
         (mapv sel/text (sel/select (sel/child (sel/tag :li)
                                               (sel/tag :a))
                                    html-sel-page))))
  ;; strings are themselves
  (is (= "solo" (sel/text "solo"))))

;;; ---- golden corpus cross-check ----

(deftest html-select-goldens-cross-check
  ;; selector counts against an independent structural walk over
  ;; the same trees, over a spread of the p1 golden corpus inputs
  ;; (the oracle-derived vectors; every assertion can fail because
  ;; the structural walk shares no code with select)
  (let [goldens (edn/read-string (slurp "tests/fixtures/html/golden.edn"))
        inputs (keep (fn [v] (when (string? (:input v)) (:input v)))
                     (take-nth 37 goldens))]
    (is (> (count inputs) 150) "the sample must stay substantive")
    (doseq [s inputs]
      (let [roots (try (html/parse-fragment s)
                       (catch e nil))]
        (when roots
          (is (= (html-sel-count-elements roots)
                 (count (sel/select sel/any roots)))
              (str "element count mismatch for " (pr-str s)))
          (is (= (html-sel-count-tag :div roots)
                 (count (sel/select (sel/tag :div) roots)))
              (str "div count mismatch for " (pr-str s)))
          (is (= (html-sel-count-tag :a roots)
                 (count (sel/select (sel/tag :a) roots)))
              (str "a count mismatch for " (pr-str s))))))))

;;; ---- v1 surface cap ----

(deftest html-select-v1-surface-cap
  ;; research section 6: no nth-of-type, n-moves-until, precede,
  ;; follow in v1, and no CSS string grammar. The public surface is
  ;; exactly the v1 subset plus the local text helper.
  (let [publics (set (keys (ns-publics 'mino.html.select)))]
    (is (= #{'select 'select-locs 'tag 'id 'class 'attr 'any
             'and 'or 'not 'child 'descendant 'first-child
             'last-child 'nth-child 'text}
           publics))
    (is (nil? (ns-resolve 'mino.html.select 'nth-of-type)))
    (is (nil? (ns-resolve 'mino.html.select 'precede)))
    (is (nil? (ns-resolve 'mino.html.select 'follow)))))

(run-tests-and-exit)
