{
  description = "Xbox 360 C/C++ Homebrew (libxenon / Free60 xenon toolchain)";

  inputs = {
    nixpkgs.url = "github:NixOS/nixpkgs/nixos-unstable";
    flake-utils.url = "github:numtide/flake-utils";
    treefmt-nix = {
      url = "github:numtide/treefmt-nix";
      inputs.nixpkgs.follows = "nixpkgs";
    };
    # libxenon is not packaged anywhere, so we pin its source and build the
    # toolchain + library ourselves. `nix flake update libxenon` to bump it.
    libxenon = {
      url = "github:Free60Project/libxenon";
      flake = false;
    };
  };

  outputs =
    {
      self,
      nixpkgs,
      flake-utils,
      treefmt-nix,
      libxenon,
    }:
    flake-utils.lib.eachSystem [ "x86_64-linux" ] (
      system:
      let
        pkgs = nixpkgs.legacyPackages.${system};
        lib = pkgs.lib;

        # ------------------------------------------------------------------
        # Stage 1: xenon cross toolchain (binutils + gcc + newlib).
        # Mirrors libxenon/toolchain/build-xenon-toolchain, but hermetic.
        # Takes a while (~30-60 min) the first time, then it's cached.
        # ------------------------------------------------------------------
        binutilsSrc = pkgs.fetchurl {
          url = "mirror://gnu/binutils/binutils-2.32.tar.gz";
          sha256 = "0vm4242a7k66xg6rdrhb9fzmy7mafvwi5vdw0b9q9w8dsfrrf3cv";
        };
        gccSrc = pkgs.fetchurl {
          url = "mirror://gnu/gcc/gcc-16.2.0/gcc-16.2.0.tar.gz";
          sha256 = "16rvlnjp00wpbiv3q08a7i8d6gz7b2g4mz4prvvmx7jpjyh00787";
        };
        newlibSrc = pkgs.fetchurl {
          url = "https://sourceware.org/pub/newlib/newlib-3.1.0.tar.gz";
          sha256 = "0ahh3n079zjp7d9wynggwrnrs27440aac04340chf1p9476a2kzv";
        };

        xenonToolchain = pkgs.stdenv.mkDerivation {
          pname = "xenon-toolchain";
          version = "gcc16.2.0-binutils2.32-newlib3.1.0";

          dontUnpack = true;
          # The target-side code is built by the freshly built xenon-gcc, but the
          # host-side tools must not get Nix's hardening flags mixed in oddly.
          hardeningDisable = [ "all" ];
          enableParallelBuilding = true;

          nativeBuildInputs = with pkgs; [
            flex
            bison
            texinfo
            perl
            m4
            which
            file
          ];
          buildInputs = with pkgs; [
            gmp
            mpfr
            libmpc
            zlib
          ];

          configurePhase = "true";

          buildPhase = ''
            runHook preBuild
            set -euo pipefail
            patches=${libxenon}/toolchain
            jobs=$NIX_BUILD_CORES
            export PATH="$out/bin:$PATH"

            # binutils 2.32 / newlib 3.1 predate C23, which modern host GCC
            # defaults to. Build the host-side bits as gnu17.
            hostcflags="-O2 -std=gnu17 -Wno-error -Wno-implicit-function-declaration -Wno-incompatible-pointer-types -Wno-int-conversion"

            tar xf ${binutilsSrc}
            patch -p0 < $patches/binutils-2.32.diff
            tar xf ${gccSrc}
            patch -p0 < $patches/gcc-16.2.0.diff
            tar xf ${newlibSrc}
            patch -p0 < $patches/newlib-3.1.0.diff

            echo "==> binutils"
            mkdir build-binutils && pushd build-binutils
            CFLAGS="$hostcflags" ../binutils-2.32/configure \
              --target=xenon --prefix=$out \
              --enable-multilib --disable-nls --disable-werror
            make -j$jobs
            make install
            popd

            echo "==> gcc (stage 1)"
            mkdir build-gcc1 && pushd build-gcc1
            ../gcc-16.2.0/configure \
              --target=xenon --prefix=$out \
              --enable-languages=c --without-headers --disable-shared \
              --with-newlib --disable-libmudflap --disable-libssp --disable-nls \
              --disable-decimal-float --enable-altivec --with-cpu=cell
            make -j$jobs all-gcc
            make install-gcc
            popd

            echo "==> newlib"
            mkdir build-newlib && pushd build-newlib
            ../newlib-3.1.0/configure \
              --target=xenon --prefix=$out \
              --enable-multilib --disable-nls \
              --enable-newlib-io-long-long --enable-newlib-io-long-double \
              CFLAGS="$hostcflags" \
              CFLAGS_FOR_TARGET="-DHAVE_BLKSIZE -O2 -Wno-implicit-function-declaration -Wno-incompatible-pointer-types -Wno-int-conversion"
            make -j$jobs
            make install
            popd

            echo "==> gcc (stage 2, C/C++)"
            mkdir build-gcc2 && pushd build-gcc2
            unset LIBRARY_PATH
            ../gcc-16.2.0/configure \
              --target=xenon --prefix=$out --with-cpu=cell \
              --disable-decimal-float --disable-libquadmath \
              --enable-languages=c,c++ --disable-libssp --disable-libsanitizer \
              --with-newlib --enable-cxx-flags="-G0" \
              --disable-libmudflap --disable-nls --disable-shared \
              --disable-linux-futex --enable-altivec \
              --disable-libatomic --disable-threads --disable-libgomp \
              --disable-libitm --disable-libstdcxx-time --disable-initfini-array
            make -j$jobs
            make install
            popd

            runHook postBuild
          '';

          installPhase = "true";
          # Don't let patchelf/strip touch cross target objects.
          dontFixup = true;
        };

        # ------------------------------------------------------------------
        # Stage 2: devkitxenon = toolchain + libxenon + bin2s + rules/app.lds.
        # libxenon's crt files must live inside gcc's own prefix (xenon/lib),
        # so we copy the toolchain rather than symlink-join it.
        # ------------------------------------------------------------------
        devkitXenon = pkgs.stdenv.mkDerivation {
          pname = "devkitxenon";
          version = "unstable-${libxenon.lastModifiedDate or "0"}";
          src = libxenon;

          nativeBuildInputs = [ pkgs.gnumake ];
          hardeningDisable = [ "all" ];
          enableParallelBuilding = true;
          dontFixup = true;

          configurePhase = ''
            mkdir -p $out
            cp -a ${xenonToolchain}/. $out/
            chmod -R u+w $out
            export DEVKITXENON=$out
            export PATH="$out/bin:$PATH"
          '';

          buildPhase = ''
            make -j$NIX_BUILD_CORES -C libxenon/ports/xenon libxenon.a
          '';

          installPhase = ''
            make -C libxenon/ports/xenon install
            $CC libxenon/ports/xenon/bin2s.c -o $out/bin/bin2s
          '';
        };

        devkitPath = "${devkitXenon}/bin:${devkitXenon}/usr/bin";

        treefmtEval = treefmt-nix.lib.evalModule pkgs {
          projectRootFile = "flake.nix";
          settings.global.excludes = [ "build/**" ];
          programs = {
            nixfmt.enable = true;
            clang-format.enable = true;
          };
        };

        clangdWrapper = lib.hiPrio (
          pkgs.writeShellScriptBin "clangd" ''
            for arg in "$@"; do
              case "$arg" in
                --query-driver|--query-driver=*) exec ${pkgs.clang-tools}/bin/clangd "$@" ;;
              esac
            done
            exec ${pkgs.clang-tools}/bin/clangd --query-driver="/nix/store/*" "$@"
          ''
        );

        buildHelper = pkgs.writeShellScriptBin "build-xbox360" ''
          set -e
          NPROC=$(nproc 2>/dev/null || echo 2)
          bear -- make -j"$NPROC" "$@"
        '';

        cleanBuildHelper = pkgs.writeShellScriptBin "rebuild-xbox360" ''
          set -e
          make clean
          build-xbox360 "$@"
        '';

        # XeXMenu can't run an ELF directly. Workflow: copy xenon.elf to the
        # root of a FAT32 USB stick, then from XeXMenu launch a XeLL launcher
        # .xex (e.g. XeLL Reloaded's); XeLL boots xenon.elf from USB.
        deployHelper = pkgs.writeShellApplication {
          name = "deploy-xbox360";
          runtimeInputs = with pkgs; [
            coreutils
            findutils
          ];
          text = ''
            DEST="''${1:-}"
            if [ -z "$DEST" ] || [ ! -d "$DEST" ]; then
              echo "Usage: deploy-xbox360 <usb-mountpoint> [file.elf32]" >&2
              echo "  Copies the build to <usb>/xenon.elf for XeLL to boot." >&2
              exit 1
            fi
            TARGET_FILE="''${2:-$(find . -maxdepth 1 -name '*.elf32' | head -n 1)}"
            if [ -z "$TARGET_FILE" ] || [ ! -f "$TARGET_FILE" ]; then
              echo "No .elf32 found. Run 'build-xbox360' first." >&2
              exit 1
            fi
            cp "$TARGET_FILE" "$DEST/xenon.elf"
            sync
            echo "Copied $TARGET_FILE -> $DEST/xenon.elf"
            echo "Now: XeXMenu -> launch XeLL launcher .xex -> XeLL boots xenon.elf"
          '';
        };
      in
      {
        packages = {
          default = pkgs.stdenv.mkDerivation {
            pname = "durhack-xbox360";
            version = "0.1.0";
            src = ./.;

            nativeBuildInputs = [ pkgs.gnumake ];
            DEVKITXENON = devkitXenon;
            dontFixup = true;

            preBuild = ''
              export PATH="${devkitPath}:$PATH"
            '';

            installPhase = ''
              runHook preInstall
              elf=$(find . -maxdepth 1 -name '*.elf32' | head -n 1)
              if [ -z "$elf" ]; then
                echo "No .elf32 produced by make" >&2
                exit 1
              fi
              mkdir -p $out
              cp "$elf" $out/xenon.elf
              cp -f *.elf $out/ 2>/dev/null || true
              runHook postInstall
            '';
          };
          toolchain = xenonToolchain;
          devkitxenon = devkitXenon;
        };

        formatter = treefmtEval.config.build.wrapper;
        checks.formatting = treefmtEval.config.build.check self;

        devShells.default = pkgs.mkShell {
          packages = [
            devkitXenon
            pkgs.gnumake
            pkgs.bear
            clangdWrapper
            pkgs.clang-tools
            buildHelper
            cleanBuildHelper
            deployHelper
          ];

          shellHook = ''
            export DEVKITXENON="${devkitXenon}"
            export PATH="${devkitPath}:$PATH"

            if [ ! -f .clangd ]; then
              cat > .clangd << 'EOF'
            CompileFlags:
              Add:
                - "--target=powerpc64-unknown-unknown"
                - "-DXENON"
              Remove:
                - "-mpowerpc64"
                - "-mhard-float"
                - "-mcpu=cell"
                - "-mtune=cell"
            EOF
            fi

            cat << 'EOF'
            ========================================================================
            Environment: Xbox 360 (PowerPC Xenon / Free60 libxenon)
            ------------------------------------------------------------------------
            Properties:
              Toolchain   : xenon-gcc 16.2 (built from source, cached in /nix/store)
              SDK         : libxenon (lwIP networking, console, input)
              Output      : .elf32 -> boot with XeLL (launch XeLL from XeXMenu)

            Commands:
              build-xbox360            Build .elf32 + compile_commands.json
              rebuild-xbox360          Clean build from scratch
              deploy-xbox360 <usb>     Copy build to USB as xenon.elf
              nix build                Build xenon.elf reproducibly
            ========================================================================
            EOF
          '';
        };
      }
    );
}
