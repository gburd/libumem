#!/bin/bash
# table.sh <section-start> <section-end> intel/out.log arm/out.log
# Markdown table from hotpath_ab.sh output: point | x86 null | x86 A/B | arm null | arm A/B
ext() { sed -n "/^$1\$/,/^$2/p" "$3" | awk '/^  (null|A\/B) /{
	kind=$1; lbl=""; for(i=2;i<=NF;i++){ if($i ~ /^last/) break; lbl=lbl" "$i }
	sub(/^ /,"",lbl); med=""; for(i=1;i<=NF;i++){ if($i=="median") med=$(i+1); if($i=="(min") mn=$(i+1); if($i=="max") mx=$(i+1) }
	gsub(/[%,)]/,"",mn); gsub(/[%,)]/,"",mx); gsub(/%/,"",med)
	print kind "\t" lbl "\t" med " (" mn ".." mx ")" }'; }
paste <(ext "$1" "$2" "$3" | awk -F'\t' '$1=="null"{n=$3} $1=="A/B"{print $2 "\t" n "\t" $3}') \
      <(ext "$1" "$2" "$4" | awk -F'\t' '$1=="null"{n=$3} $1=="A/B"{print n "\t" $3}') |
awk -F'\t' 'BEGIN{print "| point | x86 null | x86 A/B | arm null | arm A/B |\n|---|---|---|---|---|"}
	{printf "| %s | %s | %s | %s | %s |\n",$1,$2,$3,$4,$5}'
