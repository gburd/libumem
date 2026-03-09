{
  pkgs,
  pkgs-unstable,
  system,
}: let
  patchedGlibc = pkgs.glibc.overrideAttrs (oldAttrs: {
    patches = (oldAttrs.patches or []) ++ [
      ./glibc-no-fortify-warning.patch
    ];
  });

  llvmPkgs = pkgs-unstable.llvmPackages_21;

  getDeps = with pkgs; [
    # Build system
    pkgs-unstable.meson
    pkgs-unstable.ninja
    pkg-config
    autoconf
    automake
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

    # Development tools
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
  ];

  devShell = pkgs.mkShell {
    name = "dev";
    buildInputs = getDeps;

    shellHook = let
      icon = "f121";
    in ''
      export HISTFILE=.history
      export HISTSIZE=1000000
      export HISTFILESIZE=1000000

      unset LD_LIBRARY_PATH LD_PRELOAD LIBRARY_PATH C_INCLUDE_PATH CPLUS_INCLUDE_PATH

      export PATH="${pkgs.which}/bin:${pkgs.coreutils}/bin:$PATH"
      export PS1="$(echo -e '\u${icon}') {\[$(tput sgr0)\]\[\033[38;5;228m\]\w\[$(tput sgr0)\]\[\033[38;5;15m\]} ($(git rev-parse --abbrev-ref HEAD)) \\$ \[$(tput sgr0)\]"

      export PATH=${pkgs.clang-tools}/bin:$PATH
      export PATH=${pkgs.cppcheck}/bin:$PATH

      UV_PYTHON_DOWNLOADS=never

      export CC="${pkgs.gcc}/bin/gcc"
      export CXX="${pkgs.gcc}/bin/g++"
    '';
  };

in {
  inherit devShell;
}
