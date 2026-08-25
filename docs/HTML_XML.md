# mino.html and clojure.xml

HTML and XML reading in plain data. One native tokenizer, two
modes: tolerant HTML into hickory-shaped node maps, strict XML into
the JVM clojure.xml shape. Design contract:
`docs/adr/28-html-xml-native-tokenizer-two-modes.md`. The full
reference is the docstring:

```
(require '[mino.html :as html] '[clojure.repl :refer [doc]])
(doc html/parse)
(doc html/to-html)
(doc html/as-hiccup)
```

The namespaces are ungated: reading markup is info-only, so every
embedder has them.

## The surface

| Call | Returns |
|------|---------|
| `(html/parse s)` | document node map, hickory shape |
| `(html/parse-fragment s)` | vector of top-level nodes, no wrappers |
| `(html/to-html node)` | HTML text (also serializes XML trees) |
| `(html/as-hiccup node)` | hiccup vectors, direct conversion |
| `(sel/select pred node)` | vector of matching nodes |
| `(sel/select-locs pred node)` | vector of zipper locs |
| `(xml/parse s)` | root element map, JVM clojure.xml shape |

Every call takes an optional trailing opts keyword map, reserved.
`clojure.zip/xml-zip` walks both node shapes unchanged (it touches
only `:content`).

## Node shapes

HTML mode returns hickory node maps. Elements carry
`{:type :element :tag keyword :attrs {keyword string} :content
[node-or-string]}` with lowercase tag and attribute names.
`{:type :comment :content [text]}` and
`{:type :document-type :content [text]}` keep their payloads
verbatim; text nodes are bare strings inside `:content`.
`parse` returns the document map `{:type :document :content [...]}`:
the DOCTYPE first when present, then comments, then exactly one
`html` root, synthesized with `head` and `body` when absent
(explicit tags win; head elements before body content route into
head). `parse-fragment` returns a vector of nodes with no wrapper
and no synthesis.

XML mode returns the JVM clojure.xml shape:
`{:tag keyword :attrs {keyword string} :content [string-or-node]}`
without `:type`; `:attrs` and `:content` are always present. Names
keep case; a QName prefix keywordizes at its first colon
(`dc:creator` reads back as `:dc/creator`). `parse` returns the
root element only; comments, processing instructions, and the
DOCTYPE are dropped. Character data merges across them into one
string per position; whitespace-only text and tails stay verbatim
inside the root and drop outside it. Attribute values get XML 1.0
3.3.3 whitespace normalization; line endings normalize to LF and a
leading UTF-8 BOM drops.

## The tolerance tier

HTML mode recovers; every rule below is pinned by tests.

- Unclosed tags close at the parent close or EOF (reverse order).
- Stray end tags (no match on the open-element stack) drop.
- Misnested end tags pop through the match.
- The WHATWG void list never awaits end tags.
- A trailing solidus self-closes every start tag, void or not.
- Attributes: single, double, unquoted, and valueless forms;
  duplicates keep the first; names lowercase; an unterminated
  quoted value at EOF drops the tag.
- `script` and `style` are RAWTEXT (verbatim interiors);
  `title` and `textarea` are RCDATA (references resolve).
- Character references decode through the python-oracle table
  (2231 entries): longest match, semicolonless legacy names in
  text, semicolon-terminated names only in attribute values;
  numeric references with range policing (NUL, surrogates, above
  0x10FFFF become U+FFFD); a bare ampersand that matches nothing
  stays literal.
- Incorrectly opened comments (`<? ... >`, `<!foo>`) become
  comments; unterminated comments close at EOF; CDATA in HTML
  content is a bogus comment.
- The first DOCTYPE is captured; later ones drop.
- PLAINTEXT makes the rest of the input one text run.
- NUL in text becomes U+FFFD.
- Open-element depth past 256 throws `:max-depth` (the only
  non-recovering edge besides the EOF tag drop).
- Like-tag implied closes: `li dd dt option optgroup p rb rp rt
  rtc tr td th tbody thead tfoot caption colgroup` close a like
  open element when no scope barrier (`table td th caption
  template html`) intervenes.
- The WHATWG p-closing list closes an open `p`; `h1`-`h6`
  additionally close an open `h1`-`h6`.
- Text inside a table but outside any cell stays a direct string
  child of the table element.

Out, documented as such: adoption agency and active-formatting
reconstruction, foster parenting, tbody/row/cell synthesis, foreign
content mode, and the script-data-escaped sub-states.

## Strictness

XML mode implements none of the tier; it errors by class. Only the
five predefined entities plus numeric character references resolve;
anything else throws `:undefined-entity`. A DOCTYPE is accepted and
dropped, but any internal-subset ENTITY declaration throws
`:unsupported-doctype`, which makes XXE impossible by construction:
no declaration is ever honored, nothing is ever fetched. Attribute
values must be quoted. One root element; character data outside it
throws. A `<?xml` declaration is valid only exactly lowercase at
byte 0. Strict XML never silently misparses.

## Errors

Both readers throw ex-info with `:kind` (`:html/parse` or
`:xml/parse`), a `:code` keyword, `:location {:line :col}` (1-based
over bytes), and `:text` (the source line). XML codes:
`:unexpected-eof`, `:unexpected-token`, `:undefined-entity`,
`:unsupported-doctype`, `:mismatched-end-tag`,
`:duplicate-attribute`, `:multiple-roots`, `:content-before-root`,
`:invalid-prolog`, `:invalid-name`, `:max-depth`. HTML v1 carries
`:max-depth` only. Argument misuse throws `:html/opts` or
`:xml/opts`.

## Serializing

`to-html` accepts any node map, a bare string, a collection of
nodes (fragment concatenation), or a JVM element map, so XML trees
round-trip through it on the shared shape. Normalization: names as
written; attribute order preserved; values double-quoted with amp
and quot escaped; adjacent text runs merge; void elements emit no
end tag; implied closes and wrappers materialize; comments and the
DOCTYPE text verbatim; RAWTEXT and PLAINTEXT interiors verbatim.
The contract: `(parse (to-html (parse s)))` equals `(parse s)` for
every input the tier accepts, and output is byte-exact over
canonical-form fixtures.

`as-hiccup` converts directly, no reparse: elements become
`[tag attrs children...]` with the attrs map always present,
documents a vector of converted children, text escaped (amp, lt,
gt, quot), comments and the DOCTYPE their literal source strings,
script and style interiors verbatim. The reverse direction stays
out until asked for.

## Selecting

`mino.html.select` composes predicates over zipper locs:

| Constructor | Matches |
|-------------|---------|
| `(sel/tag k)` | tag keyword |
| `(sel/id v)`, `(sel/class v)` | attribute shorthand |
| `(sel/attr k)`, `(sel/attr k pred)` | presence, or value under pred |
| `(sel/any)` | any element |
| `(sel/and ...)`, `(sel/or ...)`, `(sel/not ...)` | boolean composition |
| `(sel/child p q)`, `(sel/descendant p q)` | structural combination |
| `(sel/first-child)`, `(sel/last-child)`, `(sel/nth-child n)` | position among element siblings |

`(map sel/text (sel/select (sel/tag :p) node))` extracts the deep
text of every match. Selecting over a parse-fragment vector selects
per top-level node and concatenates the results.

## Performance

Absolute budgets over the generated 1MB fixtures (nightly lanes
exclude the perf and fuzz suite files):

| Gate | Budget | Measured at land |
|------|--------|------------------|
| 1MB page mix, HTML parse | 3000 ms in-suite | 128 ms |
| parse + select + to-html pipeline | 6000 ms in-suite | 1874 ms |
| 1MB pom/rss/svg mix, XML parse | 2000 ms in-suite | 98 ms |
| fuzz lanes: 1200 mutations per mode | 5 s per parse | 169 ms |

## Divergences from hickory and the JVM

Pinned by tests, one test each:

- `parse` returns the node map directly; no parser object exists,
  so hickory's two-step parse-then-convert call collapses to one.
- Malformed input a browser recovers from yields the recovered
  tree or a positioned ex-info where hickory never throws.
- XML input is a string first; the JVM takes File, InputStream, or
  URI, and none of those surfaces exist here.
- `clojure.xml/parse` throws positioned ex-info with location data;
  the JVM throws SAX exceptions without positions in the tree API.
- Undeclared namespace prefixes parse (the JVM's
  non-namespace-aware SAX behavior); character data merges across
  comments and PIs where the JVM emits one string per SAX event.
- `as-hiccup` renders the DOCTYPE from the raw text the node
  carries; hickory renders structured name/publicid/systemid attrs.
- `select` over a vector selects per node and concatenates;
  hickory.select over a vector returns an empty vector.
- opts are keyword maps, reserved and ignored in v1.
