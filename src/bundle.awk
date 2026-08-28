# Source-to-C-string-literal bundler used by the bootstrap Makefile.
# Emits the complete src/<symbol>.h header: the AUTO-GENERATED banner
# (byte-identical to the gen-core-header / gen-stdlib-headers emitters
# in lib/mino/tasks/builtin.clj, so `make` and `./mino task build`
# produce interchangeable bytes), the static const char *<sym>_src
# definition, and one quoted, newline-terminated C string literal per
# input line that concatenates with its neighbours under K&R adjacent-
# string-literal rules. Lives as a file (not inline) because Git Bash
# on Windows mangles inline regex literals through MSYS path
# translation; -f's argument is a file path, which translates
# correctly.
#
# The escaping is char-by-char on purpose: gsub replacement-string
# backslash handling is only standardized for \\ and \&, and older
# mawk (1.3.4-20200120, Debian bookworm / gcc:13 images) silently
# drops the backslash from \\\" , corrupting every generated header
# into C that cannot compile. Explicit substr arithmetic has no
# replacement-string ambiguity on any awk.
#
# Variables (passed via -v): sym = C symbol, src = source path. The
# namespace named in the stdlib banner is derived from the path
# (lib/mino/html/select.clj -> mino.html.select), which holds for
# every bundled-stdlib entry.
BEGIN {
    print "/* AUTO-GENERATED -- DO NOT EDIT."
    print " *"
    if (sym == "core_mino") {
        print " * Produced by `gen-core-header` from " src "."
        print " * Embeds the bundled mino-side core library as a C"
        print " * string literal so the runtime can install it without"
        print " * needing core.clj on disk at startup."
        print " *"
        print " * Edit " src ", then `./mino task build` (which"
        print " * regenerates this file). Gitignored."
    } else {
        ns = src
        sub(/^lib\//, "", ns)
        gsub(/\//, ".", ns)
        sub(/\.clj$/, "", ns)
        print " * Produced by `gen-stdlib-headers` from " src "."
        print " * Embeds the bundled mino-side " ns " namespace"
        print " * source as a C string literal so the runtime can"
        print " * register it without needing the file on disk."
        print " *"
        print " * Edit " src ", then `./mino task build`"
        print " * (which regenerates this file). Gitignored."
    }
    print " */"
    print "static const char *" sym "_src ="
}
{
    out = ""
    n = length($0)
    for (i = 1; i <= n; i++) {
        c = substr($0, i, 1)
        if (c == "\\") {
            out = out "\\\\"
        } else if (c == "\"") {
            out = out "\\\""
        } else {
            out = out c
        }
    }
    print "    \"" out "\\n\""
}
END {
    # An empty source still yields one empty literal line; the clj
    # emitters append their trailing \n" unconditionally.
    if (NR == 0)
        print "    \"\\n\""
    print "    ;"
}
