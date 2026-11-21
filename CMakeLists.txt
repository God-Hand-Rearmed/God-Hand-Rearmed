cmake_minimum_required(VERSION 3.21)

# The game (USA, SLUS-21503) native PC port. Adapted from the Urban Reign port's top-level CMakeLists.txt
# (forks/urban-reign-recomp @2fa53c4); KAMITE_PORT_NOTES.md lists what changed.
project(Kamite LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# runtime/: the PS2Recomp runtime (EE/IOP/VU/GS emulation), recompiler and offline tools (LightVelox/PS2Recomp
# sotc-port ac9efa0 + the Urban Reign port's changes). thirdparty/: PCSX2's GS and what it needs (optional below).
option(KAMITE_BUILD_RECOMP "Build ps2_recomp from runtime/ps2xRecomp" OFF)
# runtime/ps2xTest holds the offline tools: vu1_recomp (VU1 microcode AOT), gs_replay, vu0_audit.
option(KAMITE_BUILD_TOOLS "Build the offline tools in runtime/ps2xTest (vu1_recomp...)" OFF)
# runtime/CMakeLists.txt (Urban's) reads these two names.
set(URBAN_BUILD_RECOMP ${KAMITE_BUILD_RECOMP})
set(URBAN_BUILD_TOOLS ${KAMITE_BUILD_TOOLS})

# A player release (Make Release.cmd; claude/release.md): no developer dump keys (F4 / F6 / F8 / F10 / Shift+F10), no
# F12 debugger key, no profilers or tracers (KAMITE_PC_SAMPLE, KAMITE_CALL_PROFILE, KAMITE_CALL_TRACE,
# KAMITE_FIND_COUNTERS). F2, test cases and the crash logs stay. Defined for GHR.exe's own code and the runtime only
# (src/CMakeLists.txt), so switching it does not recompile the generated game code; the built-in default GHR.ini gets
# the players' values (a window, no time limit).
option(KAMITE_RELEASE "Build a player release" OFF)

# In-game ImGui "Runtime Debugger" (threads, kernel, IOP/SIF, RPC history, GS, DMA, memory); F1 toggles it.
option(KAMITE_DEBUG_UI "Build the ImGui runtime debugger into GHR.exe" ON)
set(PS2X_ENABLE_DEBUG_UI ${KAMITE_DEBUG_UI} CACHE BOOL "" FORCE)
# FFmpeg MPEG decoder for the libmpeg HLE. OFF: ON downloads a prebuilt LGPL FFmpeg at configure time
# (runtime/ps2xRuntime/CMakeLists.txt, ExternalProject ffmpeg_external).
option(KAMITE_FFMPEG "Decode MPEG video with FFmpeg (downloads a prebuilt FFmpeg)" OFF)
set(PS2X_ENABLE_FFMPEG ${KAMITE_FFMPEG} CACHE BOOL "" FORCE)
# Controllers through SDL3 (downloads the SDL3 source at configure time; SDL3.dll goes next to GHR.exe): XInput,
# PlayStation, Switch and Steam controllers, vibration. OFF (or PS2X_SDL_INPUT=0 at run time): GLFW's gamepads.
option(KAMITE_SDL_INPUT "Read controllers through SDL3" ON)
set(PS2X_ENABLE_SDL_INPUT ${KAMITE_SDL_INPUT} CACHE BOOL "" FORCE)
# The launcher's Add to Steam button (a non-Steam shortcut with library art); not on Android.
if(ANDROID)
    set(KAMITE_STEAM_SHORTCUT_DEFAULT OFF)
else()
    set(KAMITE_STEAM_SHORTCUT_DEFAULT ON)
endif()
option(KAMITE_STEAM_SHORTCUT "Add to Steam in the launcher" ${KAMITE_STEAM_SHORTCUT_DEFAULT})
# Runtime logs: the normal ones stay, the very verbose ones (per-call function log tracker) are off.
set(PS2X_ENABLE_AGRESSIVE_LOGS OFF CACHE BOOL "" FORCE)
set(PS2X_ENABLE_SCCACHE OFF CACHE BOOL "" FORCE)

# GCC and Clang need the instruction set named before the runtime's SSE4.1 intrinsics compile (MSVC takes them as
# they are). SSE4.1 is the floor the Windows build already assumes (src/CMakeLists.txt KAMITE_ARCH); every x86-64 CPU
# the port runs on has it, the Steam Deck's Zen 2 included. No -mfma: the VU code must not contract into FMA.
if(NOT MSVC AND CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|AMD64|amd64)$")
    add_compile_options(-msse4.1)
endif()

# The tests, run with ctest: runtime/ps2xRuntime/test (the vblank lock on simulated displays, issue #1; the audio's
# time-stretch on a PC below full speed, issue #5), tests/unit (the operating-system layer's pure logic; also builds on
# its own, see tests/unit/CMakeLists.txt) and, further down, the PCSX2 renderer's on Linux.
option(KAMITE_BUILD_TESTS "Build and register the tests (ctest)" OFF)
if(KAMITE_BUILD_TESTS)
    enable_testing()
endif()

add_subdirectory(runtime EXCLUDE_FROM_ALL)
add_subdirectory(src)

if(KAMITE_BUILD_TESTS)
    add_subdirectory(tests/unit)
endif()

# PCSX2's GS (hardware renderer) as pcsx2gs.dll (DX11), loaded at run time when [Video] Renderer=pcsx2 (the default).
# ON by default: its wrapper carries the game's GameIndex fixes and serial (thirdparty/pcsx2-gs/compat/Bridge.cpp,
# HostStubs.cpp). The runtime falls back to its own GS when the DLL is missing (gs_pcsx2_backend.cpp,
# GSPcsx2Backend::Requested), and -DKAMITE_PCSX2_GS=OFF builds without it.
option(KAMITE_PCSX2_GS "Build PCSX2's GS (pcsx2gs.dll)" ON)
# Linux: libpcsx2gs.so with the Vulkan renderer only, presenting into an X11 child window (Xwayland under Wayland,
# gamescope's on the Steam Deck); libshaderc_shared.so.1 and resources/ next to GHR, as on Windows.
if(KAMITE_PCSX2_GS AND (WIN32 OR (CMAKE_SYSTEM_NAME STREQUAL "Linux" AND NOT ANDROID)))
    if(NOT EXISTS "${CMAKE_SOURCE_DIR}/thirdparty/pcsx2-gs/CMakeLists.txt")
        message(FATAL_ERROR "KAMITE_PCSX2_GS=ON needs thirdparty/pcsx2-gs (copy it from the Urban Reign port)")
    endif()
    add_subdirectory(thirdparty/pcsx2-gs EXCLUDE_FROM_ALL)
    if(TARGET kamite)
        add_dependencies(kamite pcsx2gs)
        target_link_libraries(kamite PRIVATE pcsx2gs_api) # the F2 menu's UI table (src/kamite/camera.cpp)
    endif()
    target_link_libraries(ps2_runtime PRIVATE pcsx2gs_api)
endif()

# Tests registered with CTest (ctest --test-dir <build dir>). pcsx2_gs_smoke: libpcsx2gs.so rendering and presenting
# on an X11 window (thirdparty/pcsx2-gs/test/SmokeTest.cpp, run_smoke.sh: Xvfb when there is no display; the Vulkan
# driver comes from the environment, e.g. Mesa's lavapipe through VK_DRIVER_FILES).
if(KAMITE_BUILD_TESTS)
    if(TARGET pcsx2_gs_smoke)
        add_custom_target(kamite_test_programs ALL)
        add_dependencies(kamite_test_programs pcsx2_gs_smoke)
        add_test(NAME pcsx2_gs_smoke
            COMMAND "${CMAKE_SOURCE_DIR}/thirdparty/pcsx2-gs/test/run_smoke.sh" "$<TARGET_FILE:pcsx2_gs_smoke>" 30)
        set_tests_properties(pcsx2_gs_smoke PROPERTIES TIMEOUT 300)

        # gs_pcsx2_backend_test: the runtime's side (gs_pcsx2_backend.cpp) with a raylib game window and no disc: the
        # library found next to the executable, the X11 child window, resizes, hiding, the API switch, KAMITE_HOME.
        add_executable(gs_pcsx2_backend_test
            runtime/ps2xRuntime/test/gs_pcsx2_backend_test.cpp runtime/ps2xRuntime/src/lib/gs/gs_pcsx2_backend.cpp)
        target_include_directories(gs_pcsx2_backend_test PRIVATE runtime/ps2xRuntime/include)
        find_package(X11 REQUIRED)
        target_link_libraries(gs_pcsx2_backend_test PRIVATE pcsx2gs_api raylib X11::X11 ${CMAKE_DL_LIBS})
        set_target_properties(gs_pcsx2_backend_test PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
        add_dependencies(gs_pcsx2_backend_test pcsx2gs)
        add_dependencies(kamite_test_programs gs_pcsx2_backend_test)
        add_test(NAME gs_pcsx2_backend_test
            COMMAND "${CMAKE_SOURCE_DIR}/thirdparty/pcsx2-gs/test/run_smoke.sh" "$<TARGET_FILE:gs_pcsx2_backend_test>" 40)
        set_tests_properties(gs_pcsx2_backend_test PROPERTIES TIMEOUT 300)
        add_test(NAME gs_pcsx2_backend_no_x11
            COMMAND "${CMAKE_COMMAND}" -E env --unset=DISPLAY "$<TARGET_FILE:gs_pcsx2_backend_test>" --no-x11)
    endif()
endif()

# KAMITE_LTO: also /GL for ps2_iop and the port sources (linked with /LTCG). ps2_runtime always gets /GL in
# RelWithDebInfo/Release from the runtime's own EnableFastReleaseMode, and GHR.exe links /LTCG in RelWithDebInfo
# (src/CMakeLists.txt). The generated code (~10k files) never gets /GL.
option(KAMITE_LTO "Whole-program optimization of the runtime, the IOP and the port (not the generated code)" OFF)
if(MSVC AND KAMITE_LTO AND TARGET kamite)
    foreach(target ps2_runtime ps2_iop kamite)
        target_compile_options(${target} PRIVATE $<$<CONFIG:RelWithDebInfo,Release>:/GL>)
    endforeach()
    set_property(TARGET ps2_runtime ps2_iop APPEND PROPERTY STATIC_LIBRARY_OPTIONS $<$<CONFIG:RelWithDebInfo,Release>:/LTCG>)
    target_link_options(kamite PRIVATE $<$<CONFIG:RelWithDebInfo,Release>:/LTCG>)
endif()
