(require "tests/test")
(require '[clojure.string :as str])
(require '[clojure.edn :as edn])
(require '[mino.html :as html])
(require '[tests.html-fixture :as hfix])

;; mino.html/as-hiccup contract vectors (html-xml campaign p6t1).
;; The reference is hickory.convert/hickory-to-hiccup 0.7.7 (read
;; from the jar, the p4 precedent): element -> [tag attrs children...]
;; with the attrs map ALWAYS present ({} when empty), document -> a
;; vector of converted children, text strings html-escaped (amp lt
;; gt quot), comment -> the literal string "<!--content-->",
;; document-type -> a rendered doctype string, script/style content
;; verbatim (hickory's unescapable-content set) with a non-string
;; child an error. Layers:
;;
;; 1. Hand vectors pinning that exact shape over every node type,
;;    including the one documented divergence: hickory renders the
;;    doctype from structured :name/:publicid/:systemid attrs, mino
;;    nodes carry the raw doctype text (ADR 28), so mino emits
;;    "<!DOCTYPE " + text + ">" -- the same bytes to-html emits.
;;
;; 2. The reconversion property over the full p1 golden corpus and
;;    the 1MB fixture: rendering the hiccup output with a minimal
;;    independent test-side renderer and reparsing yields the same
;;    tree (modulo the D10 adjacent-run merge). Strings inside hiccup
;;    output are final-form (escaped text, verbatim raw text,
;;    literal comment/doctype strings), so the renderer only wires
;;    tags and attributes.

;;; ---- shape vectors ----

(deftest html-hi-element-shape
  ;; attrs map always present, keywordized tags and attr names
  (is (= [:a {:href "/x" :data-k "v"} "t &amp; u"]
         (html/as-hiccup
           (first (html/parse-fragment "<a href=\"/x\" data-k=v>t &amp; u</a>")))))
  (is (= [:p {} "x"]
         (html/as-hiccup (first (html/parse-fragment "<p>x")))))
  ;; nested elements and mixed content keep order
  (is (= [:div {} [:p {} "a" [:b {} "c"] "d"]]
         (html/as-hiccup
           (first (html/parse-fragment "<div><p>a<b>c</b>d</p></div>")))))
  ;; parsed case-folded names arrive keywordized lowercased (rule 11)
  (is (= [:em {} "a"]
         (html/as-hiccup (first (html/parse-fragment "<EM>a</EM>"))))))

(deftest html-hi-text-escaping
  ;; hickory escapes amp lt gt quot in text (clj-html-escape);
  ;; single quotes stay raw
  (is (= [:p {} "&amp; &lt; &gt; &quot; '" ]
         (html/as-hiccup
           (first (html/parse-fragment "<p>&amp; &lt; &gt; &quot; '</p>")))))
  (is (= [:p {} "plain"]
         (html/as-hiccup (first (html/parse-fragment "<p>plain</p>"))))))

(deftest html-hi-comment-and-doctype-strings
  ;; comments become their literal source string
  (is (= ["<!--c-->"]
         (html/as-hiccup (html/parse-fragment "<!--c-->"))))
  ;; doctype: the documented divergence -- hickory renders from
  ;; structured attrs, mino carries the raw text and emits the same
  ;; bytes to-html emits (pinned here, one test per divergence)
  (is (= ["<!DOCTYPE html>"]
         (html/as-hiccup (html/parse-fragment "<!DOCTYPE html>"))))
  (is (= ["<!DOCTYPE html SYSTEM \"x>" "a&quot;&gt;"]
         (html/as-hiccup
           (html/parse-fragment "<!DOCTYPE html SYSTEM \"x>a\">")))))

(deftest html-hi-document-is-children-vector
  ;; document node -> vector of converted children; the synthesized
  ;; wrappers materialize (explicit tags would appear the same way)
  (is (= ["<!DOCTYPE html>" "<!--c-->"
          [:html {} [:head {}] [:body {} [:p {} "x"]]]]
         (html/as-hiccup (html/parse "<!DOCTYPE html><!--c--><p>x"))))
  (is (= [[:html {} [:head {}] [:body {} [:p {} "x"]]]]
         (html/as-hiccup (html/parse "<p>x")))))

(deftest html-hi-fragment-and-string-inputs
  ;; a fragment vector converts per node (the to-html input domain)
  (is (= ["a" [:b {} "c"] "d"]
         (html/as-hiccup (html/parse-fragment "a<b>c</b>d"))))
  ;; a bare string is escaped text (hickory's first branch)
  (is (= "a &amp; b" (html/as-hiccup "a & b")))
  (is (= "plain" (html/as-hiccup "plain"))))

(deftest html-hi-rawtext-verbatim
  ;; script/style children stay verbatim (hickory
  ;; unescapable-content): no entity decoding happened at parse and
  ;; no escaping happens here, in contrast to text
  (is (= [:script {} "if (a<b && c>d)"]
         (html/as-hiccup
           (first (html/parse-fragment "<script>if (a<b && c>d)</script>")))))
  (is (= [:script {:type "text/javascript"} "var s = \"x\";"]
         (html/as-hiccup
           (first
             (html/parse-fragment "<script type=text/javascript>var s = \"x\";</script>")))))
  (is (= [:style {} "p { content: '&amp;' }"]
         (html/as-hiccup
           (first (html/parse-fragment "<style>p { content: '&amp;' }</style>")))))
  ;; RCDATA title is NOT unescapable: decoded at parse, re-escaped
  ;; here like text (D6)
  (is (= [:title {} "a &amp; &lt; b"]
         (html/as-hiccup
           (first (html/parse-fragment "<title>a &amp; &lt; b</title>"))))))

(deftest html-hi-shared-jvm-shape
  ;; the shared clojure.xml element shape (no :type) converts like an
  ;; element, the to-html input-domain symmetry
  (is (= [:p {:class "x"} "t &amp; t"]
         (html/as-hiccup {:tag :p :attrs {:class "x"}
                          :content ["t & t"]})))
  (is (= [:a {}]
         (html/as-hiccup {:tag :a :attrs {} :content []}))))

(defn- html-hi-err-data
  [thunk]
  (try (thunk) :no-throw (catch e (ex-data e))))

(defn- html-hi-err-kind
  [thunk]
  (try (thunk) :no-throw (catch e (:mino/kind e))))

(deftest html-hi-argument-errors
  ;; opts keyword maps reserved in v1; bad nodes are :html/opts
  (is (= [:p {} "x"] (html/as-hiccup (first (html/parse-fragment "<p>x")) nil)))
  (is (= [:p {} "x"] (html/as-hiccup (first (html/parse-fragment "<p>x")) {})))
  (is (= :html/opts (html-hi-err-kind
                      #(html/as-hiccup [:p {}] :nope))))
  (is (= :html/opts (html-hi-err-kind #(html/as-hiccup 5))))
  (is (= :html/opts (html-hi-err-kind
                      #(html/as-hiccup {:type :bogus :content []}))))
  (is (= :html/opts (html-hi-err-kind
                      #(html/as-hiccup {:attrs {} :content []}))))
  ;; hickory throws on non-string script children; so does mino
  (is (= :html/opts
         (html-hi-err-kind
           #(html/as-hiccup {:type :element :tag :script :attrs {}
                             :content [{:type :element :tag :b
                                        :attrs {} :content []}]})))))

;;; ---- reconversion property ----

(def ^:private html-hi-void
  "to-html's void list (tier rule 4); the renderer emits no end tag,
  exactly as to-html does."
  #{:area :base :br :col :embed :hr :img :input :link :meta :param
    :source :track :wbr})

(def ^:private html-hi-attr-re #"[&\"]")
(def ^:private html-hi-attr-esc {"&" "&amp;" "\"" "&quot;"})

(defn- html-hi-attr-str
  "Attribute values escape amp and quot (the D10 attribute spelling;
  a raw amp would decode on reparse and a raw quot would end the
  value early)."
  [s]
  (if (re-find html-hi-attr-re s)
    (str/replace s html-hi-attr-re html-hi-attr-esc)
    s))

(declare html-hi-render)

(defn- html-hi-render-attrs
  [attrs]
  (if (seq attrs)
    (loop [es (seq attrs) acc ""]
      (if es
        (let [e (first es)]
          (recur (next es)
                 (str acc " " (name (key e)) "=\""
                      (html-hi-attr-str (str (val e))) "\"")))
        acc))
    ""))

(defn- html-hi-render
  "Independent minimal hiccup renderer for the reconversion
  property: strings are already final-form (as-hiccup escaped text,
  verbatim raw text, literal comment/doctype strings), so it only
  wires tags, attribute maps, and children. An element vector has a
  keyword in position 0; any other sequential is a form collection
  (document children, fragments) and concatenates. Void elements and
  PLAINTEXT emit no end tag (the to-html emission rules); everything
  else closes explicitly."
  [form]
  (cond
    (string? form) form
    (and (vector? form) (pos? (count form)) (keyword? (nth form 0)))
    (let [tag (nth form 0)
          n (count form)
          has-attrs (and (> n 1) (map? (nth form 1)))
          attrs (if has-attrs (nth form 1) {})
          kids (if has-attrs (subvec form 2) (subvec form 1))
          kn (count kids)
          open (str "<" (name tag) (html-hi-render-attrs attrs) ">")
          close (if (or (contains? html-hi-void tag)
                        (= :plaintext tag))
                  ""
                  (str "</" (name tag) ">"))]
      (loop [i 0 acc (transient [open])]
        (if (= i kn)
          (let [out (if (= "" close)
                      acc
                      (conj! acc close))]
            (str/join (persistent! out)))
          (do
            (conj! acc (html-hi-render (nth kids i)))
            (recur (inc i) acc)))))
    (sequential? form)
    (str/join (mapv html-hi-render form))
    :else (throw (ex-info "renderer got a non-form" {:form form}))))

(defn- html-hi-merge
  "The D10 normalization: adjacent string runs merge, empty text runs
  drop (the html_serialize_test html-ser-merge shape)."
  [nodes]
  (loop [nodes nodes acc []]
    (if (seq nodes)
      (let [n (first nodes)]
        (cond
          (and (string? n) (= "" n)) (recur (rest nodes) acc)
          (and (string? n) (string? (peek acc)))
          (recur (rest nodes)
                 (assoc acc (dec (count acc)) (str (peek acc) n)))
          :else (recur (rest nodes) (conj acc n))))
      acc)))

(defn- html-hi-norm
  [node]
  (if (map? node)
    (if (or (= :comment (:type node))
            (= :document-type (:type node)))
      node
      (assoc node
        :content (html-hi-merge (mapv html-hi-norm (:content node)))))
    (if (vector? node)
      (html-hi-merge (mapv html-hi-norm node))
      node)))

(deftest html-hi-golden-reconversion-property
  ;; For every golden corpus input: convert the parsed fragment to
  ;; hiccup, render with the independent renderer, reparse: the tree
  ;; is unchanged modulo D10. Needs no oracle expectations, only
  ;; reader stability (the to-html property's coverage argument).
  (let [res (reduce
              (fn [acc v]
                (if-not (string? (:input v))
                  (assoc acc :skipped (inc (:skipped acc)))
                  (let [t (html/parse-fragment (:input v))]
                    (if (= (html/parse-fragment
                             (html-hi-render (html/as-hiccup t)))
                           (html-hi-norm t))
                      (assoc acc :ok (inc (:ok acc)))
                      (-> acc
                          (assoc :fail (inc (:fail acc)))
                          (assoc :first-fail (:id v)))))))
              {:ok 0 :fail 0 :skipped 0 :first-fail nil}
              (edn/read-string (slurp "tests/fixtures/html/golden.edn")))]
    (is (zero? (:fail res))
        (str "reconversion failures: " (:fail res) " first id "
             (:first-fail res)))
    (is (> (:ok res) 6900)
        (str "corpus coverage collapsed: " (:ok res) " reconverted"))))

(deftest html-hi-document-mode-reconversion
  ;; document mode round trips the same way to-html's does: the
  ;; wrappers materialize and reparse. PLAINTEXT keeps its p3
  ;; document-mode boundary (fragment-tail only, pinned by the
  ;; fragment property above).
  (doseq [s ["<p>hello"
             "x"
             ""
             "<meta charset=x><title>T</title><p>x"
             "<script>var x=1</script><p>after"
             "<head><title>t</title><p>x"
             "<html lang=en><body class=c>z"
             "<!DOCTYPE html><p>x"
             "<!--c--><p>x"
             "<b><i>x</b></i>y"
             "a</div>b"]]
    (is (= (html-hi-norm (html/parse (html-hi-render (html/as-hiccup (html/parse s)))))
           (html-hi-norm (html/parse s)))
        (str "document-mode reconversion failed for: " s))))

(deftest html-hi-fixture-reconversion-property
  ;; the 1MB page mix through the full loop: parse -> as-hiccup ->
  ;; render -> parse equals the first parse (normalized)
  (let [doc (hfix/html-fixture-doc)
        t (html/parse doc)]
    (is (> (count doc) 1000000) "fixture must be megabyte scale")
    (is (= (html-hi-norm (html/parse (html-hi-render (html/as-hiccup t))))
           (html-hi-norm t)))))

(run-tests-and-exit)
