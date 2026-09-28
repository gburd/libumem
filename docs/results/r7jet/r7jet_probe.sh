echo "== env GLIBC_TUNABLES=[${GLIBC_TUNABLES-unset}] LD_PRELOAD=[${LD_PRELOAD-unset}]"; grep -rs GLIBC_TUNABLES /etc/environment /etc/profile.d ~/.bashrc ~/.profile || echo "no GLIBC_TUNABLES in /etc/environment, profile.d, ~/.bashrc, ~/.profile"
./scripts/ec2/clean-regen.sh && make -j$(nproc) test/bench/bench_main >/tmp/b.log 2>&1 || { tail -20 /tmp/b.log; exit 1; }
B=test/bench/.libs/bench_main; export LD_LIBRARY_PATH=$PWD/.libs
echo "== readelf PT_TLS"; for f in .libs/libumem.so.1 $B /usr/lib/x86_64-linux-gnu/libjemalloc.so.2; do echo "-- $f"; readelf -lW $f | grep -E 'TLS|Type'; done
echo "== STATIC_TLS flag"; readelf -dW .libs/libumem.so.1 | grep -i flags || echo "no FLAGS"
echo "== no tunable"; env -u GLIBC_TUNABLES -u LD_PRELOAD $B -a jemalloc -w single -n 1000 -s 16:16 -c; echo "rc=$?"
echo "== LD_DEBUG=libs (no tunable)"; env -u GLIBC_TUNABLES LD_DEBUG=libs $B -a jemalloc -w single -n 1000 -s 16:16 -c 2>&1 | grep -iE 'jemalloc|static TLS|error' | head -8
echo "== with tunable"; GLIBC_TUNABLES=glibc.rtld.optional_static_tls=65536 $B -a jemalloc -w single -n 1000 -s 16:16 -c; echo "rc=$?"
