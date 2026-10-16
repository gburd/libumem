#!/bin/sh
# docs/check_mermaid.sh -- validate every ```mermaid block in the docs by
# running it through the real mermaid parser (mmdc).  A malformed diagram
# is a release gate failure: it would render as an error box on the site.
#
# Requires mermaid-cli (mmdc), fetched on demand via npx if not on PATH.
# Run from the repo root, or via the docs Pages workflow.
set -eu

here=$(unset CDPATH; cd -- "$(dirname -- "$0")" && pwd)
DOCS="$here"
TMP=$(mktemp -d "${TMPDIR:-/tmp}/umem-mmd.XXXXXX")
trap 'rm -f "$TMP"/* 2>/dev/null; rmdir "$TMP" 2>/dev/null' EXIT

if command -v mmdc >/dev/null 2>&1; then
	MMDC="mmdc"
elif command -v npx >/dev/null 2>&1; then
	MMDC="npx --yes @mermaid-js/mermaid-cli"
else
	echo "[mermaid] SKIP: neither mmdc nor npx on PATH" >&2
	exit 0
fi

# Minimal puppeteer config so mmdc runs headless in CI sandboxes.
cat > "$TMP/pp.json" <<'JSON'
{ "args": ["--no-sandbox", "--disable-gpu"] }
JSON

# Extract each ```mermaid ... ``` block from every markdown file into a
# numbered .mmd, then validate each.
n=0
fail=0
for md in "$DOCS"/*.md "$DOCS"/*/*.md; do
	[ -e "$md" ] || continue
	rm -f "$TMP"/blk.*.mmd 2>/dev/null
	# awk pulls each fenced mermaid block into its own file.
	awk -v out="$TMP" '
		/^```mermaid[[:space:]]*$/ { inblk=1; fn=sprintf("%s/blk.%d.mmd", out, ++blk); files[fn]=1; next }
		/^```[[:space:]]*$/ && inblk { inblk=0; next }
		inblk { print >> fn }
		END { for (f in files) print f }
	' "$md" > "$TMP/list.$$" 2>/dev/null || true
	while IFS= read -r mmd; do
		[ -f "$mmd" ] || continue
		n=$((n + 1))
		if ! $MMDC -p "$TMP/pp.json" -i "$mmd" -o "$mmd.svg" >"$mmd.log" 2>&1; then
			echo "[mermaid] FAIL: a diagram in $md did not parse:" >&2
			sed 's/^/    /' "$mmd" >&2
			echo "    --- mmdc said: ---" >&2
			tail -5 "$mmd.log" | sed 's/^/    /' >&2
			fail=$((fail + 1))
		fi
	done < "$TMP/list.$$"
	rm -f "$TMP/list.$$"
done

if [ "$fail" -ne 0 ]; then
	echo "[mermaid] FAIL: $fail of $n diagram(s) are malformed" >&2
	exit 1
fi
echo "[mermaid] OK: all $n diagram(s) parse"
