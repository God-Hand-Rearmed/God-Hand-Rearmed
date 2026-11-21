# GHR built in the Nix sandbox on the stand-in game code (tools/linux/stub_game_code.py): everything but the game.
#
#   portable = true   the release toolchain (cmake/toolchains/linux-portable.cmake: zig, glibc 2.31, static libc++);
#                     what tools/package.py packs and what runs on a Steam Deck
#   portable = false  the dev shell's gcc, with the unit tests (KAMITE_BUILD_TESTS) built and run
#
# The real game needs your own disc (build.py generate), which never exists in CI; this build proves that everything
# else (runtime, launcher, renderer, platform layer) compiles, links and starts on Linux.
{ pkgs, lib, src, deps, portable ? true, buildTests ? false, buildInputs, nativeBuildInputs }:
let
  # The keep-awake tests' fake desktop services (runtime/ps2xRuntime/test/keep_awake_bus.py). CMake is pointed at it:
  # the plain python3 of nativeBuildInputs comes first on PATH and has no jeepney.
  checkPython = pkgs.python3.withPackages (p: [ p.jeepney ]);
  fetchContentFlags = lib.mapAttrsToList (name: path: "-DFETCHCONTENT_SOURCE_DIR_${name}=${path}") deps.fetchContent
    ++ [ "-DKAMITE_STB_DIR=${deps.stbDir}" ];
in
pkgs.stdenv.mkDerivation {
  pname = if portable then "ghr-portable-stub" else "ghr-native-stub";
  version = import ./version.nix { inherit src; };
  inherit src buildInputs;
  nativeBuildInputs = nativeBuildInputs ++ lib.optional portable pkgs.zig;

  dontUseCmakeConfigure = true;
  dontStrip = true; # tools/package.py strips and keeps the debug info beside the archives
  dontPatchELF = true; # the portable binary must keep its generic interpreter and no store RPATH
  dontFixup = portable;

  configurePhase = ''
    runHook preConfigure
    patchShebangs thirdparty/pcsx2-gs/test tools/linux
    export ZIG_GLOBAL_CACHE_DIR=$TMPDIR/zig-cache ZIG_LOCAL_CACHE_DIR=$TMPDIR/zig-cache
    python3 -I tools/linux/stub_game_code.py $TMPDIR/generated
    mkdir -p $TMPDIR/no-modules
    cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo \
      ${lib.optionalString portable "--toolchain cmake/toolchains/linux-portable.cmake"} \
      -DKAMITE_GENERATED_DIR=$TMPDIR/generated -DKAMITE_MODULES_DIR=$TMPDIR/no-modules \
      -DKAMITE_RELEASE=ON -DKAMITE_BUILD_TESTS=${if buildTests then "ON" else "OFF"} \
      ${lib.optionalString buildTests "-DPython3_EXECUTABLE=${checkPython}/bin/python3"} \
      -DFETCHCONTENT_FULLY_DISCONNECTED=ON ${lib.concatStringsSep " " fetchContentFlags}
    runHook postConfigure
  '';

  buildPhase = ''
    runHook preBuild
    ninja -C build -j''${NIX_BUILD_CORES:-4} kamite ${lib.optionalString buildTests "all"}
    runHook postBuild
  '';

  doCheck = buildTests;
  # The renderer's tests (pcsx2_gs_smoke, gs_pcsx2_backend_test) open an X11 window and render with Vulkan: a fresh
  # Xvfb and Mesa's lavapipe (software Vulkan), with the X / GL / Vulkan libraries GLFW and the loader dlopen.
  # The keep-awake tests (issue #12) talk to a private dbus-daemon through libdbus, with fake desktop services in
  # python3 + jeepney (checkPython), read the X server's idle time through libXss, and kill an Xvfb of their own;
  # KAMITE_REQUIRE_ALL_TESTS=1 makes a skip fail.
  nativeCheckInputs = lib.optionals buildTests [ pkgs.xvfb-run pkgs.xvfb pkgs.dbus pkgs.procps checkPython ];
  checkPhase = ''
    runHook preCheck
    export HOME=$TMPDIR KAMITE_SMOKE_XVFB=1 KAMITE_REQUIRE_ALL_TESTS=1
    export VK_DRIVER_FILES=${pkgs.mesa}/share/vulkan/icd.d/lvp_icd.x86_64.json
    export VK_ICD_FILENAMES=$VK_DRIVER_FILES __GLX_VENDOR_LIBRARY_NAME=mesa LIBGL_DRIVERS_PATH=${pkgs.mesa}/lib/dri
    export LD_LIBRARY_PATH=${lib.makeLibraryPath (with pkgs; [ vulkan-loader libglvnd mesa libx11 libxext libxrandr
      libxinerama libxcursor libxi libxrender libxfixes libxcb libxscrnsaver dbus.lib ])}
    ctest --test-dir build --output-on-failure
    runHook postCheck
  '';

  installPhase = ''
    runHook preInstall
    mkdir -p $out
    cp -a build/bin $out/bin
    runHook postInstall
  '';

  meta.description = "God Hand: Rearmed on the stand-in game code (CI build; cannot play the game)";
}
