cmake_minimum_required(VERSION 3.20)
project(DuckTalesPC C CXX)

set(DUCKTALES_ROM_SHA256_EXPECTED "97d472f7b2c1088448d1644155b2650a546bad887b3cba571e16d7986824c169")
set(DUCKTALES_ROM "" CACHE FILEPATH
    "DuckTales (USA) iNES image supplied by the user, any file name; empty = the file in roms/ with its SHA-256")
option(DUCKTALES_HEADLESS "Build the headless verification host only (no SDL window/audio)" OFF)
option(DUCKTALES_ORACLE "Also build the TriCNES reference machine used by differential tests" OFF)
option(DUCKTALES_DEV_DIAGNOSTICS "Developer diagnostics (typed state log, TCP commands); never in distribution builds" OFF)
option(DUCKTALES_NATIVE "Also build ducktales_native, the native-runtime prototype (docs/native-runtime.md)" OFF)
# Which window executable is ducktales.exe, the game as players run it
# (docs/native-runtime.md "The player build"). Window builds make both: the
# other one is ducktales_cycle.exe (native player, the default) or
# ducktales_player.exe (cycle).
set(DUCKTALES_PLAYER "native" CACHE STRING "The runtime ducktales.exe runs: native or cycle")
set_property(CACHE DUCKTALES_PLAYER PROPERTY STRINGS native cycle)
if(NOT DUCKTALES_PLAYER MATCHES "^(native|cycle)$")
    message(FATAL_ERROR "DUCKTALES_PLAYER must be native or cycle, not ${DUCKTALES_PLAYER}")
endif()

set(NESRECOMP_ROOT "${CMAKE_SOURCE_DIR}/external/nesrecomp")
if(NOT EXISTS "${NESRECOMP_ROOT}/runner/cyc/project.cmake")
    message(FATAL_ERROR "external/nesrecomp is missing; run: git submodule update --init external/nesrecomp")
endif()
set(_ducktales_rom "${DUCKTALES_ROM}")
if(NOT _ducktales_rom)
    file(GLOB _ducktales_rom_candidates LIST_DIRECTORIES false "${CMAKE_SOURCE_DIR}/roms/*")
    foreach(_candidate IN LISTS _ducktales_rom_candidates)
        file(SIZE "${_candidate}" _candidate_size)
        if(_candidate_size EQUAL 131088)
            file(SHA256 "${_candidate}" _candidate_sha256)
            if(_candidate_sha256 STREQUAL DUCKTALES_ROM_SHA256_EXPECTED)
                set(_ducktales_rom "${_candidate}")
                break()
            endif()
        endif()
    endforeach()
    if(NOT _ducktales_rom)
        message(FATAL_ERROR "No DuckTales (USA) ROM in roms/. Copy yours there under any name (baserom.nes, for "
            "example) or pass -DDUCKTALES_ROM=<path>; it is found by its SHA-256, ${DUCKTALES_ROM_SHA256_EXPECTED}.")
    endif()
endif()
if(NOT EXISTS "${_ducktales_rom}")
    message(FATAL_ERROR "ROM not found: ${_ducktales_rom}")
endif()
file(SHA256 "${_ducktales_rom}" _ducktales_rom_sha256)
if(NOT _ducktales_rom_sha256 STREQUAL DUCKTALES_ROM_SHA256_EXPECTED)
    message(FATAL_ERROR "${_ducktales_rom} is not DuckTales (USA): its SHA-256 is ${_ducktales_rom_sha256}, "
        "the supported revision's is ${DUCKTALES_ROM_SHA256_EXPECTED} (docs/rom.md).")
endif()
message(STATUS "DuckTales ROM: ${_ducktales_rom}")

# Players need no Visual C++ redistributable: the window build links the static
# MSVC runtime (/MT). Verification builds keep CMake's default.
if(MSVC AND NOT DUCKTALES_HEADLESS)
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

include("${NESRECOMP_ROOT}/runner/cyc/project.cmake")

set(_ducktales_options PLAYERS 1)
if(DUCKTALES_HEADLESS)
    list(APPEND _ducktales_options HEADLESS)
endif()
# Mods (docs/mods.md): replacement tiles, palettes, music and effects from
# .dtmod archives. They decode PNG, OGG Vorbis and zip deflate with stb
# (external/stb, MIT or public domain), compiled into src/mods/core/fm_codec.c only.
# Every host links them, the native player included.
set(STB_ROOT "${CMAKE_SOURCE_DIR}/external/stb")
if(NOT EXISTS "${STB_ROOT}/stb_vorbis.c")
    message(FATAL_ERROR "external/stb is missing; run: git submodule update --init external/stb")
endif()
set(_ducktales_mods
    "${CMAKE_SOURCE_DIR}/src/mods/core/fm_archive.c"
    "${CMAKE_SOURCE_DIR}/src/mods/core/fm_catalog.c"
    "${CMAKE_SOURCE_DIR}/src/mods/core/fm_codec.c"
    "${CMAKE_SOURCE_DIR}/src/mods/core/fm_script.c"
    "${CMAKE_SOURCE_DIR}/src/mods/nes/fm_nes.c"
    "${CMAKE_SOURCE_DIR}/src/mods/ducktales/dt_mods.c"
    "${CMAKE_SOURCE_DIR}/src/mods/ducktales/dt_mods_audio.c")
set_property(SOURCE "${CMAKE_SOURCE_DIR}/src/mods/core/fm_codec.c" APPEND PROPERTY INCLUDE_DIRECTORIES "${STB_ROOT}")
# Script mods (docs/mods.md "Scripts") run in Lua 5.4 (external/lua, the
# official repository at a v5.4 tag, MIT), built here as a static library of
# the core and the libraries the sandbox opens: the io, os, package and debug
# libraries, linit.c and the stand-alone programs are left out.
set(LUA_ROOT "${CMAKE_SOURCE_DIR}/external/lua")
if(NOT EXISTS "${LUA_ROOT}/lua.h")
    message(FATAL_ERROR "external/lua is missing; run: git submodule update --init external/lua")
endif()
set(_lua_sources lapi lcode lctype ldebug ldo ldump lfunc lgc llex lmem lobject lopcodes lparser lstate lstring
    ltable ltm lundump lvm lzio lauxlib lbaselib lcorolib lmathlib lstrlib ltablib lutf8lib)
list(TRANSFORM _lua_sources PREPEND "${LUA_ROOT}/")
list(TRANSFORM _lua_sources APPEND ".c")
add_library(ducktales_lua STATIC ${_lua_sources})
target_include_directories(ducktales_lua PUBLIC "${LUA_ROOT}")
set_target_properties(ducktales_lua PROPERTIES LINKER_LANGUAGE C)
set_property(SOURCE "${CMAKE_SOURCE_DIR}/src/mods/core/fm_script.c" APPEND PROPERTY INCLUDE_DIRECTORIES "${LUA_ROOT}")
# The licence the package carries: lua.h's own notice.
file(READ "${LUA_ROOT}/lua.h" _lua_header)
string(REGEX MATCH "\n\\* Copyright \\(C\\) 1994-[^\n]*\n(\\*[^\n]*\n)*" _lua_notice "${_lua_header}")
string(REGEX REPLACE "\n\\*+/?[ ]?" "\n" _lua_notice "${_lua_notice}")
string(STRIP "${_lua_notice}" _lua_notice)
string(APPEND _lua_notice "\n")
file(WRITE "${CMAKE_BINARY_DIR}/lua-LICENSE.txt" "${_lua_notice}")
# The port's systems layer (docs/architecture.md): typed game state, the hook
# sites it alters behaviour at, settings and options; the typed game API and
# its events (include/ducktales/) and the decompiled routines (src/decomp/,
# docs/decompilation.md). Both hosts link it.
set(_ducktales_extras
    "${CMAKE_SOURCE_DIR}/src/game/ducktales_state.c"
    "${CMAKE_SOURCE_DIR}/src/game/game_ram.c"
    "${CMAKE_SOURCE_DIR}/src/game/events.c"
    "${CMAKE_SOURCE_DIR}/src/game/ducktales_jump.c"
    "${CMAKE_SOURCE_DIR}/src/game/enhancements.c"
    "${CMAKE_SOURCE_DIR}/src/host/ducktales_sprites.c"
    "${CMAKE_SOURCE_DIR}/src/host/ducktales_host.c"
    "${CMAKE_SOURCE_DIR}/src/decomp/decomp.c"
    "${CMAKE_SOURCE_DIR}/src/decomp/motion.c"
    "${CMAKE_SOURCE_DIR}/src/decomp/sound.c"
    "${CMAKE_SOURCE_DIR}/src/decomp/player.c"
    "${CMAKE_SOURCE_DIR}/src/decomp/objcommon.c"
    "${CMAKE_SOURCE_DIR}/src/decomp/objects.c"
    "${CMAKE_SOURCE_DIR}/src/decomp/level.c"
    "${CMAKE_SOURCE_DIR}/src/decomp/collision.c"
    "${CMAKE_SOURCE_DIR}/src/decomp/flow.c"
    "${CMAKE_SOURCE_DIR}/src/host/ducktales_settings.c"
    "${CMAKE_SOURCE_DIR}/src/native/native_render.c"
    "${CMAKE_SOURCE_DIR}/src/native/native_cycle.c"
    ${_ducktales_mods})
# The native renderer checked against the PPU every frame (docs/native-render.md).
if(DUCKTALES_HEADLESS)
    list(APPEND _ducktales_extras "${CMAKE_SOURCE_DIR}/src/native/native_check.c")
endif()
# The application (docs/architecture.md "Application"): the
# program entry with the first-run ROM import, the in-game menu and live
# settings, drawn with formae-ui (external/formae-ui) on Dear ImGui
# (external/imgui) over the host's SDL_Renderer. Window builds only; the
# headless host never links it.
set(IMGUI_ROOT "${CMAKE_SOURCE_DIR}/external/imgui")
set(FORMAE_UI_ROOT "${CMAKE_SOURCE_DIR}/external/formae-ui")
if(NOT DUCKTALES_HEADLESS)
    if(NOT EXISTS "${IMGUI_ROOT}/imgui.h")
        message(FATAL_ERROR "external/imgui is missing; run: git submodule update --init external/imgui")
    endif()
    if(NOT EXISTS "${FORMAE_UI_ROOT}/CMakeLists.txt")
        message(FATAL_ERROR "external/formae-ui is missing; run: git submodule update --init external/formae-ui")
    endif()
    list(APPEND _ducktales_extras
        "${CMAKE_SOURCE_DIR}/src/host/ducktales_main.c"
        "${CMAKE_SOURCE_DIR}/src/ui/app_settings.c"
        "${CMAKE_SOURCE_DIR}/src/ui/app_present.c"
        "${CMAKE_SOURCE_DIR}/src/ui/app_rom.c"
        "${CMAKE_SOURCE_DIR}/src/ui/app_ui.cpp"
        "${IMGUI_ROOT}/backends/imgui_impl_sdl2.cpp"
        "${IMGUI_ROOT}/backends/imgui_impl_sdlrenderer2.cpp")
endif()
if(DUCKTALES_DEV_DIAGNOSTICS)
    list(APPEND _ducktales_extras "${CMAKE_SOURCE_DIR}/src/host/ducktales_diagnostics.c")
endif()
if(DUCKTALES_HEADLESS)
    list(APPEND _ducktales_extras "${CMAKE_SOURCE_DIR}/src/host/ducktales_ramhash.c"
        "${CMAKE_SOURCE_DIR}/src/ui/app_rom.c")
endif()
nesrecomp_add_cycle_game(ducktales
    ROM "${_ducktales_rom}"
    GAME_CONFIG "${CMAKE_SOURCE_DIR}/config/game.toml"
    ${_ducktales_options}
    HOST_EXTRAS ${_ducktales_extras})
target_include_directories(ducktales PRIVATE "${CMAKE_SOURCE_DIR}/src" "${CMAKE_SOURCE_DIR}/include")
target_link_libraries(ducktales PRIVATE ducktales_lua)
if(NOT DUCKTALES_HEADLESS)
    # The ROM identity the first-run import checks (docs/rom.md: SHA-256 of
    # the whole file), taken from the ROM this build was generated from.
    target_compile_features(ducktales PRIVATE cxx_std_17)
    target_compile_definitions(ducktales PRIVATE DUCKTALES_WINDOW=1
        "DUCKTALES_ROM_SHA256=\"${_ducktales_rom_sha256}\"")
    # One Dear ImGui for the program and formae-ui: its core is the imgui
    # target formae-ui links, the SDL backends are compiled into ducktales.
    add_library(imgui STATIC
        "${IMGUI_ROOT}/imgui.cpp"
        "${IMGUI_ROOT}/imgui_draw.cpp"
        "${IMGUI_ROOT}/imgui_tables.cpp"
        "${IMGUI_ROOT}/imgui_widgets.cpp")
    target_include_directories(imgui PUBLIC "${IMGUI_ROOT}" "${IMGUI_ROOT}/backends")
    target_compile_definitions(imgui PUBLIC IMGUI_DISABLE_OBSOLETE_FUNCTIONS)
    add_subdirectory("${FORMAE_UI_ROOT}" formae-ui)
    target_link_libraries(ducktales PRIVATE imgui formae::ui)
    # The About page (src/ui/app_ui.cpp): the version (git describe) and the
    # build date when CMake configures, and the Formae logo, embedded by
    # formae_ui_embed_file so the program needs no loose file.
    execute_process(COMMAND git describe --tags --always --dirty WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
        OUTPUT_VARIABLE _ducktales_version OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
    if(NOT _ducktales_version)
        set(_ducktales_version "unknown")
    endif()
    string(TIMESTAMP _ducktales_build_date "%Y-%m-%d")
    file(CONFIGURE OUTPUT "${CMAKE_BINARY_DIR}/app_about.c" CONTENT [[
/* Generated by CMakeLists.txt for the About page (src/ui/app_ui.cpp). */
const char app_about_version[] = "@_ducktales_version@";
const char app_about_build_date[] = "@_ducktales_build_date@";
]] @ONLY)
    target_sources(ducktales PRIVATE "${CMAKE_BINARY_DIR}/app_about.c")
    formae_ui_embed_file(ducktales app_about_logo_png "${CMAKE_SOURCE_DIR}/assets/brand/formae.png")
    # The program entry (src/host/ducktales_main.c) runs before NESRecomp's
    # own main, which is compiled under another name for this target only;
    # NESRecomp's sources stay unmodified.
    set_property(SOURCE "${NESRECOMP_ROOT}/runner/cyc/cyc_host.c" APPEND PROPERTY COMPILE_DEFINITIONS
        "$<$<STREQUAL:$<TARGET_PROPERTY:NAME>,ducktales>:main=ducktales_host_main>")
    if(WIN32)
        target_link_libraries(ducktales PRIVATE comdlg32 shell32)
        # The executable's icon; SDL gives every window the module's first icon.
        set(DUCKTALES_ICON "${CMAKE_SOURCE_DIR}/assets/icon/ducktales.ico")
        configure_file("${CMAKE_SOURCE_DIR}/assets/icon/ducktales.rc.in" "${CMAKE_BINARY_DIR}/ducktales.rc" @ONLY)
        target_sources(ducktales PRIVATE "${CMAKE_BINARY_DIR}/ducktales.rc")
        # A Windows program, not a console one: no terminal opens beside the
        # window. main() stays the entry; src/host/ducktales_main.c attaches
        # to the parent's console when started from one.
        set_target_properties(ducktales PROPERTIES WIN32_EXECUTABLE ON)
        if(MSVC)
            target_link_options(ducktales PRIVATE /ENTRY:mainCRTStartup)
        endif()
    endif()
endif()
if(DUCKTALES_DEV_DIAGNOSTICS)
    target_compile_definitions(ducktales PRIVATE DUCKTALES_DEV_DIAGNOSTICS=1)
endif()
if(DUCKTALES_HEADLESS)
    target_compile_definitions(ducktales PRIVATE DUCKTALES_NATIVE_CHECK=1)
endif()

# Unit test of the typed state decoder (pure C, no machine).
if(DUCKTALES_HEADLESS)
    add_executable(ducktales_state_test
        "${CMAKE_SOURCE_DIR}/tests/state_decode_test.c"
        "${CMAKE_SOURCE_DIR}/src/game/ducktales_state.c")
    target_include_directories(ducktales_state_test PRIVATE "${CMAKE_SOURCE_DIR}/src")
    # Unit test of the ROM identity check and import (no window).
    add_executable(app_rom_test
        "${CMAKE_SOURCE_DIR}/tests/app_rom_test.c"
        "${CMAKE_SOURCE_DIR}/src/ui/app_rom.c")
    target_include_directories(app_rom_test PRIVATE "${CMAKE_SOURCE_DIR}/src")
    target_compile_definitions(app_rom_test PRIVATE _CRT_SECURE_NO_WARNINGS)
endif()

# The native runtime (docs/native-runtime.md): the same generated C as the
# ducktales target, linked against src/native/ instead of NESRecomp's cycle
# host, PPU and APU. Only the CPU's instruction sequences (cpu6502.c), its
# instruction templates for uncompiled code (cpu6502_interp.c) and the hook
# registry (mod_function_hooks.c) come from NESRecomp's machine side.
set(_nes_cyc "${NESRECOMP_ROOT}/runner/cyc")
set(_native_runtime
    "${CMAKE_SOURCE_DIR}/src/native/native_bus.c"
    "${CMAKE_SOURCE_DIR}/src/native/native_video.c"
    "${CMAKE_SOURCE_DIR}/src/native/native_audio.c"
    "${CMAKE_SOURCE_DIR}/src/native/native_voices.c"
    "${CMAKE_SOURCE_DIR}/src/native/native_hooks.c"
    "${CMAKE_SOURCE_DIR}/src/native/native_route.c"
    "${_nes_cyc}/cpu6502.c"
    "${_nes_cyc}/cpu6502_interp.c"
    "${NESRECOMP_ROOT}/runner/src/mod_function_hooks.c")
function(ducktales_native_target target)
    include("${CMAKE_CURRENT_BINARY_DIR}/cycle-ducktales/sources.cmake")
    target_sources(${target} PRIVATE ${_native_runtime} ${CYC_PROJECT_SOURCES})
    target_include_directories(${target} PRIVATE "${CMAKE_SOURCE_DIR}/src" "${CMAKE_SOURCE_DIR}/include" "${_nes_cyc}"
        "${NESRECOMP_ROOT}/runner/include" "${NESRECOMP_ROOT}")
    target_compile_definitions(${target} PRIVATE NESRECOMP_NATIVE_INTERVALS=0
        NESRECOMP_HEADROOM_COUNTERS=0 NESRECOMP_NATIVE_PRG_GUARDS=0 _CRT_SECURE_NO_WARNINGS)
    target_compile_features(${target} PRIVATE c_std_11)
    if(MSVC)
        target_compile_options(${target} PRIVATE /utf-8)
    endif()
endfunction()

# ducktales_native: the headless prototype (routes, logs, dumps).
if(DUCKTALES_NATIVE)
    add_executable(ducktales_native "${CMAKE_SOURCE_DIR}/src/native/native_main.c")
    ducktales_native_target(ducktales_native)
endif()

# The player build on the native runtime (src/native/native_window.c): the
# window, menu, settings, presentation, enhancements and sprite compositor of
# the cycle build over native_run_frame(), the native renderer and the native
# synth (native_audio.c). NESRecomp contributes its settings file, input bindings, PNG writer
# and toast font, no part of its cycle host.
if(NOT DUCKTALES_HEADLESS)
    add_executable(ducktales_player
        "${CMAKE_SOURCE_DIR}/src/native/native_window.c"
        "${CMAKE_SOURCE_DIR}/src/native/native_present.c"
        "${CMAKE_SOURCE_DIR}/src/native/native_smooth.c"
        "${CMAKE_SOURCE_DIR}/src/native/native_render.c"
        "${CMAKE_SOURCE_DIR}/src/native/native_cycle.c"
        "${CMAKE_SOURCE_DIR}/src/game/ducktales_state.c"
        "${CMAKE_SOURCE_DIR}/src/game/game_ram.c"
        "${CMAKE_SOURCE_DIR}/src/game/events.c"
        "${CMAKE_SOURCE_DIR}/src/game/ducktales_jump.c"
        "${CMAKE_SOURCE_DIR}/src/game/enhancements.c"
        "${CMAKE_SOURCE_DIR}/src/host/ducktales_sprites.c"
        "${CMAKE_SOURCE_DIR}/src/host/ducktales_settings.c"
        "${CMAKE_SOURCE_DIR}/src/host/ducktales_main.c"
        "${CMAKE_SOURCE_DIR}/src/ui/app_settings.c"
        "${CMAKE_SOURCE_DIR}/src/ui/app_present.c"
        "${CMAKE_SOURCE_DIR}/src/ui/app_rom.c"
        "${CMAKE_SOURCE_DIR}/src/ui/app_ui.cpp"
        ${_ducktales_mods}
        "${IMGUI_ROOT}/backends/imgui_impl_sdl2.cpp"
        "${IMGUI_ROOT}/backends/imgui_impl_sdlrenderer2.cpp"
        "${_nes_cyc}/cyc_settings.c"
        "${_nes_cyc}/cyc_input.c"
        "${_nes_cyc}/cyc_png.c"
        "${_nes_cyc}/cyc_overlay.c"
        "${CMAKE_BINARY_DIR}/app_about.c")
    ducktales_native_target(ducktales_player)
    target_compile_features(ducktales_player PRIVATE cxx_std_17)
    target_compile_definitions(ducktales_player PRIVATE DUCKTALES_WINDOW=1 DUCKTALES_NATIVE_PLAYER=1 CYC_GAME_PLAYERS=1 NESRECOMP_GAME_PLAYERS=1
        "DUCKTALES_ROM_SHA256=\"${_ducktales_rom_sha256}\"")
    target_link_libraries(ducktales_player PRIVATE imgui formae::ui SDL2::SDL2 ducktales_lua)
    if(UNIX)
        target_link_libraries(ducktales_player PRIVATE m)
    endif()
    formae_ui_embed_file(ducktales_player app_about_logo_png "${CMAKE_SOURCE_DIR}/assets/brand/formae.png")
    if(WIN32)
        target_link_libraries(ducktales_player PRIVATE comdlg32 shell32 avrt winmm)
        target_sources(ducktales_player PRIVATE "${CMAKE_BINARY_DIR}/ducktales.rc")
        set_target_properties(ducktales_player PROPERTIES WIN32_EXECUTABLE ON)
        if(MSVC)
            target_link_options(ducktales_player PRIVATE /ENTRY:mainCRTStartup)
        endif()
        add_custom_command(TARGET ducktales_player POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_FILE:SDL2::SDL2> $<TARGET_FILE_DIR:ducktales_player>)
    endif()
    if(DUCKTALES_PLAYER STREQUAL "native")
        set_target_properties(ducktales PROPERTIES OUTPUT_NAME ducktales_cycle)
        set_target_properties(ducktales_player PROPERTIES OUTPUT_NAME ducktales)
        set(_ducktales_player_target ducktales_player)
    else()
        set(_ducktales_player_target ducktales)
    endif()
endif()

if(DUCKTALES_ORACLE)
    set(NESRECOMP_CYC_DIR "${NESRECOMP_ROOT}/runner/cyc")
    nesrecomp_cyc_add_oracle(ducktales_oracle)
endif()

# The release package (CONTRIBUTING.md "Releasing"): `cpack -C Release` in
# build/windows writes DuckTalesRecompiled-<version>-win64.zip with the
# executable, SDL2, the player README and every licence the binaries carry
# (docs/licensing.md). No ROM, generated source or test tool is installed.
if(NOT DUCKTALES_HEADLESS)
    configure_file("${CMAKE_SOURCE_DIR}/packaging/README.txt.in" "${CMAKE_BINARY_DIR}/package/README.txt" @ONLY NEWLINE_STYLE CRLF)
    install(TARGETS ${_ducktales_player_target} RUNTIME DESTINATION .)
    install(FILES "$<TARGET_FILE:SDL2::SDL2>" "${CMAKE_BINARY_DIR}/package/README.txt" DESTINATION .)
    install(FILES "${CMAKE_SOURCE_DIR}/LICENSE" DESTINATION . RENAME LICENSE.txt)
    install(FILES "${NESRECOMP_ROOT}/LICENSE" DESTINATION licenses RENAME NESRecomp-LICENSE.txt)
    install(FILES "${NESRECOMP_ROOT}/PERMISSIONS.md" DESTINATION licenses RENAME NESRecomp-PERMISSIONS.md)
    install(FILES "${NESRECOMP_ROOT}/runner/cyc/vendor/emu2413/LICENSE" DESTINATION licenses RENAME emu2413-LICENSE.txt)
    install(FILES "${NESRECOMP_ROOT}/runner/external/SDL2/COPYING.txt" DESTINATION licenses RENAME SDL2-COPYING.txt)
    install(FILES "${NESRECOMP_ROOT}/runner/external/SDL2/README-SDL.txt" DESTINATION licenses RENAME SDL2-README-SDL.txt)
    install(FILES "${IMGUI_ROOT}/LICENSE.txt" DESTINATION licenses RENAME imgui-LICENSE.txt)
    install(FILES "${FORMAE_UI_ROOT}/LICENSE" DESTINATION licenses RENAME formae-ui-LICENSE.txt)
    install(FILES "${STB_ROOT}/LICENSE" DESTINATION licenses RENAME stb-LICENSE.txt)
    install(FILES "${CMAKE_BINARY_DIR}/lua-LICENSE.txt" DESTINATION licenses)
    set(CPACK_GENERATOR ZIP)
    set(CPACK_PACKAGE_NAME DuckTalesRecompiled)
    set(CPACK_PACKAGE_VERSION "${_ducktales_version}")
    set(CPACK_PACKAGE_FILE_NAME "DuckTalesRecompiled-${_ducktales_version}-win64")
    set(CPACK_INCLUDE_TOPLEVEL_DIRECTORY ON)
    include(CPack)
endif()
