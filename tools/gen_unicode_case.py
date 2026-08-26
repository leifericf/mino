#!/usr/bin/env python3
"""Generate src/prim/unicode_case.h from the vendored Unicode UCD.

Reads vendor/unicode/UnicodeData.txt (simple case mappings, fields 12
and 13) and vendor/unicode/SpecialCasing.txt (unconditional 1:1
entries only), and emits a C header with two sorted tables plus binary-search
lookups. Mappings are 1:1 codepoints and may change the UTF-8 byte
length, so consumers size output buffers for the worst case (twice
the input length); the generator drops identity entries.

Regenerate after bumping the vendored UCD copy:
    ./mino task gen-unicode-case
The generator reads only the vendored files; no network access.
"""

import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
UNICODE_DATA = os.path.join(HERE, "..", "vendor", "unicode", "UnicodeData.txt")
SPECIAL_CASING = os.path.join(HERE, "..", "vendor", "unicode", "SpecialCasing.txt")
OUT = os.path.join(HERE, "..", "src", "prim", "unicode_case.h")


def utf8_len(cp):
    if cp <= 0x7F:
        return 1
    if cp <= 0x7FF:
        return 2
    if cp <= 0xFFFF:
        return 3
    return 4


def read_simple_mappings():
    """UnicodeData fields 12 (simple upper) and 13 (simple lower)."""
    upper, lower = {}, {}
    with open(UNICODE_DATA, encoding="utf-8") as f:
        for line in f:
            parts = line.split(";")
            if len(parts) < 14:
                continue
            cp = int(parts[0], 16)
            up, lo = parts[12], parts[13]
            if up:
                upper[cp] = int(up, 16)
            if lo:
                lower[cp] = int(lo, 16)
    return upper, lower


def read_special_1to1():
    """SpecialCasing unconditional entries with a single target
    codepoint, per direction. Line fields: code; lower; title; upper;
    condition. Multi-char and conditional entries are out of scope."""
    upper, lower = {}, {}
    with open(SPECIAL_CASING, encoding="utf-8") as f:
        for line in f:
            line = line.split("#", 1)[0].strip()
            if not line:
                continue
            parts = line.split(";")
            if len(parts) < 5:
                continue
            if parts[4].strip():
                continue  # locale or context condition
            src = int(parts[0], 16)
            lo_targets = parts[1].split()
            up_targets = parts[3].split()
            if len(lo_targets) == 1:
                lower[src] = int(lo_targets[0], 16)
            if len(up_targets) == 1:
                upper[src] = int(up_targets[0], 16)
    return upper, lower


def merge(base, extra):
    out = dict(base)
    out.update(extra)
    return out


def check_mappings(upper, lower):
    """Drop identity entries; 1:1 codepoint mappings may change the
    UTF-8 byte length (dotless i to I shrinks, U+0250 to U+2C6F
    grows), so the casing prims size the output buffer for the
    worst case: every 2-byte codepoint mapping to 4 bytes."""
    for m in (upper, lower):
        for src in [s for s, d in m.items() if d == s]:
            del m[src]


def to_ranges(pairs):
    """Compress a sorted mapping into (first, last, delta) runs and
    singleton (cp, mapped, 0) entries."""
    runs, singles = [], []
    run_start = run_prev_src = run_prev_dst = None
    for src, dst in pairs:
        if (
            run_start is not None
            and src == run_prev_src + 1
            and dst == run_prev_dst + 1
        ):
            run_prev_src, run_prev_dst = src, dst
            continue
        if run_start is not None:
            flush_run(runs, singles, run_start, run_prev_src, run_prev_dst)
        run_start = src
        run_prev_src, run_prev_dst = src, dst
    if run_start is not None:
        flush_run(runs, singles, run_start, run_prev_src, run_prev_dst)
    return runs, singles


def flush_run(runs, singles, first, last_src, last_dst):
    if last_src > first:
        runs.append((first, last_src, last_dst - last_src))
    else:
        singles.append((first, last_dst))


def emit_table(out, name, mapping):
    pairs = sorted(mapping.items())
    runs, singles = to_ranges(pairs)
    out.append("static const struct unicode_case_range %s_runs[] = {" % name)
    for first, last, delta in runs:
        out.append("    {0x%04X, 0x%04X, %d}," % (first, last, delta))
    out.append("};")
    out.append("static const struct unicode_case_single %s_singles[] = {" % name)
    for cp, dst in singles:
        out.append("    {0x%04X, 0x%04X}," % (cp, dst))
    out.append("};")
    out.append(
        "#define %s_RUNS_N (sizeof(%s_runs)/sizeof(%s_runs[0]))"
        % (name.upper(), name, name)
    )
    out.append(
        "#define %s_SINGLES_N (sizeof(%s_singles)/sizeof(%s_singles[0]))"
        % (name.upper(), name, name)
    )
    return len(pairs), len(runs), len(singles)


def main():
    up_simple, lo_simple = read_simple_mappings()
    up_special, lo_special = read_special_1to1()
    upper = merge(up_simple, up_special)
    lower = merge(lo_simple, lo_special)
    check_mappings(upper, lower)

    lines = [
        "/* unicode_case.h -- generated by tools/gen_unicode_case.py from",
        " * the vendored Unicode character database. DO NOT EDIT; regenerate",
        " * with `./mino task gen-unicode-case` after bumping",
        " * vendor/unicode/. Covers 1:1 simple mappings plus unconditional",
        " * 1:1 SpecialCasing entries; every mapping preserves the UTF-8",
        " * byte length of the codepoint (asserted by the generator). See",
        " * ADR 31 for the boundary. */",
        "#ifndef MINO_UNICODE_CASE_H",
        "#define MINO_UNICODE_CASE_H",
        "",
        "#include <stdint.h>",
        "",
        "struct unicode_case_range {",
        "    uint32_t first;",
        "    uint32_t last;",
        "    int32_t  delta;",
        "};",
        "",
        "struct unicode_case_single {",
        "    uint32_t cp;",
        "    uint32_t mapped;",
        "};",
        "",
    ]
    up_stats = emit_table(lines, "unicode_upper", upper)
    lines.append("")
    lo_stats = emit_table(lines, "unicode_lower", lower)
    lines += [
        "",
        "static uint32_t unicode_case_lookup(",
        "    const struct unicode_case_range *runs, size_t runs_n,",
        "    const struct unicode_case_single *singles, size_t singles_n,",
        "    uint32_t cp)",
        "{",
        "    size_t lo = 0, hi = runs_n;",
        "    while (lo < hi) {",
        "        size_t mid = lo + (hi - lo) / 2;",
        "        if (cp < runs[mid].first)      hi = mid;",
        "        else if (cp > runs[mid].last)  lo = mid + 1;",
        "        else return (uint32_t)((int32_t)cp + runs[mid].delta);",
        "    }",
        "    lo = 0; hi = singles_n;",
        "    while (lo < hi) {",
        "        size_t mid = lo + (hi - lo) / 2;",
        "        if (cp < singles[mid].cp)      hi = mid;",
        "        else if (cp > singles[mid].cp) lo = mid + 1;",
        "        else return singles[mid].mapped;",
        "    }",
        "    return cp;",
        "}",
        "",
        "static uint32_t mino_unicode_to_upper(uint32_t cp)",
        "{",
        "    return unicode_case_lookup(unicode_upper_runs, UNICODE_UPPER_RUNS_N,",
        "                                unicode_upper_singles, UNICODE_UPPER_SINGLES_N,",
        "                                cp);",
        "}",
        "",
        "static uint32_t mino_unicode_to_lower(uint32_t cp)",
        "{",
        "    return unicode_case_lookup(unicode_lower_runs, UNICODE_LOWER_RUNS_N,",
        "                                unicode_lower_singles, UNICODE_LOWER_SINGLES_N,",
        "                                cp);",
        "}",
        "",
        "#endif /* MINO_UNICODE_CASE_H */",
    ]
    with open(OUT, "w", encoding="utf-8") as f:
        f.write("\n".join(lines) + "\n")
    print(
        "wrote %s: upper %d mappings (%d runs, %d singles),"
        " lower %d mappings (%d runs, %d singles)"
        % ((OUT,) + up_stats + lo_stats)
    )


if __name__ == "__main__":
    main()
