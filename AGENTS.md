# AGENTS.md

Instructions for coding agents working in this repository. Read [README.md](README.md),
[CONTRIBUTING.md](CONTRIBUTING.md) and the `docs/` page for the area you are changing before editing.

## Project

DuckTales Recompiled is a native Windows port of *DuckTales* (NES, USA) built by static recompilation
on NESRecomp. The ROM is translated to C at configure time; the port layer in `src/` adds the menu,
settings, presentation and enhancements. The goal is a polished PC port, not an emulator.

## Hard rules

- Never modify `external/nesrecomp`, `external/imgui` or `external/formae-ui`. Use NESRecomp's public
  extension points only; menu look and widget changes go to the formae-ui repository, then the
  submodule moves to its new tag.
- Never commit the ROM, generated C, `build*/`, `captures/`, frame logs, screenshots, recordings,
  WAVs or `config.ini`.
- With every option at its original setting the machine must behave exactly as the console:
  `tests/verify_routes.py` must pass with native, interpreter and oracle equal on every frame, at all
  four alignments, with 100% native cycles.
- Presentation changes may alter only what the window shows, never the machine picture or any hash.
- Gameplay changes are enhancements, off by default, hooked at a site declared in `config/game.toml`
  with a length/CRC-32 content key (see docs/architecture.md, "Adding a behaviour change").
- Never open a visible window on the developer's desktop. Use the headless builds, `--hidden` or the
  self-test modes.
- This fork (gabbycat4, branch linux-240) builds on Windows x64 and Linux (CachyOS). Keep Linux
  changes small, behind `#ifdef _WIN32` / `#else`, and never change the Windows behaviour.
- Do not push to `main` or merge without being asked.

## Environment

- Windows, Visual Studio 2022, CMake, Python through `py -3 -I`, Git for Windows.
- Pass `C:/...` paths to git and to the executables, not `/c/...`.
- The ROM is the file in `roms/` (ignored by git) with the SHA-256 below, under any name (the docs call
  it `roms/baserom.nes`), or `-DDUCKTALES_ROM=<path>`. Its SHA-256 is
  `97d472f7b2c1088448d1644155b2650a546bad887b3cba571e16d7986824c169`.
- Disk space is limited. Keep at least 8 GB free, bound frame logs with `--frame-log-frames` and
  delete scratch under `captures/` when done.

## Builds

| Tree | Configure flags | Use |
|---|---|---|
| `build/windows` | (none) | The game as players run it; `tests/window_smoke.py` |
| `build/headless` | `-DDUCKTALES_HEADLESS=ON -DDUCKTALES_ORACLE=ON` | Differential tests with the TriCNES oracle |
| `build/headless-dev` | `-DDUCKTALES_HEADLESS=ON -DDUCKTALES_DEV_DIAGNOSTICS=ON` | Typed state log and scheduled changes for tests |

Configure with `-G "Visual Studio 17 2022" -A x64` and build with
`cmake --build <tree> --config Release --parallel`.

## Checks

| Change | Command |
|---|---|
| Game code, seeds, native runtime (every commit) | `py -3 -I tests/quick_verify.py --build build/headless [--changed <ranges>]` |
| Hooks, host, NESRecomp extension points (every commit) | `py -3 -I tests/quick_verify.py --build build/headless --runtime cycle [--changed <ranges>]` |
| Decompiled routine, iterating | `py -3 -I tests/decomp_test.py --build build/headless --dev-build build/headless-dev --changed <ranges> --from-state --span 600 --modes check replace` |
| Decompiled routine, before merge | `py -3 -I tests/decomp_test.py --build build/headless --dev-build build/headless-dev` (every route, all modes) |
| Nightly, before a release | `py -3 -I tests/verify_routes.py --build build/headless --jobs 3` |
| Enhancements | `py -3 -I tests/enhancements_test.py --build build/headless --dev-build build/headless-dev` |
| Sprite presentation | `py -3 -I tests/sprites_test.py --build build/headless --dev-build build/headless-dev` |
| Menu, settings, window | `py -3 -I tests/window_smoke.py --build build/windows` |
| ROM identity and import | `build/headless/Release/app_rom_test.exe` |
| Release package, install rules | `py -3 -I tests/package_test.py --build build/windows` |
| Any source change | `py -3 tools/format.py --check` |

Iterate with `tests/quick_verify.py`, not `verify_routes.py`. It runs the affected routes at alignment 0
and compares every frame with golden hashes, stopping at the first difference with the fields that
differ. On the native runtime all 12 routes take about 11 s with `--jobs 4` (39 s with one job); `--changed <[BANK:]ADDR[-ADDR] ...>` or
`--routes-for <ADDR>` picks only the routes that execute that code, and `--from-state auto --span N`
starts each of them from a recorded save state just before it. The native runtime does not run the
host's hooks or enhancements, so changes there use `--runtime cycle` (about 3.5 min for all 12 routes with `--jobs 4`).

The golden files (`build/golden/<ROM SHA-256>/`: hashes, save states, coverage) are recorded once with
`--record` from a build that has just passed the full `verify_routes.py`, and are never committed.
They hold what the console does, which depends only on the ROM and the routes, so one recording
serves every worktree through `--golden <dir>`; record again when a route changes (quick_verify says
so) or NESRecomp moves (its `trace` and `hw` hashes are its own).

Heavy runs (`--runtime cycle`, `decomp_test.py`, `verify_routes.py`) go through
`py -3 -I tools/test_slot.py -- <command>`, which waits for one of 2 machine-wide slots, so the
worktrees sharing this PC take turns instead of slowing each other down.

Never run a heavy check twice on the same inputs. A merge whose tree equals the tree a branch's gate
ran on (`git rev-parse <commit>^{tree}`) reuses that gate's numbers, and a run that failed is not
repeated with the same build and arguments in the hope of another result. End a report with what you
reused and what you did not run because another result covers it.

Run the full `verify_routes.py` (3 h 18 min with `--jobs 3` and no oracle cache) nightly and before
a release, not per branch per change. Code no route executes (quick_verify says "no route executes") is not
checked by either; add a route. Oracle results are cached in `build/verify-cache/`.

## Conventions

- C everywhere in `src/`; C++ only in `src/ui/app_ui.cpp`. Four-space indent,
  CRLF line endings.
- Layout is `.clang-format` (C/C++) and Black (Python, `pyproject.toml`): run `py -3 tools/format.py`
  before committing; `py -3 tools/format.py --check` must list nothing. Never format `external/`.
- Each source file opens with a header comment naming its purpose and the settings or options it
  owns. No comments inside function bodies.
- Prefixes: `dt_` / `Dt` for game code, `app_` for the application layer.
- Commit subjects are short imperative sentences in sentence case, no prefix, no trailing period.
- Keep the README, the matching `docs/` page, `docs/validation.md` (results) and `CHANGELOG.md`
  current in the same change.
- Report what you verified with exact numbers (routes, frames, results), and say what you did not
  verify.
