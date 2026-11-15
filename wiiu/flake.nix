{
  description = "Nintendo Wii U C/C++ Homebrew (devkitPPC / wut)";

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

        buildHelper = pkgs.writeShellScriptBin "build-wiiu" ''
          set -e
          NPROC=$(nproc 2>/dev/null || echo 2)
          bear -- make -j"$NPROC" "$@"
        '';

        cleanBuildHelper = pkgs.writeShellScriptBin "rebuild-wiiu" ''
          set -e
          make clean
          build-wiiu "$@"
        '';

        runHelper = pkgs.writeShellApplication {
          name = "run-wiiu";
          runtimeInputs = with pkgs; [
            findutils
            coreutils
          ];
          text = ''
            TARGET_FILE="''${1:-}"
            if [ -z "$TARGET_FILE" ]; then
              TARGET_FILE=$(find . -maxdepth 2 -name "*.rpx" | head -n 1 || true)
            fi

            if [ -z "$TARGET_FILE" ] || [ ! -f "$TARGET_FILE" ]; then
              echo "No .rpx file found. Running build-wiiu..." >&2
              build-wiiu
              TARGET_FILE=$(find . -maxdepth 2 -name "*.rpx" | head -n 1)
            fi

            if [ -n "''${CEMU_BIN:-}" ]; then
              "$CEMU_BIN" -g "$TARGET_FILE" >/dev/null 2>&1 &
            elif command -v cemu >/dev/null 2>&1; then
              cemu -g "$TARGET_FILE" >/dev/null 2>&1 &
            elif command -v Cemu >/dev/null 2>&1; then
              Cemu -g "$TARGET_FILE" >/dev/null 2>&1 &
            elif command -v Cemu.exe >/dev/null 2>&1 && command -v wslpath >/dev/null 2>&1; then
              WIN_PATH=$(wslpath -w "$TARGET_FILE")
              Cemu.exe -g "$WIN_PATH" >/dev/null 2>&1 &
            else
              echo "Built $TARGET_FILE, but Cemu was not found in PATH." >&2
              echo "Install Cemu on your host (or set CEMU_BIN=/path/to/Cemu)." >&2
              exit 1
            fi
          '';
        };

        deployHelper = pkgs.writeShellScriptBin "deploy-wiiu" ''
                    set -e
                    export PATH="${devkitPro}/tools/bin:$PATH"

                    TARGET_FILE=""
                    TARGET_HOST=""

                    # Handle console aliases or direct IP
                    case "''${1:-}" in
                      1|wiiu1|first)
                        TARGET_HOST="''${WIIU_IP_1:-}"
                        if [ -z "$TARGET_HOST" ]; then
                          echo "Error: WIIU_IP_1 is not set in environment." >&2
                          exit 1
                        fi
                        shift
                        ;;
                      2|wiiu2|second)
                        TARGET_HOST="''${WIIU_IP_2:-}"
                        if [ -z "$TARGET_HOST" ]; then
                          echo "Error: WIIU_IP_2 is not set in environment." >&2
                          exit 1
                        fi
                        shift
                        ;;
                      *)
                        TARGET_HOST="''${1:-}"
                        shift || true
                        ;;
                    esac

                    if [ -n "''${1:-}" ]; then
                      TARGET_FILE="$1"
                    else
                      # Prefer .wuhb for Aroma, fallback to .rpx
                      TARGET_FILE=$(find . -maxdepth 2 -name "*.wuhb" | head -n 1 || true)
                      if [ -z "$TARGET_FILE" ]; then
                        TARGET_FILE=$(find . -maxdepth 2 -name "*.rpx" | head -n 1 || true)
                      fi
                    fi

                    if [ -z "$TARGET_FILE" ] || [ ! -f "$TARGET_FILE" ]; then
                      echo "No .wuhb or .rpx found. Building..." >&2
                      build-wiiu
                      TARGET_FILE=$(find . -maxdepth 2 -name "*.wuhb" | head -n 1 || true)
                      if [ -z "$TARGET_FILE" ]; then
                        TARGET_FILE=$(find . -maxdepth 2 -name "*.rpx" | head -n 1 || true)
                      fi
                    fi

                    if [ -z "$TARGET_HOST" ] && [ -n "''${WIILOAD:-}" ]; then
                      TARGET_HOST="$WIILOAD"
                    fi

                    if [ -z "$TARGET_HOST" ]; then
                      cat << 'USAGE' >&2
          Usage:
            deploy-wiiu <ip> [file.wuhb|file.rpx]
            deploy-wiiu wiiu1 [file]   (requires WIIU_IP_1 set)
            deploy-wiiu wiiu2 [file]   (requires WIIU_IP_2 set)
          USAGE
                      exit 1
                    fi

                    case "$TARGET_HOST" in
                      tcp:*) export WIILOAD="$TARGET_HOST" ;;
                      *)     export WIILOAD="tcp:$TARGET_HOST" ;;
                    esac

                    echo "Deploying $TARGET_FILE to $WIILOAD..."
                    wiiload "$TARGET_FILE"
        '';
      in
      {
        packages.default = pkgs.stdenv.mkDerivation {
          pname = "durhack-wiiu";
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
            cp -f *.wuhb *.rpx *.elf $out/ 2>/dev/null || true
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

            # Set your console IPs here for quick alias targeting:
            export WIIU_IP_1="10.215.147.127"
            export WIIU_IP_2="10.215.147.4"

            if [ ! -f .clangd ]; then
              cat > .clangd << 'EOF'
            CompileFlags:
              Add:
                - "--target=powerpc-eabi"
                - "-D__WIIU__"
                - "-D__WUT__"
              Remove:
                - "-DESPRESSO"
                - "-meabi"
                - "-mhard-float"
            EOF
            fi

            cat << 'EOF'
            ========================================================================
            Environment: Nintendo Wii U (Espresso PowerPC / devkitPPC + wut)
            ------------------------------------------------------------------------
            Commands:
              build-wiiu                  Build .wuhb/.rpx + compile_commands.json
              rebuild-wiiu                Clean build from scratch
              run-wiiu [file]             Launch .rpx in Cemu
              deploy-wiiu <ip|wiiu1|wiiu2> [file]  Push & run on console via Wiiload
            ========================================================================
            EOF
          '';
        };
      }
    );
}
