{
  description = "God Hand: Rearmed - Linux build environment, checks and packaging";

  inputs.nixpkgs.url = "github:NixOS/nixpkgs/nixos-26.05";
  # A 2020 userland (glibc 2.31, its X11, glvnd and Mesa 20.1: Debian 11 / Ubuntu 20.04 / the Steam Runtime's sniper
  # era) for the launcher-glibc-2-31 check: the packaged GHR must start on the oldest system it claims to support.
  inputs.nixpkgs-2020.url = "github:NixOS/nixpkgs/nixos-20.09";

  # nix develop                      the dev shell: build.py, the portable toolchain, packaging tools
  # nix flake check -L               every gate below (CI runs exactly this: .github/workflows/ci.yml)
  # nix build .#linux-stub           the AppImage + tar.gz of a build on the stand-in game code (CI artifacts; they
  #                                  start the launcher but cannot play: real releases need your disc, docs/BUILDING.md)
  # nix run .#package -- --bin DIR   tools/package.py with mksquashfs, objcopy and the AppImage runtime supplied
  outputs = { self, nixpkgs, nixpkgs-2020 }:
    let
      systems = [ "x86_64-linux" ];
      forAll = f: nixpkgs.lib.genAttrs systems (system: f nixpkgs.legacyPackages.${system});

      # Only what the builds read: a change to docs/ or assets/ rebuilds nothing.
      sourceOf = lib: lib.fileset.toSource {
        root = ./.;
        fileset = lib.fileset.unions [
          ./CMakeLists.txt ./GHR.ini ./LICENSE ./cmake ./src ./runtime ./thirdparty ./resources ./tests
          ./tools/linux ./tools/package.py ./tools/win/README-windows.txt ./tools/win/test_package_zip.py ./tools/readme
          ./docs/licenses ./assets/characters ./recomplauncher.json
        ];
      };

      # The dev shell's libraries are also what the native build and the launcher smoke test need.
      libsFor = pkgs: with pkgs; [
        # raylib (GLFW, X11 + Wayland) and SDL3's video/input backends
        libx11 libxrandr libxinerama libxcursor libxi libxext libxfixes
        libxscrnsaver libxtst
        libGL wayland wayland-scanner wayland-protocols libxkbcommon libdecor
        alsa-lib libpulseaudio pipewire udev dbus
        vulkan-headers vulkan-loader
      ];
      toolsFor = pkgs: with pkgs; [ cmake ninja pkg-config python3 git ];

      # The AppImage runtime tools/package.py pins (same URL, same SHA-256; test_package.py compares them).
      runtimeFor = pkgs: pkgs.fetchurl {
        url = "https://github.com/AppImage/type2-runtime/releases/download/20251108/runtime-x86_64";
        sha256 = "2fca8b443c92510f1483a883f60061ad09b46b978b2631c807cd873a47ec260d";
      };

      perSystem = pkgs:
        let
          lib = pkgs.lib;
          src = sourceOf lib;
          deps = import ./nix/deps.nix { inherit pkgs; };
          version = import ./nix/version.nix { inherit src; };
          ghr = args: import ./nix/ghr.nix ({
            inherit pkgs lib src deps;
            buildInputs = libsFor pkgs;
            nativeBuildInputs = toolsFor pkgs;
          } // args);
          portable = ghr { portable = true; };
          native = ghr { portable = false; buildTests = true; };
          runtime = runtimeFor pkgs;
          packagingTools = [ pkgs.squashfsTools pkgs.binutils pkgs.python3 ];

          # tools/package.py over the portable build, as a release is made, with --allow-stub.
          linuxStub = pkgs.runCommand "ghr-linux-stub-${version}" { nativeBuildInputs = packagingTools; } ''
            cp -r ${src} source && chmod -R u+w source && cd source
            export SOURCE_DATE_EPOCH=${toString self.lastModified}
            python3 -I tools/package.py --bin ${portable}/bin --out $out --allow-stub \
              --appimage-runtime ${runtime} --mksquashfs ${pkgs.squashfsTools}/bin/mksquashfs \
              --objcopy ${pkgs.binutils}/bin/objcopy
          '';

          # The launcher from the packaged tar.gz, on a virtual 1280x800 screen with Mesa's software OpenGL, on a given
          # userland (glibc + the libraries GLFW, SDL and the GL driver load). GHR is copied with its interpreter set to
          # that glibc's loader (the sandbox has no /lib64 loader) and started through a wrapper that alone carries the
          # userland's library path, so the check's own tools (python, Xvfb, xwd) keep theirs. Everything else is the
          # shipped folder as a player gets it.
          smokeOn = name: u: pkgs.runCommand "ghr-${name}" {
            nativeBuildInputs = [ pkgs.python3 pkgs.patchelf pkgs.xorg-server pkgs.xwd ];
          } ''
            mkdir -p work $out && cd work
            tar xzf ${linuxStub}/GHR-v${version}-stub-Linux-x64.tar.gz
            cd "God Hand Rearmed"
            chmod u+w . GHR
            patchelf --set-interpreter ${u.glibc}/lib/ld-linux-x86-64.so.2 GHR
            cat > run.sh <<WRAP
            #!/bin/sh
            export LD_LIBRARY_PATH=${u.glibc}/lib:${u.dri}/lib:${lib.makeLibraryPath u.libs}
            export __GLX_VENDOR_LIBRARY_NAME=mesa LIBGL_DRIVERS_PATH=${u.dri}/lib/dri LIBGL_DEBUG=verbose
            exec "\''${0%/*}/GHR" "\$@"
            WRAP
            sed -i 's/^ *//' run.sh && chmod +x run.sh
            export HOME=$TMPDIR
            # The virtual screen's own GLX needs Mesa's drivers (the check's Mesa; GHR's come from its userland above).
            export LIBGL_DRIVERS_PATH=${pkgs.mesa}/lib/dri
            ./run.sh --help | tee $out/help.txt | head -1
            grep -q "^usage:" $out/help.txt
            python3 -I ${src}/tools/linux/smoke_launcher.py --out $out/launcher.png --seconds 90 --log logs/kamite.log -- ./run.sh
            grep -m3 -E "Renderer:|Version:" logs/kamite.log | tee $out/gl.txt
          '';

          launcherSmoke = smokeOn "launcher-smoke" {
            glibc = pkgs.glibc;
            dri = pkgs.mesa;
            libs = with pkgs; [ libx11 libxrandr libxinerama libxcursor libxi libxext libxrender libxfixes libxcb libxau
              libxdmcp libglvnd libxkbcommon mesa alsa-lib ];
          };

          # GHR without a usable window: no display at all, and an X server without GLX (no OpenGL). Both must end with
          # exit code 1 and a message saying why, not a crash (raylib's InitWindow carries on after either failure).
          windowErrors = pkgs.runCommand "ghr-window-errors" { nativeBuildInputs = [ pkgs.patchelf pkgs.xorg-server ]; } ''
            mkdir -p work $out && cd work
            tar xzf ${linuxStub}/GHR-v${version}-stub-Linux-x64.tar.gz
            cd "God Hand Rearmed"
            chmod u+w . GHR
            patchelf --set-interpreter ${pkgs.glibc}/lib/ld-linux-x86-64.so.2 GHR
            export HOME=$TMPDIR LD_LIBRARY_PATH=${lib.makeLibraryPath (with pkgs; [ libx11 libxrandr libxinerama
              libxcursor libxi libxext libxrender libxfixes libxcb libxau libxdmcp libglvnd libxkbcommon mesa ])}
            expect_exit_1() { # <what> <message> <command...>
              what=$1 message=$2; shift 2
              set +e; "$@" > run.txt 2>&1; code=$?; set -e
              cat run.txt logs/*.log 2>/dev/null > all.txt || true
              echo "$what: exit $code" | tee -a $out/result.txt
              [ $code -eq 1 ] || { tail -30 all.txt; exit 1; }
              grep -q "$message" all.txt || { echo "no '$message' in the output"; tail -30 all.txt; exit 1; }
            }
            expect_exit_1 "no display" "could not open the game window" env -u DISPLAY -u WAYLAND_DISPLAY ./GHR
            Xvfb :77 -screen 0 1280x800x24 -nolisten tcp -extension GLX & xvfb=$!
            for i in $(seq 50); do [ -e /tmp/.X11-unix/X77 ] && break; sleep 0.1; done
            expect_exit_1 "X server without GLX" "could not create the game window" env DISPLAY=:77 ./GHR
            kill $xvfb
          '';

          old = nixpkgs-2020.legacyPackages.${pkgs.stdenv.hostPlatform.system};
          launcherGlibc231 = smokeOn "launcher-glibc-2-31" {
            glibc = old.glibc;
            dri = old.mesa.drivers;
            libs = with old; [ xorg.libX11 xorg.libXrandr xorg.libXinerama xorg.libXcursor xorg.libXi xorg.libXext
              xorg.libXrender xorg.libXfixes xorg.libxcb xorg.libXau xorg.libXdmcp libglvnd mesa.drivers libxkbcommon
              alsaLib expat zlib libdrm libffi wayland xorg.libxshmfence xorg.libXxf86vm ];
          };

          packagingTests = pkgs.runCommand "ghr-packaging-tests" {
            nativeBuildInputs = packagingTools ++ [ pkgs.gcc ];
          } ''
            cp -r ${src} source && chmod -R u+w source && cd source
            cp ${./flake.nix} flake.nix
            python3 -I -m unittest discover -s tools/linux -p "test_*.py" -v
            python3 -I -m unittest discover -s tools/win -p "test_*.py" -v
            touch $out
          '';

          # GHR's own binaries must need nothing newer than glibc 2.31 and no library the package lacks.
          portableCheck = pkgs.runCommand "ghr-portable-check" { nativeBuildInputs = [ pkgs.python3 ]; } ''
            cd ${src}
            python3 -I -c 'import sys; sys.path.insert(0, "tools"); import package
            problems = package.check_portable(__import__("pathlib").Path("${portable}/bin"))
            print("\n".join(problems) or "portable: GHR and its libraries need glibc <= 2.31 and nothing unshipped")
            sys.exit(1 if problems else 0)'
            touch $out
          '';
        in
        {
          packages = {
            inherit portable native;
            linux-stub = linuxStub;
            appimage-runtime = runtime;
            default = linuxStub;
          };
          checks = {
            inherit native;  # gcc build + unit tests (ctest)
            linux-stub = linuxStub;
            launcher-smoke = launcherSmoke;
            launcher-glibc-2-31 = launcherGlibc231;
            window-errors = windowErrors;
            packaging-tests = packagingTests;
            portable-binary = portableCheck;
          };
          apps.package = {
            type = "app";
            program = toString (pkgs.writeShellScript "ghr-package" ''
              export PATH=${lib.makeBinPath packagingTools}:$PATH
              exec python3 -I tools/package.py --appimage-runtime ${runtime} "$@"
            '');
          };
          devShells.default = pkgs.mkShell {
            packages = toolsFor pkgs ++ libsFor pkgs ++ packagingTools ++ (with pkgs; [ gcc zig ]);
            shellHook = ''
              export KAMITE_APPIMAGE_RUNTIME=${runtime}
            '';
          };
        };

      each = forAll perSystem;
    in
    {
      packages = nixpkgs.lib.mapAttrs (_: v: v.packages) each;
      checks = nixpkgs.lib.mapAttrs (_: v: v.checks) each;
      apps = nixpkgs.lib.mapAttrs (_: v: v.apps) each;
      devShells = nixpkgs.lib.mapAttrs (_: v: v.devShells) each;
    };
}
