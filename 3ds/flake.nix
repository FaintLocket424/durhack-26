{
  description = "Nintendo 3DS C/C++ Homebrew (devkitARM / libctru)";

  inputs = {
    nixpkgs.url = "github:NixOS/nixpkgs/nixos-unstable";
    flake-utils.url = "github:numtide/flake-utils";
    treefmt-nix = {
      url = "github:numtide/treefmt-nix";
      inputs.nixpkgs.follows = "nixpkgs";
    };
    devkitNix = {
      url = "github:bandithedoge/devkitNix";
      inputs.nixpkgs.follows = "nixpkgs";
    };
  };

  outputs =
    {
      self,
      nixpkgs,
      flake-utils,
      treefmt-nix,
      devkitNix,
    }:
    flake-utils.lib.eachSystem [ "x86_64-linux" ] (
      system:
      let
        pkgs = import nixpkgs {
          inherit system;
          overlays = [ devkitNix.overlays.default ];
        };

        devkitPro = "${pkgs.devkitNix.devkitARM}/opt/devkitpro";
        devkitArm = "${devkitPro}/devkitARM";

        treefmtEval = treefmt-nix.lib.evalModule pkgs {
          projectRootFile = "flake.nix";
          settings.global.excludes = [ "build/**" ];
          programs = {
            nixfmt.enable = true;
            clang-format.enable = true;
          };
        };

        downloadExamples = pkgs.writeShellScriptBin "download-examples" ''
          git clone https://github.com/devkitPro/3ds-examples.git devkitpro-examples
        '';

        clangdWrapper = pkgs.lib.hiPrio (
          pkgs.writeShellScriptBin "clangd" ''
            has_query_driver=0
            for arg in "$@"; do
              case "$arg" in
                --query-driver|--query-driver=*)
                  has_query_driver=1
                  break
                  ;;
              esac
            done
            if [ "$has_query_driver" -eq 1 ]; then
              exec ${pkgs.clang-tools}/bin/clangd "$@"
            else
              exec ${pkgs.clang-tools}/bin/clangd --query-driver="/nix/store/*" "$@"
            fi
          ''
        );

        buildHelper = pkgs.writeShellScriptBin "build-3ds" ''
          set -e
          NPROC=$(nproc 2>/dev/null || echo 2)
          bear -- make -j"$NPROC" "$@"
        '';

        cleanBuildHelper = pkgs.writeShellScriptBin "rebuild-3ds" ''
          set -e
          make clean
          build-3ds "$@"
        '';

        runHelper = pkgs.writeShellApplication {
          name = "run-3ds";
          runtimeInputs = with pkgs; [
            findutils
            coreutils
          ];
          text = ''
            TARGET_FILE="''${1:-}"
            if [ -z "$TARGET_FILE" ]; then
              TARGET_FILE=$(find . -maxdepth 2 -name "*.3dsx" | head -n 1 || true)
            fi

            if [ -z "$TARGET_FILE" ] || [ ! -f "$TARGET_FILE" ]; then
              echo "No .3dsx file found. Running build-3ds..." >&2
              build-3ds
              TARGET_FILE=$(find . -maxdepth 2 -name "*.3dsx" | head -n 1)
            fi

            if [ -n "''${AZAHAR_BIN:-}" ]; then
              "$AZAHAR_BIN" "$TARGET_FILE" >/dev/null 2>&1 &
            elif command -v azahar >/dev/null 2>&1; then
              azahar "$TARGET_FILE" >/dev/null 2>&1 &
            elif command -v azahar.exe >/dev/null 2>&1 && command -v wslpath >/dev/null 2>&1; then
              WIN_PATH=$(wslpath -w "$TARGET_FILE")
              azahar.exe "$WIN_PATH" >/dev/null 2>&1 &
            else
              echo "Built $TARGET_FILE, but Azahar was not found in PATH." >&2
              echo "Install Azahar on your host (or set AZAHAR_BIN=/path/to/azahar)." >&2
              exit 1
            fi
          '';
        };

        deployHelper = pkgs.writeShellApplication {
          name = "deploy-3ds";
          runtimeInputs = with pkgs; [
            findutils
            coreutils
          ];
          text = ''
            TARGET_FILE=""
            IP_ADDR=""

            if [[ "''${1:-}" == *.3dsx ]]; then
              TARGET_FILE="$1"
              IP_ADDR="''${2:-}"
            else
              IP_ADDR="''${1:-}"
              TARGET_FILE=$(find . -maxdepth 2 -name "*.3dsx" | head -n 1 || true)
            fi

            if [ -z "$TARGET_FILE" ] || [ ! -f "$TARGET_FILE" ]; then
              echo "No .3dsx file found. Run 'build-3ds' first." >&2
              exit 1
            fi

            if [ -n "$IP_ADDR" ]; then
              3dslink "$TARGET_FILE" -a "$IP_ADDR"
            else
              3dslink "$TARGET_FILE"
            fi
          '';
        };
      in
      {
        packages.default = pkgs.stdenv.mkDerivation {
          pname = "durhack-3ds";
          version = "0.1.0";
          src = ./.;

          nativeBuildInputs = [
            pkgs.devkitNix.devkitARM
            pkgs.gnumake
          ];

          DEVKITPRO = devkitPro;
          DEVKITARM = devkitArm;

          preBuild = ''
            export PATH="$DEVKITPRO/tools/bin:$DEVKITARM/bin:$PATH"
          '';

          installPhase = ''
            runHook preInstall
            mkdir -p $out
            cp -f *.3dsx *.smdh *.elf $out/ 2>/dev/null || true
            runHook postInstall
          '';
        };

        formatter = treefmtEval.config.build.wrapper;
        checks.formatting = treefmtEval.config.build.check self;

        devShells.default = pkgs.mkShell {
          packages = [
            pkgs.devkitNix.devkitARM
            pkgs.gnumake
            pkgs.bear
            clangdWrapper
            pkgs.clang-tools
            buildHelper
            downloadExamples
            cleanBuildHelper
            runHelper
            deployHelper
          ];

          shellHook = ''
            export DEVKITPRO="${devkitPro}"
            export DEVKITARM="${devkitArm}"
            export PATH="$DEVKITPRO/tools/bin:$DEVKITARM/bin:$PATH"

            if [ ! -f .clangd ]; then
              cat > .clangd << 'EOF'
            CompileFlags:
              Add:
                - "--target=arm-none-eabi"
                - "-D__3DS__"
              Remove:
                - "-mword-relocations"
                - "-fomit-frame-pointer"
                - "-mtp=soft"
            EOF
            fi

            cat << 'EOF'
            ========================================================================
            Environment: Nintendo 3DS (ARM11 / devkitARM + libctru)
            ------------------------------------------------------------------------
            Properties:
              Toolchain   : devkitARM (arm-none-eabi)
              SDK         : libctru / portlibs
              LSP         : clangd (with --query-driver)

            Commands:
              build-3ds          Build .3dsx + compile_commands.json
              rebuild-3ds        Clean build from scratch
              run-3ds [file]     Launch .3dsx in Azahar (Linux or WSL .exe)
              deploy-3ds [ip]    Send .3dsx to 3DS via 3dslink
              nix build          Build .3dsx package reproducibly
            ========================================================================
            EOF
          '';
        };
      }
    );
}
