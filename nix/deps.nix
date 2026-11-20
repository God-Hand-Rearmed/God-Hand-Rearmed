# The sources CMake's FetchContent downloads at configure time, pinned for the Nix sandbox (no network there).
# Each attribute is passed as -DFETCHCONTENT_SOURCE_DIR_<NAME>=<store path> (flake.nix, fetchContentFlags), so the
# build uses exactly these trees. Keep the revisions equal to the GIT_TAG / URL in the CMake files that declare them:
#   raylib, imgui, rlImGui      runtime/ps2xRuntime/CMakeLists.txt
#   dav1d, libavif              src/CMakeLists.txt (the launcher's saloon)
# A changed revision there and here together; the hash comes from
#   nix-prefetch-url --unpack https://github.com/<owner>/<repo>/archive/<rev>.tar.gz
{ pkgs }:
let
  gh = owner: repo: rev: sha256: pkgs.fetchFromGitHub { inherit owner repo rev sha256; };
in
{
  # tag 5.5
  RAYLIB = gh "raysan5" "raylib" "c1ab645ca298a2801097931d1079b10ff7eb9df8"
    "1dhy9ghbwvz0s434j03rfa2l6wxcfj028vlkk1xbf5q97vin5pr7";
  # tag v1.92.7-docking
  IMGUI = gh "ocornut" "imgui" "b1bcb12a624af7509894c8e77dd47416997777fa"
    "0c152ja0vgznxkks0jlc9fm8b5cb2jc682zk05yql3h1dhc86ia6";
  # branch Raylib_5_5
  RLIMGUI = gh "raylib-extras" "rlImGui" "118221c8a532501a3c2c236b62950877cbefc2b4"
    "1bp841l243qpz4gvvfh3xaw9p153wdwq3ncjs4hd410d0aj5qjgp";
  # 1.2.1-SDL
  KAMITE_DAV1D = gh "libsdl-org" "dav1d" "a6b395b472c2b1911a49936840e18535b071c1bd"
    "0qb68dabqgk7sgdc1m56d0nl2jika15623hp6fj7win5az4hvpsd";
  # v1.1.1-SDL
  KAMITE_LIBAVIF = gh "libsdl-org" "libavif" "ee829431ceaed31a34b7e1420f7f18180e7eae23"
    "1gd3z3d0w0hx97pmck1sjsc6afzs187kzwmbzmr0khjqna2gbyi8";
}
