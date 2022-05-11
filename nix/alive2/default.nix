{ stdenv, cmake, llvm, pkgconfig, re2c, z3, gitMinimal, fetchgitLocal, nix-gitignore }:

let
  gitFilter = patterns: root: with nix-gitignore;
      gitignoreFilterPure (_: _: true) (withGitignoreFile patterns root) root;
in

stdenv.mkDerivation {
  name = "alive2";
  version = "0.0.1";

  src = builtins.path {
    path = ../..;
    name = "alive2-source";
    filter = gitFilter [''
      .*
      *.nix
      /build/
      /nix/
      /result
    ''] ../..;
  };

  nativeBuildInputs = [ cmake gitMinimal re2c ];
  buildInputs = [ llvm z3 ];

  postPatch = ''
    sed -i -e 's/.*GIT_EXECUTABLE.*describe.*>>/COMMAND echo 0 >>/' CMakeLists.txt
  '';

  cmakeBuildType = "Debug";
  doCheck = false;
  cmakeFlags = [
    "-DBUILD_LLVM_UTILS=1"
    "-DBUILD_TV=1"
  ];
  NIX_CFLAGS_COMPILE = [ "-Wno-error=sign-compare" ];
  hardeningDisable = [ "fortify" ];

  enableParallelBuilding = true;

  installPhase = ''
    mkdir -p $out/bin $out/lib/alive2
    cp alive alive-exec alive-jobserver alive-tv alive-worker $out/bin/
    cp tv/tv.so $out/lib/alive2/

    # libtools.a, etc.
    # alivecc
    # alive++
    # benchmark-clang-tv
    # benchmark-llvm-test-suite
    # opt-alive.sh
    # opt-alive-test.sh
  '';
}
