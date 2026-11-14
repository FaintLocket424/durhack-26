# durhack-26/flake.nix
{
  description = "durhack-26 monorepo orchestrator";

  inputs = {
    nixpkgs.url = "github:NixOS/nixpkgs/nixos-unstable";
    flake-utils.url = "github:numtide/flake-utils";
  };

  outputs =
    {
      nixpkgs,
      flake-utils,
      ...
    }:
    flake-utils.lib.eachDefaultSystem (
      system:
      let
        pkgs = nixpkgs.legacyPackages.${system};

        subprojects = [
          "arduino-q"
          "rpi5"
          "test-c"
        ];

        subprojectList = pkgs.lib.concatStringsSep " " subprojects;

        buildAll = pkgs.writeShellApplication {
          name = "build-all";
          text = ''
            for dir in ${subprojectList}; do
              if [ -f "$dir/flake.nix" ]; then
                echo "==> Building $dir"
                nix build "./$dir" --no-link
              fi
            done
          '';
        };

        checkAll = pkgs.writeShellApplication {
          name = "check-all";
          text = ''
            for dir in ${subprojectList}; do
              if [ -f "$dir/flake.nix" ]; then
                echo "==> Checking $dir"
                nix flake check "./$dir"
              fi
            done
          '';
        };

        exportLlmContext = pkgs.writeShellApplication {
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

            if [ -n "''${WAYLAND_DISPLAY:-}" ] && command -v wl-copy >/dev/null 2>&1; then
              wl-copy < "$OUTFILE"
              echo "Copied $OUTFILE to clipboard (via wl-copy)."
            elif command -v pbcopy >/dev/null 2>&1; then
              pbcopy < "$OUTFILE"
              echo "Copied $OUTFILE to clipboard (via pbcopy)."
            elif [ -n "''${DISPLAY:-}" ] && command -v xclip >/dev/null 2>&1; then
              xclip -selection clipboard < "$OUTFILE"
              echo "Copied $OUTFILE to clipboard (via xclip)."
            fi
          '';
        };
      in
      {
        apps = {
          build-all = {
            type = "app";
            program = "${buildAll}/bin/build-all";
          };
          check-all = {
            type = "app";
            program = "${checkAll}/bin/check-all";
          };
          export-llm-context = {
            type = "app";
            program = "${exportLlmContext}/bin/export-llm-context";
          };
        };

        devShells.default = pkgs.mkShell {
          packages = [
            buildAll
            checkAll
            exportLlmContext
          ];
        };
      }
    );
}
