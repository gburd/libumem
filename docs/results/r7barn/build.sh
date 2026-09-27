set -e
top=$(pwd)
./scripts/ec2/clean-regen.sh
make -j$(nproc) >/tmp/r7b-build-def.log 2>&1 || { tail -30 /tmp/r7b-build-def.log; exit 1; }
echo BUILD_DEFAULT_OK
P="$top-probe"
test ! -e "$P" || mv "$P" "$P.old.$$"
cp -a "$top" "$P" && cd "$P"
make distclean >/dev/null 2>&1 || true
./scripts/ec2/clean-regen.sh CPPFLAGS=-DUMEM_BARN_PROBE
make -j$(nproc) >/tmp/r7b-build-probe.log 2>&1 || { tail -30 /tmp/r7b-build-probe.log; exit 1; }
echo "probe syms: $(nm .libs/libumem.so | grep -c umem_barn_)"
echo "default syms: $(nm $top/.libs/libumem.so | grep -c umem_barn_)"
echo BUILD_PROBE_OK
