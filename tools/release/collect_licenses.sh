#!/usr/bin/env bash
# Copies this project's licence and the licences of everything compiled into
# the game (NESRecomp, Dear ImGui, formae-ui, stb, Lua, SDL2) into one folder
# for a release, as the Windows package's install rules do (CMakeLists.txt).
#   bash tools/release/collect_licenses.sh <dest> <build dir> [SDL2 licence file]
set -euo pipefail
dest="$1"
build="$2"
sdl="${3:-}"
mkdir -p "$dest"
cp LICENSE "$dest/DuckTalesRecompiled-LICENSE.txt"
cp external/nesrecomp/LICENSE "$dest/NESRecomp-LICENSE.txt"
cp external/nesrecomp/PERMISSIONS.md "$dest/NESRecomp-PERMISSIONS.md"
cp external/nesrecomp/runner/cyc/vendor/emu2413/LICENSE "$dest/emu2413-LICENSE.txt" 2>/dev/null || true
cp external/imgui/LICENSE.txt "$dest/imgui-LICENSE.txt"
cp external/formae-ui/LICENSE "$dest/formae-ui-LICENSE.txt"
cp external/stb/LICENSE "$dest/stb-LICENSE.txt"
cp "$build/lua-LICENSE.txt" "$dest/lua-LICENSE.txt"
if [ -n "$sdl" ] && [ -f "$sdl" ]; then
    cp "$sdl" "$dest/SDL2-LICENSE.txt"
fi
