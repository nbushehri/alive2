let
  default_nixpkgs = (import <nixpkgs> {}).fetchFromGitHub {
    owner = "NixOS";
    repo = "nixpkgs";
    rev = "248936ea5700b25cfa9b7eaf8abe13a11fe15617";
    sha256 = "1hsmyzd0194l279pm8ahz2lp0lfmaza05j7cjmlz7ryji15zvcyx";
  };
in
{ nixpkgs ? default_nixpkgs }:

with import nixpkgs {};
rec {
  llvm = (llvmPackages_13.libllvm.override {
    debugVersion = true;
  }).overrideAttrs (o: {
    # Alive2 requires LLVM to have RTTI and exception support, which aren't
    # normally enabled.
    cmakeFlags = o.cmakeFlags ++ [
      "-DLLVM_ENABLE_RTTI=ON"
      "-DLLVM_ENABLE_EH=ON"
    ];
    doCheck = false; # reduce build time
  });

  z3 = (import nixpkgs {}).z3.overrideAttrs (o: {
    src = fetchFromGitHub {
      # 2021-10-24
      owner  = "Z3Prover";
      repo   = "z3";
      rev    = "3a3cef8fcef58f31e9ec6495346eb065b816b155";
      sha256 = "1zg4mym7iiwyq1pddqmahsfr2yzd1j2c2vjizpk0v9805g8y4xk0";
    };
  });



  alive2 = callPackage nix/alive2 {
    inherit llvm z3;
  };


  # Singularity container (to be run on HTCondor cluster)
  alive-worker-singularity = singularity-tools.buildImage {
    name = "alive-worker";
    contents = [ pkgs.busybox ];
    diskSize = 4096;
    runScript = ''
      #!/bin/sh
      set +e
      for i in $(seq 4); do
        while true; do
          echo starting worker...
          ${alive2}/bin/alive-worker "$@"
          echo exit code: $?
        done &
      done
      sleep 7d
      kill $(jobs -p)
    '';
  };
}
