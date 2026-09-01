(require "tests/test")
(require '[clojure.xml :as xml])
(require '[clojure.string :as str])
(require '[clojure.test.check :as tc])
(require '[clojure.test.check.properties :as prop])
(require '[clojure.test.check.generators :as gen])

;; clojure.xml emit canon parity: emit prints the declaration line and
;; the element tree to *out* in the reference shape (single-quoted
;; attributes, one tag or text item per line, self-closing only for
;; nil content), and emit-element prints one node without the
;; declaration.
;;
;; One recorded divergence from the reference body: the reference
;; prints attribute values and character data raw, which emits
;; unparseable XML the moment content holds & or <. Here & < > are
;; entity-escaped in content, quotes too in attribute values, and
;; attribute whitespace rides character references, so emitted
;; documents always reparse and round-trip. A QName tag or attribute
;; keyword (:dc/creator) prints back as its prefixed name.

(defn- xml-emit-str [e] (with-out-str (xml/emit e)))

(defn- xml-emit-element-str [e] (with-out-str (xml/emit-element e)))

(defn- xml-emit-kind
  ":mino/kind of the diagnostic emit-element throws, or :no-throw."
  [e]
  (let [r (try (do (xml-emit-element-str e) :no-throw)
               (catch Throwable e2 (:mino/kind e2)))]
    r))

(def ^:private xml-decl "<?xml version='1.0' encoding='UTF-8'?>\n")

;;;; The declaration and element shapes

(deftest xml-emit-declaration-line
  (is (= (str xml-decl "<a>\n</a>\n")
         (xml-emit-str {:tag :a :attrs {} :content []}))))

(deftest xml-emit-self-closing-on-nil-content
  ;; Canon: only nil content self-closes; [] prints an open pair.
  (is (= (str xml-decl "<a/>\n")
         (xml-emit-str {:tag :a :attrs {} :content nil}))))

(deftest xml-emit-single-quoted-attributes
  (is (= (str xml-decl "<a k='v'/>\n")
         (xml-emit-str {:tag :a :attrs {:k "v"} :content nil}))))

(deftest xml-emit-attribute-order
  (is (= (str xml-decl "<a k='1' l='2'/>\n")
         (xml-emit-str {:tag :a :attrs {:k "1" :l "2"} :content nil}))))

(deftest xml-emit-text-content-on-its-own-line
  (is (= (str xml-decl "<a>\nt\n</a>\n")
         (xml-emit-str {:tag :a :attrs {} :content ["t"]}))))

(deftest xml-emit-nested-elements
  (is (= (str xml-decl "<a>\n<b>\n1\n</b>\nmid\n<c/>\n</a>\n")
         (xml-emit-str
          {:tag :a :attrs {}
           :content [{:tag :b :attrs {} :content ["1"]}
                     "mid"
                     {:tag :c :attrs {} :content nil}]}))))

(deftest xml-emit-element-without-declaration
  (is (= "<a>\nt\n</a>\n"
         (xml-emit-element-str {:tag :a :attrs {} :content ["t"]}))))

(deftest xml-emit-element-on-a-string
  (is (= "s\n" (xml-emit-element-str "s"))))

;;;; Entity escaping

(deftest xml-emit-escapes-content
  (is (= "<a>\nx &amp; &lt;y&gt; &gt; z\n</a>\n"
         (xml-emit-element-str
          {:tag :a :attrs {} :content ["x & <y> > z"]}))))

(deftest xml-emit-escapes-attribute-values
  (is (= "<a k='it&apos;s &quot;q&quot; &lt;&amp;&gt;'/>\n"
         (xml-emit-element-str
          {:tag :a :attrs {:k "it's \"q\" <&>"} :content nil}))))

(deftest xml-emit-attribute-whitespace-as-char-refs
  ;; Literal tabs and newlines in an attribute would normalize to
  ;; spaces on reparse (XML 1.0 3.3.3); character references survive.
  (is (= "<a k='a&#10;b&#9;c&#13;d'/>\n"
         (xml-emit-element-str
          {:tag :a :attrs {:k "a\nb\tc\rd"} :content nil}))))

(deftest xml-emit-escaped-attribute-round-trips-exactly
  (let [attrs {:k "it's \"q\" <&>" :l "a\nb\tc"}
        out (xml-emit-str {:tag :a :attrs attrs :content []})]
    (is (= attrs (:attrs (xml/parse out))))))

;;;; QNames

(deftest xml-emit-qname-tag-and-attribute
  (is (= "<dc:creator xmlns:dc='x'>\nt\n</dc:creator>\n"
         (xml-emit-element-str
          {:tag :dc/creator :attrs {:xmlns/dc "x"} :content ["t"]}))))

(deftest xml-emit-qname-round-trips
  (let [x {:tag :dc/creator :attrs {:xmlns/dc "x"} :content ["t"]}]
    (is (= x (xml/parse (str/replace (xml-emit-str x) "\n" ""))))))

;;;; Errors

(deftest xml-emit-rejects-non-node-values
  (is (= :xml/emit (xml-emit-kind 5)))
  (is (= :xml/emit (xml-emit-kind {:content ["x"]})))
  (is (= :xml/emit (xml-emit-kind [:a "b"]))))

;;;; Round-trip through the native reader

(defn- xml-normalize
  "Structural view for round-trip equality: whitespace-only text
  dropped, other text trimmed (emit adds line breaks around it),
  :attrs and :content defaulted."
  [node]
  (if (string? node)
    node
    {:tag (:tag node)
     :attrs (or (:attrs node) {})
     :content (vec (keep (fn [c]
                           (if (string? c)
                             (let [t (str/trim c)]
                               (when (not= t "") t))
                             (xml-normalize c)))
                         (or (:content node) [])))}))

(defn- xml-round-trips?
  [x]
  (= (xml-normalize x)
     (xml-normalize (xml/parse (xml-emit-str x)))))

(deftest xml-emit-round-trip-goldens
  (is (xml-round-trips? {:tag :a :attrs {} :content []}))
  (is (xml-round-trips? {:tag :a :attrs {} :content nil}))
  (is (xml-round-trips?
       {:tag :rss :attrs {:version "2.0"}
        :content [{:tag :channel :attrs {}
                   :content [{:tag :title :attrs {}
                              :content ["T & A <news>"]}
                             {:tag :item :attrs {:id "1"}
                              :content nil}]}]}))
  (is (xml-round-trips?
       {:tag :a :attrs {:k "1 < 2 & 3 > 2"}
        :content ["ampersand & lt < gt >"]})))

(deftest xml-emit-parses-own-fixture-output
  ;; The reader's own golden document survives an emit and reparse.
  (let [x (xml/parse "<a k=\"1\">t &amp; more</a>")]
    (is (xml-round-trips? x))))

;;;; Round-trip property over small trees

(def ^:private xml-trials 40)

(def ^:private xml-text-gen
  (gen/elements ["t" "x & y" "<tag>" "a > b" "it's \"q\""
                 "line1\nline2" "  padded  "]))

(def ^:private xml-attrs-gen
  (gen/fmap (fn [pairs] (into {} pairs))
            (gen/vector
             (gen/tuple (gen/elements [:k :id :class :href])
                        (gen/elements ["v" "a b" "it's" "q\"q" "<&>"
                                       "a\nb" ""]))
             0 3)))

(defn- xml-elem-gen [depth]
  (gen/bind
   (gen/tuple (gen/elements [:a :b :li :item :entry])
              xml-attrs-gen)
   (fn [[tag attrs]]
     (gen/fmap (fn [content] {:tag tag :attrs attrs :content content})
               (if (zero? depth)
                 (gen/one-of [(gen/return nil)
                              (gen/return [])
                              (gen/fmap vector xml-text-gen)])
                 (gen/one-of [(gen/return [])
                              (gen/fmap vector xml-text-gen)
                              (gen/vector (xml-elem-gen (dec depth))
                                          1 3)]))))))

(deftest xml-emit-round-trip-property
  (let [p (prop/for-all [x (xml-elem-gen 2)]
            (xml-round-trips? x))
        r (tc/quick-check xml-trials p :seed 92029)]
    (is (true? (:result r)) (pr-str r))))

(run-tests-and-exit)
