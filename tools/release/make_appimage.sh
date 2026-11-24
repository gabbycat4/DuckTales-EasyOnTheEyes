#!/usr/bin/env bash
# Builds DuckTales Recompiled as an AppImage on this PC, for testing.
#
#   bash tools/release/make_appimage.sh
#
# Writes DuckTalesRecompiled-x86_64.AppImage in the repository folder. It
# uses this system's SDL2 (not bundled), so it runs here and on systems as
# new as this one; the release workflow (.github/workflows/linux-appimage.yml)
# builds the one to share, on an older Ubuntu with its own SDL2.
# Needs: cmake, ninja, gcc, SDL2 development files, python 3.11+, curl, and
# your ROM in roms/ (the build generates the game from it; it is never put
# in the AppImage).
set -euo pipefail
cd "$(dirname "$0")/../.."
build=build-appimage
app="$build/AppDir"

cmake -S . -B "$build" -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build "$build" --parallel

rm -rf "$app"
mkdir -p "$app/usr/bin"
cp "$build/ducktales" "$app/usr/bin/"
strip "$app/usr/bin/ducktales"
bash tools/release/collect_licenses.sh "$app/usr/share/doc/ducktales" "$build"
cp packaging/linux/AppRun "$app/AppRun"
chmod +x "$app/AppRun"
cp packaging/linux/ducktales.desktop packaging/linux/ducktales.png "$app/"
ln -s ducktales.png "$app/.DirIcon"
echo "local-$(git describe --tags --always --dirty)-$(date +%s)" > "$app/VERSION"
if find "$app" -iname '*.nes' | grep -q .; then
    echo "A ROM is in the package; stopping." >&2
    exit 1
fi

tool="$build/appimagetool"
if [ ! -x "$tool" ]; then
    curl -fsSL -o "$tool" https://github.com/AppImage/appimagetool/releases/download/continuous/appimagetool-x86_64.AppImage
    chmod +x "$tool"
fi
ARCH=x86_64 "$tool" --appimage-extract-and-run "$app" DuckTalesRecompiled-x86_64.AppImage
echo "Built DuckTalesRecompiled-x86_64.AppImage"
