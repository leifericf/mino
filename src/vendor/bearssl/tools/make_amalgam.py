#!/usr/bin/env python3
"""Assemble the single-TU BearSSL TLS-client amalgam from the vendored
v0.6 tree at src/vendor/bearssl/.

Concatenation order: public headers (bearssl.h include order, local
includes stripped, system includes retained in place), config.h,
inner.h, then the client-relevant .c units each preceded by a #line
marker and with file-local identifiers suffixed _u<idx> so the
independent TUs coexist in one translation unit.

Deterministic: no timestamps, sorted directory walks, stable paste
order. Run from anywhere; paths resolve relative to this script.

    python3 src/vendor/bearssl/tools/make_amalgam.py

Output: src/vendor/bearssl/bearssl_client.c (committed; regenerate
only when re-pinning or re-trimming the vendored tree).
"""
import os
import re

HERE = os.path.dirname(os.path.abspath(__file__))
VENDOR = os.path.dirname(HERE)
SRC = VENDOR
OUT = os.path.join(VENDOR, "bearssl_client.c")

local_inc = re.compile(r'^\s*#\s*include\s*"[^"]+"')
define_re = re.compile(r'^\s*#\s*define\s+([A-Za-z_]\w*)', re.M)
tag_def_re = re.compile(r'\b(?:struct|union|enum)\s+([A-Za-z_]\w*)\s*\{')
kw = {"static", "const", "unsigned", "signed", "int", "uint", "char",
      "void", "long", "short", "uint8_t", "uint16_t", "uint32_t",
      "uint64_t", "int8_t", "int16_t", "int32_t", "int64_t", "size_t",
      "struct", "union", "enum", "typedef", "inline", "register"}


def unit_local_names(body):
    """File-local identifiers defined by a unit: macros, typedefs,
    static functions/objects (prototypes and definitions)."""
    names = set(m.group(1) for m in define_re.finditer(body))
    for m in re.finditer(r'(?m)^\s*typedef\b', body):
        j, depth, saw_brace = m.end(), 0, False
        while j < len(body):
            c = body[j]
            if c == '{':
                depth += 1
                saw_brace = True
            elif c == '}':
                depth -= 1
            elif c == ';' and depth == 0:
                break
            j += 1
        decl = body[m.end():j]
        tail = decl[decl.rfind('}') + 1:] if saw_brace else decl
        tail = re.sub(r'\b(?:struct|union|enum)\b', ' ', tail)
        for nm in re.findall(r'[A-Za-z_]\w*', tail):
            if nm not in kw:
                names.add(nm)
    lines = body.splitlines()
    i = 0
    while i < len(lines):
        if re.match(r'^\s*static\b', lines[i]):
            frag = lines[i]
            j = i
            while not re.search(r'[;={\[]', frag) and j + 1 < len(lines):
                j += 1
                frag += ' ' + lines[j].strip()
            m = re.search(r'([A-Za-z_]\w*)\s*(\(|\[|=|;)', frag)
            if m and m.group(1) not in kw:
                names.add(m.group(1))
            i = j + 1
            continue
        i += 1
    return names


def rename_unit(body, suffix):
    names = unit_local_names(body)
    for n in sorted(names, key=len, reverse=True):
        body = re.sub(r'\b%s\b' % re.escape(n), n + '_' + suffix, body)
    for n in sorted(set(tag_def_re.findall(body)), key=len, reverse=True):
        body = re.sub(r'\b%s\b' % re.escape(n), n + '_' + suffix, body)
    return body, len(names)


def emit(path, out, strip_local=True, suffix=None):
    with open(os.path.join(SRC, path)) as f:
        body = f.read()
    # MSVC linker directive; noise under clang/gcc (-Wunknown-pragmas).
    body = re.sub(r'(?m)^\s*#pragma comment\(.*\)\s*\n', '', body)
    if suffix:
        body, nren = rename_unit(body, suffix)
    else:
        nren = 0
    out.write('\n/* === %s === */\n' % path)
    out.write('#line 1 "%s"\n' % path)
    for line in body.splitlines(keepends=True):
        if strip_local and local_inc.match(line):
            continue
        out.write(line)
    return nren


headers = [
    "inc/bearssl.h",
    "inc/bearssl_hash.h", "inc/bearssl_hmac.h", "inc/bearssl_kdf.h",
    "inc/bearssl_block.h", "inc/bearssl_prf.h", "inc/bearssl_rand.h",
    "inc/bearssl_aead.h", "inc/bearssl_rsa.h", "inc/bearssl_ec.h",
    "inc/bearssl_x509.h", "inc/bearssl_ssl.h",
]

codec = ["ccopy", "dec16be", "dec16le", "dec32be", "dec32le", "dec64be",
         "dec64le", "enc16be", "enc16le", "enc32be", "enc32le", "enc64be",
         "enc64le"]

int_files = sorted(f for f in os.listdir(os.path.join(SRC, "src/int")) if f.endswith(".c"))


def symcipher():
    d = os.path.join(SRC, "src/symcipher")
    return sorted(f[:-2] for f in os.listdir(d) if f.endswith(".c"))


def rsa():
    d = os.path.join(SRC, "src/rsa")
    return sorted(f[:-2] for f in os.listdir(d)
                  if f.endswith(".c") and "keygen" not in f)


def ec():
    d = os.path.join(SRC, "src/ec")
    return sorted(f[:-2] for f in os.listdir(d)
                  if f.endswith(".c") and f != "ec_keygen.c")


ssl = ["prf", "prf_md5sha1", "prf_sha256", "prf_sha384",
       "ssl_ccert_single_ec", "ssl_ccert_single_rsa",
       "ssl_client", "ssl_client_default_rsapub", "ssl_client_full",
       "ssl_engine", "ssl_engine_default_aescbc", "ssl_engine_default_aesccm",
       "ssl_engine_default_aesgcm", "ssl_engine_default_chapol",
       "ssl_engine_default_descbc",
       "ssl_engine_default_ec", "ssl_engine_default_ecdsa",
       "ssl_engine_default_rsavrfy", "ssl_hashes", "ssl_hs_client",
       "ssl_io", "ssl_keyexport", "ssl_lru",
       "ssl_rec_cbc", "ssl_rec_ccm", "ssl_rec_chapol", "ssl_rec_gcm"]


def hashdir():
    d = os.path.join(SRC, "src/hash")
    return sorted(f[:-2] for f in os.listdir(d) if f.endswith(".c"))


x509 = ["x509_decoder", "x509_knownkey", "x509_minimal", "x509_minimal_full"]

cfiles = []
cfiles += ["src/codec/%s.c" % n for n in codec]
cfiles += ["src/hash/%s.c" % n for n in hashdir()]
cfiles += ["src/int/%s" % n for n in int_files]
cfiles += ["src/mac/hmac.c", "src/mac/hmac_ct.c"]
# sysrng.c is excluded: src/prim/tls.c provides br_prng_seeder_system
# (getentropy / BCryptGenRandom) so no Windows advapi32 link is needed.
cfiles += ["src/rand/hmac_drbg.c"]
cfiles += ["src/rsa/%s.c" % n for n in rsa()]
cfiles += ["src/ec/%s.c" % n for n in ec()]
cfiles += ["src/symcipher/%s.c" % n for n in symcipher()]
cfiles += ["src/aead/gcm.c", "src/aead/ccm.c"]
cfiles += ["src/ssl/%s.c" % n for n in ssl]
cfiles += ["src/x509/%s.c" % n for n in x509]
cfiles += ["src/settings.c"]


class Sink(list):
    def write(self, s):
        self.append(s)


sink = Sink()
out = sink
out.write("/* BearSSL v0.6 (commit 8ef7680) TLS-client amalgam, generated\n")
out.write(" * by tools/make_amalgam.py from the vendored tree in this\n")
out.write(" * directory. Not an upstream file. MIT (c) 2016 Thomas Pornin\n")
out.write(" * <pornin@bolet.org>; see LICENSE.\n")
out.write(" *\n")
out.write(" * Per-unit local identifiers (statics, macros, typedefs) carry\n")
out.write(" * an _u<idx> suffix so the independent TUs coexist in one\n")
out.write(" * translation unit.\n")
out.write(" *\n")
out.write(" * Units that use x86/POWER8 intrinsics set BR_ENABLE_INTRINSICS\n")
out.write(" * before including inner.h; with collapsed includes the global\n")
out.write(" * define below replaces the per-unit ones (inert on other\n")
out.write(" * arches).\n")
out.write(" *\n")
out.write(" * MIN/MAX renamed to br_MIN/br_MAX: hosts commonly define MIN/MAX\n")
out.write(" * macros (sys/param.h) which collide with inner.h's inlines.\n")
out.write(" */\n")
out.write("#define BR_ENABLE_INTRINSICS 1\n")
out.write("/* Collapsing the headers into this TU makes their unused static\n")
out.write(" * inline helpers visible as main-file functions; silence that for\n")
out.write(" * the header block only, under compilers that understand the\n")
out.write(" * GCC diagnostic pragmas (unknown pragmas are ignored per C99\n")
out.write(" * 6.10.6; MSVC never reaches these lines).\n")
out.write(" */\n")
out.write("#if defined(__GNUC__) || defined(__clang__)\n")
out.write("#pragma GCC diagnostic push\n")
out.write("#pragma GCC diagnostic ignored \"-Wunused-function\"\n")
out.write("#endif\n")
for h in headers:
    emit(h, out)
emit("src/config.h", out)
emit("src/inner.h", out)
out.write("#if defined(__GNUC__) || defined(__clang__)\n")
out.write("#pragma GCC diagnostic pop\n")
out.write("#endif\n")
for idx, c in enumerate(cfiles):
    emit(c, out, suffix="u%d" % idx)

text = "".join(sink)
text = re.sub(r'\bMIN\b(?=\()', 'br_MIN', text)
text = re.sub(r'\bMAX\b(?=\()', 'br_MAX', text)
with open(OUT, "w") as f:
    f.write(text)

print("file list (%d C files):" % len(cfiles))
for c in cfiles:
    print("  %-42s %7d" % (c, os.path.getsize(os.path.join(SRC, c))))
print("amalgam: %s (%d bytes)" % (OUT, os.path.getsize(OUT)))
