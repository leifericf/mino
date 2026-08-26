#!/usr/bin/env python3
"""The compression-zip campaign's python3 oracle generator (ADR 29).

Compress half (p2): emits the tests/fixtures/compress/ fixture set
the compress prims are pinned against -- the zlib.compress and gzip
member streams at levels 0/1/6/9 over a fixed deterministic corpus,
plus the pinned header vectors (the RFC 1952 10-byte header per
level bucket and the RFC 1950 CMF/FLG pair per level) in
manifest.edn.

Zip half (p3): emits the tests/fixtures/zip/ oracle archives the
zip reader is pinned against -- zipfile-made archives (deflate and
stored entries, directory entry, bit-11 non-ASCII name, a forced
zip64 member, a bit-3 data-descriptor archive via streaming open,
method 12/14 entries, duplicate names) and hand-crafted bytes
(zip64 central directory, CP437 and mojibake names without bit 11,
overlapping entries, encrypted flag, a bomb with a tiny compressed
body and a huge declared size) plus the EDN manifest of expected
entries and SHA-256s. Expected names are DERIVED from python's own
zipfile decode (the cp437 fallback is pinned to python behavior,
not asserted against it).

Golden split (R4): these oracle streams pin the DECODE side (mino
decompresses python's bytes) and the header vectors; compressed
BODIES are never compared between mino and any oracle (tdefl output
bytes are mino's own; only decode interop is claimed).

Deterministic by construction: fixed corpus, mtime pinned to 0 or
explicit date_time tuples, no timestamps, no dict-order dependence,
sorted output layout. Regeneration must yield an empty diff:

    python3 tools/gen_zip_oracle.py

Oracle host: python3 zlib / gzip / zipfile as of 3.14 (zlib level
mapping, gzip header fields, and zipfile name decoding verified
against RFC 1950/1952 and the APPNOTE at this pin).

Usage: gen_zip_oracle.py  (writes under tests/fixtures/compress/
and tests/fixtures/zip/)
"""

import calendar
import gzip
import hashlib
import os
import struct
import warnings
import zipfile
import zlib

HERE = os.path.join("tests", "fixtures", "compress")
ZIP_HERE = os.path.join("tests", "fixtures", "zip")
LEVELS = (0, 1, 6, 9)

DOS_MIN_DATE = (1980, 1, 1, 0, 0, 0)


def compress_corpus():
    """Fixed, deterministic, compressible ~28 KB corpus."""
    parts = []
    for i in range(512):
        parts.append(
            "entry-%04d: The quick brown fox jumps over the lazy dog; "
            "pass %d of the compression oracle corpus\n" % (i, i % 7)
        )
    return ("".join(parts)).encode("utf-8")


def sha256_hex(data):
    return hashlib.sha256(data).hexdigest()


def edn_vector(bytes_):
    return "[" + " ".join(str(b) for b in bytes_) + "]"


# ---------------------------------------------------------------- zip half

LOC_SIG = 0x04034B50
CDH_SIG = 0x02014B50
EOCD_SIG = 0x06054B50
ZIP64_EOCD_SIG = 0x06064B50
ZIP64_LOCATOR_SIG = 0x07064B50

METHOD_KEYWORDS = {0: ":store", 8: ":deflate"}


def dos_words(date_time):
    """Civil fields -> (dos_time, dos_date) little-endian words."""
    year, month, day, hour, minute, second = date_time
    dos_time = (hour << 11) | (minute << 5) | (second // 2)
    dos_date = ((year - 1980) << 9) | (month << 5) | day
    return dos_time, dos_date


def craft_loc(name, data, flags=0, method=0, date_time=DOS_MIN_DATE):
    """One stored local file header plus its data."""
    dos_time, dos_date = dos_words(date_time)
    crc = zlib.crc32(data) & 0xFFFFFFFF
    return (
        struct.pack(
            "<IHHHHHIIIHH",
            LOC_SIG, 20, flags, method, dos_time, dos_date,
            crc, len(data), len(data), len(name), 0,
        )
        + name
        + data
    )


def craft_cdh(name, data, loc_ofs, flags=0, method=0,
              date_time=DOS_MIN_DATE, uncomp_override=None):
    """One central directory header for a stored entry. uncomp_override
    writes a lying declared size (the bomb fixture)."""
    dos_time, dos_date = dos_words(date_time)
    crc = zlib.crc32(data) & 0xFFFFFFFF
    uncomp = len(data) if uncomp_override is None else uncomp_override
    return (
        struct.pack(
            "<IHHHHHHIIIHHHHHII",
            CDH_SIG, 20, 20, flags, method, dos_time, dos_date,
            crc, len(data), uncomp, len(name), 0, 0, 0, 0, 0, loc_ofs,
        )
        + name
    )


def craft_zip(locs, cdhs):
    """Concatenate local sections, central directory, and the EOCD."""
    cd = b"".join(cdhs)
    cd_ofs = sum(len(x) for x in locs)
    eocd = struct.pack(
        "<IHHHHIIH",
        EOCD_SIG, 0, 0, len(cdhs), len(cdhs), len(cd), cd_ofs, 0,
    )
    return b"".join(locs) + cd + eocd


def craft_zip64(locs, cdhs, cd_ofs):
    """Concatenate local sections, central directory, the 64-bit EOCD,
    its locator, and the legacy EOCD with zip64 marker words."""
    cd = b"".join(cdhs)
    zip64_eocd_ofs = cd_ofs + len(cd)
    zip64_eocd = struct.pack(
        "<IQHHIIQQQQ",
        ZIP64_EOCD_SIG, 44, 20, 20, 0, 0,
        len(cdhs), len(cdhs), len(cd), cd_ofs,
    )
    locator = struct.pack(
        "<IIQI", ZIP64_LOCATOR_SIG, 0, zip64_eocd_ofs, 1
    )
    eocd = struct.pack(
        "<IHHHHIIH",
        EOCD_SIG, 0, 0, 0xFFFF, 0xFFFF, 0xFFFFFFFF, 0xFFFFFFFF, 0,
    )
    return b"".join(locs) + cd + zip64_eocd + locator + eocd


def write_zipfile_archive(path, specs):
    """Write one zipfile-made archive. specs are (zipinfo, data,
    compress_type) or (zipinfo, data, None) for writestr with the
    info's own method."""
    with zipfile.ZipFile(path, "w") as zf:
        for zinfo, data, compress_type in specs:
            if compress_type is None:
                zf.writestr(zinfo, data)
            else:
                zf.writestr(zinfo, data, compress_type=compress_type)


def expected_entries(path):
    """Derive the expected zip-entries vector from python's own read of
    the archive: names come back decoded exactly as python decodes
    them (utf-8 on bit 11, cp437 otherwise), so the manifest pins
    python behavior instead of asserting against it."""
    out = []
    with zipfile.ZipFile(path) as zf:
        for zi in zf.infolist():
            method = METHOD_KEYWORDS.get(zi.compress_type, str(zi.compress_type))
            mtime = (
                None
                if tuple(zi.date_time) == DOS_MIN_DATE
                else calendar.timegm(tuple(zi.date_time) + (0, 0, 0))
            )
            comment = zi.comment.decode("utf-8")
            out.append(
                "{:name \"%s\"\n"
                "                 :size %d :compressed-size %d :crc32 %d\n"
                "                 :method %s :mtime %s :directory? %s\n"
                "                 :comment \"%s\"}"
                % (
                    edn_escape(zi.filename),
                    zi.file_size,
                    zi.compress_size,
                    zi.CRC,
                    method,
                    "nil" if mtime is None else str(mtime),
                    "true" if zi.is_dir() else "false",
                    edn_escape(comment),
                )
            )
    return " [" + "\n                 ".join(out) + "]"


def edn_escape(s):
    return s.replace("\\", "\\\\").replace('"', '\\"')


def sha256_hex(data):
    return hashlib.sha256(data).hexdigest()


def gen_zip_half():
    os.makedirs(ZIP_HERE, exist_ok=True)
    sections = {}
    contents = {}

    # zipfile warns on the deliberate duplicate-name archive; the
    # fixture is the point, not the warning.
    warnings.filterwarnings("ignore", message="Duplicate name")

    def archive(name, data, contents_map):
        path = os.path.join(ZIP_HERE, name)
        with open(path, "wb") as f:
            f.write(data)
        sections[name] = expected_entries(path)
        contents[name] = contents_map

    # basic.zip -- deflate and stored entries, a directory entry, an
    # explicit mtime beside DOS-minimum ones, a central-directory
    # comment, all ASCII names.
    hello = b"Hello, zip oracle! The quick brown fox.\n"
    stored = bytes(range(256)) * 4
    nested = b"nested payload\n"
    zinfo_nested = zipfile.ZipInfo("dir/nested.txt", date_time=DOS_MIN_DATE)
    zinfo_nested.comment = b"nested comment"
    basic_path = os.path.join(ZIP_HERE, "basic.zip")
    write_zipfile_archive(
        basic_path,
        [
            (
                zipfile.ZipInfo("hello.txt", date_time=(2026, 8, 20, 12, 34, 56)),
                hello,
                zipfile.ZIP_DEFLATED,
            ),
            (
                zipfile.ZipInfo("stored.bin", date_time=(2020, 2, 29, 1, 2, 3)),
                stored,
                zipfile.ZIP_STORED,
            ),
            (zipfile.ZipInfo("dir/", date_time=DOS_MIN_DATE), b"", None),
            (zinfo_nested, nested, None),
        ],
    )
    archive("basic.zip", open(basic_path, "rb").read(),
            {"hello.txt": hello, "stored.bin": stored, "dir/nested.txt": nested})

    # utf8.zip -- a bit-11 non-ASCII name beside an ASCII one.
    naive = "naïve-ünïcode.txt".encode("utf-8")
    write_zipfile_archive(
        os.path.join(ZIP_HERE, "utf8.zip"),
        [
            (
                zipfile.ZipInfo("naïve-ünïcode.txt", date_time=DOS_MIN_DATE),
                naive,
                zipfile.ZIP_DEFLATED,
            ),
            (
                zipfile.ZipInfo("plain.txt", date_time=DOS_MIN_DATE),
                b"plain ascii payload\n",
                zipfile.ZIP_DEFLATED,
            ),
        ],
    )
    archive("utf8.zip", open(os.path.join(ZIP_HERE, "utf8.zip"), "rb").read(),
            {"naïve-ünïcode.txt": naive, "plain.txt": b"plain ascii payload\n"})

    # cp437.bin -- a high-half name without bit 11: decodes CP437.
    # Built by hand (python's writer never emits cp437 high bytes) and
    # pinned to python's own decode of those bytes.
    cp_name = b"\x84\x94" + b"437.txt"
    cp_data = b"cp437 payload\n"
    cp_archive = craft_zip(
        [craft_loc(cp_name, cp_data)], [craft_cdh(cp_name, cp_data, 0)]
    )
    archive("cp437.bin", cp_archive,
            {cp_name.decode("cp437"): cp_data})

    # mojibake.bin -- raw UTF-8 name bytes WITHOUT bit 11 (a writer
    # bug python also mojibakes): decodes CP437.
    mj_raw = "naïve.txt".encode("utf-8")
    mj_data = b"mojibake payload\n"
    mj_archive = craft_zip(
        [craft_loc(mj_raw, mj_data)], [craft_cdh(mj_raw, mj_data, 0)]
    )
    archive("mojibake.bin", mj_archive,
            {mj_raw.decode("cp437"): mj_data})

    # zip64.bin -- a full 64-bit central directory: CDH size markers
    # plus a zip64 extra field, the 56-byte zip64 EOCD, its locator,
    # and the legacy EOCD with 0xFFFF/0xFFFFFFFF marker words.
    z64_name = b"zip64.txt"
    z64_data = b"zip64 payload\n"
    z64_extra = struct.pack("<HHQQ", 0x0001, 16, len(z64_data), len(z64_data))
    dos_time, dos_date = dos_words(DOS_MIN_DATE)
    z64_crc = zlib.crc32(z64_data) & 0xFFFFFFFF
    z64_cdh = (
        struct.pack(
            "<IHHHHHHIIIHHHHHII",
            CDH_SIG, 20, 20, 0, 0, dos_time, dos_date, z64_crc,
            0xFFFFFFFF, 0xFFFFFFFF, len(z64_name), len(z64_extra), 0, 0, 0, 0, 0,
        )
        + z64_name
        + z64_extra
    )
    z64_bytes = craft_zip64(
        [craft_loc(z64_name, z64_data)], [z64_cdh], 30 + len(z64_name) + len(z64_data)
    )
    archive("zip64.bin", z64_bytes, {"zip64.txt": z64_data})

    # descriptor.zip -- bit-3 data descriptors through the streaming
    # open (sizes unknown at write time).
    desc_path = os.path.join(ZIP_HERE, "descriptor.zip")
    desc_data = b"data descriptor payload, streamed in one write\n"
    with zipfile.ZipFile(desc_path, "w") as zf:
        with zf.open(
            zipfile.ZipInfo("streamed.txt", date_time=DOS_MIN_DATE), "w"
        ) as w:
            w.write(desc_data)
    archive("descriptor.zip", open(desc_path, "rb").read(),
            {"streamed.txt": desc_data})

    # methods.zip -- bzip2 (12) and lzma (14) members: listed with
    # integer method passthrough, unreadable by the miniz core.
    bzip_data = b"bzip2 payload payload payload\n"
    lzma_data = b"lzma payload payload payload\n"
    methods_path = os.path.join(ZIP_HERE, "methods.zip")
    write_zipfile_archive(
        methods_path,
        [
            (
                zipfile.ZipInfo("bzip2.txt", date_time=DOS_MIN_DATE),
                bzip_data,
                zipfile.ZIP_BZIP2,
            ),
            (
                zipfile.ZipInfo("lzma.txt", date_time=DOS_MIN_DATE),
                lzma_data,
                zipfile.ZIP_LZMA,
            ),
        ],
    )
    archive("methods.zip", open(methods_path, "rb").read(), {})

    # dup.zip -- one name, two entries, different bytes. zip-read
    # returns the FIRST central-directory match (D8, the recorded
    # divergence from python getinfo, which yields the last).
    dup_first = b"duplicate first payload\n"
    dup_second = b"duplicate SECOND payload, different length\n"
    write_zipfile_archive(
        os.path.join(ZIP_HERE, "dup.zip"),
        [
            (
                zipfile.ZipInfo("same.txt", date_time=DOS_MIN_DATE),
                dup_first,
                zipfile.ZIP_STORED,
            ),
            (
                zipfile.ZipInfo("same.txt", date_time=DOS_MIN_DATE),
                dup_second,
                zipfile.ZIP_STORED,
            ),
        ],
    )
    archive("dup.zip", open(os.path.join(ZIP_HERE, "dup.zip"), "rb").read(),
            {"same.txt": dup_first})

    # overlap.bin -- two central directory entries sharing one local
    # header (an overlapping-entry archive both python and miniz
    # accept; both reads yield the shared bytes).
    ov_data = b"shared overlapping payload\n"
    ov_name_a = b"a.txt"
    ov_name_b = b"b.txt"
    ov_archive = craft_zip(
        [craft_loc(ov_name_a, ov_data)],
        [
            craft_cdh(ov_name_a, ov_data, 0),
            craft_cdh(ov_name_b, ov_data, 0),
        ],
    )
    archive("overlap.bin", ov_archive,
            {"a.txt": ov_data, "b.txt": ov_data})

    # encrypted.bin -- general-purpose bit 0 set in both headers with
    # no encryption actually applied: listed, never extracted.
    enc_name = b"secret.txt"
    enc_data = b"classified payload\n"
    enc_date = (2026, 8, 20, 12, 34, 56)
    enc_archive = craft_zip(
        [craft_loc(enc_name, enc_data, flags=0x0001, date_time=enc_date)],
        [craft_cdh(enc_name, enc_data, 0, flags=0x0001, date_time=enc_date)],
    )
    archive("encrypted.bin", enc_archive, {})

    # bomb.bin -- a deflate entry whose central directory declares a
    # ~4 GiB uncompressed size over a tiny compressed body (the
    # zip-bomb shape; a stored entry with mismatched sizes is
    # rejected at central-directory walk by both miniz and python).
    # The declared-size cap must fire before any allocation.
    bomb_name = b"bomb.bin"
    bomb_raw = b"BOOM"
    bomb_comp = zlib.compress(bomb_raw, 9)[2:-4]  # raw deflate body
    bomb_crc = zlib.crc32(bomb_raw) & 0xFFFFFFFF
    bomb_loc = (
        struct.pack(
            "<IHHHHHIIIHH",
            LOC_SIG, 20, 0, 8, 0, 0x21,
            bomb_crc, len(bomb_comp), len(bomb_raw), len(bomb_name), 0,
        )
        + bomb_name
        + bomb_comp
    )
    bomb_cdh = craft_cdh(
        bomb_name, bomb_raw, 0, method=8, uncomp_override=0xFFFFFFF0
    )
    # craft_cdh writes comp_size = len(data); the compressed body is
    # the deflate stream, so patch the CDH compressed-size word.
    bomb_cdh = (
        bomb_cdh[:20]
        + struct.pack("<I", len(bomb_comp))
        + bomb_cdh[24:]
    )
    bomb_archive = craft_zip([bomb_loc], [bomb_cdh])
    archive("bomb.bin", bomb_archive, {})

    # The manifest: per-archive SHA-256, the expected entry vector in
    # archive order, and content SHA-256s for the readable members.
    # The bomb and encrypted sections pin only the listing; dup pins
    # the FIRST entry's bytes (the D8 rule).
    with open(os.path.join(ZIP_HERE, "manifest.edn"), "w") as f:
        f.write(
            ";; GENERATED by tools/gen_zip_oracle.py -- do not edit.\n"
            ";;\n"
            ";; Regenerate: python3 tools/gen_zip_oracle.py\n"
            ";; (empty diff on regeneration; python3 3.14 zipfile is\n"
            ";; the oracle). Each section's :entries vector is derived\n"
            ";; from python's own infolist read (names decoded exactly\n"
            ";; as python decodes them: utf-8 on bit 11, cp437 else),\n"
            ";; in central-directory archive order. :contents pins the\n"
            ";; SHA-256 of the members zip-read must return; dup.zip's\n"
            ";; entry is the FIRST central-directory match (D8).\n"
            "{\n"
        )
        for name in sorted(sections):
            f.write(
                " :%s {:sha256 \"%s\"\n"
                "  :contents {%s}\n"
                "  :entries%s}\n"
                % (
                    name.rsplit(".", 1)[0],
                    sha256_hex(open(os.path.join(ZIP_HERE, name), "rb").read()),
                    " ".join(
                        "\"%s\" \"%s\"" % (edn_escape(n), sha256_hex(d))
                        for n, d in sorted(contents[name].items())
                    ),
                    sections[name],
                )
            )
        f.write("}\n")

    with open(os.path.join(ZIP_HERE, "README.md"), "w") as f:
        f.write(
            "# zip test fixtures\n"
            "\n"
            "Generated by `tools/gen_zip_oracle.py` (the python3 zipfile\n"
            "oracle; provenance in manifest.edn):\n"
            "\n"
            "```\n"
            "python3 tools/gen_zip_oracle.py\n"
            "```\n"
            "\n"
            "- `basic.zip` -- deflate + stored members, a directory\n"
            "  entry, explicit and DOS-minimum mtimes, a CD comment.\n"
            "- `utf8.zip` -- bit-11 non-ASCII names beside ASCII.\n"
            "- `cp437.bin` -- high-half CP437 name without bit 11\n"
            "  (hand-crafted; pinned to python's cp437 decode).\n"
            "- `mojibake.bin` -- raw UTF-8 name without bit 11 (the\n"
            "  writer-bug shape; mojibakes exactly as python's).\n"
            "- `zip64.bin` -- hand-crafted 64-bit central directory\n"
            "  (zip64 EOCD + locator + CDH zip64 size extra).\n"
            "- `descriptor.zip` -- bit-3 data descriptors (streaming\n"
            "  write).\n"
            "- `methods.zip` -- bzip2 (12) and lzma (14) members.\n"
            "- `dup.zip` -- duplicate name, different bytes (D8 first\n"
            "  match).\n"
            "- `overlap.bin` -- two CD entries sharing one local\n"
            "  header.\n"
            "- `encrypted.bin` -- general-purpose bit 0 with no\n"
            "  payload encryption.\n"
            "- `bomb.bin` -- 4-byte body declaring ~4 GiB (the\n"
            "  declared-size cap fixture).\n"
            "- `manifest.edn` -- archive SHA-256s, expected entry\n"
            "  vectors in archive order, content SHA-256s.\n"
            "\n"
            "Compressed bodies are never compared between mino and\n"
            "python (the R4 golden split): the archives pin the read\n"
            "side, decode interop only.\n"
        )

    print(
        "gen_zip_oracle.py: wrote %s (%d oracle archives)"
        % (ZIP_HERE, len(sections))
    )


def main():
    corpus = compress_corpus()
    os.makedirs(HERE, exist_ok=True)

    with open(os.path.join(HERE, "corpus.txt"), "wb") as f:
        f.write(corpus)

    levels_edn = []
    for level in LEVELS:
        z = zlib.compress(corpus, level)
        g = gzip.compress(corpus, compresslevel=level, mtime=0)
        with open(os.path.join(HERE, "zlib_l%d.bin" % level), "wb") as f:
            f.write(z)
        with open(os.path.join(HERE, "gzip_l%d.gz" % level), "wb") as f:
            f.write(g)
        levels_edn.append(
            " {:level %d\n"
            "  :zlib-size %d\n"
            "  :zlib-sha256 \"%s\"\n"
            "  :gzip-size %d\n"
            "  :gzip-sha256 \"%s\"\n"
            "  :gzip-header %s\n"
            "  :zlib-cmf-flg %s}\n"
            % (
                level,
                len(z),
                sha256_hex(z),
                len(g),
                sha256_hex(g),
                edn_vector(g[:10]),
                edn_vector(z[:2]),
            )
        )

    with open(os.path.join(HERE, "manifest.edn"), "w") as f:
        f.write(
            ";; GENERATED by tools/gen_zip_oracle.py -- do not edit.\n"
            ";;\n"
            ";; Regenerate: python3 tools/gen_zip_oracle.py\n"
            ";; (empty diff on regeneration; python3 3.14 zlib/gzip is\n"
            ";; the oracle). :gzip-header is python's RFC 1952 10-byte\n"
            ";; header (mtime 0, no name, OS 255, XFL per level);\n"
            ";; :zlib-cmf-flg is python's RFC 1950 CMF/FLG pair. The\n"
            ";; header vectors pin mino's writers byte for byte; the\n"
            ";; streams themselves pin mino's zlib-decompress (decode\n"
            ";; interop only, R4: compressed bodies are never compared).\n"
            "{:corpus-size %d\n"
            " :corpus-sha256 \"%s\"\n"
            " :levels\n"
            " [%s]}\n"
            % (len(corpus), sha256_hex(corpus), "".join(levels_edn))
        )

    with open(os.path.join(HERE, "README.md"), "w") as f:
        f.write(
            "# compress test fixtures\n"
            "\n"
            "Generated by `tools/gen_zip_oracle.py` (the python3 zlib /\n"
            "gzip oracle; provenance in manifest.edn):\n"
            "\n"
            "```\n"
            "python3 tools/gen_zip_oracle.py\n"
            "```\n"
            "\n"
            "- `corpus.txt` -- the deterministic compressible corpus.\n"
            "- `zlib_l{0,1,6,9}.bin` -- `zlib.compress(corpus, level)`\n"
            "  streams; mino's zlib-decompress must decode each one.\n"
            "- `gzip_l{0,1,6,9}.gz` -- `gzip.compress(corpus,\n"
            "  compresslevel=level, mtime=0)` members.\n"
            "- `manifest.edn` -- sizes, SHA-256s, and the pinned header\n"
            "  vectors. Compressed bodies are never compared between\n"
            "  mino and python (the R4 golden split): the streams pin\n"
            "  the decode side, the header vectors pin the writers.\n"
            "\n"
            "A regenerated `random*`-style fixture does not exist here;\n"
            "the corpus is fully deterministic, so regeneration diffs\n"
            "empty on the same python3/zlib.\n"
        )

    print("gen_zip_oracle.py: wrote %s (%d byte corpus)" % (HERE, len(corpus)))
    gen_zip_half()


if __name__ == "__main__":
    main()
