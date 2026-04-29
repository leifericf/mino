# Source-to-C-string-literal escape used by the bootstrap Makefile.
# Each input line becomes one quoted, newline-terminated C string
# literal that concatenates with its neighbours under K&R adjacent-
# string-literal rules. Lives as a file (not inline) because Git Bash
# on Windows mangles inline regex literals through MSYS path
# translation; -f's argument is a file path, which translates
# correctly.
{
    gsub(/\\/, "\\\\")
    gsub(/"/, "\\\"")
    print "    \"" $0 "\\n\""
}
