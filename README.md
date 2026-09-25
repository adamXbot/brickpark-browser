# BrickPark browser

**LEGOLAND** (Windows, 2000) running in a modern browser, built from the
matching decompilation in
[brickpark-decomp](https://github.com/adamXbot/brickpark-decomp) and modelled
on [isle-portable](https://github.com/isledecomp/isle-portable). The front
end, all five tutorial lessons, free play and the campaign levels play.

You need your own copy of the game. Neither repository contains game data or
binaries, and a build packs your game files into its output, so **never
publish or share a build directory**.

## What you need

- Your LEGOLAND CD, or an image of it (`.iso` or `.bin`). The disc holds the
  installer archive `main.z`, the three `.res` volumes, `Speech/` and
  `directx.cab`.
- [Emscripten](https://emscripten.org/) 6.0.9 (the version the tree is built
  against), CMake, Ninja and Python 3.
- Optional: `ffmpeg` on the PATH, so the build can decode the advisor's video
  clips; without it the advisor's panel is plain. Node.js runs the tests.

## Build and play

```bash
git clone --recurse-submodules https://github.com/adamXbot/brickpark-browser.git
cd brickpark-browser

# 1. Game files: the CD's contents in gamedata/disc, the installer archive
#    extracted to gamedata/main, and the game's executable in original/.
mkdir -p gamedata original
cp -R /Volumes/LEGOLAND/. gamedata/disc/
python3 decomp/tools/iscab.py extract gamedata/disc/main.z -o gamedata/main
cp gamedata/main/legoland.exe original/

# 2. The player page.
emcmake cmake -S . -B build-wasm -G Ninja -DCMAKE_BUILD_TYPE=Release -DLL_ILP32=ON
ninja -C build-wasm legoland_web

# 3. Serve it locally and open http://localhost:8080/
python3 -m http.server -d build-wasm/web 8080
```

`main.z` sits on the CD (`find gamedata/disc -iname main.z` if the disc's
layout differs), and the executable it unpacks is the one the decomp README
names by SHA-256. The build packs `gamedata/` into `build-wasm/web/data/` for
the page's own download; that folder is the game's files, for your computer
only.

The page opens on its menu. **Game files** installs the game into the browser
once, either from the pack the build just made (*Download*) or from your own
disc: drop an `.iso` or `.bin`, a `.zip` of one, a copied CD or an installed
LEGOLAND folder onto it. Everything is read and stored in the browser; nothing
leaves your computer. Then **Play**.

## Playing

- The game's cursor follows your pointer at any window size. The toolbar has
  sound, full screen and **Leave**.
- Saves live in your browser. **Saved games** lists players and parks, and
  exports or imports them as a zip laid out like the game's `profiles\` folder.
- **Options → Extras → Free Play with everything** opens free play with every
  item unlocked. It is off by default and writes nothing to your profile.
- Music plays on the developer page below, which preloads the instrument set
  the build extracts from the disc's `directx.cab`. The player page does not
  install the instruments yet, so its music is silent for now.

## Developer page, native build, tests

```bash
ninja -C build-wasm legoland_browser     # legoland.html: the instrumented page
python3 -m http.server -d build-wasm 8968   # open http://localhost:8968/legoland.html

cmake -S . -B build -G Ninja && ninja -C build   # native: every source with clang
ninja -C build legoland_linkcheck && ./build/legoland_linkcheck
ctest --test-dir build --output-on-failure       # or --test-dir build-wasm
```

The developer page names traps, hashes frames and runs replay scripts; its
`version` cell links the commits the build was made from. The native build is
a symbol census and a test host rather than a playable game, because the
recovered code is 32-bit and only wasm32 keeps its layouts. `-DLL_FAITHFUL=ON`
builds without the documented fixes to the shipped game's own bugs
([QUIRKS.md](https://github.com/adamXbot/brickpark-decomp/blob/main/docs/QUIRKS.md)).

Everything else, from the build options and the layout of the tree to the
tests, CI and the lane-by-lane log of how the port was made, is in
[docs/DEVELOPMENT.md](docs/DEVELOPMENT.md).

## Legal

BrickPark is an independent, unofficial project, not affiliated with, sponsored
by or endorsed by the LEGO Group, LEGO Media or Krisalis Software. LEGO and
LEGOLAND are trademarks of the LEGO Group; they appear here only to identify
the program being reconstructed, and the project carries no LEGO branding.
This repository contains no game data or binaries. A build made from your own
copy of the game is for your own use.
