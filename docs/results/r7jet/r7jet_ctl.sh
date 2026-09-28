cd ~/libumem-iso-pb-2e88c21-2e88c219ef72
echo "== sha256 (r7harness lo-t8 provenance: libumem.so db5a76d8..., bench_main 4df46913...)"; sha256sum .libs/libumem.so test/bench/.libs/bench_main
cat > /tmp/dlj.c <<'C'
#include <dlfcn.h>
#include <stdio.h>
int main(void){ void *h=dlopen("libjemalloc.so.2",RTLD_NOW|RTLD_LOCAL); printf("dlopen jemalloc: %s\n", h?"OK":dlerror()); return !h; }
C
gcc -o /tmp/dlj /tmp/dlj.c -ldl && gcc -o /tmp/dlj_umem /tmp/dlj.c -ldl -L.libs -lumem -Wl,--no-as-needed
echo "== control: plain program, no libumem, no tunable"; env -u GLIBC_TUNABLES /tmp/dlj; echo rc=$?
echo "== same, with tunable"; GLIBC_TUNABLES=glibc.rtld.optional_static_tls=65536 /tmp/dlj; echo rc=$?
echo "== linked to libumem"; LD_LIBRARY_PATH=$PWD/.libs env -u GLIBC_TUNABLES /tmp/dlj_umem; echo rc=$?
export LD_LIBRARY_PATH=$PWD/.libs
echo "== under perf record, no tunable, r7harness-style frag -t 8"; env -u GLIBC_TUNABLES perf record -q -F 499 -g -o /tmp/p.data -- test/bench/.libs/bench_main -a jemalloc -c -w frag -t 8 -n 80000 -s 16:4096 -D 10:2 -W 1 2>&1 | grep -v '^\[' | head -3; echo "rc=${PIPESTATUS[0]}"
echo "== libtool wrapper test/bench/bench_main, no tunable"; env -u GLIBC_TUNABLES test/bench/bench_main -a jemalloc -w single -n 1000 -s 16:16 -c; echo rc=$?
echo "== LD_PRELOAD libjemalloc, no tunable"; env -u GLIBC_TUNABLES LD_PRELOAD=libjemalloc.so.2 test/bench/.libs/bench_main -a jemalloc -w single -n 1000 -s 16:16 -c; echo rc=$?
