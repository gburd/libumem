#!/usr/bin/env bash
# scripts/ec2/r8rel_abi.sh -- step 4: ABI vs v4.0.0.
# Builds v4.0.0 (from ~/v400.tar) and 744f639 (from ~/post.tar) default libs,
# reports soname + defined-export counts (nm -D --defined-only) and any diff.
# Expect: soname libumem.so.2 unchanged, libumem_malloc.so.1 unchanged, 0 export diff.
set -u
J=$(nproc)
OUT=~/abi; rm -rf "$OUT"; mkdir -p "$OUT"

build() { local d="$1" tb="$2"; rm -rf "$d"; mkdir -p "$d"; tar -xf "$tb" -C "$d"
  ( cd "$d" && ./scripts/ec2/clean-regen.sh CFLAGS="-O2 -g" >regen.log 2>&1 \
      && make -j"$J" libumem.la libumem_malloc.la >make.log 2>&1 ) \
    || { echo "ABI_BUILD_FAIL $d"; tail -20 "$d/make.log" "$d/regen.log"; return 1; }
}
soname() { objdump -p "$1" 2>/dev/null | awk '/SONAME/{print $2}'; }
exports() { nm -D --defined-only "$1" 2>/dev/null | awk '{print $3}' | grep -v '^$' | sort -u; }

echo "########## build v4.0.0 ##########"; build ~/abi_v400 ~/v400.tar || exit 1
echo "########## build 744f639 ##########"; build ~/abi_v410 ~/post.tar || exit 1

for lib in libumem libumem_malloc; do
  OLD=$(ls ~/abi_v400/.libs/${lib}.so.* 2>/dev/null | grep -E "\.so\.[0-9]+\.[0-9]+\.[0-9]+$" | head -1)
  NEW=$(ls ~/abi_v410/.libs/${lib}.so.* 2>/dev/null | grep -E "\.so\.[0-9]+\.[0-9]+\.[0-9]+$" | head -1)
  echo "==== $lib ===="
  echo "v4.0.0 file: $OLD  soname: $(soname "$OLD")"
  echo "744f639 file: $NEW  soname: $(soname "$NEW")"
  exports "$OLD" > "$OUT/${lib}.v400.exports"
  exports "$NEW" > "$OUT/${lib}.v410.exports"
  echo "v4.0.0 defined-export count: $(wc -l < "$OUT/${lib}.v400.exports")"
  echo "744f639 defined-export count: $(wc -l < "$OUT/${lib}.v410.exports")"
  echo "-- exports ONLY in v4.0.0 (removed) --"; comm -23 "$OUT/${lib}.v400.exports" "$OUT/${lib}.v410.exports"
  echo "-- exports ONLY in 744f639 (added)  --"; comm -13 "$OUT/${lib}.v400.exports" "$OUT/${lib}.v410.exports"
  echo "-- diff count: $(comm -3 "$OUT/${lib}.v400.exports" "$OUT/${lib}.v410.exports" | wc -l) --"
done
echo "########## ABI DONE ##########"
