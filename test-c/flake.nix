{
  description = "C/C++ Project Starter Template";

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

        treefmtEval = treefmt-nix.lib.evalModule pkgs {
          projectRootFile = "flake.nix";
          programs = {
            nixfmt.enable = true;
            clang-format.enable = true;
          };
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
        packages.default = pkgs.stdenv.mkDerivation {
          pname = "app";
          version = "0.1.0";
          src = ./.;

          nativeBuildInputs = with pkgs; [
            gnumake
            pkg-config
          ];

          buildInputs = with pkgs; [ ];

          installPhase = ''
            runHook preInstall
            mkdir -p $out/bin
            cp -f bin/* $out/bin/ 2>/dev/null || cp -f app $out/bin/
            runHook postInstall
          '';

          meta.mainProgram = "app";
        };

        apps.default = {
          type = "app";
          program = "${self.packages.${system}.default}/bin/app";
        };

        formatter = treefmtEval.config.build.wrapper;

        checks.formatting = treefmtEval.config.build.check self;

        devShells.default = pkgs.mkShell {
          inputsFrom = [ self.packages.${system}.default ];
          packages = with pkgs; [
            clang-tools
            bear
            gdb
            valgrind
            export-llm-context
          ];

          shellHook = ''
              cat << 'EOF'
            ========================================================================
            Environment: C/C++ Native
            ------------------------------------------------------------------------
            Properties:
              Compiler    : $(cc --version | head -n1)
              LSP         : clangd

            Commands:
              make               Build project
              bear -- make       Generate compile_commands.json
              gdb                GNU Debugger
              valgrind           Memory debugging and profiling
              export-llm-context Export workspace files to llm_context.md
            ========================================================================
            EOF
          '';
        };
      }
    );
}
