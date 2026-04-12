#!/usr/bin/env bash
# Generate compile_commands.json for clangd/LSP support
set -euo pipefail

SRCDIR="${1:-.}"
OUTPUT="compile_commands.json"

cd "$SRCDIR"

echo "Generating compile_commands.json..."

# Clean and rebuild with verbose output
make clean >/dev/null 2>&1 || true
make -n 2>&1 | grep -E "^\s*gcc|^\s*cc|^\s*g\+\+|^\s*clang" | \
    awk -v pwd="$PWD" '
BEGIN {
    print "["
    first = 1
}
{
    # Extract the full command
    cmd = $0
    # Try to extract source file
    for (i = 1; i <= NF; i++) {
        if ($i ~ /\.(c|cc|cpp|cxx)$/) {
            file = $i
            if (file !~ /^\//) {
                file = pwd "/" file
            }
            break
        }
    }

    if (file != "") {
        if (!first) print ","
        first = 0
        printf "  {\n"
        printf "    \"directory\": \"%s\",\n", pwd
        printf "    \"command\": \"%s\",\n", gensub(/"/, "\\\\\"", "g", cmd)
        printf "    \"file\": \"%s\"\n", file
        printf "  }"
        file = ""
    }
}
END {
    print "\n]"
}' > "$OUTPUT.tmp"

# Actually build to ensure it works
make >/dev/null 2>&1

# If temp file was created successfully, move it
if [ -s "$OUTPUT.tmp" ]; then
    mv "$OUTPUT.tmp" "$OUTPUT"
    echo "Generated $OUTPUT with $(grep -c '"file":' "$OUTPUT" || echo 0) entries"
else
    echo "Error: Failed to generate compile commands"
    rm -f "$OUTPUT.tmp"
    exit 1
fi
