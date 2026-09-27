{
  description = "CIRCT CI built from reusable Nix LLVM/MLIR derivations";

  inputs = {
    circt-src = {
      url = "github:llvm/circt/27622798bd566646effbc07974500b6a669f4993";
      flake = false;
    };
    llvm-src = {
      url = "github:llvm/llvm-project/040a641988f6ed6f4fab250706ca2b620c1de2d8";
      flake = false;
    };
    circt-nix = {
      url = "github:xinpian-tech/circt-nix";
      inputs.circt-src.follows = "circt-src";
      inputs.llvm-src.follows = "llvm-src";
    };
    nixpkgs.follows = "circt-nix/nixpkgs";
    cli11-src.follows = "circt-nix/cli11-src";
    fmt-src.follows = "circt-nix/fmt-src";
    googletest-src.follows = "circt-nix/googletest-src";
    ixwebsocket-src.follows = "circt-nix/ixwebsocket-src";
    nlohmann-json-src.follows = "circt-nix/nlohmann-json-src";
    zlib-src.follows = "circt-nix/zlib-src";
  };

  outputs =
    {
      self,
      circt-nix,
      circt-src,
      cli11-src,
      fmt-src,
      googletest-src,
      ixwebsocket-src,
      llvm-src,
      nixpkgs,
      nlohmann-json-src,
      zlib-src,
    }:
    let
      supportedSystems = [ "x86_64-linux" ];
      forAllSystems = nixpkgs.lib.genAttrs supportedSystems;
      mkPython = system: circt-nix.legacyPackages.${system}.circtPython;
      mkCore =
        system:
        let
          pkgs = import nixpkgs {
            inherit system;
            overlays = [ circt-nix.overlays.default ];
          };
        in
        {
          inherit pkgs;
          inherit (pkgs.circtFlakePkgs)
            libllvm
            mlir
            mkCirct
            slang
            ;
        };
    in
    {
      packages = forAllSystems (
        system:
        let
          core = mkCore system;
          circt = core.mkCirct { };
        in
        {
          # CI overrides circt-src with the pull request checkout and llvm-src
          # with its gitlink revision. The latter changes only when CIRCT
          # intentionally bumps LLVM.
          default = circt;
          inherit circt;
          inherit (core) libllvm mlir;
        }
      );

      checks = forAllSystems (
        system:
        let
          core = mkCore system;
          inherit (core)
            libllvm
            mlir
            mkCirct
            pkgs
            ;
          ciPkgs = import nixpkgs { inherit system; };
          python = mkPython system;
          # CI runners mount their machine-wide scratch object store here. The
          # path is absent in ordinary local builds, which use the disk fallback.
          sccacheObjectRoot = "/nix-sccache";

          ciInputs = [
            pkgs.clang-tools
            pkgs.espresso
            ciPkgs.iverilog
            python
            ciPkgs.sby
            ciPkgs.verilator
            ciPkgs.yosys
            pkgs.z3
          ];

          mkCirctCheck =
            {
              name,
              circt,
              mlir,
              extraCmakeFlags ? [ ],
              extraCheckTargets ? [ ],
              pycde ? false,
            }:
            circt.overrideAttrs (old: {
              pname = "circt-${name}";
              # Simulator/tool discovery happens during CMake configuration,
              # so these must be build inputs rather than check-only inputs.
              nativeBuildInputs =
                (old.nativeBuildInputs or [ ])
                ++ ciInputs
                ++ [
                  ciPkgs.rclone
                  ciPkgs.sccache
                ];
              nativeCheckInputs = (old.nativeCheckInputs or [ ]) ++ ciInputs;
              cmakeFlags =
                (old.cmakeFlags or [ ])
                ++ extraCmakeFlags
                ++ [
                  "-DBUILD_TESTING=ON"
                  "-DPython_EXECUTABLE=${python}/bin/python3"
                  "-DPython3_EXECUTABLE=${python}/bin/python3"
                  "-DCMAKE_C_COMPILER_LAUNCHER=${ciPkgs.sccache}/bin/sccache"
                  "-DCMAKE_CXX_COMPILER_LAUNCHER=${ciPkgs.sccache}/bin/sccache"
                  "-DSYSTEMC_PATH=${ciPkgs.lib.getDev ciPkgs.systemc}/include"
                ];
              preConfigure = (old.preConfigure or "") + ''
                # Keep the value containing whitespace as one CMake argv.
                cmakeFlagsArray+=("-DLLVM_LIT_ARGS=-v --show-unsupported --timeout=300")
                export PATH="${python}/bin:$PATH"
                sccache_rclone_pid=
                sccache_rclone_log="$NIX_BUILD_TOP/rclone-s3.log"
                sccache_object_root=${ciPkgs.lib.escapeShellArg sccacheObjectRoot}
                if [[ -d "$sccache_object_root" ]]; then
                  test -w "$sccache_object_root"
                  mkdir -p "$sccache_object_root/circt-sccache"

                  # The S3 server and sccache both remain inside this build's
                  # network/mount namespaces. Only the server's plain object
                  # directory crosses the sandbox boundary.
                  (
                    umask 000
                    exec ${ciPkgs.rclone}/bin/rclone serve s3 \
                      --addr 127.0.0.1:19000 \
                      :local:"$sccache_object_root"
                  ) >"$sccache_rclone_log" 2>&1 &
                  sccache_rclone_pid=$!

                  rclone_ready=false
                  for _ in $(seq 1 100); do
                    if ! kill -0 "$sccache_rclone_pid" 2>/dev/null; then
                      break
                    fi
                    if (exec 3<>/dev/tcp/127.0.0.1/19000) \
                        2>/dev/null; then
                      rclone_ready=true
                      break
                    fi
                    sleep 0.1
                  done
                  if [[ "$rclone_ready" != true ]]; then
                    echo "The sandbox-local S3 server failed to start:" >&2
                    tail -n 200 "$sccache_rclone_log" >&2 || true
                    exit 1
                  fi

                  export SCCACHE_BUCKET=circt-sccache
                  export SCCACHE_ENDPOINT=http://127.0.0.1:19000
                  export SCCACHE_REGION=us-east-1
                  export SCCACHE_S3_ENABLE_VIRTUAL_HOST_STYLE=false
                  export SCCACHE_S3_NO_CREDENTIALS=true
                  export SCCACHE_S3_USE_SSL=false
                  unset SCCACHE_DIR SCCACHE_S3_KEY_PREFIX
                  unset AWS_ACCESS_KEY AWS_ACCESS_KEY_ID AWS_SECRET_ACCESS_KEY
                  unset AWS_SECRET_KEY AWS_SESSION_TOKEN
                else
                  # Local builds keep using an isolated on-disk cache.
                  export SCCACHE_DIR="$NIX_BUILD_TOP/.sccache"
                  mkdir -p "$SCCACHE_DIR"
                fi
                export SCCACHE_SERVER_UDS="$NIX_BUILD_TOP/sccache.sock"
                export SCCACHE_BASEDIRS="$NIX_BUILD_TOP:$out:$lib:$dev"
                export SCCACHE_CACHE_SIZE=1G
                export SCCACHE_ERROR_LOG="''${TMPDIR:-/build}/sccache-errors.log"
                export SCCACHE_IDLE_TIMEOUT=0
                export SCCACHE_IGNORE_SERVER_IO_ERROR=1
                # The variables also cover nested CMake projects in postCheck.
                export CMAKE_C_COMPILER_LAUNCHER="${ciPkgs.sccache}/bin/sccache"
                export CMAKE_CXX_COMPILER_LAUNCHER="${ciPkgs.sccache}/bin/sccache"

                sccache_cleanup() {
                  local status=$?
                  trap - EXIT
                  ${ciPkgs.sccache}/bin/sccache --show-stats || true
                  if [[ -s "$SCCACHE_ERROR_LOG" ]]; then
                    echo "Last 200 lines from the sccache error log:" >&2
                    tail -n 200 "$SCCACHE_ERROR_LOG" >&2
                  fi
                  ${ciPkgs.sccache}/bin/sccache --stop-server \
                    >/dev/null || true
                  if [[ -n "$sccache_rclone_pid" ]]; then
                    rclone_was_running=true
                    if ! kill -0 "$sccache_rclone_pid" 2>/dev/null; then
                      rclone_was_running=false
                      echo "The sandbox-local S3 server exited early:" >&2
                    else
                      kill "$sccache_rclone_pid" 2>/dev/null || true
                    fi
                    wait "$sccache_rclone_pid" 2>/dev/null || true
                    if [[ "$status" -ne 0 \
                          || "$rclone_was_running" != true ]]; then
                      tail -n 200 "$sccache_rclone_log" >&2 || true
                    fi
                  fi
                  exit "$status"
                }
                trap sccache_cleanup EXIT
              '';
              doCheck = true;
              checkTarget = pkgs.lib.concatStringsSep " " (
                [
                  "check-circt"
                  "check-circt-unit"
                  "check-circt-capi"
                  "check-circt-integration"
                ]
                ++ extraCheckTargets
              );
              preCheck =
                (old.preCheck or "")
                + ''
                  for tool in clang-tidy iverilog sby verilator yosys \
                    yosys-abc z3
                  do
                    command -v "$tool" >/dev/null
                  done
                  test -r "${ciPkgs.lib.getDev ciPkgs.systemc}/include/systemc"
                ''
                + pkgs.lib.optionalString pycde ''
                  export PYTHONPATH="$PWD/python_packages/pycde:$PWD/lib/Dialect/ESI/runtime/python"
                  runtime_build="$PWD/lib/Dialect/ESI/runtime"
                  export PATH="$PWD/bin:$runtime_build:$PATH"
                  # ESI's C++ integration fixtures run nested CMake projects.
                  # FetchContent uses the immutable CLI11 source directly in
                  # Nix, so expose its include root to those nested searches.
                  export CMAKE_INCLUDE_PATH="${cli11-src}/include:''${CMAKE_INCLUDE_PATH:-}"
                  export LD_LIBRARY_PATH="$PWD/lib:$runtime_build:''${LD_LIBRARY_PATH:-}"
                  export LIBRARY_PATH="$PWD/lib:$runtime_build:''${LIBRARY_PATH:-}"
                  export ESI_RUNTIME_TESTS_BIN="$PWD/lib/Dialect/ESI/runtime/tests/cpp/ESIRuntimeCppTests"
                '';
              postCheck =
                (old.postCheck or "")
                + ''
                  cmake -G Ninja \
                    -S "$CIRCT_SOURCE_ROOT/examples/circt-standalone" \
                    -B "$PWD/circt-standalone" \
                    -DCMAKE_BUILD_TYPE=Release \
                    -DCIRCT_DIR="$PWD/lib/cmake/circt" \
                    -DMLIR_DIR="${pkgs.lib.getDev mlir}/lib/cmake/mlir" \
                    -DLLVM_EXTERNAL_LIT="${python}/bin/.lit-wrapped" \
                    "-DLLVM_LIT_ARGS=-v --show-unsupported --timeout=300" \
                    -DPython_EXECUTABLE="${python}/bin/python3" \
                    -DPython3_EXECUTABLE="${python}/bin/python3"
                  cmake --build "$PWD/circt-standalone" \
                    --target check-circt-standalone
                ''
                + pkgs.lib.optionalString pycde ''
                  runtime_prefix="$PWD/lib/Dialect/ESI/runtime/python/esiaccel"
                  cmake --install . \
                    --prefix "$runtime_prefix" \
                    --component ESIRuntime
                  # The Nix CMake hook gives CMAKE_INSTALL_INCLUDEDIR an
                  # absolute $dev path, so --prefix cannot redirect these
                  # runtime headers beside esiaccelConfig.cmake for pytest.
                  mkdir -p "$runtime_prefix/include"
                  cp -r "$CIRCT_SOURCE_ROOT/lib/Dialect/ESI/runtime/cpp/include/esi" \
                    "$runtime_prefix/include/esi"
                  test -r "$runtime_prefix/include/esi/Accelerator.h"
                  test -x "$ESI_RUNTIME_TESTS_BIN"
                  command -v esiquery >/dev/null
                  command -v esitester >/dev/null
                  python3 -c 'import cocotb, cocotb_test, pycde'
                  python3 -m pytest "$CIRCT_SOURCE_ROOT/lib/Dialect/ESI/runtime/tests" \
                    -v --log-cli-level=INFO

                  # Preserve the legacy standalone-runtime packaging coverage
                  # inside the Nix check without rerunning every cosim test.
                  standalone_runtime_build="$PWD/esi-runtime-standalone"
                  standalone_runtime_prefix="$PWD/esi-runtime-install"
                  cmake -G Ninja \
                    -S "$CIRCT_SOURCE_ROOT/lib/Dialect/ESI/runtime" \
                    -B "$standalone_runtime_build" \
                    -DBUILD_TESTING=ON \
                    -DCMAKE_BUILD_TYPE=Release \
                    -DCMAKE_INSTALL_INCLUDEDIR=include \
                    -DCMAKE_INSTALL_LIBDIR=lib \
                    -DESI_COSIM=ON \
                    -DESI_RUNTIME_TRACE=ON \
                    -DFETCHCONTENT_SOURCE_DIR_CLI11_PROJ=${cli11-src} \
                    -DFETCHCONTENT_SOURCE_DIR_FMT=${fmt-src} \
                    -DFETCHCONTENT_SOURCE_DIR_GOOGLETEST=${googletest-src} \
                    -DFETCHCONTENT_SOURCE_DIR_IXWEBSOCKET=${ixwebsocket-src} \
                    -DFETCHCONTENT_SOURCE_DIR_JSON=${nlohmann-json-src} \
                    -DFETCHCONTENT_SOURCE_DIR_ZLIB=${zlib-src} \
                    -DPython_EXECUTABLE=${python}/bin/python3 \
                    -DPython3_EXECUTABLE=${python}/bin/python3
                  cmake --build "$standalone_runtime_build" \
                    --target ESIRuntime ESIRuntimeCppTests
                  cmake --install "$standalone_runtime_build" \
                    --prefix "$standalone_runtime_prefix" \
                    --component ESIRuntime
                  test -r "$standalone_runtime_prefix/include/esi/Accelerator.h"
                  test -r "$standalone_runtime_prefix/cmake/esiaccelConfig.cmake"
                  test -x "$standalone_runtime_prefix/bin/esiquery"
                  env \
                    ESI_RUNTIME_TESTS_BIN="$standalone_runtime_build/tests/cpp/ESIRuntimeCppTests" \
                    LD_LIBRARY_PATH="$standalone_runtime_prefix/lib:$PWD/lib:''${LD_LIBRARY_PATH:-}" \
                    LIBRARY_PATH="$standalone_runtime_prefix/lib:$PWD/lib:''${LIBRARY_PATH:-}" \
                    PATH="$standalone_runtime_prefix/bin:$PATH" \
                    PYTHONPATH="$standalone_runtime_prefix:$PWD/python_packages/pycde" \
                    python3 -m pytest \
                      "$CIRCT_SOURCE_ROOT/lib/Dialect/ESI/runtime/tests/unit" \
                      -v --log-cli-level=INFO
                '';
              passthru = (old.passthru or { }) // {
                inherit libllvm mlir;
              };
            });

          # Normal CI directly reuses these cached libllvm and mlir derivations.
          fullCirct = mkCirct {
            buildSharedLibs = true;
            enableAssertions = true;
          };

          clangCirct = mkCirct {
            stdenv = ciPkgs.llvmPackages_21.stdenv;
            buildSharedLibs = false;
            # Assertion mode is part of the shared LLVM/MLIR derivation. Keep
            # it identical across variants while retaining the Clang/static
            # CIRCT configuration from the previous matrix.
            enableAssertions = true;
          };

          fullCheck = mkCirctCheck {
            name = "full-check";
            circt = fullCirct;
            inherit mlir;
            pycde = true;
            extraCmakeFlags = [
              "-DCIRCT_BINDINGS_PYTHON_ENABLED=ON"
              "-DCIRCT_ENABLE_FRONTENDS=PyCDE"
              "-DESI_COSIM=ON"
              "-DESI_RUNTIME=ON"
              "-DESI_RUNTIME_TRACE=ON"
              "-DFETCHCONTENT_SOURCE_DIR_CLI11_PROJ=${cli11-src}"
              "-DFETCHCONTENT_SOURCE_DIR_FMT=${fmt-src}"
              "-DFETCHCONTENT_SOURCE_DIR_GOOGLETEST=${googletest-src}"
              "-DFETCHCONTENT_SOURCE_DIR_IXWEBSOCKET=${ixwebsocket-src}"
              "-DFETCHCONTENT_SOURCE_DIR_JSON=${nlohmann-json-src}"
              "-DFETCHCONTENT_SOURCE_DIR_ZLIB=${zlib-src}"
              "-DMLIR_ENABLE_BINDINGS_PYTHON=ON"
              # zlib is an ESI build dependency, not part of the CIRCT
              # package.  Its generated pkg-config file combines absolute
              # multi-output paths into an invalid prefix during fixup.
              "-DZLIB_INSTALL=OFF"
            ];
            extraCheckTargets = [
              "check-pycde"
              "check-pycde-integration"
              "ESIRuntime"
              "ESIRuntimeCppTests"
            ];
          };

          clangCheck = mkCirctCheck {
            name = "clang-check";
            circt = clangCirct;
            inherit mlir;
            extraCmakeFlags = [
              # GCC-built shared MLIR and Clang-built CIRCT derive different
              # implicit trait TypeIDs.  The full check covers every Python
              # test with a compiler-consistent stack; keep this variant for
              # Clang/static/reverse-iteration coverage.
              "-DCIRCT_BINDINGS_PYTHON_ENABLED=OFF"
              "-DLLVM_ENABLE_REVERSE_ITERATION=ON"
              "-DMLIR_ENABLE_BINDINGS_PYTHON=OFF"
            ];
          };
        in
        {
          full = fullCheck;
          clang = clangCheck;
        }
      );

      devShells = forAllSystems (
        system:
        let
          pkgs = import nixpkgs { inherit system; };
          python = pkgs.python3.withPackages (ps: [
            ps.yapf
          ]);
        in
        {
          # This shell is only for cheap repository linting. Builds and tests
          # use the derivations above so LLVM is never rebuilt in a dev shell.
          lint = pkgs.mkShell {
            packages = with pkgs; [
              clang-tools
              git
              llvmPackages_21.stdenv.cc.cc.python
              python
            ];
          };
          default = self.devShells.${system}.lint;
        }
      );
    };
}
