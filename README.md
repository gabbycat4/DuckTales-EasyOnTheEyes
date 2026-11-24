<div align="center">

<img src="assets/icon/ducktales-128.png" alt="DuckTales Recompiled" width="128" height="128">

# DuckTales Recompiled for Linux

An unofficial fork of [formae-group/ducktales-recompiled](https://github.com/formae-group/ducktales-recompiled)
that adds a **Linux build (AppImage)**, **up to 240 pictures a second** and **smooth scrolling**.

</div>

> **All credit for the port itself goes to the original authors.** This fork only adds the
> Linux build and the motion options below; everything else is their work, described in their
> README further down. Please report problems with the Linux build or the smooth scrolling
> here, not to them.

## Play on Linux

**You need your own DuckTales (USA) ROM** ([which one](#supported-rom)). The download
contains nothing from the game.

1. Download `DuckTalesRecompiled-x86_64.AppImage` from the [Releases](../../releases) page.
2. Make it executable: right-click it → Properties → Permissions → *Allow executing file as
   program*. Or in a terminal: `chmod +x DuckTalesRecompiled-x86_64.AppImage`
3. Double-click it. On the first start, choose your ROM file (or drop it on the window).

It runs on distributions from about 2022 on (glibc 2.35 or newer: Ubuntu 22.04, Fedora 36,
Debian 12, Arch, CachyOS, Linux Mint 21 and newer). Your settings, save states and the checked
copy of your ROM are kept in `~/.local/share/DuckTales Recompiled/`.

### Recommended settings for a fast monitor

Escape (or RB on a controller) opens the menu. In **Settings → Graphics**:

| Setting | Recommended | What it does |
|---|---|---|
| Pictures per second | Your monitor's refresh rate: 120, 180 or 240 | Draws in-between pictures. At 240 the game runs at 59.94 frames a second (0.26% slower, not noticeable) so the pictures fit a 240 Hz monitor exactly |
| Smooth scrolling | Calm | Eases the camera: the level glides instead of stepping 1 and 2 pixels, starting and stopping at once. Scrooge, enemies and items glide too. Calmer and Slowest are gentler. Needs Pixels: Nearest and Pixel aspect: Square |
| Screen changes | Soft | The screen moves up or down to the next area with a gentle start and stop (Even: one steady speed) |
| Frame pacing | Auto or Steady | Steady holds every picture to an even rhythm without VSync |

With a FreeSync or G-Sync monitor, also turn on your desktop's adaptive sync (KDE Plasma:
System Settings → Display → Adaptive sync: *Always*).

All of these change only the picture: the game itself runs exactly as the original. Set
Smooth scrolling to Off and Pictures per second to 60 for the original look.

### Build it yourself on Linux

Needs: git, cmake, ninja, gcc/g++, Python 3.11+, SDL2 development files
(Arch/CachyOS: `sudo pacman -S --needed cmake ninja sdl2-compat`; Ubuntu/Debian:
`sudo apt install cmake ninja-build g++ libsdl2-dev python3`). Optional: `kdialog` or `zenity`
for the ROM file chooser.

```sh
git clone --recurse-submodules https://github.com/gabbycat4/DuckTalesRecomp.git
cd DuckTalesRecomp
cp /path/to/your/DuckTales.nes roms/baserom.nes
cmake -S . -B build-play -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build-play --parallel
./build-play/ducktales
```

`bash tools/release/make_appimage.sh` packs your build into an AppImage. For developers:
`CYC_PACING_LOG=pacing.log ./build-play/ducktales` writes when every picture was drawn and
shown, and how long each part of a frame took.

### What this fork changes

- **Linux:** the mod loader's folders and paths, case-insensitive names, the save-state
  folder, a KDE/GNOME ROM chooser and menu fonts on other distributions. Windows code is
  unchanged.
- **A faster picture drawer** (`src/native/native_render.c`): the same pixels, about 2.6
  times faster, checked on 12,000 random frames (`tools/render-test/render_equal.c`).
- **Pictures per second** 60-240 and 59.94 frames a second at 240 (`src/native/native_window.c`).
- **Smooth scrolling** (`src/native/native_smooth.c`): the HUD, the level, Scrooge and every
  enemy and item are drawn as separate layers that the graphics card places between pixels.
  The camera is eased the way gabbycat4's other NES ports do it (a moving average with a spring); Scrooge
  and the objects are followed by their slots in the game's own object table, so their glide
  never confuses one object with another. Tested offline on the game's own Amazon route:
  `tools/smooth-test/`.

---

<div align="center">

*The original project's README follows.*

# DuckTales Recompiled

A native Windows port of *DuckTales* (NES, Capcom, 1989), built by static recompilation.

[![CI](https://github.com/formae-group/ducktales-recompiled/actions/workflows/ci.yml/badge.svg)](https://github.com/formae-group/ducktales-recompiled/actions/workflows/ci.yml)
[![latest release](https://img.shields.io/github/v/release/formae-group/ducktales-recompiled)](https://github.com/formae-group/ducktales-recompiled/releases)
[![license: MIT](https://img.shields.io/badge/license-MIT-blue.svg)](LICENSE)
[![platform: Windows x64](https://img.shields.io/badge/platform-Windows%20x64-0078D6.svg)](#building)

</div>

DuckTales Recompiled translates the game's 6502 program into C at build time and compiles it into a
native executable. The game logic runs as native code on a small native runtime that gives it the
bus it talks to and draws its picture from the video state it writes, with no PPU emulation; the
verification builds run the same code on the cycle-exact [NESRecomp](https://github.com/mstan/nesrecomp)
host, and a port layer adds the menu, settings, presentation and enhancements on top. Every build is
checked frame by frame against an independent reference machine.

The repository contains no game data. You supply your own ROM, and the build generates all
ROM-derived code locally.

## Features

- **Native execution.** 100% of the CPU cycles executed across the test routes run in recompiled
  code; the interpreter remains only as a fallback for code no route has reached.
- **Verified against a reference.** Recompiled, interpreted and TriCNES runs must produce identical
  per-frame state on every route at all four CPU/PPU alignments.
- **No sprite flicker.** Every sprite is drawn on every frame in a fixed order, with no 8-per-line or
  64-sprite limit, and the HUD stays visible. Presentation only; the machine is untouched.
- **Native runtime.** The game runs on a native bus and renderer instead of an emulated PPU, and its
  presented picture is checked pixel for pixel against the cycle-exact build. Its sound comes from a
  native synth of the APU, compared with the cycle build's APU on every route
  ([docs/validation.md](docs/validation.md#native-synth)).
- **No slowdown.** VBlank waits until the frame's logic is done, removing the console's lag frames
  without changing game logic.
- **Modern presentation.** Direct3D 11, per-monitor DPI awareness, refresh-aware frame pacing,
  sharp scaling, 8:7 pixel aspect and overscan crop.
- **Enhancements.** Optional gameplay changes, all off by default; when off, the original code runs.
- **Input.** Keyboard, mouse and SDL game controllers, fully rebindable.
- **Mods.** `.dtmod` archives and loose folders replace tiles, palettes, music and sound effects,
  and Lua 5.4 scripts react to the game's events in a sandbox. Off by default and ordered in the
  Mod Menu; asset mods never touch the machine. See [docs/mods.md](docs/mods.md).
- **A start on decompilation.** A typed C API over the game's RAM, game events for mods, and
  C versions of Scrooge's per-frame routines and parts of the game loop and HUD. The cycle host can
  compare them with the original (`--decomp check`) or run them in its place
  (`--decomp replace`). Off by default.

## Supported ROM

Only one revision is supported. It is identified by its hashes, never by its file name, so name the
file as you like. It is verified when imported and again at every start; any other dump, revision or
modified image is refused.

| Field | Value |
|---|---|
| File | Any name (No-Intro: `DuckTales (USA).nes`), 131,088 bytes, iNES 1.0, mapper 2 (UxROM), 128 KiB PRG, 8 KiB CHR-RAM |
| SHA-256 | `97d472f7b2c1088448d1644155b2650a546bad887b3cba571e16d7986824c169` |
| SHA-1 | `f83191749d019d75eb5ff95d247ef25882c2571a` |
| MD5 | `ee0812f62f6595e45812a8b26bbb637e` |
| CRC32 | `06B994B2` |
| CRC32 (without header) | `EFB09075` |

Further detail on the header and board behaviour is in [docs/rom.md](docs/rom.md).

## Building

Requirements: Windows x64, Visual Studio 2022 (*Desktop development with C++*), CMake 3.20+,
Python 3.11+ and Git.

```powershell
git clone --recurse-submodules https://github.com/formae-group/ducktales-recompiled.git
cd ducktales-recompiled
copy "C:\path\to\your ROM.nes" roms\baserom.nes
cmake -S . -B build/windows -G "Visual Studio 17 2022" -A x64
cmake --build build/windows --config Release --parallel
```

The build takes the file in `roms\` whose SHA-256 matches the table above, whatever its name
(`baserom.nes` is only the name these docs use), or the file `-DDUCKTALES_ROM=<path>` names; a ROM
with another hash stops the configure step with both hashes.

The executable is written to `build\windows\Release\ducktales.exe`. Code generation runs at configure
time and again whenever the ROM, `config/` or the recompiler changes; the generated sources live under
`build/*/cycle-ducktales/` and are never committed. The window build links the static MSVC runtime,
and `cmake --build build/windows --config Release --target PACKAGE` writes the player package
`DuckTalesRecompiled-<version>-win64.zip` (see [CONTRIBUTING.md](CONTRIBUTING.md#releasing)).

| CMake option | Effect |
|---|---|
| `-DDUCKTALES_ROM=<path>` | Use a ROM outside `roms/` |
| `-DDUCKTALES_HEADLESS=ON` | Windowless build for verification |
| `-DDUCKTALES_ORACLE=ON` | Also build the TriCNES reference machine (`ducktales_oracle.exe`) |
| `-DDUCKTALES_DEV_DIAGNOSTICS=ON` | Typed state log, scheduled changes, TCP commands; development only |
| `-DNESRECOMP_DEV_UI=ON` | Developer keys and native-coverage display |
| `-DDUCKTALES_PLAYER=native\|cycle` | Runtime of `ducktales.exe` in a window build: native (default; the cycle host build is `ducktales_cycle.exe`) or cycle |

## Running

On first launch, select your ROM (any file name) or drop it on the window. The file's SHA-256 is
checked against the table above, and the verified copy is stored in `%APPDATA%\DuckTales Recompiled\`
for later launches. Headless import: `ducktales.exe --import-rom <path>` (exit code 0 on success,
1 when refused). `DUCKTALES_DATA_DIR` overrides the data folder.

| Action | Keyboard | Controller |
|---|---|---|
| D-pad | Arrow keys | D-pad |
| A / B | Z / X | A / X |
| Start / Select | Return / Backslash | Start / Back |
| Menu | Escape | RB |
| Fullscreen | F11, Alt+Enter | |
| Fast-forward (hold) | Tab | RT |
| Save / load state | F8 / F9 | |

Settings are saved to `config.ini` next to the executable (`--config FILE` selects another file).

### Menu

Escape or RB pauses the game and opens the menu; Escape, B, RB or the Resume button closes it. The bar
at the top holds the **Settings**, **Enhancements** and **About** tabs, and Reset, Quit (each asks
first) and Resume on the right. Each tab lists its categories on the left and the chosen one's page
on the right. Everything works with the mouse, the keyboard (arrows, Tab, Enter, Space) and a
controller (D-pad, A, B); changes apply at once and are saved to `config.ini`. On the Controls page,
choose a cell and press the new key or button: Escape cancels and Delete clears.

| Tab, category | Setting | Values | Key |
|---|---|---|---|
| Settings, General | Menu background opacity | 0% to 100% (80%) | `[Game] MenuOpacity` |
| | Controller navigation | On (default), off | `[Game] MenuControllerNav` |
| | Pause game while menu is open | Off (default), on | `[Game] MenuPausesGame` |
| Settings, Graphics | Display mode | Windowed, borderless, exclusive fullscreen | `[Display] Fullscreen` |
| | Window size | 1x to 8x | `[Display] WindowScale` |
| | Integer scaling | On, off | `[Display] IntegerScale` |
| | Pixels | Nearest, sharp, smooth | `[Game] Pixels` |
| | Pixel aspect | Square, 8:7 | `[Game] PixelAspect` |
| | Crop overscan | Off, top and bottom, all edges | `[Game] Overscan` |
| | HUD split line | Clean, original | `[Game] HudSplit` |
| | Frame pacing | Auto, VSync, steady | `[Game] FramePacing` |
| | No sprite flicker | On (default), off | `[Game] NoSpriteFlicker` |
| Settings, Audio | Master volume | 0% to 100% | `[Audio] Volume` |
| | Keep music under sound effects (native player only) | On, off (default) | `[Audio] MusicUnderEffects` |
| | Overlapping sound effects (native player only) | On, off (default) | `[Audio] OverlapEffects` |
| Settings, Controls | Player 1 keyboard and controller bindings, Menu shortcut | | `[Keyboard.*]`, `[Gamepad.*]` |
| Settings, Gameplay | No slowdown | On (default), off | `[Game] NoSlowdown` |
| Settings, Mods | Enable mods, the mod list (order and switches), Open mods folder, Rescan | Off (default), on | `[Mods] Enabled`, `[Mods] Order` |
| Enhancements, Gameplay | The enhancements below | | `[Game]` |
| Enhancements, Restore defaults | Restore the original game | | |

The two sound options bring back what the console's four channels drop: the music part a sound
effect takes over (39 of the 40 effects take pulse 2 and/or noise), and an effect the sound driver
ignores or cuts off for a newer one. They play on extra channels of the native player's synth and
change nothing in the game; `ducktales_cycle.exe` keeps them in `config.ini` but plays the console's
sound only ([docs/audio.md](docs/audio.md#extra-voices)).

Frame pacing applies at the next launch: *Auto* uses VSync on displays up to 70 Hz and a steady
present hold above that. Measurements are in [docs/presentation.md](docs/presentation.md). The About
tab shows the version, the build date and the credits.

### Enhancements

| Enhancement | Key | Option | Values |
|---|---|---|---|
| Jump height | `JumpScale` | `--jump-scale N` | 25 to 400 percent (100 = original) |
| No damage | `NoDamage` | `--no-damage on` | Hits knock Scrooge back but cost no health |
| Infinite time | `InfiniteTime` | `--infinite-time on` | The level timer stops |
| Infinite lives | `InfiniteLives` | `--infinite-lives on` | Losing a life costs none |
| Extra lives at start | `StartLives` | `--start-lives N` | 0 to 9, or off (original: 2) |

Each enhancement hooks a single, documented site in the original code, guarded by a content key
(address, length and CRC-32) so it can never fire on unexpected code.

## How it works

1. **Recompilation.** NESRecomp's compiler reads the ROM, starts from the vectors, the functions
   declared in `config/game.toml` and the entry points recorded in `config/ducktales.seeds`, and
   emits one C function per 1 KiB chunk of each 4 KiB bank view. Each instruction performs its CPU cycles through the runtime, so timing stays
   cycle-exact with the PPU and APU.
2. **Runtime.** The game (`ducktales.exe`) runs the generated code on the native runtime
   (`src/native`): CPU RAM, the UxROM banks, controllers, sprite DMA and the PPU registers and
   memories, with VBlank, the NMI and the sprite-0 hit placed from the cycle count; the native renderer
   draws each frame from what the game wrote. The verification builds and `ducktales_cycle.exe` link
   the same code against NESRecomp's cycle host (PPU, APU, mapper). Code that no seed covers runs on
   the interpreter and is reported, so it can be added to the seeds. See
   [native-runtime.md](docs/native-runtime.md).
3. **Port layer.** `src/game` (typed game state, revision-specific symbols, enhancement hooks),
   `src/host` (settings, options, the sprite compositor, diagnostics) and `src/ui` (the menu, built with
   [formae-ui](https://github.com/formae-group/formae-ui) on Dear ImGui,
   ROM import, presentation) attach to the host through its public extension points. The NESRecomp
   submodule is never modified.
4. **Presentation.** The window shows a picture composed from the machine's output: all sprites
   without hardware limits, the cleaned HUD split, the chosen scaling and pacing. The machine's own
   picture, used for verification and save states, is unchanged.

## Verification

```powershell
cmake -S . -B build/headless -G "Visual Studio 17 2022" -A x64 -DDUCKTALES_HEADLESS=ON -DDUCKTALES_ORACLE=ON
cmake --build build/headless --config Release --parallel
py -3 -I tests/verify_routes.py --build build/headless
```

Each route in `tests/routes/` is a deterministic input schedule from power-on. It covers the five
lands, played to their LAND CLEAR screens, plus intro and mapping routes. Every route runs three
ways at four CPU/PPU clock alignments:

| Run | Purpose |
|---|---|
| Recompiled | The build under test |
| Interpreter only | Same host, no native code |
| TriCNES oracle | Independent reference machine |

Per-frame hashes cover the bus trace, CPU RAM, nametables, OAM, palette, CHR-RAM, the picture and
the CPU registers, and must be identical on every frame. The oracle's hashes are cached in
`build/verify-cache/`, keyed on the oracle binary, ROM, route, frame count and alignment
(`--no-cache` reruns it).

| Test | Checks |
|---|---|
| `tests/verify_routes.py` | Recompiled, interpreter and oracle equality on every route |
| `tests/quick_verify.py` | The fast tier: affected routes at alignment 0 against golden hashes |
| `tests/sprites_test.py` | Sprite presentation versus the machine picture |
| `tests/enhancements_test.py` | Each enhancement's effect, and identity with the original when off |
| `tests/decomp_test.py` | Decompiled routines against the original on every call, and the routes with them running in its place |
| `tests/window_smoke.py` | Start-up, ROM import and refusal, settings persistence, every menu tab and category |
| `tests/package_test.py` | The release ZIP's contents, and the game starting from it unpacked |

New routes can reach code no route has executed; append its interpreter misses to the seeds and
rebuild:

```powershell
build\headless\Release\ducktales.exe roms\baserom.nes --input tests\routes\NEW.txt --frames N --miss-log config\ducktales.seeds
```

## Documentation

| Document | Contents |
|---|---|
| [architecture.md](docs/architecture.md) | Port layer, hooks, enhancements, application |
| [validation.md](docs/validation.md) | Routes, coverage and verification results |
| [rendering.md](docs/rendering.md) | Screen data, sprite builder, flicker analysis |
| [presentation.md](docs/presentation.md) | Pacing, DPI, scaling and picture measurements |
| [native-runtime.md](docs/native-runtime.md) | Native runtime, the player build and lag analysis |
| [decompilation.md](docs/decompilation.md) | Game API, events and the decompiled routines |
| [mods.md](docs/mods.md) | Using and making mods: tiles, palettes, sounds and Lua scripts |
| [memory-map.md](docs/memory-map.md), [functions.md](docs/functions.md) | RAM and routine reference |
| [player.md](docs/player.md), [game-state.md](docs/game-state.md), [levels.md](docs/levels.md), [audio.md](docs/audio.md) | Reverse-engineering notes |

## License

The code in this repository is MIT licensed ([LICENSE](LICENSE)). NESRecomp is licensed under
PolyForm Noncommercial 1.0.0 with additional permissions, so builds of this port must be distributed
free of charge. [formae-ui](https://github.com/formae-group/formae-ui) (MIT, Formae's menu library),
Dear ImGui, SDL2, stb and Lua are used under their own licenses. See
[docs/licensing.md](docs/licensing.md).

*DuckTales* is a trademark of Disney. This project is not affiliated with Disney or Capcom and
distributes no part of the game.
