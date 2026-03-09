{
  pkgs,
  pkgs-unstable,
  system,
}: let
  # Create a patched glibc only for the dev shell
  patchedGlibc = pkgs.glibc.overrideAttrs (oldAttrs: {
    patches = (oldAttrs.patches or []) ++ [
      ./glibc-no-fortify-warning.patch
    ];
  });

  llvmPkgs = pkgs-unstable.llvmPackages_21;

  # Single dependency function that can be used for all environments
  getDeps =
    with pkgs;
      [
        # Build system (always use host tools)
        pkgs-unstable.meson
        pkgs-unstable.ninja
        pkg-config
        autoconf
        libtool
        git
        which
        binutils
        gnumake

        # Parser/lexer tools
        bison
        flex

        # Documentation
        docbook_xml_dtd_45
        docbook-xsl-nons
        fop
        gettext
        libxslt
        libxml2

        # Development tools (always use host tools)
        coreutils
        shellcheck
        ripgrep
        valgrind
        curl
        uv
        pylint
        black
        lcov
        strace
        ltrace
        perf-tools
        perf
        flamegraph
        htop
        iotop
        sysstat
        ccache
        cppcheck
        compdb

        # GCC/GDB
#        pkgs-unstable.gcc15
        gcc
        gdb

        # LLVM toolchain
        llvmPkgs.clang-tools

      # Glibc target libraries
          readline
          zlib
          openssl
          icu
          lz4
          zstd
          libuuid
          libkrb5
          linux-pam
          libxcrypt
          numactl
          openldap
          liburing
          libselinux
          patchedGlibc
          glibcInfo
          glibc.dev
        ]
      );

  # Development shell (GCC + glibc)
  devShell = pkgs.mkShell {
    name = "dev";
    buildInputs =
      (getPostgreSQLDeps false)
      ++ [
        flameGraphScript
        pgbenchScript
      ];

    shellHook = let
      icon = "f121";
    in ''
      # History configuration
      export HISTFILE=.history
      export HISTSIZE=1000000
      export HISTFILESIZE=1000000

      # Clean environment
      unset LD_LIBRARY_PATH LD_PRELOAD LIBRARY_PATH C_INCLUDE_PATH CPLUS_INCLUDE_PATH

      # Essential tools in PATH
      export PATH="${pkgs.which}/bin:${pkgs.coreutils}/bin:$PATH"
      export PS1="$(echo -e '\u${icon}') {\[$(tput sgr0)\]\[\033[38;5;228m\]\w\[$(tput sgr0)\]\[\033[38;5;15m\]} ($(git rev-parse --abbrev-ref HEAD)) \\$ \[$(tput sgr0)\]"

      # Development tools in PATH
      export PATH=${pkgs.clang-tools}/bin:$PATH
      export PATH=${pkgs.cppcheck}/bin:$PATH

      # Python UV
      UV_PYTHON_DOWNLOADS=never

      # GCC configuration
      export CC="${pkgs.gcc}/bin/gcc"
      export CXX="${pkgs.gcc}/bin/g++"
  };

in {
  inherit devShell; 
}
