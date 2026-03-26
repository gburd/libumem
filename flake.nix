{
  description = "Port of Solaris umem slab allocator";

  inputs = {
    nixpkgs.url = "github:NixOS/nixpkgs/nixos-unstable";
    flake-utils.url = "github:numtide/flake-utils";
  };

  outputs = { self, nixpkgs, flake-utils }:
    flake-utils.lib.eachDefaultSystem (system:
      let
        pkgs = nixpkgs.legacyPackages.${system};
        lib = pkgs.lib;
      in
      {
        packages = {
          default = self.packages.${system}.libumem;

          libumem = pkgs.stdenv.mkDerivation (finalAttrs: {
            pname = "libumem";
            version = "1.0.2";

            src = lib.cleanSource ./.;

            outputs = [ "out" "dev" "doc" ];

            nativeBuildInputs = with pkgs; [
              autoconf
              automake
              libtool
              pkg-config
              doxygen
              graphviz
            ];

            enableParallelBuilding = true;

            preConfigure = ''
              ./autogen.sh
            '';

            configureFlags = [
              # Platform detection works automatically via config.guess
            ];

            doCheck = true;

            preCheck = ''
              # Patch shell script test shebang
              patchShebangs umem_test4

              # Set LD_LIBRARY_PATH so LD_PRELOAD tests can find libumem.so
              export LD_LIBRARY_PATH="$PWD/.libs:''${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
            '';

            postBuild = ''
              # Build documentation
              make html-local
            '';

            postInstall = ''
              # Generate pkg-config files for easier downstream integration
              mkdir -p $dev/lib/pkgconfig

              cat > $dev/lib/pkgconfig/libumem.pc <<EOF
              prefix=$out
              exec_prefix=$out
              libdir=$out/lib
              includedir=$dev/include

              Name: libumem
              Version: ${finalAttrs.version}
              Description: Port of Solaris's slab allocator
              Libs: -L\''${libdir} -lumem
              Cflags: -I\''${includedir}
              EOF

              cat > $dev/lib/pkgconfig/libumem-malloc.pc <<EOF
              prefix=$out
              exec_prefix=$out
              libdir=$out/lib
              includedir=$dev/include

              Name: libumem-malloc
              Version: ${finalAttrs.version}
              Description: Port of Solaris's slab allocator - malloc replacement
              Libs: -L\''${libdir} -lumem_malloc
              Cflags: -I\''${includedir}
              EOF

              # Install documentation
              mkdir -p $doc/share/doc/libumem
              if [ -d docs/html ]; then
                cp -r docs/html $doc/share/doc/libumem/
              fi
            '';

            meta = with lib; {
              description = "Port of Solaris umem slab allocator";
              longDescription = ''
                libumem is a port of Solaris umem to Linux. Provides high-performance
                slab allocation with per-thread caching (PTC) on x86/x86_64 architectures.

                The library includes:
                - libumem.so: Core allocator with umem_alloc/umem_free API
                - libumem_malloc.so: Drop-in malloc/free replacement via LD_PRELOAD

                PTC (Per-Thread Cache) provides lock-free fast paths for allocation
                on supported architectures through runtime code generation.
              '';
              homepage = "https://codeberg.org/gregburd/libumem";
              license = licenses.cddl;
              platforms = platforms.linux;
              maintainers = [ ];
            };
          });
        };

        devShells.default = pkgs.mkShell {
          inputsFrom = [ self.packages.${system}.libumem ];

          packages = with pkgs; [
            # Documentation
            doxygen
            graphviz

            # Development and debugging tools
            gdb
            valgrind

            # Code quality
            clang-tools
            cppcheck
          ];

          shellHook = ''
            echo "╔════════════════════════════════════════════╗"
            echo "║  libumem development environment          ║"
            echo "╚════════════════════════════════════════════╝"
            echo ""
            echo "Build:  ./autogen.sh && ./configure && make"
            echo "Test:   make check"
            echo "Docs:   make html-local"
            echo ""
            echo "Available tools: gdb, valgrind, doxygen, clang-tools, cppcheck"
            echo ""
          '';
        };

        checks = {
          default = self.packages.${system}.libumem;

          integration = pkgs.runCommand "libumem-integration-test"
            {
              buildInputs = [ self.packages.${system}.libumem pkgs.gcc ];
            }
            ''
              cat > test.c <<EOF
              #include <umem.h>
              #include <stdio.h>
              #include <string.h>

              int main() {
                // Test basic allocation
                void *p = umem_alloc(100, UMEM_DEFAULT);
                if (!p) {
                  fprintf(stderr, "umem_alloc failed\n");
                  return 1;
                }
                memset(p, 0xAA, 100);
                umem_free(p, 100);

                // Test cache allocation
                umem_cache_t *cache = umem_cache_create(
                  "test_cache", 64, 8, NULL, NULL, NULL, NULL, NULL, 0);
                if (!cache) {
                  fprintf(stderr, "umem_cache_create failed\n");
                  return 1;
                }

                void *obj = umem_cache_alloc(cache, UMEM_DEFAULT);
                if (!obj) {
                  fprintf(stderr, "umem_cache_alloc failed\n");
                  return 1;
                }
                umem_cache_free(cache, obj);
                umem_cache_destroy(cache);

                printf("All integration tests passed\n");
                return 0;
              }
              EOF

              gcc test.c \
                -I${self.packages.${system}.libumem.dev}/include \
                -L${self.packages.${system}.libumem.out}/lib \
                -lumem \
                -o test

              export LD_LIBRARY_PATH="${self.packages.${system}.libumem.out}/lib:''${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
              ./test

              touch $out
            '';
        };
      }
    );
}
