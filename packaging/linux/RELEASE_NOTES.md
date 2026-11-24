DuckTales Recompiled for Linux: an unofficial fork of
[formae-group/ducktales-recompiled](https://github.com/formae-group/ducktales-recompiled)
with a Linux build, up to 240 pictures a second and smooth scrolling.

**You need your own DuckTales (USA) ROM.** Nothing from the game is in this download;
on first start the game asks for your ROM file and checks it.

### Play

1. Download `DuckTalesRecompiled-x86_64.AppImage`.
2. Make it executable: right-click → Properties → Permissions → "Allow executing", or
   `chmod +x DuckTalesRecompiled-x86_64.AppImage` in a terminal.
3. Double-click it and choose your ROM.

Runs on Linux distributions from about 2022 on (glibc 2.35 or newer: Ubuntu 22.04,
Fedora 36, Arch/CachyOS, and newer).

### On a fast monitor (120-240 Hz)

Escape (or RB) opens the menu. In **Settings → Graphics → Motion**, set **Pictures per
second** to your monitor's refresh rate (or a divisor of it). Keep **Smooth scrolling**
on **Calm**. With FreeSync/G-Sync, turn on your desktop's adaptive sync (KDE: Display
settings → Adaptive sync: Always).

Your settings, saves and ROM copy are kept in `~/.local/share/DuckTales Recompiled/`.
