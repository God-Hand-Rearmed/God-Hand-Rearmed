# Game layer (GHR.exe) and the recompiled game code. Adapted from the Urban Reign port's src/CMakeLists.txt.

# KAMITE_ARCH: instruction set the compiler may use in the generated code (game code + VU1 AOT, the bulk of the build).
# SSE41 (default): one exe for every CPU with SSE4.1 (MSVC has no /arch:SSE4.2, so its own code stays SSE2 and the
# runtime's explicit SSE4.1 intrinsics still apply). AVX2: only to compare speed; the exe then needs an AVX2 CPU.
set(KAMITE_ARCH "SSE41" CACHE STRING "Instruction set of the generated code: SSE41 (default) or AVX2 (comparison only)")
set_property(CACHE KAMITE_ARCH PROPERTY STRINGS SSE41 AVX2)
if(KAMITE_ARCH STREQUAL "AVX2")
    set(KAMITE_ARCH_FLAG /arch:AVX2)
else()
    set(KAMITE_ARCH_FLAG "")
endif()

# Where build.py generate writes the recompiled game code. A cache path so a disc-free build (CI, tools/linux/
# stub_game_code.py) can point it at a stand-in that has the same entry points and no game code.
set(KAMITE_GENERATED_DIR "${CMAKE_SOURCE_DIR}/generated" CACHE PATH "The recompiled game code (build.py generate)")
set(KAMITE_RUNTIME_INCLUDE "${CMAKE_SOURCE_DIR}/runtime/ps2xRuntime/include")

if(NOT EXISTS "${KAMITE_GENERATED_DIR}/register_functions.cpp")
    if(KAMITE_BUILD_RECOMP)
        # Configure for building ps2_recomp only (build.py generate on a fresh tree): no kamite target yet.
        message(WARNING
            "Recompiled game code not found in ${KAMITE_GENERATED_DIR}; configuring without the kamite target.\n"
            "Build ps2_recomp, then run: python build.py generate")
        return()
    endif()
    message(FATAL_ERROR
        "Recompiled game code not found in ${KAMITE_GENERATED_DIR}.\n"
        "Put your own disc image (USA, SLUS-21503) in game/ and run: python build.py generate")
endif()

file(GLOB KAMITE_GENERATED_SOURCES CONFIGURE_DEPENDS "${KAMITE_GENERATED_DIR}/*.cpp")

set(KAMITE_HOST_SOURCES
    main.cpp
    kamite/button_prompt_sheets.cpp
    kamite/button_prompts.cpp
    kamite/crate_prompts.cpp
    kamite/call_profile.cpp
    kamite/guest_symbols.cpp
    kamite/call_trace.cpp
    kamite/movie_watchdog.cpp
    kamite/frame_patches.cpp
    kamite/frame_wait.cpp
    kamite/game_options.cpp
    kamite/character_mods.cpp
    kamite/builtin_characters.cpp
    kamite/character_build.cpp
    kamite/disc_files.cpp
    kamite/file_layer.cpp
    kamite/xram_heap.cpp
    kamite/hd_textures.cpp
    kamite/prompt_hd.cpp
    kamite/style_presets.cpp
    kamite/style_sfx.cpp
    kamite/style_pose.cpp
    kamite/buck_breaker.cpp
    kamite/punish_fit.cpp
    kamite/texture_slots.cpp
    kamite/model_detail.cpp
    kamite/body_detail.cpp
    kamite/body_detail_build.cpp
    kamite/olivia_model.cpp
    kamite/olivia_player.cpp
    kamite/olivia_player_build.cpp
    kamite/jukebox.cpp
    kamite/widescreen.cpp
    kamite/camera.cpp
    kamite/npc_poses.cpp
    kamite/mot_edit.cpp
    kamite/sprint_traversal.cpp
    kamite/debug_tools.cpp
    kamite/npc_tools.cpp
    kamite/hitbox_overlay.cpp
    kamite/menu_buttons.cpp
    kamite/mod_list.cpp
    kamite/adx_codec.cpp
    kamite/afs_archive.cpp
    kamite/mod_audio.cpp
    kamite/mods.cpp
    kamite/modern_controls.cpp
    kamite/near_geometry.cpp
    kamite/render/character_skin.cpp
    kamite/render/clip_scenery.cpp
    kamite/render/scenery_strip.cpp
    kamite/render/vu1_programs.cpp
    kamite/native/native.cpp
    kamite/native/shadow.cpp
    kamite/native/libvu0.cpp
    kamite/native/packet.cpp
    kamite/native/recording.cpp
    kamite/native/chain_digest.cpp
    kamite/crash_history.cpp
    kamite/hud_layout.cpp
    kamite/hotkeys.cpp
    kamite/dev_menu.cpp
    kamite/roulette_bar.cpp
    kamite/radar_glow.cpp
    kamite/tutorial_text.cpp
    kamite/options_overlay.cpp
    kamite/options_screen.cpp
    kamite/options_pages.cpp
    kamite/keys_page.cpp
    kamite/mapping_page.cpp
    kamite/settings_table.cpp
    kamite/presets.cpp
    kamite/ini_file.cpp
    kamite/counter_finder.cpp
    kamite/test_mode.cpp
    kamite/message_guard.cpp
    kamite/bone_tweaks.cpp
    kamite/pc_sampler.cpp
    kamite/function_hooks.cpp
    kamite/game_disc.cpp
    kamite/hang_dump.cpp
    kamite/hle_bindings.cpp
    kamite/host_window.cpp
    kamite/log.cpp
    kamite/prompt_art.cpp
    kamite/sha256.cpp
    kamite/thread_policy.cpp
    kamite/paths.cpp
    kamite/steam_deck.cpp
    kamite/crash_posix.cpp
)

# GHR.ini (top level) is built into GHR.exe as its default settings: kamite_generated/kamite/default_ini.h.
# The header is rewritten only when the text changes, so editing GHR.ini rebuilds main.cpp alone.
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${CMAKE_SOURCE_DIR}/GHR.ini")
file(READ "${CMAKE_SOURCE_DIR}/GHR.ini" KAMITE_DEFAULT_INI)
# A release's built-in default: what a player needs (the template's defaults are the developers' hidden test runs).
# Make Release.cmd ships the same values in the zip's GHR.ini.
if(KAMITE_RELEASE)
    foreach(KAMITE_INI_PAIR "Headless=0" "TimeLimitSeconds=0" "DumpOnTimeLimit=0" "Renderer=pcsx2" "LogFile=logs/kamite.log")
        string(REGEX REPLACE "=.*" "" KAMITE_INI_KEY "${KAMITE_INI_PAIR}")
        string(REGEX REPLACE "\n${KAMITE_INI_KEY}=[^\n]*" "\n${KAMITE_INI_PAIR}" KAMITE_DEFAULT_INI "${KAMITE_DEFAULT_INI}")
    endforeach()
endif()
string(LENGTH "${KAMITE_DEFAULT_INI}" KAMITE_DEFAULT_INI_LENGTH)
# MSVC caps one string literal at 16380 bytes and a concatenation of them at 65535: the text goes in as adjacent raw
# literals of at most 8000 bytes, each cut after a line break.
if(KAMITE_DEFAULT_INI_LENGTH GREATER 60000)
    message(FATAL_ERROR "GHR.ini is ${KAMITE_DEFAULT_INI_LENGTH} bytes; MSVC string literals stop at 65535 in all")
endif()
if(KAMITE_DEFAULT_INI MATCHES "\\)KMINI\"")
    message(FATAL_ERROR "GHR.ini must not contain )KMINI\" (the raw string delimiter)")
endif()
set(KAMITE_INI_REST "${KAMITE_DEFAULT_INI}")
set(KAMITE_INI_PIECES "")
while(NOT "${KAMITE_INI_REST}" STREQUAL "")
    string(LENGTH "${KAMITE_INI_REST}" KAMITE_INI_REST_LENGTH)
    if(KAMITE_INI_REST_LENGTH GREATER 8000)
        string(SUBSTRING "${KAMITE_INI_REST}" 0 8000 KAMITE_INI_PIECE)
        string(FIND "${KAMITE_INI_PIECE}" "\n" KAMITE_INI_CUT REVERSE)
        if(KAMITE_INI_CUT LESS 0)
            message(FATAL_ERROR "GHR.ini has a line over 8000 bytes")
        endif()
        math(EXPR KAMITE_INI_CUT "${KAMITE_INI_CUT} + 1")
        string(SUBSTRING "${KAMITE_INI_REST}" 0 ${KAMITE_INI_CUT} KAMITE_INI_PIECE)
        string(SUBSTRING "${KAMITE_INI_REST}" ${KAMITE_INI_CUT} -1 KAMITE_INI_REST)
    else()
        set(KAMITE_INI_PIECE "${KAMITE_INI_REST}")
        set(KAMITE_INI_REST "")
    endif()
    string(APPEND KAMITE_INI_PIECES "\n        R\"KMINI(${KAMITE_INI_PIECE})KMINI\"")
endwhile()
set(KAMITE_INI_HEADER "${CMAKE_CURRENT_BINARY_DIR}/kamite_generated/kamite/default_ini.h")
file(WRITE "${KAMITE_INI_HEADER}.tmp"
    "// Generated from GHR.ini by src/CMakeLists.txt. Do not edit.\n"
    "#pragma once\n"
    "namespace kamite\n"
    "{\n"
    "    inline constexpr const char *kDefaultIni =${KAMITE_INI_PIECES};\n"
    "}\n")
file(COPY_FILE "${KAMITE_INI_HEADER}.tmp" "${KAMITE_INI_HEADER}" ONLY_IF_DIFFERENT)

# Does the runtime have ps2_host_display::setHeadless (LightVelox/PS2Recomp sotc-port 29d26f9)? The Urban Reign runtime
# (2fa53c4) does not; kamite/host_window.cpp then hides the raylib window itself.
file(STRINGS "${KAMITE_RUNTIME_INCLUDE}/runtime/ps2_display_options.h" KAMITE_SET_HEADLESS_DECL REGEX "void[ \t]+setHeadless[ \t]*\\(")
if(KAMITE_SET_HEADLESS_DECL)
    set(KAMITE_RUNTIME_HAS_SET_HEADLESS 1)
else()
    set(KAMITE_RUNTIME_HAS_SET_HEADLESS 0)
endif()
message(STATUS "Kamite: runtime setHeadless available: ${KAMITE_RUNTIME_HAS_SET_HEADLESS}")

# Reuse an already-built game code library when only host sources change (the full EE build is long).
set(KAMITE_GAME_CODE_LIBRARY "" CACHE FILEPATH "Reuse an unchanged generated EE library for host-only builds")
if(KAMITE_GAME_CODE_LIBRARY)
    if(NOT EXISTS "${KAMITE_GAME_CODE_LIBRARY}")
        message(FATAL_ERROR "Cached generated EE library does not exist: ${KAMITE_GAME_CODE_LIBRARY}")
    endif()
    add_library(kamite_game_code STATIC IMPORTED)
    set_target_properties(kamite_game_code PROPERTIES IMPORTED_LOCATION "${KAMITE_GAME_CODE_LIBRARY}")
    target_include_directories(kamite_game_code INTERFACE "${KAMITE_GENERATED_DIR}")
    target_link_libraries(kamite_game_code INTERFACE ps2_runtime)
    # The huge-function sources live in a sibling library built next to it.
    get_filename_component(KAMITE_GAME_CODE_LIBRARY_DIR "${KAMITE_GAME_CODE_LIBRARY}" DIRECTORY)
    target_link_libraries(kamite_game_code INTERFACE "${KAMITE_GAME_CODE_LIBRARY_DIR}/kamite_game_code_huge.lib")
else()
    # /d2OptimizeHugeFunctions forces full optimization of functions MSVC would otherwise skip. Fine for most files, but
    # one huge function (Urban Reign: ~11k guest instructions, 2 MB of C++) ran over 40 minutes with it. Files above the
    # threshold go into kamite_game_code_huge, which keeps MSVC's default huge-function handling.
    set(KAMITE_HUGE_SOURCE_BYTES 900000 CACHE STRING "Generated files larger than this skip /d2OptimizeHugeFunctions")
    set(KAMITE_HUGE_SOURCES "")
    foreach(source IN LISTS KAMITE_GENERATED_SOURCES)
        file(SIZE "${source}" sourceBytes)
        if(sourceBytes GREATER KAMITE_HUGE_SOURCE_BYTES AND NOT source MATCHES "register_functions\\.cpp$")
            list(APPEND KAMITE_HUGE_SOURCES "${source}")
        endif()
    endforeach()
    if(KAMITE_HUGE_SOURCES)
        list(REMOVE_ITEM KAMITE_GENERATED_SOURCES ${KAMITE_HUGE_SOURCES})
    endif()
    list(LENGTH KAMITE_HUGE_SOURCES KAMITE_HUGE_COUNT)
    message(STATUS "Kamite: ${KAMITE_HUGE_COUNT} generated files over ${KAMITE_HUGE_SOURCE_BYTES} bytes go to kamite_game_code_huge")

    # An empty static library is an error in some generators: a stub source keeps it valid when no file is huge.
    set(KAMITE_HUGE_STUB "${CMAKE_CURRENT_BINARY_DIR}/kamite_generated/huge_stub.cpp")
    file(WRITE "${KAMITE_HUGE_STUB}.tmp" "// Keeps kamite_game_code_huge non-empty.\nnamespace kamite { int hugeStub() { return 0; } }\n")
    file(COPY_FILE "${KAMITE_HUGE_STUB}.tmp" "${KAMITE_HUGE_STUB}" ONLY_IF_DIFFERENT)
    add_library(kamite_game_code_huge STATIC ${KAMITE_HUGE_SOURCES} "${KAMITE_HUGE_STUB}")
    target_include_directories(kamite_game_code_huge PUBLIC "${KAMITE_GENERATED_DIR}")
    target_link_libraries(kamite_game_code_huge PUBLIC ps2_runtime)
    # Each huge file needs several GB in MSVC's pass 2; compiling them all at once runs out of heap (C1002).
    set(KAMITE_HUGE_JOBS 2 CACHE STRING "Concurrent compiles of the huge generated sources (Ninja)")
    set_property(GLOBAL APPEND PROPERTY JOB_POOLS kamite_huge=${KAMITE_HUGE_JOBS})
    set_property(TARGET kamite_game_code_huge PROPERTY JOB_POOL_COMPILE kamite_huge)
    if(MSVC)
        target_compile_options(kamite_game_code_huge PRIVATE /bigobj /fp:precise ${KAMITE_ARCH_FLAG} /GS- /W0)
        # Huge files that still compile in reasonable time with /d2OptimizeHugeFunctions (file names, ';'-separated).
        # Urban Reign listed three of its own here; the game's are not measured yet (TODO after the first build).
        set(KAMITE_HUGE_OPTIMIZED "" CACHE STRING "Huge generated files still compiled with /d2OptimizeHugeFunctions")
        foreach(source IN LISTS KAMITE_HUGE_SOURCES)
            get_filename_component(sourceName "${source}" NAME)
            if(sourceName IN_LIST KAMITE_HUGE_OPTIMIZED)
                set_source_files_properties("${source}" PROPERTIES COMPILE_OPTIONS "/d2OptimizeHugeFunctions")
            endif()
        endforeach()
    endif()

    add_library(kamite_game_code STATIC ${KAMITE_GENERATED_SOURCES})
    target_link_libraries(kamite_game_code PUBLIC kamite_game_code_huge)
    target_include_directories(kamite_game_code PUBLIC "${KAMITE_GENERATED_DIR}")
    target_link_libraries(kamite_game_code PUBLIC ps2_runtime)
    set_target_properties(kamite_game_code PROPERTIES UNITY_BUILD OFF)
    target_precompile_headers(kamite_game_code PRIVATE
        "${KAMITE_RUNTIME_INCLUDE}/ps2_runtime_macros.h"
        "${KAMITE_RUNTIME_INCLUDE}/ps2_runtime.h"
        "${KAMITE_GENERATED_DIR}/ps2_recompiled_functions.h"
        "${KAMITE_GENERATED_DIR}/ps2_recompiled_stubs.h"
        "${KAMITE_RUNTIME_INCLUDE}/ps2_syscalls.h"
        "${KAMITE_RUNTIME_INCLUDE}/ps2_stubs.h"
        "${KAMITE_RUNTIME_INCLUDE}/ps2_cg.h" # register-locals generated code (build.py recomp_env)
    )
    if(MSVC)
        # No /GL here, ever: whole-program optimization over ~10k generated files blew up link time and memory (Urban
        # Reign docs/ROADMAP.md A4), and the game's own Release/LTCG link died in "Generating code" (STATUS_2026-10-16.md).
        target_compile_options(kamite_game_code PRIVATE /bigobj /fp:precise ${KAMITE_ARCH_FLAG} /GS- /d2OptimizeHugeFunctions /W0 /MP $<$<CONFIG:RelWithDebInfo>:/Ob2>)
        # One ~100k-line table initializer that runs once at startup: compiles in seconds without PCH/optimizer.
        set_source_files_properties("${KAMITE_GENERATED_DIR}/register_functions.cpp" PROPERTIES
            COMPILE_OPTIONS "/Od" SKIP_PRECOMPILE_HEADERS ON)
    endif()
endif()

# Recompiled SNR2 modules (the game's DLLs): 'python -I build.py modules <mod>' writes generated_modules/<mod>/ (the
# module's functions in namespace km_<mod>, its header <mod>_functions.h and <mod>_pack.cpp; never
# register_functions.cpp, which would define the boot table's globals a second time). All modules go into ONE static
# library with ONE runtime-only precompiled header: every room pack is generated for the same slot base (see
# KAMITE_PORT_NOTES.md section 9), so their address-suffixed names repeat and live in per-module namespaces, and a
# per-module PCH (~160 MB each) would cost ~13 GB for 81 modules. The module headers are not in the PCH (each source
# includes its own "<mod>_functions.h" from its folder). kamite_generated/kamite/module_index.cpp lists the packs
# (and keeps the linker from dropping them).
set(KAMITE_MODULES_DIR "${CMAKE_SOURCE_DIR}/generated_modules" CACHE PATH "The recompiled SNR2 modules (build.py modules)")
file(GLOB KAMITE_MODULE_PACKS CONFIGURE_DEPENDS "${KAMITE_MODULES_DIR}/*/*_pack.cpp")
set(KAMITE_MODULE_SOURCES "")
set(KAMITE_MODULE_DECLS "")
set(KAMITE_MODULE_REFS "")
set(KAMITE_MODULE_COUNT 0)
foreach(modulePack IN LISTS KAMITE_MODULE_PACKS)
    get_filename_component(moduleDir "${modulePack}" DIRECTORY)
    get_filename_component(moduleName "${moduleDir}" NAME)
    if(NOT moduleName MATCHES "^[A-Za-z_][A-Za-z0-9_]*$" OR NOT modulePack MATCHES "/${moduleName}_pack\\.cpp$")
        message(FATAL_ERROR "generated_modules/${moduleName}: module folder must be a C identifier holding ${moduleName}_pack.cpp")
    endif()
    if(EXISTS "${moduleDir}/register_functions.cpp")
        message(FATAL_ERROR "${moduleDir}/register_functions.cpp would redefine the boot function table (rerun build.py modules)")
    endif()
    if(EXISTS "${moduleDir}/ps2_recompiled_functions.h" OR NOT EXISTS "${moduleDir}/${moduleName}_functions.h")
        message(FATAL_ERROR "${moduleDir} has the old module layout (rerun: python -I build.py modules --force ${moduleName})")
    endif()
    file(GLOB moduleSources CONFIGURE_DEPENDS "${moduleDir}/*.cpp")
    list(APPEND KAMITE_MODULE_SOURCES ${moduleSources})
    string(APPEND KAMITE_MODULE_DECLS "    extern const ModulePack kModulePack_${moduleName};\n")
    string(APPEND KAMITE_MODULE_REFS "&kModulePack_${moduleName}, ")
    math(EXPR KAMITE_MODULE_COUNT "${KAMITE_MODULE_COUNT} + 1")
endforeach()
set(KAMITE_MODULE_LIBRARIES "")
if(KAMITE_MODULE_SOURCES)
    add_library(kamite_modules STATIC ${KAMITE_MODULE_SOURCES})
    target_include_directories(kamite_modules PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
    target_link_libraries(kamite_modules PUBLIC ps2_runtime)
    set_target_properties(kamite_modules PROPERTIES UNITY_BUILD OFF)
    target_precompile_headers(kamite_modules PRIVATE
        "${KAMITE_RUNTIME_INCLUDE}/ps2_runtime_macros.h"
        "${KAMITE_RUNTIME_INCLUDE}/ps2_runtime.h"
        "${KAMITE_RUNTIME_INCLUDE}/ps2_syscalls.h"
        "${KAMITE_RUNTIME_INCLUDE}/ps2_stubs.h"
        "${KAMITE_RUNTIME_INCLUDE}/ps2_cg.h" # register-locals generated code (build.py recomp_env)
    )
    if(MSVC)
        # Same flags as kamite_game_code (no /GL).
        target_compile_options(kamite_modules PRIVATE /bigobj /fp:precise ${KAMITE_ARCH_FLAG} /GS- /d2OptimizeHugeFunctions /W0 /MP $<$<CONFIG:RelWithDebInfo>:/Ob2>)
    endif()
    set(KAMITE_MODULE_LIBRARIES kamite_modules)
endif()
message(STATUS "Kamite: ${KAMITE_MODULE_COUNT} recompiled SNR2 module(s) in ${KAMITE_MODULES_DIR}")
set(KAMITE_MODULE_INDEX "${CMAKE_CURRENT_BINARY_DIR}/kamite_generated/kamite/module_index.cpp")
file(WRITE "${KAMITE_MODULE_INDEX}.tmp"
    "// Generated by src/CMakeLists.txt from generated_modules/*/*_pack.cpp. Do not edit.\n"
    "#include \"kamite/module_registry.h\"\n"
    "namespace kamite\n"
    "{\n"
    "${KAMITE_MODULE_DECLS}"
    "    const ModulePack *const *modulePacks(size_t &count)\n"
    "    {\n"
    "        static const ModulePack *const packs[] = {${KAMITE_MODULE_REFS}nullptr};\n"
    "        count = ${KAMITE_MODULE_COUNT}u;\n"
    "        return packs;\n"
    "    }\n"
    "}\n")
file(COPY_FILE "${KAMITE_MODULE_INDEX}.tmp" "${KAMITE_MODULE_INDEX}" ONLY_IF_DIFFERENT)

add_executable(kamite ${KAMITE_HOST_SOURCES} "${KAMITE_MODULE_INDEX}")
# The native VU1 programs (kamite/render) and native EE functions (kamite/native) round every float operation on its
# own, as the VU / EE does: no contraction into FMA (MSVC's /fp:precise below does not contract).
if(NOT MSVC)
    set_source_files_properties(kamite/render/character_skin.cpp kamite/render/clip_scenery.cpp kamite/render/scenery_strip.cpp
                                kamite/native/libvu0.cpp kamite/native/packet.cpp
                                PROPERTIES COMPILE_OPTIONS "-ffp-contract=off")
endif()
if(KAMITE_RELEASE) # CMakeLists.txt: GHR.exe's own code and the runtime, not the generated game code
    target_compile_definitions(kamite PRIVATE KAMITE_RELEASE=1)
    if(TARGET ps2_runtime)
        target_compile_definitions(ps2_runtime PRIVATE KAMITE_RELEASE=1)
    endif()
endif()
# The player-facing name: GHR.exe (the target, the code and the build folders stay "kamite"). On Windows the version
# resource (kamite/ghr.rc) gives Explorer and Task Manager the name "God Hand: Rearmed" and the icon.
set_target_properties(kamite PROPERTIES OUTPUT_NAME GHR)
if(WIN32)
    enable_language(RC)
    target_sources(kamite PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/kamite/ghr.rc")
    set_source_files_properties("${CMAKE_CURRENT_SOURCE_DIR}/kamite/ghr.rc" PROPERTIES
        OBJECT_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/kamite/ghr.ico")
endif()
# The checkout's commit for the test cases' meta (kamite_generated/kamite/git_commit.h), refreshed at every build.
set(KAMITE_COMMIT_HEADER "${CMAKE_CURRENT_BINARY_DIR}/kamite_generated/kamite/git_commit.h")
execute_process(COMMAND "${CMAKE_COMMAND}" -DSRC=${CMAKE_SOURCE_DIR} -DOUT=${KAMITE_COMMIT_HEADER}
    -P "${CMAKE_CURRENT_SOURCE_DIR}/git_commit.cmake")
add_custom_target(kamite_git_commit
    COMMAND "${CMAKE_COMMAND}" -DSRC=${CMAKE_SOURCE_DIR} -DOUT=${KAMITE_COMMIT_HEADER}
            -P "${CMAKE_CURRENT_SOURCE_DIR}/git_commit.cmake"
    BYPRODUCTS "${KAMITE_COMMIT_HEADER}"
    VERBATIM)
add_dependencies(kamite kamite_git_commit)
if(KAMITE_MODULE_LIBRARIES)
    target_link_libraries(kamite PRIVATE ${KAMITE_MODULE_LIBRARIES})
endif()
# The port's own resources (resources/images/logo.png: the launcher's logo, npc_motions, prompts) next to the exe,
# merged into the resources/ folder pcsx2gs.dll also fills (shaders, OSD font). A target of its own that runs at every
# build of kamite (the build script builds --target kamite): as a POST_BUILD step it ran only when GHR.exe was
# linked again, so a change to resources/ alone never reached the build (Oct 26: the counter idle takes).
# The built-in characters' own work (assets/characters/<id>: their nodes, textures, painted layers, character.ini) goes
# to resources/characters: GHR makes their player files from the player's disc at boot (builtin_characters.h).
# recomplauncher.json (tools/recomplauncher/gen_manifest.py) goes next to GHR.exe: a release zip carries it at the top,
# where RecompLauncher looks for it.
add_custom_target(kamite_resources
    COMMAND ${CMAKE_COMMAND} -E copy_directory "${CMAKE_SOURCE_DIR}/resources" "$<TARGET_FILE_DIR:kamite>/resources"
    COMMAND ${CMAKE_COMMAND} -E copy_directory "${CMAKE_SOURCE_DIR}/assets/characters" "$<TARGET_FILE_DIR:kamite>/resources/characters"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "${CMAKE_SOURCE_DIR}/recomplauncher.json" "$<TARGET_FILE_DIR:kamite>/recomplauncher.json"
    COMMENT "Copying the port's resources")
add_dependencies(kamite kamite_resources)
# A build on the stand-in game code (tools/linux/stub_game_code.py: CI, packaging tests) says so next to the binary,
# and tools/package.py refuses to make a release of it. A real build removes a marker a stub build left.
set(KAMITE_STUB_NOTE "${CMAKE_BINARY_DIR}/bin/STUB-GAME-CODE.txt")
if(EXISTS "${KAMITE_GENERATED_DIR}/KAMITE_STUB")
    message(STATUS "Kamite: building on the STAND-IN game code (${KAMITE_GENERATED_DIR}); this GHR cannot play the game")
    file(WRITE "${KAMITE_STUB_NOTE}" "Built on the stand-in game code (tools/linux/stub_game_code.py): not a playable build.\n")
else()
    file(REMOVE "${KAMITE_STUB_NOTE}")
endif()
target_include_directories(kamite PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}"
    "${CMAKE_CURRENT_BINARY_DIR}/kamite_generated"
    "${KAMITE_GENERATED_DIR}")
target_link_libraries(kamite PRIVATE kamite_game_code ps2_runtime)
target_compile_definitions(kamite PRIVATE KAMITE_RUNTIME_HAS_SET_HEADLESS=${KAMITE_RUNTIME_HAS_SET_HEADLESS})

if(MSVC)
    target_compile_options(kamite PRIVATE /fp:precise /W3 /utf-8)
    target_compile_definitions(kamite PRIVATE _CRT_SECURE_NO_WARNINGS KAMITE_ARCH_NAME="${KAMITE_ARCH}")
    # /FORCE:MULTIPLE: WinAPI vs raylib CloseWindow clash. Large stack: deep guest call chains run on the host stack.
    # /LTCG in RelWithDebInfo as Urban does: ps2_runtime is compiled with /GL by EnableFastReleaseMode
    # (runtime/ps2xRuntime/cmake/ReleaseMode.cmake, CMakeLists.txt:609-611), so the link does code generation for the
    # runtime objects only. The generated game code never gets /GL (above), so it is not part of that step.
    target_link_options(kamite PRIVATE /FORCE:MULTIPLE /STACK:16777216 $<$<CONFIG:RelWithDebInfo>:/LTCG>)
    target_link_libraries(kamite PRIVATE dbghelp winmm)
endif()

if(UNIX AND NOT APPLE AND NOT ANDROID)
    # Linux: libcurl is opened with dlopen (launcher_platform.cpp), which glibc before 2.34 keeps in libdl. The
    # shared libraries GHR ships (libSDL3.so.0) sit next to it: $ORIGIN finds them wherever the folder or the
    # AppImage is.
    target_link_libraries(kamite PRIVATE ${CMAKE_DL_LIBS})
    set_target_properties(kamite PROPERTIES BUILD_WITH_INSTALL_RPATH ON INSTALL_RPATH "\$ORIGIN")
endif()

# The launcher's saloon (resources/saloon, claude/saloon-launcher.md) is AVIF: libavif with dav1d to decode it, fetched
# at configure time like SDL3 (the libsdl-org forks SDL_image vendors; both build with CMake). dav1d builds C only, so no
# NASM or Meson is needed; KAMITE_DAV1D_ASM=ON adds its x86 assembly (faster, needs nasm on the PATH). Without it
# (KAMITE_SALOON=OFF) the launcher keeps its plain look.
option(KAMITE_SALOON "The launcher's saloon (libavif + dav1d, fetched at configure time)" ON)
option(KAMITE_DAV1D_ASM "dav1d with its NASM assembly (needs nasm)" OFF)
if(KAMITE_SALOON AND KAMITE_DEBUG_UI AND TARGET rlImGui)
    include(FetchContent)
    set(KAMITE_SHARED_LIBS_SAVED ${BUILD_SHARED_LIBS})
    set(BUILD_SHARED_LIBS OFF)
    set(DAV1D_ASM ${KAMITE_DAV1D_ASM} CACHE BOOL "" FORCE)
    set(DAV1D_BITDEPTHS "8" CACHE STRING "" FORCE)
    set(DAV1D_LOG OFF CACHE BOOL "" FORCE)
    FetchContent_Declare(kamite_dav1d
        GIT_REPOSITORY https://github.com/libsdl-org/dav1d.git
        GIT_TAG a6b395b472c2b1911a49936840e18535b071c1bd) # 1.2.1-SDL
    FetchContent_MakeAvailable(kamite_dav1d)
    set(AVIF_CODEC_DAV1D "SYSTEM" CACHE STRING "" FORCE) # the dav1d target above
    set(AVIF_CODEC_AOM "OFF" CACHE STRING "" FORCE)
    set(AVIF_CODEC_LIBGAV1 "OFF" CACHE STRING "" FORCE)
    set(AVIF_CODEC_RAV1E "OFF" CACHE STRING "" FORCE)
    set(AVIF_CODEC_SVT "OFF" CACHE STRING "" FORCE)
    set(AVIF_LIBYUV "OFF" CACHE STRING "" FORCE)
    set(AVIF_LIBSHARPYUV "OFF" CACHE STRING "" FORCE)
    set(AVIF_JPEG "OFF" CACHE STRING "" FORCE)
    set(AVIF_ZLIBPNG "OFF" CACHE STRING "" FORCE)
    set(AVIF_BUILD_APPS OFF CACHE BOOL "" FORCE)
    set(AVIF_BUILD_TESTS OFF CACHE BOOL "" FORCE)
    FetchContent_Declare(kamite_libavif
        GIT_REPOSITORY https://github.com/libsdl-org/libavif.git
        GIT_TAG ee829431ceaed31a34b7e1420f7f18180e7eae23) # v1.1.1-SDL
    FetchContent_MakeAvailable(kamite_libavif)
    set(BUILD_SHARED_LIBS ${KAMITE_SHARED_LIBS_SAVED})
    target_sources(kamite PRIVATE kamite/launcher_saloon.cpp kamite/launcher_paper.cpp kamite/saloon_image.cpp)
    target_link_libraries(kamite PRIVATE avif)
    target_compile_definitions(kamite PRIVATE KAMITE_HAS_AVIF=1)
elseif(KAMITE_DEBUG_UI AND TARGET rlImGui)
    # without the decoder the saloon reports itself unavailable (saloon_image.cpp) and the launcher stays plain
    target_sources(kamite PRIVATE kamite/launcher_saloon.cpp kamite/launcher_paper.cpp kamite/saloon_image.cpp)
endif()

if(KAMITE_DEBUG_UI AND TARGET rlImGui)
    target_sources(kamite PRIVATE "${CMAKE_SOURCE_DIR}/runtime/ps2xRuntime/src/lib/ps2_debug_panel.cpp")
    target_include_directories(kamite PRIVATE "${CMAKE_SOURCE_DIR}/runtime/ps2xRuntime/src/lib")
    target_link_libraries(kamite PRIVATE rlImGui imgui)
    target_compile_definitions(kamite PRIVATE PS2X_ENABLE_DEBUG_UI=1)
    # The launcher (src/kamite/launcher.cpp) draws with the same Dear ImGui.
    target_sources(kamite PRIVATE kamite/launcher.cpp kamite/launcher_bark.cpp kamite/launcher_book.cpp kamite/launcher_platform.cpp
                                  kamite/launcher_update.cpp kamite/update_archive.cpp kamite/mod_zip.cpp)
    target_compile_definitions(kamite PRIVATE KAMITE_HAS_LAUNCHER=1)
    if(WIN32)
        target_link_libraries(kamite PRIVATE winhttp) # downloads: updates, Add to Steam's art
    endif()
    # Add to Steam (src/kamite/launcher_steam.cpp): desktop only, left out of the Android build.
    if(KAMITE_STEAM_SHORTCUT)
        target_sources(kamite PRIVATE kamite/launcher_steam.cpp kamite/steam_shortcuts.cpp)
        target_compile_definitions(kamite PRIVATE KAMITE_STEAM_SHORTCUT=1)
    endif()
endif()

# FFmpeg is off by default (KAMITE_FFMPEG); with it on, the runtime stages its DLLs next to the exe.
if(COMMAND ps2x_stage_ffmpeg_runtime_dlls)
    ps2x_stage_ffmpeg_runtime_dlls(kamite)
endif()

# VU1 microcode compiled ahead of time: capture the code images with PS2X_VU1_CAPTURE, then run vu1_recomp (configure
# with -DKAMITE_BUILD_TOOLS=ON) into generated/vu1. Images not compiled stay interpreted. GENERIC (Urban, SotC).
file(GLOB KAMITE_VU1_SOURCES CONFIGURE_DEPENDS "${KAMITE_GENERATED_DIR}/vu1/*.cpp")
if(KAMITE_VU1_SOURCES)
    add_library(kamite_vu1_code STATIC ${KAMITE_VU1_SOURCES})
    target_include_directories(kamite_vu1_code PRIVATE "${CMAKE_SOURCE_DIR}/runtime/ps2xRuntime/src/lib/vu")
    target_link_libraries(kamite_vu1_code PUBLIC ps2_runtime)
    target_link_libraries(kamite PRIVATE kamite_vu1_code)
    target_compile_definitions(kamite PRIVATE KAMITE_HAS_VU1_PROGRAMS=1)
    if(MSVC)
        target_compile_options(kamite_vu1_code PRIVATE /bigobj /fp:precise ${KAMITE_ARCH_FLAG} /W0 /MP /d2OptimizeHugeFunctions $<$<CONFIG:RelWithDebInfo>:/Ob2>)
    endif()
endif()

set_target_properties(kamite PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
    VS_DEBUGGER_WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
)
