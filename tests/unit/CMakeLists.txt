# GHR's unit tests (tests/unit/unit_tests.cpp): one plain program, registered with CTest. It links only the pure-logic
# sources it tests, so it needs neither the recompiled game code nor a display.
#
# From the top level: -DKAMITE_BUILD_TESTS=ON, then ctest. On its own (no game code at all):
#   cmake -S tests/unit -B build/unit -G Ninja && ninja -C build/unit && ctest --test-dir build/unit
cmake_minimum_required(VERSION 3.21)
if(NOT PROJECT_NAME)
    project(GhrUnitTests LANGUAGES C CXX)
    set(CMAKE_CXX_STANDARD 20)
    set(CMAKE_CXX_STANDARD_REQUIRED ON)
    enable_testing()
endif()

set(KAMITE_UNIT_ROOT "${CMAKE_CURRENT_LIST_DIR}/../..")
cmake_path(NORMAL_PATH KAMITE_UNIT_ROOT)

# The tar.gz tests need raylib's inflate (sinfl.h), the one GHR's updater uses. In the full build it is already fetched
# (runtime/ps2xRuntime); on its own, only its source is fetched (same release, nothing of it is built).
include(FetchContent)
FetchContent_GetProperties(raylib)
if(NOT raylib_POPULATED AND NOT raylib_SOURCE_DIR)
    FetchContent_Declare(raylib_unit_tests
        GIT_REPOSITORY "https://github.com/raysan5/raylib.git"
        GIT_TAG "5.5"
        SOURCE_SUBDIR "does-not-exist") # source only: no add_subdirectory
    FetchContent_MakeAvailable(raylib_unit_tests)
    set(KAMITE_UNIT_SINFL_DIR "${raylib_unit_tests_SOURCE_DIR}/src/external")
else()
    set(KAMITE_UNIT_SINFL_DIR "${raylib_SOURCE_DIR}/src/external")
endif()

add_executable(ghr_unit_tests
    unit_tests.cpp
    sinfl_impl.c
    "${KAMITE_UNIT_ROOT}/src/kamite/paths.cpp"
    "${KAMITE_UNIT_ROOT}/src/kamite/steam_deck.cpp"
    "${KAMITE_UNIT_ROOT}/src/kamite/steam_shortcuts.cpp"
    "${KAMITE_UNIT_ROOT}/src/kamite/update_archive.cpp")
target_include_directories(ghr_unit_tests PRIVATE "${KAMITE_UNIT_ROOT}/src" "${CMAKE_CURRENT_LIST_DIR}")
target_include_directories(ghr_unit_tests PRIVATE "${KAMITE_UNIT_SINFL_DIR}")
target_compile_definitions(ghr_unit_tests PRIVATE KAMITE_SOURCE_DIR="${KAMITE_UNIT_ROOT}")
if(MSVC)
    target_compile_options(ghr_unit_tests PRIVATE /utf-8 /W3)
else()
    target_compile_options(ghr_unit_tests PRIVATE -Wall -Wextra)
endif()

add_test(NAME ghr_unit_tests COMMAND ghr_unit_tests)
