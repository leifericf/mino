#!/usr/bin/env python3
"""The html-xml campaign's strict-XML oracle dump script (ADR 28,
design D11).

Runs the python3 stdlib well-formedness oracle (xml.etree.
ElementTree.fromstring) over an EDN input fixture and writes the
golden EDN vector file the p5 reader is pinned against. Every
expected value is generated here, locally, from the oracle; the
input fixture carries only authored inputs.

Tree shape: the JVM clojure.xml element shape with STRING tags and
attribute names (the p1 golden decision: names keywordize at the
comparison boundary in tests/xml_test.clj). Element text and child
tails map onto :content positions in document order; the :content
vector is always present, empty for element-only empties.

Deterministic by construction: input order in, file order out, no
timestamps, no dict-order dependence (attribute order is etree's
insertion order, which is document order). Regeneration must yield
an empty diff:

    python3 tools/gen_xml_oracle.py -o tests/fixtures/xml/golden.edn \\
        tests/fixtures/xml/inputs.edn

Oracle host: python3 xml.etree (expat): XML 1.0 well-formedness,
document-wide line-ending normalization (CR LF and lone CR to LF),
attribute-value whitespace normalization for CDATA-type attributes
(literal tab/newline/CR to space; character references pass through),
character data merged across comments, PIs, and CDATA sections.

Usage: gen_xml_oracle.py -o OUT.edn IN.edn
"""

import argparse
import sys
import xml.etree.ElementTree as ET

# Restricted EDN reader shared with gen_html_oracle.py (vector of
# maps, keyword keys, string values; \\uXXXX escapes supported).


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
        if not isinstance(m, dict) or not isinstance(m.get(":input"), str):
            raise EdnError("%s[%d]: not an input map" % (path, i))
        out.append(m)
    return out


# ---- the oracle itself ------------------------------------------------


def convert(el):
    """One etree element -> the JVM clojure.xml shape with string
    names. Text and tails become string entries in :content at their
    document positions; comments, PIs, and the doctype never appear
    (the oracle drops them, the JVM SAX behavior the shape pins)."""
    node = {"tag": el.tag, "attrs": dict(el.attrib), "content": []}
    if el.text:
        node["content"].append(el.text)
    for child in el:
        node["content"].append(convert(child))
        if child.tail:
            node["content"].append(child.tail)
    return node


# ---- EDN writing ------------------------------------------------------


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


def edn_node(node):
    parts = []
    for c in node["content"]:
        if isinstance(c, str):
            parts.append(edn_string(c))
        else:
            parts.append(edn_node(c))
    if node["attrs"]:
        attrs = ", ".join(
            "%s %s" % (edn_string(k), edn_string(v))
            for k, v in node["attrs"].items()
        )
    else:
        attrs = ""
    return "{:tag %s :attrs {%s} :content [%s]}" % (
        edn_string(node["tag"]),
        attrs,
        ", ".join(parts),
    )


LEDGER = """\
;; XML golden vectors. GENERATED by tools/gen_xml_oracle.py (the
;; python3 xml.etree well-formedness oracle) over the committed input
;; fixture; do not edit by hand. Regenerate and diff empty (the regen
;; discipline, design D11/NFR-4).
;;
;; Divergence ledger (one line per known python divergence; the p5
;; reader reconciles against this):
;; 1. internal entities: python accepts and EXPANDS internal-subset
;;    ENTITY declarations (even to element content); mino throws
;;    :unsupported-doctype on any internal-subset ENTITY (design D3,
;;    XXE structurally impossible). No such inputs here; hand vectors
;;    pin mino's side.
;; 2. namespaces: etree rewrites tags and attribute names into Clark
;;    notation {uri}local and drops xmlns declarations; mino (and JVM
;;    clojure.xml) keep literal prefix:local qnames and xmlns attrs,
;;    without requiring prefixes to be declared. Namespace-carrying
;;    inputs are therefore excluded from this corpus and pinned by
;;    hand vectors instead.
;; 3. text merging: python merges all character data of a position
;;    into one string, across comments, PIs, and CDATA sections; the
;;    JVM SAX mirror emits one string per characters() event, so
;;    JVM clojure.xml shows adjacent strings there. mino merges (the
;;    oracle wins for goldens; the JVM divergence is logged).
;; 4. line endings: python normalizes CR LF and lone CR to LF
;;    document-wide, CDATA included; mino implements the same, so
;;    CRLF fixtures here carry single \\n in expectations.
;; 5. attribute values: python applies XML 1.0 3.3.3 CDATA-type
;;    normalization (literal tab/newline/CR to one space each;
;;    character references pass through verbatim); mino matches.
;; 6. BOM: python accepts and strips a leading UTF-8 BOM; mino strips
;;    it too (the reader is byte-based, so the fixture carries it as
;;    its three UTF-8 bytes).
;; 7. error edges: python's error taxonomy and event-based positions
;;    are not mirrored; malformed inputs are pinned by hand vectors
;;    in tests/xml_test.clj with mino's own :code and byte positions.
"""


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("inputs", nargs=1, help="input EDN fixture")
    ap.add_argument("-o", "--output", required=True)
    args = ap.parse_args()

    lines = [LEDGER.rstrip("\n"), "["]
    counts = {"vectors": 0, "errors": 0}
    for m in read_input_fixture(args.inputs[0]):
        try:
            root = ET.fromstring(m[":input"])
        except ET.ParseError as exc:
            raise SystemExit(
                "%s: input %s is not well-formed for the oracle: %s"
                % (args.inputs[0], m[":id"], exc)
            )
        entry = [
            ":id %s" % edn_string(m[":id"]),
            ":input %s" % edn_string(m[":input"]),
        ]
        if ":description" in m:
            entry.append(":description %s" % edn_string(m[":description"]))
        entry.append(":tree %s" % edn_node(convert(root)))
        lines.append(" {%s}" % " ".join(entry))
        counts["vectors"] += 1
    lines.append("]")

    with open(args.output, "w", encoding="utf-8") as f:
        f.write("\n".join(lines) + "\n")
    print(
        "%s: %d vectors" % (args.output, counts["vectors"])
    )


if __name__ == "__main__":
    main()
