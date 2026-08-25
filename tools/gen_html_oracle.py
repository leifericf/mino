#!/usr/bin/env python3
"""The html-xml campaign's tolerant-HTML oracle dump script (ADR 28,
design D11).

Runs the python3 stdlib oracle (html.parser.HTMLParser,
convert_charrefs=True) over EDN input fixtures and writes the golden
EDN vector file the p2 reader is pinned against. Every expected
value is generated here, locally, from the oracle; input fixtures
carry only inputs (html5lib tokenizer transcriptions carry inputs
only by the D4 reconciliation recorded in ADR 28).

Events are the tokenizer pin; :tree is a naive stack reconstruction
applying only tier rules 1-4 (EOF auto-balance, stray end tags
dropped, misnested end tags pop-until, void elements never await
end tags). The remaining fixup rules (implied closes, p-closing,
html/head/body synthesis, table text placement) are NOT oracle
territory: python does no end-tag matching, so those rules are
spec-cited and pinned by the hand-written tolerance vectors in
tests/html_test.clj.

Small documented transforms are applied to the raw oracle stream
and named one line each in the divergence ledger written at the top
of the output file; known divergences we do NOT transform are named
there too.

Deterministic by construction: input order in, file order out, no
timestamps, no dict-order dependence. Regeneration must yield an
empty diff:

    python3 tools/gen_html_oracle.py -o tests/fixtures/html/golden.edn \\
        tests/fixtures/html/html5lib_tokenizer_inputs.edn \\
        tests/fixtures/html/curated_pages.edn

Oracle host: python3 html.parser as of 3.14 (RCDATA title/textarea,
RAWTEXT script/style/xmp/iframe/noembed/noframes, PLAINTEXT
rest-of-input; tolerant attribute regexes).

Usage: gen_html_oracle.py -o OUT.edn IN.edn [IN.edn ...]
"""

import argparse
import re
import sys
from html.parser import HTMLParser

# Tier rule 4, the WHATWG void list the design pins.
VOID_ELEMENTS = frozenset(
    "area base br col embed hr img input link meta param source track wbr".split()
)

DOCTYPE_STRIP = re.compile(r"^doctype\s+", re.IGNORECASE)


# ---- EDN reading (restricted dialect: vector of maps, keyword keys,
# ---- string values; the strings use \\" \\\\ \\n \\r \\t \\b \\f \\uXXXX) ---


class EdnError(Exception):
    pass


def _read_edn(text):
    pos = [0]
    n = len(text)

    def ws():
        while pos[0] < n:
            c = text[pos[0]]
            if c in " \t\r\n,":
                pos[0] += 1
            elif c == ";":
                while pos[0] < n and text[pos[0]] != "\n":
                    pos[0] += 1
            else:
                break

    def expr():
        ws()
        if pos[0] >= n:
            raise EdnError("unexpected end of input")
        c = text[pos[0]]
        if c == "[":
            pos[0] += 1
            out = []
            while True:
                ws()
                if pos[0] < n and text[pos[0]] == "]":
                    pos[0] += 1
                    return out
                out.append(expr())
        if c == "{":
            pos[0] += 1
            out = {}
            while True:
                ws()
                if pos[0] < n and text[pos[0]] == "}":
                    pos[0] += 1
                    return out
                k = expr()
                v = expr()
                if k in out:
                    raise EdnError("duplicate map key %r" % (k,))
                out[k] = v
        if c == '"':
            pos[0] += 1
            buf = []
            while True:
                if pos[0] >= n:
                    raise EdnError("unterminated string")
                ch = text[pos[0]]
                pos[0] += 1
                if ch == '"':
                    return "".join(buf)
                if ch == "\\":
                    if pos[0] >= n:
                        raise EdnError("unterminated escape")
                    e = text[pos[0]]
                    pos[0] += 1
                    if e == "n":
                        buf.append("\n")
                    elif e == "t":
                        buf.append("\t")
                    elif e == "r":
                        buf.append("\r")
                    elif e == "b":
                        buf.append("\b")
                    elif e == "f":
                        buf.append("\f")
                    elif e in ('"', "\\"):
                        buf.append(e)
                    elif e == "u":
                        if pos[0] + 4 > n:
                            raise EdnError("short \\u escape")
                        buf.append(chr(int(text[pos[0] : pos[0] + 4], 16)))
                        pos[0] += 4
                    else:
                        raise EdnError("unsupported escape \\%s" % e)
                else:
                    buf.append(ch)
        if c == ":":
            start = pos[0]
            pos[0] += 1
            while pos[0] < n and text[pos[0]] not in " \t\r\n,{}[]\"();":
                pos[0] += 1
            return text[start:pos[0]]
        raise EdnError("unexpected character %r at %d" % (c, pos[0]))

    v = expr()
    ws()
    if pos[0] != n:
        raise EdnError("trailing data at %d" % pos[0])
    return v


def read_input_fixture(path):
    with open(path, "r", encoding="utf-8") as f:
        data = _read_edn(f.read())
    out = []
    for i, m in enumerate(data):
        if not isinstance(m, dict):
            raise EdnError("%s[%d]: not an input map" % (path, i))
        if not isinstance(m.get(":input"), str):
            if m.get(":transcribe") == "skipped":
                out.append(m)
                continue
            raise EdnError("%s[%d]: not an input map" % (path, i))
        out.append(m)
    return out


# ---- the oracle itself ------------------------------------------------


class OracleParser(HTMLParser):
    """Collects the transformed event stream.

    Transforms (each named in the divergence ledger):
    - startendtag expanded to starttag + immediate endtag (D7)
    - attribute list deduped keeping the FIRST duplicate, valueless
      attributes normalized to "" (tier rule 6)
    - adjacent data runs merged (the node spec's adjacent-run merge)
    - handle_decl's leading DOCTYPE stripped (node spec doctype text)
    - handle_pi / unknown_decl remapped to comments (tier rule 9,
      the WHATWG bogus-comment rule)
    """

    def __init__(self):
        super().__init__(convert_charrefs=True)
        self.events = []

    def _attrs(self, attrs):
        seen = []
        for name, value in attrs:
            if name in (k for k, _ in seen):
                continue
            seen.append([name, value if value is not None else ""])
        return seen

    def _emit(self, ev):
        if ev[0] == ":data" and self.events and self.events[-1][0] == ":data":
            self.events[-1] = [":data", self.events[-1][1] + ev[1]]
        else:
            self.events.append(ev)

    def handle_starttag(self, tag, attrs):
        self._emit([":starttag", tag, self._attrs(attrs)])

    def handle_endtag(self, tag):
        self._emit([":endtag", tag])

    def handle_startendtag(self, tag, attrs):
        self.handle_starttag(tag, attrs)
        self.handle_endtag(tag)

    def handle_data(self, data):
        self._emit([":data", data])

    def handle_comment(self, data):
        self._emit([":comment", data])

    def handle_decl(self, decl):
        self._emit([":doctype", DOCTYPE_STRIP.sub("", decl).strip()])

    def handle_pi(self, data):
        self._emit([":comment", data])

    def unknown_decl(self, data):
        # python reports the marked-section interior; the WHATWG
        # bogus comment for "<![CDATA[x]]>" carries "[CDATA[x]]".
        self._emit([":comment", "[" + data + "]]"])


def run_oracle(text):
    parser = OracleParser()
    parser.feed(text)
    parser.close()
    return parser.events


def naive_tree(events):
    """Stack reconstruction over tier rules 1-4 only (see module
    docstring). Nodes are EDN-shaped maps with string tags; the p2
    test keywords them per the hickory node spec."""
    root = []
    stack = []  # [tag, node]

    def add(child):
        (stack[-1][1]["content"] if stack else root).append(child)

    for ev in events:
        kind = ev[0]
        if kind == ":starttag":
            _, tag, attrs = ev
            node = {
                "type": "element",
                "tag": tag,
                "attrs": {k: v for k, v in attrs},
                "content": [],
            }
            add(node)
            if tag not in VOID_ELEMENTS:
                stack.append([tag, node])
        elif kind == ":endtag":
            _, tag = ev
            if tag in VOID_ELEMENTS:
                continue  # rule 4: an end tag naming a void element drops
            hit = None
            for i in range(len(stack) - 1, -1, -1):
                if stack[i][0] == tag:
                    hit = i
                    break
            if hit is None:
                continue  # rule 2: stray end tag dropped
            del stack[hit :]  # rule 3: pop-until the match
        elif kind == ":data":
            add(ev[1])
        elif kind == ":comment":
            add({"type": "comment", "content": [ev[1]]})
        elif kind == ":doctype":
            add({"type": "document-type", "content": [ev[1]]})
    return root  # rule 1: whatever stays open simply closes at EOF


# ---- EDN writing -------------------------------------------------------


def edn_string(s):
    out = []
    for ch in s:
        if ch == "\\":
            out.append("\\\\")
        elif ch == '"':
            out.append('\\"')
        elif ch == "\n":
            out.append("\\n")
        elif ch == "\r":
            out.append("\\r")
        elif ch == "\t":
            out.append("\\t")
        elif ch == "\b":
            out.append("\\b")
        elif ch == "\f":
            out.append("\\f")
        elif ord(ch) < 0x20 or ord(ch) == 0x7F:
            out.append("\\u%04x" % ord(ch))
        else:
            out.append(ch)
    return '"%s"' % "".join(out)


def edn_events(events):
    parts = []
    for ev in events:
        if ev[0] == ":starttag":
            attrs = ", ".join("[%s, %s]" % (edn_string(k), edn_string(v)) for k, v in ev[2])
            parts.append("[:starttag %s [%s]]" % (edn_string(ev[1]), attrs))
        elif ev[0] in (":endtag", ":data", ":comment", ":doctype"):
            parts.append("[%s %s]" % (ev[0], edn_string(ev[1])))
        else:
            raise AssertionError("unknown event %r" % (ev,))
    return "[%s]" % ", ".join(parts)


def edn_tree(nodes):
    parts = []
    for node in nodes:
        if isinstance(node, str):
            parts.append(edn_string(node))
            continue
        content = ", ".join(edn_tree([c]) for c in node["content"])
        if node["type"] == "element":
            attrs = ", ".join("%s %s" % (edn_string(k), edn_string(v)) for k, v in node["attrs"].items())
            parts.append(
                "{:type :element :tag %s :attrs {%s} :content [%s]}"
                % (edn_string(node["tag"]), attrs, content)
            )
        else:
            parts.append(
                "{:type :%s :content [%s]}" % (node["type"], edn_string(node["content"][0]))
            )
    return "%s" % ", ".join(parts)


LEDGER = """\
;; HTML golden vectors. GENERATED by tools/gen_html_oracle.py (the
;; python3 stdlib html.parser oracle, convert_charrefs=True) over the
;; committed input fixtures; do not edit by hand. Regenerate and diff
;; empty (the regen discipline, design D11/NFR-4).
;;
;; Divergence ledger (one line per known python divergence or
;; documented dump transform; the p2 reader reconciles against this):
;; 1. solidus: python emits one startendtag for self-closed start
;;    tags; the dump expands it to :starttag plus an immediate
;;    :endtag, the D7 rule the tokenizer implements.
;; 2. adjacent text: python splits data around character-reference
;;    conversion; the dump merges adjacent :data events (the node
;;    spec's adjacent-run merge; ours does the same).
;; 3. attributes: python's attribute list is deduped here keeping the
;;    FIRST occurrence and valueless attributes normalize to ""
;;    (tier rule 6); a plain dict() over python's list would keep the
;;    LAST duplicate.
;; 4. attribute entities: python decodes some semicolonless legacy
;;    references inside attribute values; our tier decodes
;;    semicolon-terminated names only there (tier rule 8). The dump
;;    keeps python's values; the p2 comparison applies our rule.
;; 5. doctype text: python's decl carries the leading "DOCTYPE"; the
;;    dump strips it (node spec: raw text between <!DOCTYPE and >,
;;    trimmed).
;; 6. bogus comments: python reports pi and unknown_decl events; the
;;    dump remaps both to :comment per tier rule 9 (the WHATWG
;;    bogus-comment rule; CDATA content reconstructed as
;;    "[CDATA[...]]"). <!foo> already arrives as a python comment.
;; 7. title/textarea: the host python (3.14) treats them as RCDATA,
;;    matching D6 exactly (no tag interpretation, references
;;    resolve); no transform needed. Vectors from older pythons
;;    without RCDATA_CONTENT_ELEMENTS would show tags inside them.
;; 8. raw text scope: the host python also treats xmp, iframe,
;;    noembed, and noframes as RAWTEXT (with script/style); the
;;    design's tier pins RAWTEXT for script/style only, so vectors
;;    whose input nests markup inside xmp/iframe/noembed/noframes
;;    diverge from this oracle; p2 decides them by the tier list.
;; 9. NUL in text: python passes NUL through; our tier replaces text
;;    NUL with U+FFFD (rule 13). The dump keeps python's bytes.
;; 10. PLAINTEXT: the host python makes the rest of the input one
;;     text run (matching rule 12); older pythons kept tokenizing.
;; 11. error edges: python has no nesting cap and recovers an
;;     unterminated quoted attribute value at EOF differently from
;;     our eof-in-tag drop; both edges are pinned by hand-written
;;     p2 vectors (:max-depth, eof-in-tag), not by this oracle.
;; 12. :tree field: naive stack matching over tier rules 1-4 only
;;     (EOF auto-balance, stray drop, pop-until, void). Implied
;;     closes, p-closing, html/head/body synthesis, and table text
;;     placement are spec-cited, pinned by the hand-written vectors
;;     in tests/html_test.clj, never by this file.
;; 13. skipped vectors: inputs carrying an initialState override or
;;     lastStartTag configure the tokenizer in ways the python
;;     oracle cannot accept; they are recorded with :oracle
;;     "skipped" and pinned by hand in the p2 tolerance vectors.
"""


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("inputs", nargs="+", help="input EDN fixtures")
    ap.add_argument("-o", "--output", required=True)
    args = ap.parse_args()

    lines = [LEDGER.rstrip("\n"), "["]
    counts = {"vectors": 0, "skipped": 0, "errors": 0}
    for path in args.inputs:
        for m in read_input_fixture(path):
            entry = [
                ":id %s" % edn_string(m[":id"]),
                ":description %s" % edn_string(m.get(":description", "")),
            ]
            if ":input" in m:
                entry.append(":input %s" % edn_string(m[":input"]))
            if ":initial-state" in m and m[":initial-state"] != "Data state":
                entry.append(":initial-state %s" % edn_string(m[":initial-state"]))
            if ":last-start-tag" in m:
                entry.append(":last-start-tag %s" % edn_string(m[":last-start-tag"]))
            if m.get(":transcribe") == "skipped":
                entry.append(':oracle "skipped"')
                entry.append(":reason %s" % edn_string(m.get(":reason", "")))
                counts["skipped"] += 1
            elif (":initial-state" in m and m[":initial-state"] != "Data state") or (
                ":last-start-tag" in m
            ):
                entry.append(':oracle "skipped"')
                entry.append(
                    ":reason %s"
                    % edn_string(
                        "tokenizer state override not representable in the "
                        "python oracle; pinned by hand in the p2 tolerance "
                        "vectors"
                    )
                )
                counts["skipped"] += 1
            else:
                try:
                    events = run_oracle(m[":input"])
                except Exception as exc:  # deterministic per input; recorded
                    entry.append(':oracle "error"')
                    entry.append(":reason %s" % edn_string("%s: %s" % (type(exc).__name__, exc)))
                    counts["errors"] += 1
                else:
                    entry.append(":events %s" % edn_events(events))
                    entry.append(":tree [%s]" % edn_tree(naive_tree(events)))
            lines.append(" {%s}" % " ".join(entry))
            counts["vectors"] += 1
    lines.append("]")

    with open(args.output, "w", encoding="utf-8") as f:
        f.write("\n".join(lines) + "\n")
    print(
        "%s: %d vectors (%d skipped, %d oracle errors)"
        % (args.output, counts["vectors"], counts["skipped"], counts["errors"])
    )


if __name__ == "__main__":
    main()
