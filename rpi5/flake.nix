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

            buildInputs = [ ];

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
          echo "🐻 Compiling for Raspberry Pi 5 (aarch64) with bear (-j$NPROC)..."
          bear -- make -j"$NPROC" "$@"
          echo "✅ Done! Binary ready in bin/."
        '';

        cleanBuildHelper = pkgs.writeShellScriptBin "rebuild-rpi5" ''
          echo "🧹 Cleaning previous build..."
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
              echo "❌ No binary found! Running build first..."
              build-rpi5
              TARGET_FILE=$(find ./bin -maxdepth 1 -type f -executable | head -n 1)
            fi

            shift || true
            if [ "$(uname -m)" = "aarch64" ]; then
              echo "🍓 Running $TARGET_FILE natively..."
              "$TARGET_FILE" "$@"
            else
              echo "🕹️ Running $TARGET_FILE under qemu-aarch64..."
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
            TARGET_HOST="''${1:-''${RPI5_HOST:-pi@raspberrypi.local}}"
            REMOTE_DIR="''${2:-~/homebrew}"
            TARGET_FILE=$(find ./bin -maxdepth 1 -type f -executable 2>/dev/null | head -n 1 || true)

            if [ -z "$TARGET_FILE" ] || [ ! -f "$TARGET_FILE" ]; then
              if [ -d "./result/bin" ]; then
                TARGET_FILE=$(find ./result/bin -maxdepth 1 -type f -executable | head -n 1 || true)
              fi
            fi

            if [ -z "$TARGET_FILE" ] || [ ! -f "$TARGET_FILE" ]; then
              echo "❌ No binary found! Run 'build-rpi5' or 'nix build' first."
              exit 1
            fi

            echo "🚀 Deploying $TARGET_FILE to $TARGET_HOST:$REMOTE_DIR/..."
            ssh "$TARGET_HOST" "mkdir -p '$REMOTE_DIR'"
            rsync -avzL --chmod=u+rwx "$TARGET_FILE" "$TARGET_HOST:$REMOTE_DIR/"
            echo "✅ Deployed! Run remotely with: ssh $TARGET_HOST '$REMOTE_DIR/$(basename "$TARGET_FILE")'"
          '';
        };

        export-llm-context = pkgs.writeShellApplication {
          name = "export-llm-context";
          runtimeInputs =
            with pkgs;
            [
              findutils
              coreutils
              tree
            ]
            ++ pkgs.lib.optionals pkgs.stdenv.hostPlatform.isLinux [
              wl-clipboard
              xclip
            ];
          text = ''
            SEARCH_PATH="''${1:-.}"
            OUTFILE="llm_context.md"
            TMPFILE=$(mktemp)

            echo "Generating context markdown for path: $SEARCH_PATH"

            if [ ! -d "$SEARCH_PATH" ]; then
              echo "Error: Directory '$SEARCH_PATH' does not exist." >&2
              exit 1
            fi

            {
              echo "# Project Context"
              echo
              echo "## File Tree"
              echo "\`\`\`"
              tree "$SEARCH_PATH" -a \
                -I '.git|.direnv|bin|build|result|llm_context.md' \
                --dirsfirst
              echo "\`\`\`"
              echo
              echo "## Files"
              echo
            } > "$TMPFILE"

            find "$SEARCH_PATH" -type f \( \
              -name "*.c" -o \
              -name "*.h" -o \
              -name "*.cpp" -o \
              -name "*.hpp" -o \
              -name "*.nix" -o \
              -name "Makefile" -o \
              -name "*.json" -o \
              -name "*.toml" -o \
              -name "*.md" \
            \) \
              -not -path '*/.*' \
              -not -path '*/build/*' \
              -not -path '*/bin/*' \
              -not -name "$OUTFILE" \
              -print0 | sort -z | while IFS= read -r -d "" file; do

              CLEAN_PATH="''${file#./}"
              echo "Found: $CLEAN_PATH" >&2

              BASE=$(basename "$CLEAN_PATH")
              EXT="''${CLEAN_PATH##*.}"
              LANG_TAG="text"
              case "$BASE" in
                Makefile|Makefile.*) LANG_TAG="makefile" ;;
              esac
              case "$EXT" in
                c|h)            LANG_TAG="c" ;;
                cpp|hpp)        LANG_TAG="cpp" ;;
                nix)            LANG_TAG="nix" ;;
                json|jsonc)     LANG_TAG="json" ;;
                toml)           LANG_TAG="toml" ;;
                md)             LANG_TAG="markdown" ;;
              esac

              {
                printf "### \`%s\`\n\`\`\`%s\n" "$CLEAN_PATH" "$LANG_TAG"
                cat "$file"
                printf "\n\`\`\`\n\n"
              } >> "$TMPFILE"
            done

            mv "$TMPFILE" "$OUTFILE"
            echo "Successfully exported context to $OUTFILE"

            if command -v wl-copy >/dev/null 2>&1; then
              wl-copy < "$OUTFILE"
              echo "Copied $OUTFILE to clipboard (via wl-copy)."
            elif command -v pbcopy >/dev/null 2>&1; then
              pbcopy < "$OUTFILE"
              echo "Copied $OUTFILE to clipboard (via pbcopy)."
            elif command -v xclip >/dev/null 2>&1; then
              xclip -selection clipboard < "$OUTFILE"
              echo "Copied $OUTFILE to clipboard (via xclip)."
            fi
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
            export-llm-context
          ]
          ++ pkgs.lib.optionals (!pkgs.stdenv.hostPlatform.isAarch64) [
            pkgs.qemu-user
          ];

          shellHook = ''
            export CC="${crossPkgs.stdenv.cc.targetPrefix}cc"
            export CXX="${crossPkgs.stdenv.cc.targetPrefix}c++"
            export AR="${crossPkgs.stdenv.cc.targetPrefix}ar"
            export STRIP="${crossPkgs.stdenv.cc.targetPrefix}strip"

            mkdir -p .zed
            cat << 'EOF' > .zed/tasks.json
            [
              { "label": "RPi 5: Build", "command": "build-rpi5", "use_new_terminal": false, "allow_concurrent_runs": false },
              { "label": "RPi 5: Run (QEMU)", "command": "run-rpi5", "use_new_terminal": true, "allow_concurrent_runs": true },
              { "label": "RPi 5: Deploy (SSH)", "command": "deploy-rpi5", "use_new_terminal": true, "allow_concurrent_runs": false }
            ]
            EOF

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
              build-rpi5         Build project and update compile_commands.json
              rebuild-rpi5       Clean build from scratch
              run-rpi5 [bin]     Run binary (via qemu-aarch64 on x86_64)
              deploy-rpi5 [host] Push binary via rsync/ssh (default: $RPI5_HOST)
              nix build          Build reproducible static aarch64 package
              export-llm-context Export workspace files to llm_context.md
            ========================================================================
            EOF
          '';
        };
      }
    );
}
