(require "tests/test")
(require '[clojure.string :as str])
(require '[mino.html :as html])
(require '[clojure.zip :as zip])

;; clojure.zip/xml-zip compatibility pins over mino.html node maps
;; (html-xml campaign p3t3; context invariant: xml-zip walks the
;; hickory shape because it touches only :content; ADR 28 D8). The
;; pins cover branch?/children over mixed string/node :content
;; vectors, a full walk vector, root/document behavior, a zip/edit
;; then to-html round trip, and the shared JVM element shape
;; ({:tag :attrs :content} without :type) that xml-zip walks the
;; same way.

(defn- html-zip-el
  ([tag] {:type :element :tag tag :attrs {} :content []})
  ([tag content] {:type :element :tag tag :attrs {} :content content})
  ([tag attrs content] {:type :element :tag tag :attrs attrs
                       :content content}))

(defn- html-zip-walk
  "Every loc reachable by zip/next from loc (pre-order, strings
  included as leaves), as [:type tag] / [:text s] entries."
  [loc]
  (loop [loc loc acc []]
    (if (zip/end? loc)
      acc
      (let [n (zip/node loc)]
        (recur (zip/next loc)
               (conj acc (if (map? n)
                           [(:type n) (:tag n)]
                           [:text n])))))))

(deftest html-zip-mixed-content-branch-and-children
  (let [div (first (html/parse-fragment "<div>text1<p>a</p>text2</div>"))
        loc (zip/xml-zip div)]
    (is (zip/branch? loc))
    (is (= ["text1" (html-zip-el :p ["a"]) "text2"]
           (vec (zip/children loc))))
    ;; strings are leaves: no branch, children throws
    (let [sloc (zip/down loc)]
      (is (= "text1" (zip/node sloc)))
      (is (not (zip/branch? sloc)))
      (is (= :leaf (try (zip/children sloc) (catch e :leaf)))))
    ;; right moves over mixed siblings: element then string
    (let [ploc (zip/right (zip/down loc))]
      (is (= :p (:tag (zip/node ploc))))
      (is (zip/branch? ploc))
      (let [t2 (zip/right ploc)]
        (is (= "text2" (zip/node t2)))
        (is (nil? (zip/right t2)))))))

(deftest html-zip-full-walk-vector
  ;; walk order over a document: doctype and comment interiors are
  ;; string children of their nodes (xml-zip only knows :content);
  ;; empty elements (head, the void br) are leaves
  (let [doc (html/parse "<!DOCTYPE html><!--c--><p>a<br>b</p>")
        loc (zip/xml-zip doc)]
    (is (= [[:document nil]
            [:document-type nil]
            [:text "html"]
            [:comment nil]
            [:text "c"]
            [:element :html]
            [:element :head]
            [:element :body]
            [:element :p]
            [:text "a"]
            [:element :br]
            [:text "b"]]
           (html-zip-walk loc)))))

(deftest html-zip-path-and-ancestors
  (let [doc (html/parse "<!DOCTYPE html><p>a</p>")
        loc (-> doc zip/xml-zip zip/down       ; doctype
                zip/right                      ; html
                zip/down zip/right             ; head -> body
                zip/down zip/down)]            ; p -> "a" text
    (is (= "a" (zip/node loc)))
    (is (= [[:document nil] [:element :html] [:element :body]
            [:element :p]]
           (mapv #(if (map? %) [(:type %) (:tag %)] %) (zip/path loc))))
    ;; up walks the same chain in reverse (a -> p -> body -> html ->
    ;; document)
    (is (= :p (:tag (zip/node (zip/up loc)))))
    (is (= :document
           (:type (zip/node (zip/up (zip/up (zip/up (zip/up loc))))))))))

(deftest html-zip-document-root-behavior
  (let [doc (html/parse "<p>x</p>")
        loc (zip/xml-zip doc)]
    ;; the loc's node is the document itself; no up from the top
    (is (= doc (zip/node loc)))
    (is (nil? (zip/up loc)))
    ;; root of any descent is the unchanged document
    (is (= doc (zip/root (zip/down loc))))
    ;; an edit deep in the tree surfaces at the document root
    (let [ploc (-> loc zip/down zip/down zip/right zip/down)]
      (is (= :p (:tag (zip/node ploc))))
      (is (= :document
             (:type (zip/root (zip/edit ploc assoc :attrs {:id "x"}))))))))

(deftest html-zip-edit-then-to-html-round-trip
  ;; element edit: attribute change through the zipper, then to-html
  (let [ul (first (html/parse-fragment "<ul><li>a</li><li>b</li></ul>"))
        zloc (zip/xml-zip ul)
        li (zip/down zloc)
        edited (zip/edit li assoc :attrs {:class "first"})
        out (html/to-html (zip/root edited))]
    (is (= "<ul><li class=\"first\">a</li><li>b</li></ul>" out))
    (is (= [(html-zip-el :ul
                         [(html-zip-el :li {:class "first"} ["a"])
                          (html-zip-el :li ["b"])])]
           (html/parse-fragment out))))
  ;; text edit: strings are nodes like any other
  (let [ul (first (html/parse-fragment "<ul><li>a</li><li>b</li></ul>"))
        zloc (zip/xml-zip ul)
        out (html/to-html
              (zip/root (zip/edit (zip/down (zip/down zloc))
                                  str/upper-case)))]
    (is (= "<ul><li>A</li><li>b</li></ul>" out)))
  ;; structural edit: append-child routes through make-node
  (let [ul (first (html/parse-fragment "<ul><li>a</li></ul>"))
        out (html/to-html
              (zip/root (zip/append-child (zip/xml-zip ul)
                                          (html-zip-el :li ["c"]))))]
    (is (= "<ul><li>a</li><li>c</li></ul>" out))))

(deftest html-zip-jvm-shared-shape
  ;; xml-zip walks the JVM clojure.xml shape (no :type) identically;
  ;; to-html serializes it (the shared shape, research section 6)
  (let [root {:tag :root :attrs {}
              :content [{:tag :a :attrs {:x "1"} :content ["va"]}
                        " tail"]}
        zloc (zip/xml-zip root)]
    (is (zip/branch? zloc))
    (is (= :a (:tag (zip/node (zip/down zloc)))))
    (let [edited (zip/edit (zip/down zloc) assoc :attrs {:x "2"})
          out (html/to-html (zip/root edited))]
      (is (= "<root><a x=\"2\">va</a> tail</root>" out))
      (is (= [(html-zip-el :root
                           [(html-zip-el :a {:x "2"} ["va"]) " tail"])]
             (html/parse-fragment out))))))

(deftest html-zip-mixed-content-edge-vectors
  ;; text-only element
  (let [p (first (html/parse-fragment "<p>only</p>"))
        loc (zip/xml-zip p)]
    (is (= ["only"] (vec (zip/children loc))))
    (is (= "only" (zip/node (zip/down loc))))
    (is (nil? (zip/right (zip/down loc)))))
  ;; text before, between, and after nested elements
  (let [p (first (html/parse-fragment "<p>x<b>y</b>z</p>"))
        loc (zip/xml-zip p)]
    (is (= ["x" (html-zip-el :b ["y"]) "z"] (vec (zip/children loc))))
    (is (= :b (:tag (zip/node (zip/right (zip/down loc)))))
        "right from a string lands on the element")
    (is (= "z" (zip/node (zip/right (zip/right (zip/down loc)))))))
  ;; void elements are leaves ([] content: branch? falsey, down nil)
  (let [br (first (html/parse-fragment "<br>"))
        loc (zip/xml-zip br)]
    (is (not (zip/branch? loc)))
    (is (nil? (zip/down loc))))
  ;; adjacent strings stay distinct to the zipper (serialization
  ;; merges them, the zipper is structure-faithful)
  (let [node (html-zip-el :p ["a" "b"])
        loc (zip/xml-zip node)]
    (is (= ["a" "b"] (vec (zip/children loc))))
    (is (= "b" (zip/node (zip/right (zip/down loc)))))))

(deftest clojure-zip-root-errors-are-classified
  ;; Root-op faults carry :mino/kind :zip/invalid and a specific message
  ;; (ADR 37/38), not a bare string.
  (let [root (zip/vector-zip [1 2])]
    (is (= :zip/invalid (try (zip/insert-left root 0) (catch e (:mino/kind e)))))
    (is (= :zip/invalid (try (zip/insert-right root 0) (catch e (:mino/kind e)))))
    (is (= :zip/invalid (try (zip/remove root) (catch e (:mino/kind e)))))
    (is (= :caught (try (zip/remove root) (catch :zip/invalid _ :caught))))
    (is (str/includes?
          (try (zip/remove root) (catch e (:mino/message e)))
          "remove at the root"))))

(run-tests-and-exit)
