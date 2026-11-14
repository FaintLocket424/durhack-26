{
  description = "Raspberry Pi 5 (aarch64) C/C++ Cross-Compilation Template";

  inputs = {
    nixpkgs.url = "github:NixOS/nixpkgs/nixos-unstable";
    flake-utils.url = "github:numtide/flake-utils";
    treefmt-nix = {
      url = "github:numtide/treefmt-nix";
      inputs.nixpkgs.follows = "nixpkgs";
    };
  };

  outputs =
    {
      self,
      nixpkgs,
      flake-utils,
      treefmt-nix,
    }:
    flake-utils.lib.eachDefaultSystem (
      system:
      let
        pkgs = nixpkgs.legacyPackages.${system};
        crossPkgs = pkgs.pkgsCross.aarch64-multiplatform-musl.pkgsStatic;
        crossPkgsGlibc = pkgs.pkgsCross.aarch64-multiplatform;

        treefmtEval = treefmt-nix.lib.evalModule pkgs {
          projectRootFile = "flake.nix";
          programs = {
            nixfmt.enable = true;
            clang-format.enable = true;
          };
        };

        mkRpi5Package =
          targetPkgs:
          targetPkgs.stdenv.mkDerivation {
            pname = "app";
            version = "0.1.0";
            src = ./.;

            nativeBuildInputs = with pkgs; [
              gnumake
              pkg-config
            ];

            makeFlags = [
              "CC=${targetPkgs.stdenv.cc.targetPrefix}cc"
              "CXX=${targetPkgs.stdenv.cc.targetPrefix}c++"
            ];

            installPhase = ''
              runHook preInstall
              mkdir -p $out/bin
              cp -f bin/* $out/bin/ 2>/dev/null || cp -f app $out/bin/
              runHook postInstall
            '';

            meta.mainProgram = "app";
          };

        buildHelper = pkgs.writeShellScriptBin "build-rpi5" ''
          set -e
          NPROC=$(nproc 2>/dev/null || sysctl -n hw.ncpu 2>/dev/null || echo 2)
          bear -- make -j"$NPROC" "$@"
        '';

        cleanBuildHelper = pkgs.writeShellScriptBin "rebuild-rpi5" ''
          set -e
          make clean
          build-rpi5 "$@"
        '';

        runHelper = pkgs.writeShellApplication {
          name = "run-rpi5";
          runtimeInputs =
            with pkgs;
            [
              findutils
              coreutils
            ]
            ++ pkgs.lib.optionals (!pkgs.stdenv.hostPlatform.isAarch64) [
              qemu-user
            ];
          text = ''
            TARGET_FILE="''${1:-}"
            if [ -z "$TARGET_FILE" ]; then
              TARGET_FILE=$(find ./bin -maxdepth 1 -type f -executable 2>/dev/null | head -n 1 || true)
            fi

            if [ -z "$TARGET_FILE" ] || [ ! -f "$TARGET_FILE" ]; then
              echo "No binary found. Running build-rpi5..." >&2
              build-rpi5
              TARGET_FILE=$(find ./bin -maxdepth 1 -type f -executable | head -n 1)
            fi

            shift || true
            if [ "$(uname -m)" = "aarch64" ]; then
              "$TARGET_FILE" "$@"
            else
              qemu-aarch64 "$TARGET_FILE" "$@"
            fi
          '';
        };

        deployHelper = pkgs.writeShellApplication {
          name = "deploy-rpi5";
          runtimeInputs = with pkgs; [
            findutils
            openssh
            rsync
          ];
          text = ''
            TARGET_HOST="''${1:-''${RPI5_HOST:-}}"
            REMOTE_DIR="''${2:-homebrew}"

            if [ -z "$TARGET_HOST" ]; then
              echo "Usage: deploy-rpi5 <user@host> [remote_dir]" >&2
              echo "   or: RPI5_HOST=user@host deploy-rpi5" >&2
              exit 1
            fi

            TARGET_FILE=$(find ./bin -maxdepth 1 -type f -executable 2>/dev/null | head -n 1 || true)
            if [ -z "$TARGET_FILE" ] && [ -d "./result/bin" ]; then
              TARGET_FILE=$(find ./result/bin -maxdepth 1 -type f -executable | head -n 1 || true)
            fi

            if [ -z "$TARGET_FILE" ] || [ ! -f "$TARGET_FILE" ]; then
              echo "No binary found. Run 'build-rpi5' or 'nix build' first." >&2
              exit 1
            fi

            ssh "$TARGET_HOST" "mkdir -p '$REMOTE_DIR'"
            rsync -avzL --chmod=u+rwx "$TARGET_FILE" "$TARGET_HOST:$REMOTE_DIR/"
          '';
        };
      in
      {
        packages = {
          default = mkRpi5Package crossPkgs;
          glibc = mkRpi5Package crossPkgsGlibc;
        };

        apps.default = {
          type = "app";
          program = "${self.packages.${system}.default}/bin/app";
        };

        formatter = treefmtEval.config.build.wrapper;
        checks.formatting = treefmtEval.config.build.check self;

        devShells.default = pkgs.mkShell {
          packages = [
            crossPkgs.stdenv.cc
            pkgs.gnumake
            pkgs.pkg-config
            pkgs.clang-tools
            pkgs.bear
            pkgs.rsync
            pkgs.openssh
            buildHelper
            cleanBuildHelper
            runHelper
            deployHelper
          ]
          ++ pkgs.lib.optionals (!pkgs.stdenv.hostPlatform.isAarch64) [
            pkgs.qemu-user
          ];

          shellHook = ''
            export CC="${crossPkgs.stdenv.cc.targetPrefix}cc"
            export CXX="${crossPkgs.stdenv.cc.targetPrefix}c++"
            export AR="${crossPkgs.stdenv.cc.targetPrefix}ar"
            export STRIP="${crossPkgs.stdenv.cc.targetPrefix}strip"

            if [ ! -f .clangd ]; then
              cat > .clangd << 'EOF'
            CompileFlags:
              Add:
                - "--target=aarch64-unknown-linux-musl"
                - "-mcpu=cortex-a76"
            EOF
            fi

            cat << 'EOF'
            ========================================================================
            Environment: Raspberry Pi 5 (ARM64 / Cortex-A76)
            ------------------------------------------------------------------------
            Properties:
              Toolchain   : aarch64-unknown-linux-musl (static)
              LSP         : clangd

            Commands:
              build-rpi5               Build project + compile_commands.json
              rebuild-rpi5             Clean build from scratch
              run-rpi5 [bin]           Run locally (via qemu-aarch64 on non-ARM64)
              deploy-rpi5 <user@host>  Push binary via rsync/ssh (or $RPI5_HOST)
              nix build                Build static aarch64 package
            ========================================================================
            EOF
          '';
        };
      }
    );
}
