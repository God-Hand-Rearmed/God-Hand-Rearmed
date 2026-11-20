# The sources CMake's FetchContent downloads at configure time, pinned for the Nix sandbox (no network there).
# Each attribute is passed as -DFETCHCONTENT_SOURCE_DIR_<NAME>=<store path> (flake.nix, fetchContentFlags), so the
# build uses exactly these trees. Keep the revisions equal to the GIT_TAG / URL in the CMake files that declare them:
#   raylib, imgui, rlImGui      runtime/ps2xRuntime/CMakeLists.txt
#   dav1d, libavif              src/CMakeLists.txt (the launcher's saloon)
#   SDL3                        runtime/ps2xRuntime/CMakeLists.txt (controllers; a release tarball, fetchzip)
#   libchdr                     runtime/ps2xRuntime/CMakeLists.txt (CHD disc images)
#   shaderc, glslang, SPIRV-*   thirdparty/pcsx2-gs/shaderc.cmake (the Vulkan renderer's shader compiler; zips)
# stb_image.h is no FetchContent: thirdparty/pcsx2-gs/CMakeLists.txt downloads it into KAMITE_STB_DIR (stbDir below).
# A changed revision there and here together; the hash comes from
#   nix-prefetch-url --unpack https://github.com/<owner>/<repo>/archive/<rev>.tar.gz
{ pkgs }:
let
  gh = owner: repo: rev: sha256: pkgs.fetchFromGitHub { inherit owner repo rev sha256; };
  zip = url: sha256: pkgs.fetchzip { inherit url sha256; };
in
{
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
