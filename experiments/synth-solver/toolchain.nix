let pkgs = import <nixpkgs> {}; in
pkgs.mkShell {
  packages = [ pkgs.cmake pkgs.ninja pkgs.clang pkgs.lld
    (pkgs.python3.withPackages (ps: [ ps.nanobind ps.numpy ps.pyyaml ps.pybind11 ])) ];
  shellHook = ''
    export LD_LIBRARY_PATH=${pkgs.stdenv.cc.cc.lib}/lib''${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}
  '';
}
