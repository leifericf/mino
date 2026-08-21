# Source-to-C-string-literal escape used by the bootstrap Makefile.
# Each input line becomes one quoted, newline-terminated C string
# literal that concatenates with its neighbours under K&R adjacent-
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
