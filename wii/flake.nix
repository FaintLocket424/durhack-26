{
  description = "Nintendo Wii C/C++ Homebrew (devkitPPC / libogc)";

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

        devkitPro = "${pkgs.devkitNix.devkitPPC}/opt/devkitpro";
        devkitPpc = "${devkitPro}/devkitPPC";

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

        buildHelper = pkgs.writeShellScriptBin "build-wii" ''
          set -e
          NPROC=$(nproc 2>/dev/null || echo 2)
          bear -- make -j"$NPROC" "$@"
        '';

        cleanBuildHelper = pkgs.writeShellScriptBin "rebuild-wii" ''
          set -e
          make clean
          build-wii "$@"
        '';

        runHelper = pkgs.writeShellApplication {
          name = "run-wii";
          runtimeInputs = with pkgs; [
            findutils
            coreutils
          ];
          text = ''
            TARGET_FILE="''${1:-}"
            if [ -z "$TARGET_FILE" ]; then
              TARGET_FILE=$(find . -maxdepth 2 -name "*.dol" | head -n 1 || true)
            fi

            if [ -z "$TARGET_FILE" ] || [ ! -f "$TARGET_FILE" ]; then
              echo "No .dol file found. Running build-wii..." >&2
              build-wii
              TARGET_FILE=$(find . -maxdepth 2 -name "*.dol" | head -n 1)
            fi

            if [ -n "''${DOLPHIN_BIN:-}" ]; then
              "$DOLPHIN_BIN" -b -e "$TARGET_FILE" >/dev/null 2>&1 &
            elif command -v dolphin-emu >/dev/null 2>&1; then
              dolphin-emu -b -e "$TARGET_FILE" >/dev/null 2>&1 &
            elif command -v Dolphin.exe >/dev/null 2>&1 && command -v wslpath >/dev/null 2>&1; then
              WIN_PATH=$(wslpath -w "$TARGET_FILE")
              Dolphin.exe -b -e "$WIN_PATH" >/dev/null 2>&1 &
            else
              echo "Built $TARGET_FILE, but Dolphin was not found in PATH." >&2
              echo "Install Dolphin on your host (or set DOLPHIN_BIN=/path/to/dolphin-emu)." >&2
              exit 1
            fi
          '';
        };

        deployHelper = pkgs.writeShellApplication {
          name = "deploy-wii";
          runtimeInputs = with pkgs; [
            findutils
            coreutils
          ];
          text = ''
            TARGET_FILE=""
            IP_ARG=""

            if [[ "''${1:-}" == *.dol ]] || [[ "''${1:-}" == *.elf ]]; then
              TARGET_FILE="$1"
              IP_ARG="''${2:-}"
            else
              IP_ARG="''${1:-}"
              TARGET_FILE=$(find . -maxdepth 2 -name "*.dol" | head -n 1 || true)
            fi

            if [ -z "$TARGET_FILE" ] || [ ! -f "$TARGET_FILE" ]; then
              echo "No .dol file found. Run 'build-wii' first." >&2
              exit 1
            fi

            if [ -n "$IP_ARG" ]; then
              case "$IP_ARG" in
                tcp:*) export WIILOAD="$IP_ARG" ;;
                *)     export WIILOAD="tcp:$IP_ARG" ;;
              esac
            fi

            if [ -z "''${WIILOAD:-}" ]; then
              echo "Usage: deploy-wii <wii-ip> [file.dol]  (or export WIILOAD=tcp:<ip>)" >&2
              exit 1
            fi

            wiiload "$TARGET_FILE"
          '';
        };
      in
      {
        packages.default = pkgs.stdenv.mkDerivation {
          pname = "durhack-wii";
          version = "0.1.0";
          src = ./.;

          nativeBuildInputs = [
            pkgs.devkitNix.devkitPPC
            pkgs.gnumake
          ];

          DEVKITPRO = devkitPro;
          DEVKITPPC = devkitPpc;

          preBuild = ''
            export PATH="$DEVKITPRO/tools/bin:$DEVKITPPC/bin:$PATH"
          '';

          installPhase = ''
            runHook preInstall
            mkdir -p $out
            cp -f *.dol *.elf $out/ 2>/dev/null || true
            runHook postInstall
          '';
        };

        formatter = treefmtEval.config.build.wrapper;
        checks.formatting = treefmtEval.config.build.check self;

        devShells.default = pkgs.mkShell {
          packages = [
            pkgs.devkitNix.devkitPPC
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
            export DEVKITPPC="${devkitPpc}"
            export PATH="$DEVKITPRO/tools/bin:$DEVKITPPC/bin:$PATH"

            if [ ! -f .clangd ]; then
              cat > .clangd << 'EOF'
            CompileFlags:
              Add:
                - "--target=powerpc-eabi"
                - "-DGEKKO"
                - "-DHW_RVL"
              Remove:
                - "-mrvl"
                - "-meabi"
                - "-mhard-float"
            EOF
            fi

            cat << 'EOF'
            ========================================================================
            Environment: Nintendo Wii / GameCube (PowerPC / devkitPPC + libogc)
            ------------------------------------------------------------------------
            Properties:
              Toolchain   : devkitPPC (powerpc-eabi)
              SDK         : libogc / wiiuse / portlibs
              LSP         : clangd (with --query-driver)

            Commands:
              build-wii          Build .dol + compile_commands.json
              rebuild-wii        Clean build from scratch
              run-wii [file]     Launch .dol in Dolphin (Linux or WSL .exe)
              deploy-wii <ip>    Send .dol to Wii via wiiload
              nix build          Build .dol package reproducibly
            ========================================================================
            EOF
          '';
        };
      }
    );
}
