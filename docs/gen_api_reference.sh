#!/bin/sh
# docs/gen_api_reference.sh -- generate the in-site API reference from the
# man pages.  Renders each man page in the repo root (*.1, *.3, *.7) to an
# HTML fragment with mandoc and emits categorized Jekyll pages under
# docs/reference/api/.
#
# libumem's man pages use the man(7) macro package (.TH/.SH), not mdoc, and
# live in the repository root (not a man/ subtree).  The one-line summary is
# taken from the second line of the ".SH NAME" block.
#
# Run from the repo root (or via the Pages workflow, which runs it before
# the Jekyll build so the site is always current).  Requires mandoc.
set -eu

here=$(unset CDPATH; cd -- "$(dirname -- "$0")" && pwd)
ROOT=$(unset CDPATH; cd -- "$here/.." && pwd)
OUT="$here/reference/api"

if ! command -v mandoc >/dev/null 2>&1; then
	echo "gen_api_reference: mandoc not found; skipping" >&2
	exit 0
fi

mkdir -p "$OUT"

# Categories: each is "slug|Title|space-separated man basenames with section
# suffix".  A page listed here lands in that category.
cat_defs='
tools|Command-line tools|umem.1
core|Core allocation API|umem_alloc.3 umem_cache_create.3 libumem.3
hooks|Allocation hooks|umem_hooks.3
debug|Debugging and introspection|umem_debug.3 umem_inspect.3 umem_debugging.7
'

# Render one man file to an HTML fragment on stdout.
render() { mandoc -T html -O 'fragment' "$1" 2>/dev/null; }

# Pull the one-line summary out of a man(7) page: the line after ".SH NAME",
# with the leading "name \- " stripped to just the description half.
summary() {
	awk '
		/^\.SH NAME/ { grab=1; next }
		grab && /^\.SH/ { exit }
		grab {
			line = line " " $0
		}
		END {
			sub(/^ +/, "", line)
			# drop roff line-continuations and the "names \- desc" head
			gsub(/\\$/, "", line)
			n = index(line, "\\-")
			if (n > 0) line = substr(line, n + 2)
			sub(/^ +/, "", line)
			print line
		}
	' "$1"
}

emit_page() {
	# $1 basename.section (e.g. umem_alloc.3)  $2 nav_parent  $3 nav_order
	file=$1 parent=$2 order=$3
	f="$ROOT/$file"
	[ -f "$f" ] || return 0
	base=${file%.*}
	sec=${file##*.}
	nd=$(summary "$f")
	{
		echo '---'
		echo "title: $base($sec)"
		echo "parent: $parent"
		echo "grand_parent: API reference"
		echo "nav_order: $order"
		echo "permalink: /reference/api/$base/"
		echo '---'
		echo
		echo "# ${base}($sec)"
		echo
		[ -n "$nd" ] && { echo "$nd"; echo; }
		echo '<div class="manpage" markdown="0">'
		render "$f"
		echo '</div>'
		echo
		echo "[View the man-page source](https://codeberg.org/gregburd/libumem/src/branch/master/$file){: .fs-2 }"
	} > "$OUT/$base.md"
}

# Category index pages + their member pages.
order=1
echo "$cat_defs" | while IFS='|' read -r slug title members; do
	[ -n "$slug" ] || continue
	{
		echo '---'
		echo "title: $title"
		echo 'parent: API reference'
		echo "nav_order: $order"
		echo "permalink: /reference/api/$slug/"
		echo 'has_children: true'
		echo '---'
		echo
		echo "# $title"
		echo
		for m in $members; do
			f="$ROOT/$m"
			[ -f "$f" ] || continue
			base=${m%.*}
			sec=${m##*.}
			nd=$(summary "$f")
			echo "- [\`$base($sec)\`]({{ '/reference/api/$base/' | relative_url }}) -- $nd"
		done
	} > "$OUT/cat-$slug.md"
	sub=1
	for m in $members; do
		emit_page "$m" "$title" "$sub"
		sub=$((sub + 1))
	done
	order=$((order + 1))
done

echo "gen_api_reference: wrote pages under $OUT"
