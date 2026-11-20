{
  description = "God Hand: Rearmed - Linux build environment, checks and packaging";

  inputs.nixpkgs.url = "github:NixOS/nixpkgs/nixos-26.05";

  # nix develop                      the dev shell: build.py, the portable toolchain, packaging tools
  # nix flake check -L               every gate below (CI runs exactly this: .github/workflows/linux.yml)
  # nix build .#linux-stub           the AppImage + tar.gz of a build on the stand-in game code (CI artifacts; they
  #                                  start the launcher but cannot play: real releases need your disc, docs/BUILDING.md)
  # nix run .#package -- --bin DIR   tools/linux/package.py with mksquashfs, objcopy and the AppImage runtime supplied
  outputs = { self, nixpkgs }:
    let
      systems = [ "x86_64-linux" ];
      forAll = f: nixpkgs.lib.genAttrs systems (system: f nixpkgs.legacyPackages.${system});

      # Only what the builds read: a change to docs/ or assets/ rebuilds nothing.
      sourceOf = lib: lib.fileset.toSource {
        root = ./.;
        fileset = lib.fileset.unions [
          ./CMakeLists.txt ./GHR.ini ./LICENSE ./cmake ./src ./runtime ./thirdparty ./resources ./tests
          ./tools/linux ./tools/readme ./docs/licenses
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

      # The AppImage runtime tools/linux/package.py pins (same URL, same SHA-256; test_package.py compares them).
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

          # tools/linux/package.py over the portable build, as a release is made, with --allow-stub.
          linuxStub = pkgs.runCommand "ghr-linux-stub-${version}" { nativeBuildInputs = packagingTools; } ''
            cp -r ${src} source && chmod -R u+w source && cd source
            export SOURCE_DATE_EPOCH=${toString self.lastModified}
            python3 -I tools/linux/package.py --bin ${portable}/bin --out $out --allow-stub \
              --appimage-runtime ${runtime} --mksquashfs ${pkgs.squashfsTools}/bin/mksquashfs \
              --objcopy ${pkgs.binutils}/bin/objcopy
          '';

          # The launcher from the packaged tar.gz, on a virtual 1280x800 screen with Mesa's software OpenGL. GHR is
          # copied with its interpreter pointed at this glibc (the sandbox has no /lib64 loader); everything else is
          # the shipped folder as a player gets it.
          launcherSmoke = pkgs.runCommand "ghr-launcher-smoke" {
            nativeBuildInputs = [ pkgs.python3 pkgs.patchelf pkgs.xorg-server pkgs.xwd ];
          } ''
            mkdir -p work $out && cd work
            tar xzf ${linuxStub}/GHR-v${version}-stub-linux-x86_64.tar.gz
            cd "God Hand Rearmed"
            chmod u+w . GHR
            patchelf --set-interpreter ${pkgs.glibc}/lib/ld-linux-x86-64.so.2 GHR
            export HOME=$TMPDIR
            export LD_LIBRARY_PATH=${lib.makeLibraryPath (with pkgs; [ libx11 libxrandr libxinerama libxcursor libxi
              libxext libxrender libxfixes libxcb libxau libxdmcp libglvnd libxkbcommon mesa alsa-lib ])}
            export __GLX_VENDOR_LIBRARY_NAME=mesa LIBGL_DRIVERS_PATH=${pkgs.mesa}/lib/dri
            python3 -I ${src}/tools/linux/smoke_launcher.py --out $out/launcher.png --seconds 90 -- ./GHR
          '';

          packagingTests = pkgs.runCommand "ghr-packaging-tests" {
            nativeBuildInputs = packagingTools ++ [ pkgs.gcc ];
          } ''
            cp -r ${src} source && chmod -R u+w source && cd source
            cp ${./flake.nix} flake.nix
            python3 -I -m unittest discover -s tools/linux -p "test_*.py" -v
            touch $out
          '';

          # GHR's own binaries must need nothing newer than glibc 2.31 and no library the package lacks.
          portableCheck = pkgs.runCommand "ghr-portable-check" { nativeBuildInputs = [ pkgs.python3 ]; } ''
            cd ${src}
            python3 -I -c 'import sys; sys.path.insert(0, "tools/linux"); import package
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
            packaging-tests = packagingTests;
            portable-binary = portableCheck;
          };
          apps.package = {
            type = "app";
            program = toString (pkgs.writeShellScript "ghr-package" ''
              export PATH=${lib.makeBinPath packagingTools}:$PATH
              exec python3 -I tools/linux/package.py --appimage-runtime ${runtime} "$@"
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
