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

        # Example: run repo-wide LLM context export (or delegate to ./test-c)
        exportLlmContext = pkgs.writeShellApplication {
          name = "export-llm-context";
          runtimeInputs = with pkgs; [
            git
            coreutils
            findutils
          ];
          text = ''
            OUT="''${1:-llm_context.md}"
            : > "$OUT"

            while IFS= read -r -d "" file; do
              case "$file" in
                *.c|*.cpp|*.h|*.hpp|*.ino|*.nix|Makefile|README.md)
                  {
                    printf '## %s\n\n```%s\n' "$file" "''${file##*.}"
                    cat "$file"
                    printf '\n```\n\n'
                  } >> "$OUT"
                  ;;
              esac
            done < <(git ls-files -z)

            echo "Wrote $OUT"
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
