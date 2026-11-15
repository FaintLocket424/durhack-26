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

            if [ -n "''${EMU_BIN:-}" ]; then
              "$EMU_BIN" "$TARGET_FILE" >/dev/null 2>&1 &
            elif command -v azahar >/dev/null 2>&1; then
              azahar "$TARGET_FILE" >/dev/null 2>&1 &
            elif command -v Azahar.exe >/dev/null 2>&1 && command -v wslpath >/dev/null 2>&1; then
              WIN_PATH=$(wslpath -w "$TARGET_FILE")
              Azahar.exe "$WIN_PATH" >/dev/null 2>&1 &
            else
              echo "Built $TARGET_FILE, but Azahar/Citra was not found in PATH." >&2
              echo "Install Azahar (or Citra) on your host (or set EMU_BIN=/path/to/azahar)." >&2
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
            IP_ARG=""

            if [[ "''${1:-}" == *.3dsx ]] || [[ "''${1:-}" == *.elf ]]; then
              TARGET_FILE="$1"
              IP_ARG="''${2:-}"
            else
              IP_ARG="''${1:-}"
              TARGET_FILE=$(find . -maxdepth 2 -name "*.3dsx" | head -n 1 || true)
            fi

            if [ -z "$TARGET_FILE" ] || [ ! -f "$TARGET_FILE" ]; then
              echo "No .3dsx file found. Run 'build-3ds' first." >&2
              exit 1
            fi

            if [ -z "$IP_ARG" ]; then
              echo "Usage: deploy-3ds <3ds-ip> [file.3dsx]  (3DS must be in hbmenu, Y = netloader)" >&2
              exit 1
            fi

            3dslink -a "$IP_ARG" "$TARGET_FILE"
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

          dontFixup = true;

          preBuild = ''
            export PATH="$DEVKITPRO/tools/bin:$DEVKITARM/bin:$PATH"
          '';

          installPhase = ''
            runHook preInstall

            dol=$(find . -maxdepth 1 -name '*.3dsx' | head -n 1)
            if [ -z "$dol" ]; then
              echo "No .3dsx produced by make" >&2
              exit 1
            fi

            mkdir -p $out
            cp "$dol" $out/app.3dsx
            cp -f *.elf $out/ 2>/dev/null || true

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
            cleanBuildHelper
            runHelper
            deployHelper
          ];

          shellHook = ''
            export DEVKITPRO="${devkitPro}"
            export DEVKITARM="${devkitArm}"
            export PATH="$DEVKITPRO/tools/bin:$DEVKITARM/bin:$PATH"

            if [ ! -f .clangd ]; then
              cat > .clangd << EOF
            CompileFlags:
              Add:
                - "--target=arm-none-eabi"
                - "-D__3DS__"
                - "-march=armv6k"
                - "-mfloat-abi=hard"
                - "-I$DEVKITPRO/libctru/include"
                - "-I$DEVKITARM/arm-none-eabi/include"
            EOF
            fi

            cat << 'EOF'
            ========================================================================
            Environment: Nintendo 3DS (ARM11 / devkitARM + libctru)
            ------------------------------------------------------------------------
            Properties:
              Toolchain   : devkitARM (arm-none-eabi)
              SDK         : libctru / citro3d / portlibs
              LSP         : clangd (with --query-driver)

            Commands:
              build-3ds          Build .3dsx + compile_commands.json
              rebuild-3ds        Clean build from scratch
              run-3ds [file]     Launch .3dsx in Azahar (Linux or WSL .exe)
              deploy-3ds <ip>    Send .3dsx to 3DS via 3dslink
              nix build          Build .3dsx package reproducibly
            ========================================================================
            EOF
          '';
        };
      }
    );
}
