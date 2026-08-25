(require "tests/test")
(require '[clojure.string :as str])
(require '[clojure.edn :as edn])
(require '[mino.html :as html])
(require '[tests.html-fixture :as hfix])

;; mino.html/to-html contract vectors (html-xml campaign p3, design
;; D10). Red first: the serializer lands in the commit that flips
;; this file. Layers:
;;
;; 1. The D10 normalization list, one rule per vector: text escapes
;;    amp lt gt; attribute values always double-quoted with amp quot
;;    escaped (minimal spelling; lt gt stay raw, reparse-safe in
;;    quoted values); attribute order preserved as written; valueless
;;    attributes re-emit as name=""; void elements without end tags;
;;    non-void elements explicitly closed; implied closes and the
;;    html/head/body wrappers materialized; RAWTEXT script/style
;;    content verbatim; PLAINTEXT verbatim with no end tag; RCDATA
;;    title/textarea re-encoded like text (D6 decoded them at parse);
;;    comments and the DOCTYPE text verbatim; adjacent text runs
;;    merge into one serialized run; empty text runs (parse output
;;    for dropped-codepoint references) vanish, being
;;    unrepresentable in HTML.
;;
;; 2. Round-trip property (A-3): (parse (to-html (parse s))) equals
;;    (parse s) modulo the D10 adjacent-run merge, over the full p1
;;    golden corpus, the 1MB fixture, and curated document-mode
;;    vectors. Byte-exactness only over canonical-form fixtures
;;    (already-lowercased names, double-quoted values, explicit
;;    closes, minimal entity spelling).
;;
;; 3. The facade surface: opts keyword maps reserved in v1, argument
;;    errors as :html/opts ex-info, and the shared JVM element shape
;;    (research section 6: XML round-trips through to-html on the
;;    shared {:tag :attrs :content} shape).

;;; ---- node builders (unique names, shared suite namespace) ----

(defn- html-ser-el
  ([tag] {:type :element :tag tag :attrs {} :content []})
  ([tag content] {:type :element :tag tag :attrs {} :content content})
  ([tag attrs content] {:type :element :tag tag :attrs attrs
                       :content content}))

;;; ---- D10 normalization vectors ----

(deftest html-ser-text-node-escaping
  (is (= "a &amp; b &lt; c &gt; d" (html/to-html "a & b < c > d")))
  (is (= "&amp;amp;" (html/to-html "&amp;")))
  (is (= "plain" (html/to-html "plain")))
  ;; ampersand escapes first so no double escaping
  (is (= "&amp;&lt;" (html/to-html "&<"))))

(deftest html-ser-attribute-quoting
  ;; always double-quoted; amp and quot escaped; minimal spelling
  ;; keeps lt gt raw (round-trip-safe inside quoted values)
  (is (= "<p title=\"a&quot;b&amp;c\"></p>"
         (html/to-html (html-ser-el :p {:title "a\"b&c"} []))))
  (is (= "<p alt=\"a<b>c\"></p>"
         (html/to-html (html-ser-el :p {:alt "a<b>c"} []))))
  ;; valueless attributes re-emit as name=""
  (is (= "<input disabled=\"\">"
         (html/to-html (first (html/parse-fragment "<input disabled>")))))
  ;; attribute order preserved as written (D10)
  (is (= "<div a=\"1\" b=\"2\" c=\"3\">x</div>"
         (html/to-html
           (first (html/parse-fragment "<div a=\"1\" b=\"2\" c=\"3\">x</div>")))))
  ;; single quotes in values stay raw
  (is (= "<p t=\"it's\"></p>"
         (html/to-html (first (html/parse-fragment "<p t=\"it's\">"))))))

(deftest html-ser-void-elements
  (is (= "<br>" (html/to-html (first (html/parse-fragment "<br>")))))
  (is (= "<img src=\"x.png\">"
         (html/to-html (first (html/parse-fragment "<img src=x.png>")))))
  (is (= "<hr>"
         (html/to-html (first (html/parse-fragment "<hr/>")))))
  ;; never an end tag for a void element
  (is (not (str/includes?
             (html/to-html (first (html/parse-fragment "<img src=x>")))
             "</img>")))
  ;; parsed case-folded names serialize lowercased (rule 11)
  (is (= "<em>a</em>"
         (html/to-html (first (html/parse-fragment "<EM>a</EM>"))))))

(deftest html-ser-explicit-closes-materialized
  ;; non-void elements explicitly closed, even when parse implied
  ;; the close or the solidus provided it
  (is (= "<li>a</li><li>b</li>"
         (html/to-html (html/parse-fragment "<li>a<li>b"))))
  (is (= "<div></div>"
         (html/to-html (first (html/parse-fragment "<div/>")))))
  (is (= "<b><i>x</i></b>"
         (html/to-html (first (html/parse-fragment "<b><i>x</b>"))))))

(deftest html-ser-rawtext-verbatim
  ;; script/style content passes through undecoded and unescaped
  (is (= "<script>if (a<b) x && y</b></script>tail"
         (html/to-html
           (html/parse-fragment "<script>if (a<b) x && y</b></script>tail"))))
  (is (= "<style>p { content: '&amp;' }</style>"
         (html/to-html
           (html/parse-fragment "<style>p { content: '&amp;' }</style>"))))
  ;; attributes on a raw-text element still quote-normally
  (is (= "<script type=\"text/javascript\">var x = 1 && 2;</script>"
         (html/to-html
           (html/parse-fragment "<script type=text/javascript>var x = 1 && 2;</script>")))))

(deftest html-ser-rcdata-reencoded
  ;; title/textarea content was entity-decoded at parse (D6); the
  ;; serializer re-encodes it like any text run
  (is (= "<title>a &amp; &lt; b</title>"
         (html/to-html (html/parse-fragment "<title>a &amp; &lt; b</title>"))))
  (is (= "<textarea>&lt;code&gt; &amp; copy</textarea>"
         (html/to-html
           (html/parse-fragment "<textarea><code> &amp; copy</textarea>")))))

(deftest html-ser-plaintext-verbatim
  ;; PLAINTEXT holds the rest of the input verbatim; emitting an end
  ;; tag would corrupt it (the reparse would keep the closing tag in
  ;; the run), so the start tag and raw content are the whole element
  (is (= "<plaintext>x&amp;<b>y</plaintext>z"
         (html/to-html
           (html/parse-fragment "<plaintext>x&amp;<b>y</plaintext>z")))))

(deftest html-ser-comments-and-doctype
  (is (= "<!-- hi -->"
         (html/to-html (first (html/parse-fragment "<!-- hi -->")))))
  ;; bogus comments normalize to the comment form (tier rule 9)
  (is (= "<!--php echo 1?-->"
         (html/to-html (first (html/parse-fragment "<?php echo 1?>")))))
  (is (= "<!DOCTYPE html>"
         (html/to-html (first (html/parse-fragment "<!DOCTYPE html>")))))
  ;; doctype text verbatim; the first > ended it at parse, so the
  ;; re-emitted > is the same one
  (is (= "<!DOCTYPE html SYSTEM \"x>"
         (html/to-html
           (first
             (html/parse-fragment "<!DOCTYPE html SYSTEM \"x>a\">"))))))

(deftest html-ser-document-node
  ;; the implied wrappers materialize as explicit html/head/body
  (is (= "<html><head></head><body><p>x</p></body></html>"
         (html/to-html (html/parse "<p>x"))))
  (is (= "<html><head></head><body></body></html>"
         (html/to-html (html/parse ""))))
  ;; document children serialize in order: doctype, comments, root
  (is (= "<!DOCTYPE html><!--c--><html><head></head><body><p>x</p></body></html>"
         (html/to-html (html/parse "<!DOCTYPE html><!--c--><p>x"))))
  ;; adopted attrs survive
  (is (= "<html lang=\"en\"><head></head><body class=\"c\">z</body></html>"
         (html/to-html (html/parse "<html lang=en><body class=c>z")))))

(deftest html-ser-collections-and-shared-shape
  ;; a vector of nodes concatenates (fragment serialization)
  (is (= "a &amp; b<b>c</b>"
         (html/to-html ["a & b"
                        (first (html/parse-fragment "<b>c</b>"))])))
  ;; adjacent text runs merge into one serialized run (D10)
  (is (= "ab" (html/to-html (html/parse-fragment "a</div>b"))))
  ;; the shared JVM element shape (no :type) serializes too: XML
  ;; round-trips through to-html on the shared shape (research 6)
  (is (= "<p class=\"x\">t &amp; t</p>"
         (html/to-html {:tag :p :attrs {:class "x"} :content ["t & t"]}))))

;;; ---- facade surface ----

(defn- html-ser-err-data
  [thunk]
  (try (thunk) :no-throw (catch e (ex-data e))))

(deftest html-ser-opts-validation
  ;; opts are keyword maps, reserved and ignored in v1
  (is (= "<p>x</p>" (html/to-html (html-ser-el :p ["x"]) nil)))
  (is (= "<p>x</p>" (html/to-html (html-ser-el :p ["x"]) {})))
  (is (= :html/opts (:kind (html-ser-err-data
                             #(html/to-html (html-ser-el :p) :nope)))))
  (is (= :html/opts (:kind (html-ser-err-data #(html/to-html 5)))))
  (is (= :html/opts (:kind (html-ser-err-data
                             #(html/to-html {:attrs {} :content []}))))))

;;; ---- byte-exactness over canonical-form fixtures (D10) ----

;; Canonical form: lowercase names, double-quoted values, explicit
;; closes for non-void elements, minimal entity spelling (amp lt gt
;; in text, amp quot in attribute values). Over such inputs the
;; serializer must be byte-exact, not merely tree-faithful.

(deftest html-ser-canonical-byte-exact-fragment
  (doseq [s ["<div a=\"1\" b=\"2\" c=\"3\">x</div>"
             "<input disabled=\"\">"
             "<!-- hi -->"
             "<script>if (a<b) x && y</script>"
             "<style>p { content: '&amp;' }</style>"
             "<textarea>&lt;code&gt; &amp; copy</textarea>"
             "<img src=\"x.png\">"
             "<ul><li>a</li><li>b</li></ul>"
             "<title>a &amp; &lt; b</title>"]]
    (is (= s (html/to-html (html/parse-fragment s)))
        (str "not byte-exact over canonical form: " s))))

(deftest html-ser-canonical-byte-exact-document
  (doseq [s ["<html><head></head><body><p>x</p></body></html>"
             "<!DOCTYPE html><html lang=\"en\"><head><title>a &amp; b</title></head><body><p class=\"x\">t &amp; t &lt; u &gt; v</p><br></body></html>"
             "<html><head></head><body><ul><li>a</li><li>b &amp; c</li></ul><hr></body></html>"]]
    (is (= s (html/to-html (html/parse s)))
        (str "not byte-exact over canonical form: " s))))

;;; ---- round-trip property (A-3): equal trees, modulo D10 merges ----

(defn- html-ser-merge
  "Merges adjacent string siblings and drops empty string runs (D10
  normalization; the serializer emits one run, and an empty text
  node is unrepresentable in HTML, so it vanishes on reparse)."
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

(defn- html-ser-norm
  "Any tree normalized per D10: adjacent text runs merged and empty
  text runs dropped at every container level. Comment and
  document-type payloads are verbatim data, not text runs; they pass
  through untouched. Idempotent."
  [node]
  (if (map? node)
    (if (or (= :comment (:type node))
            (= :document-type (:type node)))
      node
      (assoc node
        :content (html-ser-merge (mapv html-ser-norm (:content node)))))
    (if (vector? node)
      (html-ser-merge (mapv html-ser-norm node))
      node)))

(defn- html-ser-round-trips?
  [parse-fn s]
  (= (html-ser-norm (parse-fn (html/to-html (parse-fn s))))
     (html-ser-norm (parse-fn s))))

(deftest html-ser-doc-mode-round-trip
  (doseq [s ["<p>hello"
             "x"
             ""
             "   <p>x"
             "<meta charset=x><title>T</title><p>x"
             "<script>var x=1</script><p>after"
             "<head><title>t</title><p>x"
             "<html><head><title>t</title></head><body><p>x</p></body></html>"
             "<meta x><body>y"
             "<html lang=en><body class=c>z"
             "<div>a</div></body><div>b</div>"
             "</head><meta x><body>y"
             "<!DOCTYPE html><p>x"
             "<!DOCTYPE a><p>x<!DOCTYPE b>"
             "<!--c--><p>x"
             "<title>t</title><h1>x"
             "<b><i>x</b></i>y"
             "a</div>b"
             "<p>1<table><p>2"
             "<table>stray<tr><td>cell"
             "<textarea><b>x"
             ;; PLAINTEXT is excluded from document mode: its content
             ;; is verbatim to EOF, so the wrappers' materialized end
             ;; tags would join the run on reparse. In fragment mode,
             ;; as the tail node, it round trips (pinned above).
             "x<!--tail-->"
             "<html><head></head><body><p>x</p></body><!--tail--></html>"
             "<!--lead--><!DOCTYPE h><p>x"]]
    (is (html-ser-round-trips? html/parse s)
        (str "document-mode round trip failed for: " s))))

(deftest html-ser-golden-round-trip-property
  ;; Every golden input the reader accepts must reparse to the same
  ;; tree after serialization (idempotence always, D10; the oracle
  ;; expectations are irrelevant here, only reader stability is).
  (let [res (reduce
              (fn [acc v]
                (if-not (string? (:input v))
                  (assoc acc :skipped (inc (:skipped acc)))
                  (let [t (html/parse-fragment (:input v))]
                    (if (= (html/parse-fragment (html/to-html t))
                           (html-ser-norm t))
                      (assoc acc :ok (inc (:ok acc)))
                      (-> acc
                          (assoc :fail (inc (:fail acc)))
                          (assoc :first-fail (:id v)))))))
              {:ok 0 :fail 0 :skipped 0 :first-fail nil}
              (edn/read-string (slurp "tests/fixtures/html/golden.edn")))]
    (is (zero? (:fail res))
        (str "round-trip failures: " (:fail res) " first id "
             (:first-fail res)))
    (is (> (:ok res) 6900)
        (str "corpus coverage collapsed: " (:ok res) " round-tripped"))))

(deftest html-ser-fixture-round-trip-property
  ;; the 1MB page mix: parse -> to-html -> parse yields equal trees
  (let [doc (hfix/html-fixture-doc)
        t (html/parse doc)]
    (is (> (count doc) 1000000) "fixture must be megabyte scale")
    (is (= (html-ser-norm (html/parse (html/to-html t)))
           (html-ser-norm t)))))

(run-tests-and-exit)
