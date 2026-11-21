# The sources CMake's FetchContent downloads at configure time, pinned for the Nix sandbox (no network there).
# Each attribute is passed as -DFETCHCONTENT_SOURCE_DIR_<NAME>=<store path> (flake.nix, fetchContentFlags), so the
# build uses exactly these trees. Keep the revisions equal to the GIT_TAG / URL in the CMake files that declare them:
#   raylib, imgui, rlImGui      runtime/ps2xRuntime/CMakeLists.txt
#   dav1d, libavif              src/CMakeLists.txt (the launcher's saloon)
#   SDL3                        runtime/ps2xRuntime/CMakeLists.txt (controllers; a release tarball, fetchzip)
#   libchdr                     runtime/ps2xRuntime/CMakeLists.txt (CHD disc images)
#   shaderc, glslang, SPIRV-*   thirdparty/pcsx2-gs/shaderc.cmake (the Vulkan renderer's shader compiler; zips)
#   ELFIO, toml11, fmt, libdwarf, rabbitizer   runtime/ps2xRecomp/CMakeLists.txt (the recompiler, built by
#                               `build.py generate`: the tags it names, pinned here to the commits they point at)
# stb_image.h is no FetchContent: thirdparty/pcsx2-gs/CMakeLists.txt downloads it into KAMITE_STB_DIR (stbDir below).
# A changed revision there and here together; the hash comes from
#   nix-prefetch-url --unpack https://github.com/<owner>/<repo>/archive/<rev>.tar.gz
#
# cmakeInit: all of the above as one CMake initial cache (cmake -C). The flake's builds configure with it, and
# `nix develop` exports it as KAMITE_CMAKE_INIT, which build.py's configure passes on: a build in the dev shell uses the
# same trees as the flake's and downloads nothing at configure time.
{ pkgs }:
let
  gh = owner: repo: rev: sha256: pkgs.fetchFromGitHub { inherit owner repo rev sha256; };
  zip = url: sha256: pkgs.fetchzip { inherit url sha256; };
  lib = pkgs.lib;
in
rec {
  # The recompiler's (KAMITE_BUILD_RECOMP=ON only; the flake's own builds never make it: they have no disc).
  recompFetchContent = {
    # tag Release_3.12
    ELFIO = gh "serge1" "ELFIO" "8ae6cec5d60495822ecd57d736f66149da9b1830"
      "0zl5jbmbimz0j50mrgzw9am72wzjgsyvvl02cbhgsbrnrfql2d5l";
    # tag v4.4.0 (its submodules are only its own tests and docs, off by default)
    TOML11 = gh "ToruNiina" "toml11" "be08ba2be2a964edcdb3d3e3ea8d100abc26f286"
      "0d15b50cf9jgvh3w99xh6crh03bn2dmv9bdyvzq6knsk2diql1dj";
    # tag 12.1.0
    FMT = gh "fmtlib" "fmt" "407c905e45ad75fc29bf0f9bb7c5c2fd3475976f"
      "17b1iwmmpd9cb1g3lzn5pjky73i3268kldmdqpjv6s8rzl73aqk6";
    # tag v2.2.0
    LIBDWARF = gh "davea42" "libdwarf-code" "2e088b983562860f497b4fd789a3e61524eb857c"
      "1aqi8m3qhqxk4sggc5x1awgw95s8krgwq2dnrk5d0r0qvfn6361w";
    # tag 1.14.3
    RABBITIZER = gh "Decompollaborate" "rabbitizer" "d5804fa488474830a47fbf9f3693048ec1e45589"
      "0w5mqjvm5m0f7q1wm8k6pmpcf5pm34mf9spdfa6m6br0gkajkyh6";
  };

  # Every pinned tree as a CMake initial cache (see the header). FORCE: an existing build tree that once downloaded is
  # switched over too.
  cmakeInit = pkgs.writeText "ghr-pinned-deps.cmake" (lib.concatStrings (
    lib.mapAttrsToList (name: path: ''
      set(FETCHCONTENT_SOURCE_DIR_${name} "${path}" CACHE PATH "pinned by nix/deps.nix" FORCE)
    '') (fetchContent // recompFetchContent)
    ++ [ ''
      set(KAMITE_STB_DIR "${stbDir}" CACHE PATH "pinned by nix/deps.nix" FORCE)
      set(FETCHCONTENT_FULLY_DISCONNECTED ON CACHE BOOL "every source is pinned (nix/deps.nix)" FORCE)
    '' ]));

  # -DKAMITE_STB_DIR: a folder holding stb_image.h at the commit and SHA-256 the CMake file names.
  stbDir = pkgs.linkFarm "stb" [{
    name = "stb_image.h";
    path = pkgs.fetchurl {
      url = "https://raw.githubusercontent.com/nothings/stb/f58f558c120e9b32c217290b80bad1a0729fbb2c/stb_image.h";
      sha256 = "594c2fe35d49488b4382dbfaec8f98366defca819d916ac95becf3e75f4200b3";
    };
  }];

  fetchContent = {
    # tag 5.5
    RAYLIB = gh "raysan5" "raylib" "c1ab645ca298a2801097931d1079b10ff7eb9df8"
      "1dhy9ghbwvz0s434j03rfa2l6wxcfj028vlkk1xbf5q97vin5pr7";
    # tag v1.92.7-docking
    IMGUI = gh "ocornut" "imgui" "b1bcb12a624af7509894c8e77dd47416997777fa"
      "0c152ja0vgznxkks0jlc9fm8b5cb2jc682zk05yql3h1dhc86ia6";
    # branch Raylib_5_5
    RLIMGUI = gh "raylib-extras" "rlImGui" "118221c8a532501a3c2c236b62950877cbefc2b4"
      "1bp841l243qpz4gvvfh3xaw9p153wdwq3ncjs4hd410d0aj5qjgp";
    # 2026-10-11 (CHD disc images; runtime/ps2xRuntime/CMakeLists.txt)
    PS2X_LIBCHDR = gh "rtissera" "libchdr" "47d794151b2b64f6aba60304e6480f4f5a2bc24b"
      "1d4bnlrb6hp2dmjv25kgsnyffc3z10ih3j733yipv68wmgvcz7rl";
    # 1.2.1-SDL
    KAMITE_DAV1D = gh "libsdl-org" "dav1d" "a6b395b472c2b1911a49936840e18535b071c1bd"
      "0qb68dabqgk7sgdc1m56d0nl2jika15623hp6fj7win5az4hvpsd";
    # release-3.4.10 (the release tarball runtime/ps2xRuntime/CMakeLists.txt downloads; URL_HASH checks the archive)
    SDL3 = pkgs.fetchzip {
      url = "https://github.com/libsdl-org/SDL/releases/download/release-3.4.10/SDL3-3.4.10.tar.gz";
      sha256 = "0kf1kfw4k8a6j56lq4jygz971ljyhilqbv2wdsmzqh6zxysihg29";
    };
    # v1.1.1-SDL
    KAMITE_LIBAVIF = gh "libsdl-org" "libavif" "ee829431ceaed31a34b7e1420f7f18180e7eae23"
      "1gd3z3d0w0hx97pmck1sjsc6afzs187kzwmbzmr0khjqna2gbyi8";
    KAMITE_GLSLANG = zip "https://github.com/KhronosGroup/glslang/archive/275822a6261ee689aadb1da5f09a0ec2f058685c.zip"
      "0qdnazfv1sk5w60k74zbcv2ig3yqwwcb75bia395ba2z8ckmrjf1";
    KAMITE_SPIRV_HEADERS = zip "https://github.com/KhronosGroup/SPIRV-Headers/archive/58006c901d1d5c37dece6b6610e9af87fa951375.zip"
      "0fwpz5j0mmbf8a2wzqdv3454g2dpy1b5dcjlrhdbsw5sykf3xvwd";
    KAMITE_SPIRV_TOOLS = zip "https://github.com/KhronosGroup/SPIRV-Tools/archive/6337eb62cadd7d124ac6789bf39c0f71148f0a73.zip"
      "0glhyk1qspqjwhz7wsvbiddnzspl2wx3bfzqyl8yh5qij5vc8l87";
    KAMITE_SHADERC = zip "https://github.com/google/shaderc/archive/refs/tags/v2026.2.zip"
      "0p6l43lk0ykrg7wbdi846nby487h36dmgrrxi5rvl1b3q4q3b0id";
  };
}
