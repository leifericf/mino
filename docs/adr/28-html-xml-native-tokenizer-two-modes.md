# ADR 28: HTML and XML readers as one native tokenizer, two modes

Date: 2026-08-25

## Context

ki-24: zero HTML/XML surface while scraping is a documented top
babashka use case (nokogiri Ruby #15, bs4/lxml top-100, Jsoup
bb-builtin). The API contract is settled by the ecosystem research:
hickory's function vocabulary under mino.html and mino.html.select,
a JVM-shaped clojure.xml mirror for strict XML, hickory node maps
({:type :element :tag :attrs :content}) as the convergence shape
hickory, enlive, clojure.xml, and data.xml already share, with
clojure.zip/xml-zip (shipped) walking it unchanged.

Every prior wire-format reader measured mino-side Clojure 35-41x over
an absolute budget and was rewritten native (ADR 23 json, 24 csv, 25
toml, 26 yaml): five for five. The tree-assembly question for HTML was
therefore not re-spiked; the measured history forces the same answer.

## Decision

The readers are native prims that construct the node-map trees
directly C-side, single pass over a byte cursor, allocating maps,
vectors, and strings from byte spans exactly as json.c and toml.c do.
The open-element stack, implied-end-tag table, and implied wrapper
live in C. No event objects cross the boundary; a public token-stream
API is rejected until a customer exists.

One tokenizer core in one file carries two modes behind two prim
names, html-parse and xml-parse (names are the public registry; a
positional mode integer would violate the keyword-opts invariant).
The WHATWG byte machinery (tag states, attribute quoting, raw-text
scanning, entity decode, comment/doctype capture) is shared; the
modes differ by a flag set: XML adds case-sensitive names, CDATA and
PI capture, mandatory attribute quoting, single-root and prolog
checks, no implied closes, and its own error classes. HTML mode
implements the python-html.parser recovery tier plus an enumerated
fixup set (the full list is pinned in the campaign's technical
design): implied closes via like-tag scope-barrier rules with the
verbatim WHATWG p-closing list, stray end tags dropped, misnested end
tags pop-until, simplified html/head/body synthesis, EOF
auto-balance, nesting cap 256 with :max-depth in both modes. script
and style are RAWTEXT; title and textarea are RCDATA (references
resolve), matching the oracle. The trailing solidus on any start tag
closes immediately, diverging from the WHATWG HTML-content rule that
only exists to feed insertion modes we do not implement. HTML node
maps carry :type; XML returns the JVM clojure.xml shape (no :type,
comments/PIs/doctype dropped, root element only). Strict XML resolves
only the five predefined entities plus numeric references; any other
named entity or any internal-subset ENTITY declaration throws a
positioned error, which makes XXE structurally impossible. Errors are
positioned ex-info in the yaml contract (:kind :html/parse or
:xml/parse, :location line/col). Gates land in the same commit as
each reader: HTML 1MB page mix under 3000ms in-suite (pipeline 6000ms),
XML 1MB feed mix under 2000ms in-suite.

Corpus licensing, recorded here so the vendor-nothing constraint and
the vendored fixture are never read as conflicting: mino transcribes
INPUT strings only (data, not code, not expectations) from the
html5lib-tests tokenizer suite (MIT, verified from the raw LICENSE
2026-08-25, copyright 2006-2013 James Graham, Geoffrey Sneddon, and
contributors) into an EDN fixture and generates every expected value
through the local python3 oracle. The fixture header carries the
upstream URL and pinned commit sha, the inputs-only statement, and
the upstream MIT notice verbatim.

## Consequences

Two node shapes over one tokenizer instead of one unified shape;
cheaper than one shape with mode-dependent holes, but facade code and
tests are per-mode. The tolerance tier is ours to defend: no adoption
agency, foster parenting, or table formatting, so pathological pages
parse to a tree that is not the browser's tree. Budgets are absolute
and carry in-suite headroom per campaign history. The entity table is
generated from the python oracle, not vendored.

## Alternatives

An internal event stream with Clojure-side tree assembly was the
research's provisional answer and is superseded: it would be the last
unmeasured Clojure-assembled tree in a runtime whose every measured
attempt missed by an order of magnitude. Two tokenizer files were
rejected for duplicating the attribute and entity machinery and
doubling the security surface. A tolerant-only engine with a Clojure
strictness wrapper cannot check strictness after tolerant recovery;
the misparse would be silent. Resolving the HTML named entity set in
XML mode was rejected for breaking JVM clojure.xml guessability.
