{
  description = "Arduino UNO Q (QRB2210 MPU + STM32U585 MCU) C/C++ Template";

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

        fqbn = "arduino:zephyr:unoq";

        treefmtEval = treefmt-nix.lib.evalModule pkgs {
          projectRootFile = "flake.nix";
          settings.global.excludes = [
            "sketch/build/**"
          ];
          programs = {
            nixfmt.enable = true;
            clang-format.enable = true;
          };
        };

        # Wrap arduino-cli in an FHS env on Linux so downloaded Zephyr/ARM
        # toolchains and remoteocd binaries in ~/.arduino15 work on NixOS.
        arduinoCli =
          if pkgs.stdenv.hostPlatform.isLinux then
            pkgs.buildFHSEnv {
              name = "arduino-cli";
              targetPkgs =
                p: with p; [
                  arduino-cli
                  android-tools
                  udev
                  libusb1
                  zlib
                  stdenv.cc.cc.lib
                  ncurses5
                  python3
                  gnumake
                  cmake
                  ninja
                ];
              runScript = "arduino-cli";
            }
          else
            pkgs.arduino-cli;

        mkMpuPackage =
          targetPkgs:
          targetPkgs.stdenv.mkDerivation {
            pname = "unoq-mpu";
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

        # --- MPU (Qualcomm QRB2210 Linux) helpers ---
        buildMpuHelper = pkgs.writeShellScriptBin "build-mpu" ''
          set -e
          NPROC=$(nproc 2>/dev/null || sysctl -n hw.ncpu 2>/dev/null || echo 2)
          echo "🐻 Compiling MPU binary (aarch64) with bear (-j$NPROC)..."
          bear -- make -j"$NPROC" "$@"
          echo "✅ Done! MPU binary ready in bin/."
        '';

        cleanBuildMpuHelper = pkgs.writeShellScriptBin "rebuild-mpu" ''
          echo "🧹 Cleaning previous MPU build..."
          make clean
          build-mpu "$@"
        '';

        runMpuHelper = pkgs.writeShellApplication {
          name = "run-mpu";
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
              echo "❌ No MPU binary found! Running build first..."
              build-mpu
              TARGET_FILE=$(find ./bin -maxdepth 1 -type f -executable | head -n 1)
            fi

            shift || true
            if [ "$(uname -m)" = "aarch64" ]; then
              echo "🚀 Running $TARGET_FILE natively..."
              "$TARGET_FILE" "$@"
            else
              echo "🕹️ Running $TARGET_FILE under qemu-aarch64..."
              qemu-aarch64 "$TARGET_FILE" "$@"
            fi
          '';
        };

        deployMpuHelper = pkgs.writeShellApplication {
          name = "deploy-mpu";
          runtimeInputs = with pkgs; [
            findutils
            android-tools
            openssh
            rsync
          ];
          text = ''
            DEST="''${1:-adb}"
            REMOTE_DIR="''${2:-/home/arduino/homebrew}"
            TARGET_FILE=$(find ./bin -maxdepth 1 -type f -executable 2>/dev/null | head -n 1 || true)

            if [ -z "$TARGET_FILE" ] || [ ! -f "$TARGET_FILE" ]; then
              if [ -d "./result/bin" ]; then
                TARGET_FILE=$(find ./result/bin -maxdepth 1 -type f -executable | head -n 1 || true)
              fi
            fi

            if [ -z "$TARGET_FILE" ] || [ ! -f "$TARGET_FILE" ]; then
              echo "❌ No MPU binary found! Run 'build-mpu' or 'nix build' first."
              exit 1
            fi

            BIN_NAME=$(basename "$TARGET_FILE")
            if [ "$DEST" = "adb" ]; then
              echo "🚀 Pushing $TARGET_FILE to UNO Q via ADB ($REMOTE_DIR/)..."
              adb shell "mkdir -p '$REMOTE_DIR'"
              adb push "$TARGET_FILE" "$REMOTE_DIR/$BIN_NAME"
              adb shell "chmod +x '$REMOTE_DIR/$BIN_NAME'"
              echo "✅ Deployed! Run with: adb shell '$REMOTE_DIR/$BIN_NAME'"
            else
              echo "🚀 Deploying $TARGET_FILE to $DEST:$REMOTE_DIR/ via SSH..."
              ssh "$DEST" "mkdir -p '$REMOTE_DIR'"
              rsync -avzL --chmod=u+rwx "$TARGET_FILE" "$DEST:$REMOTE_DIR/"
              echo "✅ Deployed! Run with: ssh $DEST '$REMOTE_DIR/$BIN_NAME'"
            fi
          '';
        };

        # --- MCU (STM32U585 Zephyr / Arduino) helpers ---
        setupUnoQHelper = pkgs.writeShellScriptBin "setup-unoq" ''
          set -e
          echo "📦 Installing Arduino UNO Q Zephyr core (${fqbn})..."
          arduino-cli core update-index
          arduino-cli core install arduino:zephyr
          echo "✅ UNO Q core installed!"
        '';

        buildUnoQHelper = pkgs.writeShellScriptBin "build-unoq" ''
          set -e
          SKETCH_DIR="''${1:-sketch}"
          echo "🔨 Compiling MCU sketch in $SKETCH_DIR (${fqbn})..."
          arduino-cli compile --fqbn ${fqbn} --export-binaries "$SKETCH_DIR"
          echo "✅ MCU firmware built!"
        '';

        deployUnoQHelper = pkgs.writeShellScriptBin "deploy-unoq" ''
          set -e
          SKETCH_DIR="sketch"
          PORT_ARG=""

          if [ -n "''${1:-}" ] && [ -d "$1" ]; then
            SKETCH_DIR="$1"
            shift
          fi
          if [ -n "''${1:-}" ]; then
            PORT_ARG="-p $1"
          fi

          echo "🚀 Compiling & uploading $SKETCH_DIR to Arduino UNO Q (${fqbn})..."
          # shellcheck disable=SC2086
          arduino-cli compile --fqbn ${fqbn} $PORT_ARG --upload "$SKETCH_DIR"
          echo "✅ MCU sketch flashed!"
        '';

        unoQShellHelper = pkgs.writeShellApplication {
          name = "unoq-shell";
          runtimeInputs = with pkgs; [ android-tools ];
          text = ''
            echo "🔌 Opening ADB shell on Arduino UNO Q..."
            adb shell "$@"
          '';
        };

        unoQSerialHelper = pkgs.writeShellApplication {
          name = "unoq-serial";
          runtimeInputs = with pkgs; [ tio ];
          text = ''
            DEV="''${1:-}"
            if [ -z "$DEV" ]; then
              for p in /dev/serial/by-id/*Arduino* /dev/ttyACM*; do
                if [ -e "$p" ]; then
                  DEV="$p"
                  break
                fi
              done
            fi

            if [ -z "$DEV" ]; then
              echo "❌ No serial port found (/dev/ttyACM*). Pass device explicitly: unoq-serial /dev/ttyACM0" >&2
              exit 1
            fi

            echo "📡 Connecting to $DEV (Press Ctrl+T then Q to disconnect)..."
            tio -b 115200 "$DEV"
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
              -name "*.ino" -o \
              -name "*.yaml" -o \
              -name "*.yml" -o \
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
                cpp|hpp|ino)    LANG_TAG="cpp" ;;
                yaml|yml)       LANG_TAG="yaml" ;;
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
          default = mkMpuPackage crossPkgs;
          glibc = mkMpuPackage crossPkgsGlibc;
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
            pkgs.android-tools
            pkgs.tio
            pkgs.rsync
            pkgs.openssh
            arduinoCli
            buildMpuHelper
            cleanBuildMpuHelper
            runMpuHelper
            deployMpuHelper
            setupUnoQHelper
            buildUnoQHelper
            deployUnoQHelper
            unoQShellHelper
            unoQSerialHelper
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
              { "label": "UNO Q: Build MPU", "command": "build-mpu", "use_new_terminal": false, "allow_concurrent_runs": false },
              { "label": "UNO Q: Run MPU (QEMU)", "command": "run-mpu", "use_new_terminal": true, "allow_concurrent_runs": true },
              { "label": "UNO Q: Deploy MPU (ADB)", "command": "deploy-mpu", "use_new_terminal": true, "allow_concurrent_runs": false },
              { "label": "UNO Q: Build MCU Sketch", "command": "build-unoq", "use_new_terminal": false, "allow_concurrent_runs": false },
              { "label": "UNO Q: Flash MCU Sketch", "command": "deploy-unoq", "use_new_terminal": true, "allow_concurrent_runs": false },
              { "label": "UNO Q: ADB Shell", "command": "unoq-shell", "use_new_terminal": true, "allow_concurrent_runs": true }
            ]
            EOF

            if [ ! -f .clangd ]; then
              cat > .clangd << 'EOF'
            CompileFlags:
              Add:
                - "--target=aarch64-unknown-linux-musl"
                - "-mcpu=cortex-a53"
            EOF
            fi

            cat << 'EOF'
            ========================================================================
            Environment: Arduino UNO Q (QRB2210 MPU + STM32U585 MCU)
            ------------------------------------------------------------------------
            Properties:
              MPU         : aarch64-unknown-linux-musl (Cortex-A53, static)
              MCU         : STM32U585 / Zephyr (arduino:zephyr:unoq)
              LSP         : clangd

            Commands:
              setup-unoq         Install arduino:zephyr core (first-time setup)
              build-unoq         Compile MCU sketch (sketch/)
              deploy-unoq [port] Compile & flash MCU sketch
              build-mpu          Build Linux MPU C/C++ binary + compile_commands.json
              run-mpu [bin]      Test MPU binary locally (qemu-aarch64)
              deploy-mpu [dest]  Push MPU binary via adb (default) or user@host
              unoq-shell         Open ADB shell on UNO Q Linux MPU
              unoq-serial [dev]  Open serial monitor (tio)
              nix build          Build reproducible static MPU package
              export-llm-context Export workspace files to llm_context.md
            ========================================================================
            EOF
          '';
        };
      }
    );
}
