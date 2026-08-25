#!/usr/bin/env python3
"""Transcribe html5lib-tests tokenizer INPUT strings into the mino
fixture tests/fixtures/html/html5lib_tokenizer_inputs.edn.

INPUT strings only: data, never code, never expected outputs. Every
expected value is generated locally through the python3 oracle
(tools/gen_html_oracle.py, ADR 28's D4 reconciliation). The fixture
header carries the upstream URL and pinned commit sha, the
inputs-only statement, and the upstream MIT notice verbatim.

Per-test context that configures the tokenizer (initialStates,
lastStartTag) is input-side setup, not expectations, and is carried
along: one fixture entry per (test, initialState) pair. Entries in
the Data state omit :initial-state; the others carry it and the
oracle marks them skipped (they pin tokenizer states the python
oracle cannot enter).

doubleEscaped tests (README: each \\uHHHH sequence in the JSON input
stands for that code point) are unescaped here at transcription
time, once, so the fixture's :input strings are the literal
character streams.

Reproduction requires a checkout of the pinned sha:
    git clone https://github.com/html5lib/html5lib-tests \\
        /tmp/html5lib-tests
    git -C /tmp/html5lib-tests checkout <sha>
    python3 tools/transcribe_html5lib.py /tmp/html5lib-tests

Deterministic: sorted filenames, test order, fixed-width ids, no
timestamps. Re-running against the same sha yields an empty diff.
"""

import argparse
import json
import os
import re
import subprocess

UPSTREAM_URL = "https://github.com/html5lib/html5lib-tests"
PINNED_SHA = "224991ec10db04f056a89eed8b0bd8695fd2950e"

OUT = os.path.join(
    os.path.dirname(os.path.dirname(os.path.abspath(__file__))),
    "tests/fixtures/html/html5lib_tokenizer_inputs.edn",
)

UHHHH = re.compile(r"\\u([0-9a-fA-F]{4})")

MIT_NOTICE = """Copyright (c) 2006-2013 James Graham, Geoffrey Sneddon, and
other contributors

Permission is hereby granted, free of charge, to any person obtaining
a copy of this software and associated documentation files (the
"Software"), to deal in the Software without restriction, including
without limitation the rights to use, copy, modify, merge, publish,
distribute, sublicense, and/or sell copies of the Software, and to
permit persons to whom the Software is furnished to do so, subject to
the following conditions:

The above copyright notice and this permission notice shall be
included in all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE
LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION
OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION
WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE."""


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


def comment_block(text):
    return "\n".join(";; " + line if line else ";;" for line in text.split("\n"))


def unescape_double_escaped(s):
    return UHHHH.sub(lambda m: chr(int(m.group(1), 16)), s)


def header():
    intro = (
        "html5lib-tests tokenizer inputs, transcribed for the\n"
        "html-xml campaign (ADR 28, D4 corpus reconciliation).\n"
        "\n"
        "Upstream: %s\n"
        "Pinned commit sha at transcription: %s\n"
        "Transcribed from: tokenizer/*.test (the \"tests\" member, and\n"
        "xmlViolation.test's \"xmlViolationTests\" member).\n"
        "\n"
        "INPUT STRINGS ONLY are transcribed here. No html5lib expected\n"
        "output, token, or error text is carried into this repository:\n"
        "every expected value is generated locally through the python3\n"
        "oracle (tools/gen_html_oracle.py over this fixture writes\n"
        "tests/fixtures/html/golden.edn).\n"
        "\n"
        "Per-test tokenizer setup rides along as input-side context:\n"
        ":initial-state (one entry per initialState; Data state entries\n"
        "omit the key) and :last-start-tag. doubleEscaped inputs were\n"
        "unescaped once at transcription time (each upstream \\uHHHH\n"
        "sequence became that code point). Four unicodeCharsProblematic\n"
        "inputs carry lone surrogates, which UTF-8 and the mino reader\n"
        "cannot represent; they are marked :transcribe \"skipped\" with\n"
        "the reason, and their U+FFFD preprocessing is pinned by hand\n"
        "in the p2 vectors.\n"
        "\n"
        "Upstream MIT license notice, verbatim:" % (UPSTREAM_URL, PINNED_SHA)
    )
    return comment_block(intro) + "\n" + comment_block(MIT_NOTICE)


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("checkout", help="path to a html5lib-tests checkout")
    args = ap.parse_args()

    sha = subprocess.run(
        ["git", "-C", args.checkout, "rev-parse", "HEAD"],
        capture_output=True, text=True, check=True,
    ).stdout.strip()
    if sha != PINNED_SHA:
        raise SystemExit(
            "checkout is %s but the fixture pins %s" % (sha, PINNED_SHA)
        )

    tok_dir = os.path.join(args.checkout, "tokenizer")
    names = sorted(n for n in os.listdir(tok_dir) if n.endswith(".test"))

    lines = [header(), "["]
    total = 0
    for name in names:
        stem = name[: -len(".test")]
        with open(os.path.join(tok_dir, name), "r", encoding="utf-8") as f:
            doc = json.load(f)
        tests = doc.get("tests") or doc.get("xmlViolationTests") or []
        for i, t in enumerate(tests):
            inp = t.get("input", "")
            if t.get("doubleEscaped"):
                inp = unescape_double_escaped(inp)
            desc = t.get("description", "")
            representable = True
            try:
                inp.encode("utf-8")
            except UnicodeEncodeError:
                # lone surrogates: not encodable as UTF-8, rejected by
                # the mino reader, and replaced with U+FFFD by WHATWG
                # input preprocessing before any tokenizer sees them
                representable = False
            states = t.get("initialStates", ["Data state"])
            for state in states:
                idx = "%s-%04d" % (stem, i)
                entry = [
                    ":id %s" % edn_string(idx),
                    ":description %s" % edn_string(desc),
                ]
                if representable:
                    entry.append(":input %s" % edn_string(inp))
                else:
                    entry.append(':transcribe "skipped"')
                    entry.append(
                        ":reason %s"
                        % edn_string(
                            "lone-surrogate input not representable as "
                            "UTF-8/EDN; WHATWG input preprocessing replaces "
                            "unpaired surrogates with U+FFFD, pinned by "
                            "hand in the p2 vectors"
                        )
                    )
                if state != "Data state":
                    entry.append(":initial-state %s" % edn_string(state))
                if t.get("lastStartTag"):
                    entry.append(":last-start-tag %s" % edn_string(t["lastStartTag"]))
                lines.append(" {%s}" % " ".join(entry))
                total += 1
    lines.append("]")

    os.makedirs(os.path.dirname(OUT), exist_ok=True)
    with open(OUT, "w", encoding="utf-8") as f:
        f.write("\n".join(lines) + "\n")
    print("%s: %d input entries from %d files" % (OUT, total, len(names)))


if __name__ == "__main__":
    main()
