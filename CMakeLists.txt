cmake_minimum_required(VERSION 3.20)
project(Mother3Recomp C CXX)

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(GBARECOMP_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/gbarecomp"
    CACHE PATH "Path to this game's gbarecomp worktree")
set(RECOMP_UI_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/recomp-ui"
    CACHE PATH "Path to the shared recomp-ui checkout")
set(GBARECOMP_RUNTIME_UI_ROOT "${RECOMP_UI_ROOT}"
    CACHE PATH "recomp-ui root used by the live GBA runtime adapter")
if(NOT EXISTS "${GBARECOMP_ROOT}/CMakeLists.txt")
    message(FATAL_ERROR
        "GBARECOMP_ROOT is not a gbarecomp checkout:\n"
        "  ${GBARECOMP_ROOT}\n"
        "Configure with -DGBARECOMP_ROOT=<path> if the local worktree link is absent.")
endif()

if(WIN32)
    # Launcher, runtime UI and bezel must share one SDL instance.
    set(GBARECOMP_STATIC_RELEASE OFF CACHE BOOL "Use the bundled shared SDL2 backend" FORCE)
endif()
if(ANDROID)
    set(FETCHCONTENT_UPDATES_DISCONNECTED ON CACHE BOOL "" FORCE)
endif()
add_subdirectory("${GBARECOMP_ROOT}" gbarecomp_build EXCLUDE_FROM_ALL)
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/generated/recompiled.cpp")
    message(FATAL_ERROR
        "Stale monolithic generated/recompiled.cpp found. "
        "Run tools/regen.ps1 with the current gbarecomp worktree.")
endif()
set(MOTHER3_GENERATED_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/generated" CACHE PATH "Private regenerated cart sources")
file(GLOB GAME_GENERATED_SOURCES CONFIGURE_DEPENDS
    "${MOTHER3_GENERATED_ROOT}/*.c"
    "${MOTHER3_GENERATED_ROOT}/*.cpp")

if(NOT EXISTS "${MOTHER3_GENERATED_ROOT}/dispatch_table.cpp")
    list(APPEND GAME_GENERATED_SOURCES src/empty_dispatch_table.cpp)
endif()

set(MOTHER3_SOURCES
    src/main.cpp
    src/mother3_runtime_hooks.cpp
    src/m3_assets.cpp
    src/m3_launcher.cpp
    src/m3_settings.cpp
    src/m3_setup.cpp
    src/m3_smooth.cpp
    src/m3_prompts.cpp
    src/m3_bezel.cpp
    src/m3_mods.cpp
    src/m3_widescreen.cpp
    ${GAME_GENERATED_SOURCES})
if(ANDROID)
    # android/ builds this through gbarecomp's Android shell, which loads libmain.so.
    set(MOTHER3_TARGET main)
    add_library(main SHARED ${MOTHER3_SOURCES})
else()
    set(MOTHER3_TARGET Mother3Recomp)
    add_executable(Mother3Recomp ${MOTHER3_SOURCES})
endif()

# A native game corpus without the recompiled BIOS runs the IRQ/SWI handlers on
# the interpreter bridge, which breaks audio pacing on every platform. Fail the
# game target only, so gba_recompile can still be built to generate the BIOS.
if(EXISTS "${MOTHER3_GENERATED_ROOT}/dispatch_table.cpp" AND
   NOT EXISTS "${GBARECOMP_ROOT}/src/runtime/generated_bios/bios_recompiled.cpp")
    add_custom_target(mother3_require_native_bios
        COMMAND "${CMAKE_COMMAND}" -E echo
            "Recompiled BIOS missing. Run: gba_recompile --bios <gba_bios.bin> --config gbarecomp/bios/gba_bios.toml --out gbarecomp/src/runtime/generated_bios, then re-run cmake."
        COMMAND "${CMAKE_COMMAND}" -E false)
    add_dependencies(${MOTHER3_TARGET} mother3_require_native_bios)
endif()

# Embed only the HQ mixer code; the ROM itself is patched in memory after the
# user's selected language patch has been applied.
file(READ "${CMAKE_CURRENT_SOURCE_DIR}/mods/High-Quality Sound Mixing/source/m4a_hq_mixer_no_fix.bin" HQ_AUDIO_MIXER_HEX HEX)
string(REGEX REPLACE "(..)" "0x\\1," HQ_AUDIO_MIXER_BYTES "${HQ_AUDIO_MIXER_HEX}")
file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/generated")
file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/generated/hq_audio_mixer_data.h"
    "#pragma once\n#include <cstdint>\nstatic constexpr std::uint8_t kHqAudioMixer[] = {${HQ_AUDIO_MIXER_BYTES}};\n")
target_include_directories(${MOTHER3_TARGET} PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/generated")

set(GAME_RECOMPILED_SOURCES ${GAME_GENERATED_SOURCES})
list(FILTER GAME_RECOMPILED_SOURCES INCLUDE REGEX
    "/recompiled_[0-9]+\\.cpp$")
if(GAME_RECOMPILED_SOURCES)
    if(MSVC)
        set_source_files_properties(${GAME_RECOMPILED_SOURCES} PROPERTIES
            COMPILE_OPTIONS "/O1;/EHs-c-;/GR-")
    else()
        set_source_files_properties(${GAME_RECOMPILED_SOURCES} PROPERTIES
            COMPILE_OPTIONS "-O1;-g0;-fno-exceptions;-fno-rtti;-fno-strict-aliasing;-fno-math-errno")
    endif()
endif()

target_include_directories(${MOTHER3_TARGET} PRIVATE
    "${GBARECOMP_ROOT}/src/armv4t"
    "${GBARECOMP_ROOT}/src/gba"
    "${GBARECOMP_ROOT}/src/runtime"
    "${GBARECOMP_ROOT}/src/debug")

target_link_libraries(${MOTHER3_TARGET} PRIVATE
    "-Wl,--start-group"
    gbarecomp_runtime
    gbarecomp_debug
    gbarecomp_gba
    gbarecomp_armv4t
    "-Wl,--end-group")

target_compile_definitions(${MOTHER3_TARGET} PRIVATE
    GBARECOMP_DEFAULT_DEBUG_PORT=19893
    GBARECOMP_WINDOW_TITLE="Mother3Recomp"
    GBARECOMP_DEFAULT_GAME_CONFIG="game.toml")

option(GBAGAME_RECOMP_UI "Build the recomp-ui launcher and in-game menu" ON)
if(GBAGAME_RECOMP_UI)
    if(NOT EXISTS "${RECOMP_UI_ROOT}/recomp_ui.cmake")
        message(FATAL_ERROR
            "recomp-ui is not available at ${RECOMP_UI_ROOT}. "
            "Run git submodule update --init --recursive or configure with "
            "-DRECOMP_UI_ROOT=<path>.")
    endif()
    include("${RECOMP_UI_ROOT}/recomp_ui.cmake")
    add_library(game_launcher_ui STATIC src/game_launcher_boot.cpp)
    target_include_directories(game_launcher_ui PRIVATE
        "${GBARECOMP_ROOT}/src/runtime"
        "${GBARECOMP_ROOT}/src/gba"
        src)
    recomp_target_launcher_ui(game_launcher_ui CONSOLE gba)
    if(ANDROID)
        # Linked into libmain.so; the Android shell already defines SDL2::SDL2.
        set_target_properties(game_launcher_ui PROPERTIES POSITION_INDEPENDENT_CODE ON)
    else()
        find_package(SDL2 REQUIRED)
    endif()
    target_link_libraries(game_launcher_ui PUBLIC SDL2::SDL2)
    target_link_libraries(${MOTHER3_TARGET} PRIVATE game_launcher_ui)
    set_property(SOURCE src/main.cpp APPEND PROPERTY
        COMPILE_DEFINITIONS GBAGAME_RECOMP_UI)
    # The game-owned launcher writes gamepad binds through recomp-ui's input.ini store.
    set_property(SOURCE src/m3_launcher.cpp src/m3_prompts.cpp src/m3_mods.cpp src/m3_setup.cpp APPEND PROPERTY
        COMPILE_DEFINITIONS GBAGAME_RECOMP_UI)
    target_include_directories(${MOTHER3_TARGET} PRIVATE
        "${RECOMP_UI_ROOT}/src/common" "${RECOMP_UI_ROOT}/src/consoles/gba")
endif()

if(MSVC)
    target_link_options(${MOTHER3_TARGET} PRIVATE /STACK:268435456)
elseif(MINGW OR WIN32)
    target_link_options(${MOTHER3_TARGET} PRIVATE -Wl,--stack,268435456)
endif()

if(WIN32)
    enable_language(RC)
    file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/mother3_icon.rc"
        "1 ICON \"${CMAKE_CURRENT_SOURCE_DIR}/Mother 3.ico\"\n"
        "1 24 \"${CMAKE_CURRENT_SOURCE_DIR}/src/mother3.manifest\"\n")
    target_sources(${MOTHER3_TARGET} PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/mother3_icon.rc")
    add_library(mother3_bezel SHARED interposer/mother3_bezel.c)
    set_target_properties(mother3_bezel PROPERTIES PREFIX "")
    target_include_directories(mother3_bezel PRIVATE
        "${GBARECOMP_ROOT}/platform/android/third_party/SDL/include"
        "${GBARECOMP_ROOT}/platform/android/third_party/SDL/src/dynapi")
    add_dependencies(${MOTHER3_TARGET} mother3_bezel)
endif()

include(CTest)
if(BUILD_TESTING AND GBAGAME_RECOMP_UI AND NOT ANDROID)
    add_executable(host_menu_test tests/host_menu_test.cpp
        "${GBARECOMP_ROOT}/tools/bios_smoke/dispatch_stub.cpp")
    target_link_gbarecomp_runtime_stack(host_menu_test)
    target_link_libraries(host_menu_test PRIVATE game_launcher_ui)
    target_compile_options(host_menu_test PRIVATE -UNDEBUG)
    add_test(NAME host_menu_test COMMAND host_menu_test)
    set_tests_properties(host_menu_test PROPERTIES ENVIRONMENT "SDL_VIDEODRIVER=dummy;SDL_AUDIODRIVER=dummy")
endif()

if(WIN32 AND BUILD_TESTING)
    add_executable(sdl_hook_test tests/sdl_hook_test.c)
    target_link_libraries(sdl_hook_test PRIVATE SDL2::SDL2)
    target_compile_options(sdl_hook_test PRIVATE -UNDEBUG)
    add_dependencies(sdl_hook_test mother3_bezel)
    add_test(NAME sdl_hook_test COMMAND sdl_hook_test)
    set_tests_properties(sdl_hook_test PROPERTIES ENVIRONMENT
        "SDL_DYNAMIC_API=$<TARGET_FILE:mother3_bezel>;SDL_VIDEODRIVER=dummy;SDL_AUDIODRIVER=dummy")
endif()
