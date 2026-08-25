(require "tests/test")
(require '[mino.yaml :as yaml])

;; YAML 1.2 subset reader (ADR 26). Golden vectors are ported from the
;; official yaml-test-suite examples; expected values follow each
;; test's json field, and pyyaml 6 (YAML 1.1) was used only as a
;; structural cross-check where 1.1 and 1.2 agree. Scalar resolution
;; is the 1.2 core schema: yes/no/on/off are strings, 017 is decimal
;; seventeen, underscored and sexagesimal numbers are strings; those
;; are the recorded divergences from 1.1 parsers.
;;
;; Error vectors pin :kind :yaml/parse plus a :reason keyword and, for
;; the indentation and position classes, :location {:line :col} over
;; 1-based byte positions. Out-of-subset constructs (anchors, aliases,
;; tags, directives, complex keys) are errors, never silent misparses.

(defn- yaml-err
  "ex-data of parse-string on s, or :no-throw when it succeeds."
  [s]
  (try (yaml/parse-string s) :no-throw
       (catch e (ex-data e))))

(defn- yaml-err-reason
  [s reason]
  (let [d (yaml-err s)]
    (and (map? d)
         (= :yaml/parse (:kind d))
         (= reason (:reason d)))))

(defn- yaml-err-at
  [s reason line col]
  (let [d (yaml-err s)]
    (and (map? d)
         (= :yaml/parse (:kind d))
         (= reason (:reason d))
         (= line (get-in d [:location :line]))
         (= col (get-in d [:location :col])))))

;;; Block mappings and nesting

(deftest yaml-single-pair-block-mapping
  (is (= {:foo "bar"} (yaml/parse-string "foo: bar\n"))))

(deftest yaml-multiple-pair-block-mapping
  (is (= {:foo "blue" :bar "arrr" :baz "jazz"}
         (yaml/parse-string "foo: blue\nbar: arrr\nbaz: jazz\n"))))

(deftest yaml-block-submapping
  (is (= {:foo {:bar 1} :baz 2}
         (yaml/parse-string "foo:\n  bar: 1\nbaz: 2\n"))))

(deftest yaml-simple-mapping-indent
  (is (= {:foo {:bar "baz"}}
         (yaml/parse-string "foo:\n  bar: baz\n"))))

(deftest yaml-multi-level-mapping-indent
  (is (= {:a {:b {:c "d"} :e {:f "g"}} :h "i"}
         (yaml/parse-string "a:\n  b:\n    c: d\n  e:\n    f: g\nh: i\n"))))

(deftest yaml-one-space-nested-mapping
  ;; suite TE2A, spec 8.16
  (is (= {(keyword "block mapping") {:key "value"}}
         (yaml/parse-string "block mapping:\n key: value\n"))))

(deftest yaml-comment-lines-and-blank-lines-between-keys
  ;; suite 5NYZ / P94K / J7VC
  (is (= {:key "value"}
         (yaml/parse-string "key:    # Comment\n      value\n")))
  (is (= {:key "value"}
         (yaml/parse-string "key:    # Comment\n        # lines\n      value\n\n\n")))
  (is (= {:one 2 :three 4}
         (yaml/parse-string "one: 2\n\n\nthree: 4\n"))))

(deftest yaml-value-which-looks-like-a-comment-marker
  ;; "# not a: key" after a value is a comment; '#' glued to text is not
  (is (= {:a "this is#not a comment"}
         (yaml/parse-string "a: this is#not a comment\n"))))

(deftest yaml-hash-needs-preceding-whitespace
  (is (= {(keyword "this is#not") "a comment"}
         (yaml/parse-string "this is#not: a comment\n"))))

(deftest yaml-empty-lines-at-end-of-document
  ;; suite NHX8: the empty key is the null scalar, so it parses to nil
  (is (= {nil nil}
         (yaml/parse-string ":\n\n\n"))))

(deftest yaml-empty-key-and-value
  ;; suite SM9W / UKK6 / NKF9 / S3PD
  (is (= {nil nil} (yaml/parse-string ":\n")))
  (is (= {(keyword ":") nil} (yaml/parse-string "::\n")))
  (is (= [{nil nil}] (yaml/parse-string "- :\n")))
  (is (= {nil nil :key "value"}
         (yaml/parse-string "key: value\n: \n")))
  (is (= {(keyword "plain key") "in-line value"
          nil nil
          (keyword "quoted key") ["entry"]}
         (yaml/parse-string "plain key: in-line value\n: # Both empty\n\"quoted key\":\n- entry\n"))))

;;; Block sequences

(deftest yaml-sequence-of-scalars
  ;; suite FQ7F, spec 2.1
  (is (= ["Mark McGwire" "Sammy Sosa" "Ken Griffey"]
         (yaml/parse-string "- Mark McGwire\n- Sammy Sosa\n- Ken Griffey\n"))))

(deftest yaml-multiple-entry-block-sequence
  (is (= ["foo" "bar" 42]
         (yaml/parse-string "- foo\n- bar\n- 42\n"))))

(deftest yaml-sequence-of-mappings
  ;; suite 229Q, spec 2.4
  (is (= [{:name "Mark McGwire" :hr 65 :avg 0.278}
          {:name "Sammy Sosa" :hr 63 :avg 0.288}]
         (yaml/parse-string "-\n  name: Mark McGwire\n  hr:   65\n  avg:  0.278\n-\n  name: Sammy Sosa\n  hr:   63\n  avg:  0.288\n"))))

(deftest yaml-sequence-in-sequence
  (is (= [["s1_i1" "s1_i2"] "s2"]
         (yaml/parse-string "- - s1_i1\n  - s1_i2\n- s2\n"))))

(deftest yaml-mappings-in-sequences
  (is (= [{:a 1} {:b 2}]
         (yaml/parse-string "- a: 1\n- b: 2\n"))))

(deftest yaml-sequence-under-key-indented
  ;; suite PBJ2, spec 2.3
  (is (= {:american ["Boston Red Sox" "Detroit Tigers" "New York Yankees"]
          :national ["New York Mets" "Chicago Cubs" "Atlanta Braves"]}
         (yaml/parse-string "american:\n  - Boston Red Sox\n  - Detroit Tigers\n  - New York Yankees\nnational:\n  - New York Mets\n  - Chicago Cubs\n  - Atlanta Braves\n"))))

(deftest yaml-sequence-under-key-at-parent-indent
  ;; suite AZ63 / RLU9: the sequence may sit at the key's own indent
  (is (= {:foo [42] :bar [44]}
         (yaml/parse-string "foo:\n- 42\nbar:\n  - 44\n")))
  (is (= {:foo [nil {:bar [2 3]}]}
         (yaml/parse-string "foo:\n-\n- bar:\n  - 2\n  - 3\n"))))

(deftest yaml-block-sequence-entry-types
  ;; suite W42U, spec 8.15
  (is (= [nil "block node\n" ["one" "two"] {:one "two"}]
         (yaml/parse-string "- # Empty\n- |\n block node\n- - one # Compact\n  - two # sequence\n- one: two # Compact mapping\n"))))

(deftest yaml-sequence-with-mixed-entry-shapes
  ;; suite M6YH
  (is (= ["x\n" {:foo "bar"} [42]]
         (yaml/parse-string "- |\n x\n-\n foo: bar\n-\n - 42\n"))))

(deftest yaml-block-sequence-in-block-mapping
  ;; suite 8QBE, spec 8.14
  (is (= {(keyword "block sequence") ["one" {:two "three"}]}
         (yaml/parse-string "block sequence:\n  - one\n  - two : three\n"))))

(deftest yaml-compact-nested-mapping
  ;; suite 9U5K, spec 2.12
  (is (= [{:item "Super Hoop" :quantity 1}
          {:item "Basketball" :quantity 4}
          {:item "Big Shoes" :quantity 1}]
         (yaml/parse-string "---\n# Products purchased\n- item    : Super Hoop\n  quantity: 1\n- item    : Basketball\n  quantity: 4\n- item    : Big Shoes\n  quantity: 1\n"))))

;;; Plain scalars and folding

(deftest yaml-plain-multiline-value
  ;; suite A984
  (is (= {:a "b c" :d "e f"}
         (yaml/parse-string "a: b\n c\nd:\n e\n  f\n"))))

(deftest yaml-plain-multiline-with-empty-line
  (is (= "a b\nc"
         (yaml/parse-string "a\nb\n\nc\n"))))

(deftest yaml-plain-multiline-at-top-level
  ;; suite 9YRD, spec example
  (is (= "a b c d\ne"
         (yaml/parse-string "a\nb  \n  c\nd\n\ne\n"))))

(deftest yaml-plain-multiline-with-tab-only-empty-line
  ;; suite NB6Z: a tab-only line inside a plain scalar is an empty line
  (is (= {:key "value with\ntabs"}
         (yaml/parse-string "key:\n  value\n  with\n  \t\n  tabs\n"))))

(deftest yaml-plain-multiline-swallowing-dash-line
  ;; suite AB8U: a deeper "- ..." line continues the plain scalar
  (is (= ["single multiline - sequence entry"]
         (yaml/parse-string "- single multiline\n - sequence entry\n"))))

(deftest yaml-plain-and-quoted-multiline-mapping
  ;; suite 4CQQ, spec 2.18
  (is (= {:plain "This unquoted scalar spans many lines."
          :quoted "So does this quoted scalar.\n"}
         (yaml/parse-string "plain:\n  This unquoted scalar\n  spans many lines.\n\nquoted: \"So does this\n  quoted scalar.\\n\"\n"))))

(deftest yaml-plain-scalar-with-backslashes
  ;; suite 4V8U shape: backslashes are plain content
  (is (= {:a "C:\\path\\to\\file"}
         (yaml/parse-string "a: C:\\path\\to\\file\n"))))

(deftest yaml-plain-keys-with-symbols
  ;; suite 2EBW
  (is (= {(keyword "a!\"#$%&'()*+,-./09:;<=>?@AZ[\\]^_`az{|}~") "safe"
          :?foo "safe question mark"
          (keyword ":foo") "safe colon"
          :-foo "safe dash"}
         (yaml/parse-string "a!\"#$%&'()*+,-./09:;<=>?@AZ[\\]^_`az{|}~: safe\n?foo: safe question mark\n:foo: safe colon\n-foo: safe dash\n"))))

(deftest yaml-plain-key-ending-with-colons
  ;; suite 8CWC: the last ": " colon is the separator
  (is (= {(keyword "key ends with two colons::") "value"}
         (yaml/parse-string "---\nkey ends with two colons::: value\n"))))

(deftest yaml-plain-keys-with-quote-and-bracket-chars
  ;; suite AZW3
  (is (= [{(keyword "bla\"keks") "foo"} {(keyword "bla]keks") "foo"}]
         (yaml/parse-string "- bla\"keks: foo\n- bla]keks: foo\n"))))

;;; Quoted scalars

(deftest yaml-single-and-double-quoted
  (is (= {:single "text" :double "text"}
         (yaml/parse-string "single: 'text'\ndouble: \"text\"\n"))))

(deftest yaml-single-quote-escaping
  ;; suite 4GC6, spec 7.7
  (is (= "here's to \"quotes\""
         (yaml/parse-string "'here''s to \"quotes\"'\n"))))

(deftest yaml-single-quoted-with-backslashes
  ;; suite 6H3V: single quotes keep backslashes raw
  (is (= {:a "C:\\path\\no\ttab"}
         (yaml/parse-string "a: 'C:\\path\\no\ttab'\n"))))

(deftest yaml-quoted-scalars-escape-set
  ;; suite G4RS, spec 2.17
  (is (= {:unicode (str "Sosa did fine." (char 0x263A))
          :control "\b1998\t1999\t2000\n"
          (keyword "hex esc") "\r\n is \r\n"
          :single "\"Howdy!\" he cried."
          :quoted " # Not a 'comment'."
          (keyword "tie-fighter") "|\\-*-/|"}
         (yaml/parse-string "unicode: \"Sosa did fine.\\u263A\"\ncontrol: \"\\b1998\\t1999\\t2000\\n\"\nhex esc: \"\\x0d\\x0a is \\r\\n\"\n\nsingle: '\"Howdy!\" he cried.'\nquoted: ' # Not a ''comment''.'\ntie-fighter: '|\\-*-/|'\n"))))

(deftest yaml-single-quoted-line-folding
  ;; suite PRH3, spec 7.9
  (is (= " 1st non-empty\n2nd non-empty 3rd non-empty "
         (yaml/parse-string "' 1st non-empty\n\n 2nd non-empty \n\t3rd non-empty '\n"))))

(deftest yaml-double-quoted-line-folding
  ;; suite 7A4E, spec 7.6
  (is (= " 1st non-empty\n2nd non-empty 3rd non-empty "
         (yaml/parse-string "\" 1st non-empty\n\n 2nd non-empty \n\t3rd non-empty \"\n"))))

(deftest yaml-double-quoted-escaped-line-break
  ;; suite NP9H, spec 7.5: the escaped break keeps surrounding whitespace
  (is (= "folded to a space,\nto a line feed, or \t \tnon-content"
         (yaml/parse-string "\"folded \nto a space,\t\n \nto a line feed, or \t\\\n \tnon-content\"\n"))))

(deftest yaml-flow-folding-in-double-quotes
  ;; suite 6WPF/TL85, spec 6.8
  (is (= " foo\nbar\nbaz "
         (yaml/parse-string "\"\n  foo \n \n    bar\n\n  baz\n\"\n"))))

(deftest yaml-empty-and-newline-only-quoted-strings
  ;; suite NAT4
  (is (= {:a " " :b " " :c " " :d " "
          :e "\n" :f "\n" :g "\n\n" :h "\n\n"}
         (yaml/parse-string "---\na: '\n  '\nb: '  \n  '\nc: \"\n  \"\nd: \"  \n  \"\ne: '\n\n  '\nf: \"\n\n  \"\ng: '\n\n\n  '\nh: \"\n\n\n  \"\n"))))

(deftest yaml-doublequoted-starting-with-tab
  ;; suite CPZ3
  (is (= {:tab "\tstring"}
         (yaml/parse-string "---\ntab: \"\tstring\"\n"))))

(deftest yaml-quoted-key-with-colon
  ;; suite 4UYU: a top level quoted scalar keeps its colon
  (is (= "foo: bar\": baz"
         (yaml/parse-string "\"foo: bar\\\": baz\"\n"))))

(deftest yaml-quoted-keys-with-odd-characters
  ;; suite 6SLA
  (is (= {(keyword "foo\nbar:baz\tx \\$%^&*()x") 23
          (keyword "x\\ny:z\\tx $%^&*()x") 24}
         (yaml/parse-string "\"foo\\nbar:baz\\tx \\\\$%^&*()x\": 23\n'x\\ny:z\\tx $%^&*()x': 24\n"))))

(deftest yaml-quoted-keys-and-spaces-around-colon
  ;; suite 26DV minus the anchors
  (is (= {(keyword "top1") {(keyword "key1") "scalar1"}
          (keyword "top2") {(keyword "key2") "scalar2"}
          :top5 "scalar5"
          :top6 {(keyword "key6") "scalar6"}}
         (yaml/parse-string "\"top1\" :\n  \"key1\" : scalar1\n'top2' :\n  'key2' : scalar2\ntop5   :    \n  scalar5\ntop6:\n  'key6' : scalar6\n"))))

;;; Block scalars: literal

(deftest yaml-literal-block-ascii-art
  ;; suite 6JQW, spec 2.13
  (is (= "\\//||\\/||\n// ||  ||__\n"
         (yaml/parse-string "--- |\n  \\//||\\/||\n  // ||  ||__\n"))))

(deftest yaml-literal-with-inner-blank-and-spaces
  ;; suite M29M
  (is (= {:a "ab\n\ncd\nef\n"}
         (yaml/parse-string "a: |\n ab\n \n cd\n ef\n \n\n...\n"))))

(deftest yaml-literal-keeps-tab-content
  ;; suite M9B4, spec 8.7
  (is (= "literal\n\ttext\n"
         (yaml/parse-string "|\n literal\n\ttext\n\n\n"))))

(deftest yaml-literal-leading-empty-lines
  ;; suite DWX9, spec 8.8
  (is (= "\n\nliteral\n \n\ntext\n"
         (yaml/parse-string "|\n \n  \n  literal\n   \n  \n  text\n\n # Comment\n"))))

(deftest yaml-literal-with-more-indented-lines
  ;; suite H2RW
  (is (= {:foo 1 :bar 2 :text "a\n  \nb\n\nc\n\nd\n"}
         (yaml/parse-string "foo: 1\n\nbar: 2\n    \ntext: |\n  a\n    \n  b\n\n  c\n \n  d\n"))))

(deftest yaml-literal-chomping-final-line-break
  ;; suite A6F9, spec 8.4
  (is (= {:strip "text" :clip "text\n" :keep "text\n"}
         (yaml/parse-string "strip: |-\n  text\nclip: |\n  text\nkeep: |+\n  text\n"))))

(deftest yaml-literal-chomping-trailing-lines
  ;; suite F8F9, spec 8.5
  (is (= {:strip "# text" :clip "# text\n" :keep "# text\n\n"}
         (yaml/parse-string " # Strip\n  # Comments:\nstrip: |-\n  # text\n  \n # Clip\n  # comments:\n\nclip: |\n  # text\n \n # Keep\n  # comments:\n\nkeep: |+\n  # text\n\n # Trail\n  # comments.\n"))))

(deftest yaml-literal-chomping-empty-content
  ;; suite K858, spec 8.6
  (is (= {:strip "" :clip "" :keep "\n"}
         (yaml/parse-string "strip: >-\n\nclip: >\n\nkeep: |+\n\n"))))

(deftest yaml-literal-strip-with-trailing-blanks
  ;; suite MYW6 / 753E
  (is (= "ab" (yaml/parse-string "|-\n ab\n \n \n...\n"))))

(deftest yaml-literal-keep-with-trailing-space-line
  ;; suite 6FWR / JEF9 / L24T
  (is (= "ab\n\n \n" (yaml/parse-string "--- |+\n ab\n \n  \n...\n")))
  (is (= ["\n\n"] (yaml/parse-string "- |+\n\n\n")))
  (is (= ["\n"] (yaml/parse-string "- |+\n   \n")))
  (is (= {:foo "x\n \n"} (yaml/parse-string "foo: |\n  x\n   \n"))))

(deftest yaml-literal-in-sequence-entry
  (is (= ["x\n"] (yaml/parse-string "- |\n x\n"))))

;;; Block scalars: folded

(deftest yaml-folded-newlines-become-spaces
  ;; suite 96L6, spec 2.14
  (is (= "Mark McGwire's year was crippled by a knee injury.\n"
         (yaml/parse-string "--- >\n  Mark McGwire's\n  year was crippled\n  by a knee injury.\n"))))

(deftest yaml-literal-and-folded-together
  ;; suite HMK4/5BVJ
  (is (= {:name "Mark McGwire"
          :accomplishment "Mark set a major league home run record in 1998.\n"
          :stats "65 Home Runs\n0.278 Batting Average\n"}
         (yaml/parse-string "name: Mark McGwire\naccomplishment: >\n  Mark set a major league\n  home run record in 1998.\nstats: |\n  65 Home Runs\n  0.278 Batting Average\n")))
  (is (= {:literal "some\ntext\n" :folded "some text\n"}
         (yaml/parse-string "literal: |\n  some\n  text\nfolded: >\n  some\n  text\n"))))

(deftest yaml-folded-empty-lines
  ;; suite TS54: 1 empty line is one newline, 2 are two
  (is (= "ab cd\nef\n\ngh\n"
         (yaml/parse-string ">\n ab\n cd\n \n ef\n\n\n gh\n"))))

(deftest yaml-folded-trimmed-empty-lines
  ;; suite K527, spec 6.6
  (is (= "trimmed\n\n\nas space"
         (yaml/parse-string ">-\n  trimmed\n  \n \n\n  as\n  space\n"))))

(deftest yaml-folded-lines-final-empty-lines
  ;; suite 7T8X, spec 8.10 through 8.13
  (is (= "\nfolded line\nnext line\n  * bullet\n\n  * list\n  * lines\n\nlast line\n"
         (yaml/parse-string ">\n\n folded\n line\n\n next\n line\n   * bullet\n\n   * list\n   * lines\n\n last\n line\n\n# Comment\n"))))

(deftest yaml-folded-more-indented-preserve-breaks
  ;; suite 6VJK, spec 2.15
  (is (= "Sammy Sosa completed another fine season with great stats.\n\n  63 Home Runs\n  0.288 Batting Average\n\nWhat a year!\n"
         (yaml/parse-string ">\n Sammy Sosa completed another\n fine season with great stats.\n\n   63 Home Runs\n   0.288 Batting Average\n\n What a year!\n"))))

(deftest yaml-folded-zero-indented-top-level
  ;; suite FP8R / DK3J
  (is (= "line1 line2 line3\n"
         (yaml/parse-string "--- >\nline1\nline2\nline3\n")))
  (is (= "line1 # no comment line3\n"
         (yaml/parse-string "--- >\nline1\n# no comment\nline3\n"))))

(deftest yaml-folded-with-explicit-indent-two
  ;; suite F6MC
  (is (= {:a " more indented\nregular\n" :b "\n\n more indented\nregular\n"}
         (yaml/parse-string "---\na: >2\n   more indented\n  regular\nb: >2\n\n\n   more indented\n  regular\n"))))

(deftest yaml-block-scalar-headers-with-comments
  ;; suite P2AD, spec 8.1
  (is (= ["literal\n" " folded\n" "keep\n\n" " strip"]
         (yaml/parse-string "- | # Empty header\n literal\n- >1 # Indentation indicator\n  folded\n- |+ # Chomping indicator\n keep\n\n- >1- # Both indicators\n  strip\n"))))

(deftest yaml-block-indentation-indicator
  ;; suite R4YG, spec 8.2
  (is (= ["detected\n" "\n\n# detected\n" " explicit\n" "\t\ndetected\n"]
         (yaml/parse-string "- |\n detected\n- >\n \n  \n  # detected\n- |1\n  explicit\n- >\n \t\n detected\n"))))

;;; Flow collections

(deftest yaml-sequence-of-sequences-flow
  ;; suite YD5X, spec 2.5
  (is (= [["name" "hr" "avg"]
          ["Mark McGwire" 65 0.278]
          ["Sammy Sosa" 63 0.288]]
         (yaml/parse-string "- [name        , hr, avg  ]\n- [Mark McGwire, 65, 0.278]\n- [Sammy Sosa  , 63, 0.288]\n"))))

(deftest yaml-mapping-of-mappings-flow
  ;; suite ZF4X, spec 2.6
  (is (= {(keyword "Mark McGwire") {:hr 65 :avg 0.278}
          (keyword "Sammy Sosa") {:hr 63 :avg 0.288}}
         (yaml/parse-string "Mark McGwire: {hr: 65, avg: 0.278}\nSammy Sosa: {\n    hr: 63,\n    avg: 0.288\n  }\n"))))

(deftest yaml-flow-mapping
  (is (= {:foo "you" :bar "far"}
         (yaml/parse-string "{foo: you, bar: far}\n"))))

(deftest yaml-flow-mapping-separate-values
  ;; suite 4ABK: spaces around colons, bare keys, omitted values
  (is (= {(keyword "unquoted") "separate"
          (keyword "http://foo.com") nil
          (keyword "omitted value") nil}
         (yaml/parse-string "{\nunquoted : \"separate\",\nhttp://foo.com,\nomitted value:,\n}\n"))))

(deftest yaml-flow-whitespace-and-multiline
  ;; suite LP6E
  (is (= [["a" "b" "c"] {:a "b" :c "d" :e "f"} []]
         (yaml/parse-string "- [a, b , c ]\n- { \"a\"  : b\n   , c : 'd' ,\n   e   : \"f\"\n  }\n- [      ]\n"))))

(deftest yaml-flow-comment-before-comma
  ;; suite 7TMG
  (is (= ["word1" "word2"]
         (yaml/parse-string "---\n[ word1\n# comment\n, word2]\n"))))

(deftest yaml-flow-comments-inside-sequence
  ;; suite 6HB6, spec 6.1
  (is (= {(keyword "Not indented")
          {(keyword "By one space") "By four\n  spaces\n"
           (keyword "Flow style") ["By two" "Also by two" "Still by two"]}}
         (yaml/parse-string " # Leading comment line spaces are\n  # neither content nor indentation.\n    \nNot indented:\n By one space: |\n    By four\n      spaces\n Flow style: [    # Leading spaces\n   By two,        # in flow style\n  Also by two,    # are neither\n  \tStill by two   # content nor\n    ]             # indentation.\n"))))

(deftest yaml-plain-url-in-flow
  (is (= [{:url "http://example.org"}]
         (yaml/parse-string "- { url: http://example.org }\n"))))

(deftest yaml-scalars-starting-with-syntax-chars
  ;; suite HM87 / S7BG / 58MP
  (is (= [":x"] (yaml/parse-string "[:x]\n")))
  (is (= ["?x"] (yaml/parse-string "[?x]\n")))
  (is (= [":,"] (yaml/parse-string "---\n- :,\n")))
  (is (= {:x ":x"} (yaml/parse-string "{x: :x}\n"))))

(deftest yaml-question-marks-in-scalars
  ;; suite JR7V
  (is (= ["a?string" "another ? string" {:key "value?"}
          ["a?string"] ["another ? string"]
          {:key "value?"} {:key "value?"} {:key? "value"}]
         (yaml/parse-string "- a?string\n- another ? string\n- key: value?\n- [a?string]\n- [another ? string]\n- {key: value? }\n- {key: value?}\n- {key?: value }\n"))))

(deftest yaml-colon-adjacent-after-quoted-key
  ;; suite 5T43 / 5MUD / K3WX / 4MUZ
  (is (= [{:key "value"} {:key ":value"}]
         (yaml/parse-string "- { \"key\":value }\n- { \"key\"::value }\n")))
  (is (= {(keyword "foo") "bar"}
         (yaml/parse-string "---\n{ \"foo\"\n  :bar }\n")))
  (is (= {(keyword "foo") "bar"}
         (yaml/parse-string "---\n{ \"foo\" # comment\n  :bar }\n")))
  (is (= {(keyword "foo") "bar"}
         (yaml/parse-string "{foo\n: bar}\n"))))

(deftest yaml-nested-flow-on-one-line
  (is (= {:a [1 2 {:b [3 {:c 4}]}] :d {}}
         (yaml/parse-string "a: [1, 2, {b: [3, {c: 4}]}]\nd: {}\n"))))

(deftest yaml-empty-flow-collections
  (is (= [[] {}] (yaml/parse-string "[[], {}]\n"))))

(deftest yaml-deeply-nested-flow-over-lines
  ;; suite ZK9H
  (is (= {:key [[["value"]]]}
         (yaml/parse-string "{ key: [[[\n  value\n ]]]\n}\n"))))

(deftest yaml-flow-multiline-under-block-key
  ;; suite VJP3 second shape
  (is (= {:k {:k "v"}}
         (yaml/parse-string "k: {\n k\n :\n v\n }\n")))

  ;; suite VJP3 first shape: column zero continuations are an error
  (is (yaml-err-at "k: {\nk\n:\nv\n}\n" :bad-indentation 2 1)))

(deftest yaml-flow-multiline-key-and-percent
  ;; suite UT92, spec 9.4
  (is (= [{(keyword "matches %") 20} nil]
         (yaml/parse-string-all "---\n{ matches\n% : 20 }\n...\n---\n# Empty\n...\n"))))

(deftest yaml-single-pair-implicit-entries
  ;; suite 9MMW / CFD4
  (is (= [[{:YAML "separate"}] [{(keyword "JSON like") "adjacent"}]]
         (yaml/parse-string "- [ YAML : separate ]\n- [ \"JSON like\":adjacent ]\n")))
  (is (= [[{nil "empty key"}] [{nil "another empty key"}]]
         (yaml/parse-string "- [ : empty key ]\n- [: another empty key]\n")))
  ;; the third 9MMW entry has a collection key: out of subset
  (is (yaml-err-reason "- [ {JSON: like}:adjacent ]\n" :unsupported-complex-key)))

(deftest yaml-question-mark-starting-flow-key
  ;; suite 652Z
  (is (= {:?foo "bar" :bar 42}
         (yaml/parse-string "{ ?foo: bar,\nbar: 42\n}\n"))))

(deftest yaml-empty-flow-keys-and-values
  ;; suite NKF9 documents 2 and 4
  (is (= {nil nil}
         (yaml/parse-string "{ : }\n"))))

;;; Documents and streams

(deftest yaml-empty-stream-has-no-documents
  ;; suite AVM7 / 98YD / 8G76 / HWV9 / QT73
  (is (nil? (yaml/parse-string "")))
  (is (= [] (yaml/parse-string-all "")))
  (is (nil? (yaml/parse-string "# Comment only.\n")))
  (is (= [] (yaml/parse-string-all "  # Comment\n    \n\n\n")))
  (is (= [] (yaml/parse-string-all "...\n")))
  (is (= [] (yaml/parse-string-all "# comment\n...\n"))))

(deftest yaml-explicit-document-with-comment
  ;; suite J9HZ, spec 2.9
  (is (= {:hr ["Mark McGwire" "Sammy Sosa"]
          :rbi ["Sammy Sosa" "Ken Griffey"]}
         (yaml/parse-string "---\nhr: # 1998 hr ranking\n  - Mark McGwire\n  - Sammy Sosa\nrbi:\n  # 1998 rbi ranking\n  - Sammy Sosa\n  - Ken Griffey\n"))))

(deftest yaml-multi-doc-play-by-play
  ;; suite U9NS, spec 2.8: parse-string takes the first document
  (is (= {:time "20:03:20" :player "Sammy Sosa" :action "strike (miss)"}
         (yaml/parse-string "---\ntime: 20:03:20\nplayer: Sammy Sosa\naction: strike (miss)\n...\n---\ntime: 20:03:47\nplayer: Sammy Sosa\naction: grand slam\n...\n")))
  (is (= [{:time "20:03:20" :player "Sammy Sosa" :action "strike (miss)"}
          {:time "20:03:47" :player "Sammy Sosa" :action "grand slam"}]
         (yaml/parse-string-all "---\ntime: 20:03:20\nplayer: Sammy Sosa\naction: strike (miss)\n...\n---\ntime: 20:03:47\nplayer: Sammy Sosa\naction: grand slam\n...\n"))))

(deftest yaml-log-file-three-docs
  ;; suite RZT7, spec 2.28
  (is (= [{:Time "2001-11-23 15:01:42 -5" :User "ed"
           :Warning "This is an error message for the log file"}
          {:Time "2001-11-23 15:02:31 -5" :User "ed"
           :Warning "A slightly different error message."}
          {:Date "2001-11-23 15:03:17 -5" :User "ed"
           :Fatal "Unknown variable \"bar\""
           :Stack [{:file "TopClass.py" :line 23 :code "x = MoreObject(\"345\\n\")\n"}
                   {:file "MoreClass.py" :line 58 :code "foo = bar"}]}]
         (yaml/parse-string-all "---\nTime: 2001-11-23 15:01:42 -5\nUser: ed\nWarning:\n  This is an error message\n  for the log file\n---\nTime: 2001-11-23 15:02:31 -5\nUser: ed\nWarning:\n  A slightly different error\n  message.\n---\nDate: 2001-11-23 15:03:17 -5\nUser: ed\nFatal:\n  Unknown variable \"bar\"\nStack:\n  - file: TopClass.py\n    line: 23\n    code: |\n      x = MoreObject(\"345\\n\")\n  - file: MoreClass.py\n    line: 58\n    code: |-\n      foo = bar\n"))))

(deftest yaml-scalar-docs-with-trailing-comments
  ;; suite L383
  (is (= ["foo" "foo"]
         (yaml/parse-string-all "--- foo  # comment\n--- foo  # comment\n"))))

(deftest yaml-document-start-on-last-line
  ;; suite PUW8: a bare trailing --- opens an empty document
  (is (= [{:a "b"} nil]
         (yaml/parse-string-all "---\na: b\n---\n"))))

(deftest yaml-two-empty-explicit-documents
  ;; suite 6XDY
  (is (= [nil nil]
         (yaml/parse-string-all "---\n---\n"))))

(deftest yaml-document-with-footer
  ;; suite S4T7 / 7Z25
  (is (= {:aaa "bbb"} (yaml/parse-string "aaa: bbb\n...\n")))
  (is (= ["scalar1" {:key "value"}]
         (yaml/parse-string-all "---\nscalar1\n...\nkey: value\n"))))

(deftest yaml-bare-documents-and-footers
  ;; suite M7A3, spec 9.3: comment-only regions emit no document
  (is (= ["Bare document" "%!PS-Adobe-2.0 # Not the first line\n"]
         (yaml/parse-string-all "Bare\ndocument\n...\n# No document\n...\n|\n  %!PS-Adobe-2.0 # Not the first line\n"))))

(deftest yaml-scalar-doc-containing-dots-in-quotes
  ;; suite 9MQT: "...x" without a space is content, "... x" is a marker
  (is (= ["a ...x b"]
         (yaml/parse-string-all "--- \"a\n...x\nb\"\n")))
  (is (yaml-err-reason "--- \"a\n... x\nb\"\n" :doc-marker)))

(deftest yaml-content-on-the-marker-line
  ;; suite L383 shape and K54U
  (is (= "scalar" (yaml/parse-string "---\tscalar\n")))
  (is (= 42 (yaml/parse-string "--- 42\n"))))

;;; Scalar resolution: YAML 1.2 core schema

(deftest yaml-resolves-booleans
  (let [m (yaml/parse-string "a: true\nb: True\nc: TRUE\nd: false\ne: False\nf: FALSE\n")]
    (is (true? (:a m)))
    (is (true? (:b m)))
    (is (true? (:c m)))
    (is (false? (:d m)))
    (is (false? (:e m)))
    (is (false? (:f m))))
  (is (= {:a "yes" :b "no" :c "on" :d "off" :e "y" :f "n" :g "tRue"}
         (yaml/parse-string "a: yes\nb: no\nc: on\nd: off\ne: y\nf: n\ng: tRue\n"))))

(deftest yaml-resolves-nulls
  (let [m (yaml/parse-string "a: null\nb: Null\nc: NULL\nd: ~\ne: \n")]
    (is (nil? (:a m)))
    (is (nil? (:b m)))
    (is (nil? (:c m)))
    (is (nil? (:d m)))
    (is (nil? (:e m))))
  (is (= {:a "NuLL" :b "nULL" :c "None"}
         (yaml/parse-string "a: NuLL\nb: nULL\nc: None\n"))))

(deftest yaml-resolves-integers
  (is (= {:a 0 :b 0 :c 7 :d -7 :e 7 :f 123456789012345}
         (yaml/parse-string "a: 0\nb: -0\nc: 7\nd: -7\ne: +7\nf: 123456789012345\n")))
  (is (= {:a 26 :b 15 :c 17}
         (yaml/parse-string "a: 0x1A\nb: 0o17\nc: 017\n")))
  (is (= {:a 9223372036854775807 :b -9223372036854775808}
         (yaml/parse-string "a: 9223372036854775807\nb: -9223372036854775808\n")))
  ;; core schema has no signed radix and no binary/underscore/sexagesimal forms
  (is (= {:a "-0x1A" :b "+0x1A" :c "0b101" :d "1_000" :e "1:30" :f "12:34:56"}
         (yaml/parse-string "a: -0x1A\nb: +0x1A\nc: 0b101\nd: 1_000\ne: 1:30\nf: 12:34:56\n")))
  (is (yaml-err-at "a: 9223372036854775808\n" :int-overflow 1 4)))

(deftest yaml-resolves-floats
  (is (= {:a 0.0 :b -0.5 :c 0.5 :d 5.0}
         (yaml/parse-string "a: 0.0\nb: -0.5\nc: .5\nd: 5.\n")))
  (is (= {:a 1000.0 :b 0.0125 :c 1000.0}
         (yaml/parse-string "a: 1e3\nb: 1.25e-2\nc: 1E+3\n")))
  (let [m (yaml/parse-string "a: .inf\nb: -.Inf\nc: .INF\nd: .nan\ne: .NaN\nf: +.inf\n")]
    (is (= ##Inf (:a m)))
    (is (= ##-Inf (:b m)))
    (is (= ##Inf (:c m)))
    (is (NaN? (:d m)))
    (is (NaN? (:e m)))
    (is (= ##Inf (:f m))))
  (is (= {:a ##Inf} (yaml/parse-string "a: 1e400\n")))
  (is (= {:a "0x1p3" :b "1.2.3" :c "." :d "-.nan"}
         (yaml/parse-string "a: 0x1p3\nb: 1.2.3\nc: .\nd: -.nan\n"))))

(deftest yaml-timestamps-stay-strings
  (is (= {:a "2001-12-14" :b "2001-12-14t21:59:43.10-05:00" :c "2001-12-14 21:59:43.10 -5"}
         (yaml/parse-string "a: 2001-12-14\nb: 2001-12-14t21:59:43.10-05:00\nc: 2001-12-14 21:59:43.10 -5\n"))))

(deftest yaml-keys-resolve-too
  (is (= {23 "x" true "yes" :a 1}
         (yaml/parse-string "23: x\ntrue: yes\na: 1\n"))))

(deftest yaml-duplicate-keys-last-wins
  (is (= {:a 2} (yaml/parse-string "a: 1\na: 2\n")))
  (is (= {:m {:x 2}} (yaml/parse-string "m:\n  x: 1\n  x: 2\n")))
  (is (= {1 "b" 2 "c"} (yaml/parse-string "1: a\n1: b\n2: c\n"))))

;;; Options and API surface

(deftest yaml-keywords-false-keeps-string-keys
  (is (= {"a" {"b" [1 {"c" "d"}]}}
         (yaml/parse-string "a:\n  b: [1, {c: d}]\n" {:keywords false})))
  (is (= {:a {:b [1 {:c "d"}]}}
         (yaml/parse-string "a:\n  b: [1, {c: d}]\n" {:keywords true})))
  (is (= {nil 1}
         (yaml/parse-string ": 1\n" {:keywords false}))))

(deftest yaml-parse-string-argument-validation
  (is (thrown? (yaml/parse-string 42)))
  (is (thrown? (yaml/parse-string nil)))
  (is (thrown? (yaml/parse-string "a: 1\n" :not-a-map)))
  (is (thrown? (yaml/parse-string "a: 1\n" {:keywords :yes}))))

(deftest yaml-parse-string-all-shape
  (is (vector? (yaml/parse-string-all "a: 1\n")))
  (is (= [{:a 1} 2 "three"] (yaml/parse-string-all "a: 1\n--- 2\n--- three\n"))))

(deftest yaml-crlf-input
  (is (= {:a [1 2] :b "line\n"}
         (yaml/parse-string "a:\r\n  - 1\r\n  - 2\r\nb: |\r\n  line\r\n"))))

;;; Errors: indentation and tabs

(deftest yaml-error-tab-indentation
  ;; suite 4EJS
  (is (yaml-err-at "a:\n\tb:\n\t\tc: value\n" :tab-indentation 2 1)))

(deftest yaml-error-wrong-indentation-in-mapping
  ;; suite DMG6 / EW3V / N4JP / U44R
  (is (yaml-err-at "key:\n  ok: 1\n wrong: 2\n" :bad-indentation 3 1))
  (is (yaml-err-at "k1: v1\n k2: v2\n" :mapping-in-scalar 2 4))
  (is (yaml-err-at "map:\n  key1: \"quoted1\"\n key2: \"bad indentation\"\n" :bad-indentation 3 1))
  (is (yaml-err-at "map:\n  key1: \"quoted1\"\n   key2: \"bad indentation\"\n" :bad-indentation 3 1)))

(deftest yaml-error-mapping-in-plain-scalar
  ;; suite 2CMS / HU3P / ZCZ6 / 7MNF / 236B / GDY7
  (is (yaml-err-at "this\n is\n  invalid: x\n" :mapping-in-scalar 3 10))
  (is (yaml-err-at "key:\n  word1 word2\n  no: key\n" :mapping-in-scalar 3 5))
  (is (yaml-err-at "a: b: c: d\n" :mapping-in-scalar 1 5))
  (is (yaml-err-at "--- key1: value1\n    key2: value2\n" :mapping-in-scalar 1 9))
  (is (yaml-err-at "top1:\n  key1: val1\ntop2\n" :unexpected-content 3 1))
  (is (yaml-err-at "foo:\n  bar\ninvalid\n" :unexpected-content 3 1))
  (is (yaml-err-at "key: value\nthis is #not a: key\n" :unexpected-content 2 1)))

(deftest yaml-error-unexpected-content-after-values
  ;; suite JY7Z / Q4CL / SU5Z / 62EZ / P2EQ / KS4U / TD5N / BD7L / BS4K / 8XDJ / BF9H
  (is (yaml-err-at "key1: \"quoted1\"\nkey2: \"quoted2\" no key: nor value\nkey3: \"quoted3\"\n" :unexpected-content 2 17))
  (is (yaml-err-at "key1: \"quoted1\"\nkey2: \"quoted2\" trailing content\nkey3: \"quoted3\"\n" :unexpected-content 2 17))
  (is (yaml-err-at "key: \"value\"# invalid comment\n" :unexpected-content 1 13))
  (is (yaml-err-at "---\nx: { y: z }in: valid\n" :unexpected-content 2 12))
  (is (yaml-err-at "---\n- { y: z }- invalid\n" :unexpected-content 2 11))
  (is (yaml-err-at "[\nsequence item\n]\ninvalid item\n" :unexpected-content 4 1))
  (is (yaml-err-at "- item1\n- item2\ninvalid\n" :unexpected-content 3 1))
  (is (yaml-err-at "- item1\n- item2\ninvalid: x\n" :unexpected-content 3 1))
  (is (yaml-err-at "word1  # comment\nword2\n" :unexpected-content 2 1))
  (is (yaml-err-at "key: word1\n#  xxx\n  word2\n" :bad-indentation 3 1))
  (is (yaml-err-at "---\nplain: a\n       b # end of scalar\n       c\n" :bad-indentation 4 1))
  (is (yaml-err-at "a: 'b': c\n" :unexpected-content 1 7)))

(deftest yaml-error-sequence-indentation
  ;; suite 4HVU / ZVH3 / 6S55 / 9CWY / 5U3A
  (is (yaml-err-at "key:\n   - ok\n   - also ok\n  - wrong\n" :bad-indentation 4 1))
  (is (yaml-err-at "- key: value\n - item1\n" :bad-indentation 2 1))
  (is (yaml-err-at "key:\n - bar\n - baz\n invalid\n" :bad-indentation 4 1))
  (is (yaml-err-at "key:\n - item1\n - item2\ninvalid\n" :unexpected-content 4 1))
  (is (yaml-err-at "key: - a\n     - b\n" :unexpected-content 1 6)))

(deftest yaml-error-multiline-implicit-keys
  ;; suite 7LBH / JKF3 / QB6E
  (is (yaml-err-at "\"a\\nb\": 1\n\"c\n d\": 1\n" :multiline-key 2 1))
  (is (yaml-err-at "- - \"bar\nbar\": x\n" :bad-indentation 2 1))
  (is (yaml-err-at "---\nquoted: \"a\nb\nc\"\n" :bad-indentation 3 1)))

;;; Errors: quoted scalars

(deftest yaml-error-unterminated-and-invalid-escapes
  ;; suite CQ3W / 55WF
  (is (yaml-err-at "---\nkey: \"missing closing quote\n" :unterminated-quote 2 6))
  (is (yaml-err-at "\"\\.\"\n" :invalid-escape 1 2))
  (is (yaml-err-at "\"\\u00\"\n" :invalid-escape 1 2))
  (is (yaml-err-at "\"\\uD800\"\n" :bad-codepoint 1 2))
  (is (yaml-err-at "\"\\U00110000\"\n" :bad-codepoint 1 2))
  (is (yaml-err-at "a: 'unterminated\n" :unterminated-quote 1 4)))

;;; Errors: flow collections

(deftest yaml-error-flow-structure
  ;; suite 9MAG / CTN5 / T833 / CML9 / 4H7K / 6JTT / 9JBA / CVW2 / DK4H / ZXT5 / C2SP
  (is (yaml-err-at "---\n[ , a, b, c ]\n" :flow-syntax 2 3))
  (is (yaml-err-at "---\n[ a, b, c, , ]\n" :flow-syntax 2 12))
  (is (yaml-err-at "---\n{\n foo: 1\n bar: 2 }\n" :flow-syntax 4 2))
  (is (yaml-err-at "key: [ word1\n#  xxx\n  word2 ]\n" :flow-syntax 3 3))
  (is (yaml-err-at "---\n[ a, b, c ] ]\n" :unexpected-content 2 13))
  (is (yaml-err-at "---\n[ [ a, b, c ]\n" :unterminated-flow 2 1))
  (is (yaml-err-at "---\n[ a, b, c, ]#invalid\n" :unexpected-content 2 13))
  (is (yaml-err-at "---\n[ a, b, c,#invalid\n]\n" :unexpected-content 2 11))
  (is (yaml-err-at "---\n[ key\n  : value ]\n" :flow-syntax 3 3))
  (is (yaml-err-at "[ \"key\"\n  :value ]\n" :flow-syntax 2 3))
  (is (yaml-err-reason "[23\n]: 42\n" :unsupported-complex-key))
  (is (yaml-err-at "a: [1, 2}\n" :flow-syntax 1 9))
  ;; suite G5U8 / YJV2: a lone dash is not a plain scalar
  (is (yaml-err-reason "[-]\n" :flow-syntax))
  (is (yaml-err-reason "---\n- [-, -]\n" :flow-syntax))
  ;; suite 9C9N: flow continuations under a block key need indent
  (is (yaml-err-at "---\nflow: [a,\nb,\nc]\n" :bad-indentation 3 1)))

;;; Errors: block scalars

(deftest yaml-error-block-scalar-headers
  ;; suite S4GJ / X4QW / 2G84
  (is (yaml-err-at "---\nfolded: > first line\n  second line\n" :block-scalar-header 2 11))
  (is (yaml-err-at "block: ># comment\n  scalar\n" :block-scalar-header 1 9))
  (is (yaml-err-at "--- |0\n" :block-scalar-header 1 6))
  (is (yaml-err-at "--- |10\n" :block-scalar-header 1 7))
  (is (yaml-err-at "a: |x\n  b\n" :block-scalar-header 1 5)))

(deftest yaml-error-block-scalar-leading-indent
  ;; suite W9L4 / S98Z / 5LLU
  (is (yaml-err-at "---\nblock scalar: |\n     \n  more spaces at the beginning\n  are invalid\n" :block-scalar-indent 3 1))
  (is (yaml-err-at "empty block scalar: >\n \n  \n   \n # comment\n" :block-scalar-indent 3 1))
  (is (yaml-err-at "block scalar: >\n \n  \n   \n invalid\n" :block-scalar-indent 3 1)))

;;; Errors: document markers

(deftest yaml-error-document-markers
  ;; suite 3HFZ / N782 / 5TRB / 9MQT
  (is (yaml-err-at "---\nkey: value\n... invalid\n" :unexpected-content 3 5))
  (is (yaml-err-at "[\n--- ,\n...\n]\n" :doc-marker 2 1))
  (is (yaml-err-at "---\n\"\n---\n\"\n" :doc-marker 3 1))
  (is (yaml-err-at "--- \"a\n... x\nb\"\n" :doc-marker 2 1)))

;;; Out-of-subset constructs are errors with clear reasons

(deftest yaml-anchors-are-unsupported
  (is (yaml-err-at "&sequence\n- a\n" :unsupported-anchor 1 1))
  (is (yaml-err-at "key: &anchor value\n" :unsupported-anchor 1 6))
  (is (yaml-err-at "---\n- &named unicode anchor\n" :unsupported-anchor 2 3)))

(deftest yaml-aliases-are-unsupported
  (is (yaml-err-at "First: &a Foo\nSecond: *a\n" :unsupported-anchor 1 8))
  (is (yaml-err-reason "key: *alias\n" :unsupported-alias))
  (is (yaml-err-reason "- &a a\n- *a\n" :unsupported-anchor)))

(deftest yaml-tags-are-unsupported
  (is (yaml-err-at "- !!str a\n- b\n" :unsupported-tag 1 3))
  (is (yaml-err-reason "foo: !!seq\n  - a\n" :unsupported-tag))
  (is (yaml-err-reason "!!str a: b\n" :unsupported-tag))
  (is (yaml-err-reason "!invalid{}tag scalar\n" :unsupported-tag))
  (is (yaml-err-reason "--- !\n" :unsupported-tag)))

(deftest yaml-directives-are-unsupported
  (is (yaml-err-at "%YAML 1.2\n---\nDocument\n" :unsupported-directive 1 1))
  (is (yaml-err-reason "%FOO\n--- bar\n" :unsupported-directive))
  (is (yaml-err-reason "%TAG ! tag:example.com,2000:\n--- x\n" :unsupported-directive)))

(deftest yaml-complex-keys-are-unsupported
  (is (yaml-err-at "? - a\n: b\n" :unsupported-complex-key 1 1))
  (is (yaml-err-reason "? foo\n: bar\n" :unsupported-complex-key))
  (is (yaml-err-reason "? a\n: b\nc: d\n" :unsupported-complex-key))
  (is (yaml-err-reason "[a]: b\n" :unsupported-complex-key))
  (is (yaml-err-reason "{a: 1}: b\n" :unsupported-complex-key)))

;;; Real-world shape: a service configuration file

(def ^:private service-yaml
  "name: image-resizer
version: \"2.3\"
enabled: true
replicas: 3
timeout_s: 1.5
log_level: info
image: registry.internal/resizer:2.3
command: [\"resizer\", \"--workers\", \"4\", \"--queue\", \"resize\"]
env:
  RESIZE_BACKEND: gpu
  MAX_PIXELS: 4096
  TRACE_ENDPOINT: ~
resources:
  requests: {cpu: 0.5, memory: \"512Mi\"}
  limits:
    cpu: 2
    memory: 2Gi
backoff:
  base: 0.25
  retries: 8
buckets:
  - name: avatar
    max: 512
    strip_meta: true
    formats: [png, webp]
  - name: hero
    max: 2048
    strip_meta: false
    formats:
      - jpeg
      - webp
banner: |
  Care and feeding
  of the resizer service.
notes: >
  Folded release note text
  that becomes one line.
health: /healthz
ports:
  - {port: 8080, name: http}
  - {port: 9090, name: metrics}
")

(deftest yaml-service-config-document
  (is (= {:name "image-resizer"
          :version "2.3"
          :enabled true
          :replicas 3
          :timeout_s 1.5
          :log_level "info"
          :image "registry.internal/resizer:2.3"
          :command ["resizer" "--workers" "4" "--queue" "resize"]
          :env {:RESIZE_BACKEND "gpu" :MAX_PIXELS 4096 :TRACE_ENDPOINT nil}
          :resources {:requests {:cpu 0.5 :memory "512Mi"}
                      :limits {:cpu 2 :memory "2Gi"}}
          :backoff {:base 0.25 :retries 8}
          :buckets [{:name "avatar" :max 512 :strip_meta true :formats ["png" "webp"]}
                    {:name "hero" :max 2048 :strip_meta false :formats ["jpeg" "webp"]}]
          :banner "Care and feeding\nof the resizer service.\n"
          :notes "Folded release note text that becomes one line.\n"
          :health "/healthz"
          :ports [{:port 8080 :name "http"} {:port 9090 :name "metrics"}]}
         (yaml/parse-string service-yaml))))

(run-tests-and-exit)
