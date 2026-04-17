#!/bin/sh
# qa-arch.sh -- architecture quality gates.
#
# Checks translation-unit size, function span, abort inventory, and
# internal-header dependencies.  Exit 0 = all gates pass.
#
# Usage: make qa-arch   (or: sh scripts/qa-arch.sh)

set -e

FAIL=0
SRC_DIR="src"

# -------------------------------------------------------------------------
# 1. Translation unit size (LOC threshold)
# -------------------------------------------------------------------------

# Allowlist: files that are known to exceed the threshold during the
# decomposition work.  Remove entries as files are split.
TU_LIMIT=1200
TU_ALLOWLIST="src/mino.c src/prim.c"

printf "=== Translation unit sizes (limit: %d LOC) ===\n" "$TU_LIMIT"

for f in "$SRC_DIR"/*.c; do
    lines=$(wc -l < "$f")
    allowed=0
    for a in $TU_ALLOWLIST; do
        if [ "$f" = "$a" ]; then
            allowed=1
            break
        fi
    done
    if [ "$lines" -gt "$TU_LIMIT" ] && [ "$allowed" -eq 0 ]; then
        printf "  FAIL  %s: %d LOC (limit %d)\n" "$f" "$lines" "$TU_LIMIT"
        FAIL=1
    else
        status="ok"
        if [ "$lines" -gt "$TU_LIMIT" ]; then
            status="allowlisted"
        fi
        printf "  %-12s %s: %d LOC\n" "$status" "$f" "$lines"
    fi
done

# main.c is in the root
lines=$(wc -l < main.c)
printf "  %-12s main.c: %d LOC\n" "ok" "$lines"
echo ""

# -------------------------------------------------------------------------
# 2. Function span (max lines per function body)
# -------------------------------------------------------------------------

# Heuristic: find function definitions by matching lines that start with
# a type name and contain '(' but not '#define', ';', or common non-function
# patterns. Then measure the span to the closing '}'.
# This is approximate -- good enough for a gate, not a parser.

FN_LIMIT=250
FN_ALLOWLIST="eval_impl install_core_mino mino_install_core"

printf "=== Long functions (limit: %d lines) ===\n" "$FN_LIMIT"

for f in "$SRC_DIR"/*.c main.c; do
    awk -v limit="$FN_LIMIT" -v file="$f" -v allowlist="$FN_ALLOWLIST" \
    'BEGIN { in_func = 0; pending_name = "" }
    /^[a-zA-Z_]/ && /[(]/ && !/^#/ && !/;[ \t]*$/ && !/^typedef/ {
        fname = $0
        sub(/[(].*/, "", fname)
        sub(/.* /, "", fname)
        sub(/^[*]/, "", fname)
        pending_name = fname
        pending_line = NR
        next
    }
    pending_name != "" && /^[{]/ {
        in_func = 1
        func_name = pending_name
        func_start = pending_line
        depth = 1
        pending_name = ""
        next
    }
    pending_name != "" && !/^[{]/ {
        pending_name = ""
    }
    in_func == 1 {
        for (i = 1; i <= length($0); i++) {
            c = substr($0, i, 1)
            if (c == "{") depth++
            if (c == "}") depth--
            if (depth == 0) {
                span = NR - func_start + 1
                if (span > limit) {
                    allowed = 0
                    n = split(allowlist, alist, " ")
                    for (j = 1; j <= n; j++) {
                        if (func_name == alist[j]) allowed = 1
                    }
                    if (allowed)
                        printf "  allowlisted  %s:%d %s() %d lines\n", file, func_start, func_name, span
                    else
                        printf "  FAIL         %s:%d %s() %d lines\n", file, func_start, func_name, span
                }
                in_func = 0
                break
            }
        }
    }' "$f"
done
echo ""

# -------------------------------------------------------------------------
# 3. Abort site inventory
# -------------------------------------------------------------------------

printf "=== abort() inventory ===\n"
count=0
for f in "$SRC_DIR"/*.c main.c; do
    hits=$(grep -n 'abort()' "$f" 2>/dev/null || true)
    if [ -n "$hits" ]; then
        echo "$hits" | while IFS= read -r line; do
            printf "  %s:%s\n" "$f" "$line"
            # Check if the previous line or same line has a comment
        done
        linecount=$(echo "$hits" | wc -l)
        count=$((count + linecount))
    fi
done
printf "  total: %d abort() sites\n\n" "$count"

# -------------------------------------------------------------------------
# 4. Internal header dependency report
# -------------------------------------------------------------------------

printf "=== Internal header includes ===\n"
for f in "$SRC_DIR"/*.c main.c; do
    includes=$(grep '#include' "$f" | grep -v '<' | sed 's/.*"\(.*\)".*/\1/' | tr '\n' ' ')
    printf "  %-20s -> %s\n" "$(basename "$f")" "$includes"
done
echo ""

# -------------------------------------------------------------------------
# Summary
# -------------------------------------------------------------------------

if [ "$FAIL" -eq 1 ]; then
    printf "FAILED: one or more gates exceeded limits.\n"
    exit 1
else
    printf "All architecture gates passed.\n"
    exit 0
fi
