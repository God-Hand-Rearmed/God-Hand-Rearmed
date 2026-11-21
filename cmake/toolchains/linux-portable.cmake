# The portable Linux build: GHR and its libraries for any x86-64 Linux with glibc 2.31 or newer (the Steam Deck,
# SteamOS, Debian 11+, Ubuntu 20.04+). zig's clang links against glibc 2.31's symbol versions and its own libc++
# statically, so the binary carries no dependency on the build machine's glibc or libstdc++ (a Nix store, a newer
# distro). Used by the flake's packages (flake.nix) and tools/package.py; needs `zig` on PATH.
#   cmake -S . -B build/linux -G Ninja --toolchain cmake/toolchains/linux-portable.cmake
set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR x86_64)
set(CMAKE_C_COMPILER "${CMAKE_CURRENT_LIST_DIR}/../../tools/linux/zig-cc")
set(CMAKE_CXX_COMPILER "${CMAKE_CURRENT_LIST_DIR}/../../tools/linux/zig-c++")
# zig has no separate archiver binary on PATH; its own ar / ranlib do.
set(CMAKE_AR "${CMAKE_CURRENT_LIST_DIR}/../../tools/linux/zig-ar" CACHE FILEPATH "" FORCE)
set(CMAKE_RANLIB "${CMAKE_CURRENT_LIST_DIR}/../../tools/linux/zig-ranlib" CACHE FILEPATH "" FORCE)

# GLFW and raylib look these up with find_library, which would return the BUILD machine's libm.so / librt.so by full
# path (a newer glibc's, whose private symbols the 2.31 link refuses). By name, zig links its own 2.31 stubs.
set(RT_LIBRARY rt CACHE STRING "" FORCE)
set(MATH_LIBRARY m CACHE STRING "" FORCE)
set(pthread pthread CACHE STRING "" FORCE)

set(KAMITE_LINUX_PORTABLE ON CACHE BOOL "Built with the portable toolchain (glibc 2.31, static libc++)" FORCE)
