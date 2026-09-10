# LEGOLAND portable

The path from the matching decompilation in `../LEGOLAND` to a game that runs
natively and in the browser, modelled on
[isle-portable](https://github.com/isledecomp/isle-portable): the same
recovered C compiled with a modern compiler, Win32 and DirectX replaced by a
host shim (SDL3 later), and Emscripten for the web build.

This directory never touches the VC6 matching build. Every change it needs in
`../LEGOLAND` sits under `#ifdef LEGOLAND_PORTABLE` (or `#ifndef`), which the
VC6 gate does not define, so `tools/verify.py` sees the original text.

## Status: merged to main, census refreshed (2026-09-11)

All 258 game sources compile with clang under `LEGOLAND_PORTABLE`, and a
whole-archive link of the recovered game succeeds against a generated
closure. Nothing runs yet; the generated pieces trap by name when reached.
The census below (`linkreport.py` over the clang objects at the merge) is the
work list. The matching frontier closed on 2026-09-09, so the "unwritten game
functions" row is gone: the 12 symbols `linkreport.py` still files there are
all CRT-range wrappers (`HeapAlloc_w`, `MemAlloc`, `Format`, `CRT_calloc`,
`NameCompare`, ... at 0x0049e4xx..0x004aab90), which the host shim's CRT
supplies, not matching lanes.

| what the link still needs | count | where it comes from |
| --- | --- | --- |
| CRT-range wrappers filed as "game-fn" | 12 | host shim / libc (`HeapAlloc_w`, `MemAlloc`, `Format`, `rand_w`, ...) |
| externs with no address comment | 84 | rename or write; see `linkreport.md` "Unclassified" |
| stale extern names (address defined under another name) | 228 | rename pass; bridged by generated aliases for now |
| Win32 / DirectX imports | 149 | host shim (`src/hostwin/`), stubbed per DLL |
| inline-asm bodies still stubbed | 26 | port by hand, see below |
| extern-only globals | 2,612 | rebuilt from `original/legoland.exe` by `gen_link.py` |

Host imports by DLL: KERNEL32 42, USER32 34, GDI32 18, AVIFIL32 16 (Indeo 5
FMV), MSACM32 6 (ADPCM), WINMM 5 (MIDI out, timers), VERSION 3, ole32 2
(DirectMusic), DSOUND 1, DINPUT 1, DDRAW 1, ADVAPI32 1.

## Build

```bash
cmake -S portable -B portable/build -G Ninja -DPython3_EXECUTABLE=$HOME/.venvs/legoland/bin/python
ninja -C portable/build                      # liblegoland_core.a: every game source
ninja -C portable/build legoland_linkcheck   # generate the closure and link the whole archive
python3 portable/tools/linkreport.py portable/build/CMakeFiles/legoland_core.dir --out portable/build/linkreport.md
```

`gen_link.py` reads the original binary at `../original/legoland.exe` to
initialise data (the path is the `LL_EXE` cache variable). Without it the
globals are zero-filled and the link still closes.

### ILP32 is the real target

The sources are ILP32 code: `int`, `long` and pointers are 4 bytes, and
struct offsets are load-bearing. A 64-bit host build compiles and links, and
is a correct symbol census, but `long` fields and pointer tables change size,
so it is not runnable. The run targets are wasm32 (Emscripten) and 32-bit
native. `-DLL_ILP32=ON` makes `gen_link.py` re-point 4-byte words that equal
a known symbol address at the rebuilt symbol, which is what makes the
original's pointer tables (callback tables, string tables) valid again on a
32-bit target.

## Layout

| path | role |
| --- | --- |
| `CMakeLists.txt` | `legoland_core` (the game), `legoland_gen` (generated closure), `legoland_linkcheck` |
| `hostwin/include/ll_portable.h` | force-included into every game source: fistp/fixed-point/rdtsc/blit helpers behind the asm fallbacks |
| `hostwin/include/windows.h`, `io.h`, `direct.h` | the slice of the Windows headers the sources include |
| `src/hostwin/portable.c` | trap helpers, `stricmp`/`strnicmp`, `ll_rdtsc` |
| `src/hostwin/msvcrt.c` | MSVC CRT file/directory calls on POSIX (`_open`, `_findfirst`, ...) |
| `src/linkcheck.c` | the smallest program that links everything |
| `tools/linkreport.py` | the census: what is undefined and why |
| `tools/gen_link.py` | the closure: globals from the exe, aliases, trapping stubs |
| `tools/win32_imports.txt` | import table of `legoland.exe` (name, DLL) used to classify host API |

## What the LEGOLAND_PORTABLE guards changed

Inline x86 assembly (about 100 sites in 36 files at the merge) is invisible
to a non-x86 compiler, so each site has a C fallback:

- **Ported to C**: x87 `fld/fmul/fistp` conversions (round-to-nearest, as the
  game's control word sets), the 16.16 `imul/shrd` fixed-point macros in
  person3d.c and math3d.c, the rotation-matrix and vector-transform loops,
  rdtsc timing brackets (coaster10.c's three model draw passes included),
  byte swaps, control-word save/restore (no-ops), the flat z-buffer span
  fillers (`ZBuffer_FillPoly` in schoolcar6.c, `ZBuffer_FillShadedPoly` in
  unref1.c, `Span_FillFlat` / `Span_FillFlatZ` in coastershade2.c), the
  clear/fill loops in tri3d.c and text.c, the 8-bit paletted and 16-bit
  rectangle blits, the fast sqrt/rsqrt table helpers (`ll_FastSqrt`,
  `ll_FastRSqrt`), and unref7.c's hand-written texture sampler
  `SampleTexturePixel`.
- **Stubbed with `LL_UNPORTED_ASM()`** (aborts when reached): the four
  triangle rasterisers in tri3d.c, the eight RLE painters in rlepaint.c and
  two in rlepaint2.c, the RLE animation blitters in softblit.c/softblit2.c,
  the z-buffer RLE walker in bigrender.c, the two blits in blitmisc.c, the
  50% blend blit in popup.c, the Gouraud span fillers `Span_FillShade` /
  `Span_FillShadeZ` (coastershade2.c) and the textured `TrackShade_FillPoly`
  (coaster13.c), and the ST(0)-ABI helpers `sub_458930`, `FastSqrt`,
  `FastRSqrt`. About 6,000 lines of hand-written blitter asm; the full list
  is the "Unported inline-asm bodies" section of `linkreport.md`.
- Structured exception handling in exceptlog.c and winmain.c compiles to
  plain blocks (`__try` -> `if (1)`, `__except` -> `else if (0)`).
- castleobj.c's `Track_Update` (0x00427b20) collides with coaster.c's
  (0x004275d0); the portable build renames the former `Track_Update_427b20`.

Every new `__asm` site a matching lane adds must get the same treatment, or
the clang build breaks: wrap it in `#ifndef LEGOLAND_PORTABLE` and give the
`#else` arm a C fallback or `LL_UNPORTED_ASM()`. The guard lines go INSIDE
the function body, never between a `// FUNCTION:` marker and its signature.

## Headless behaviour tests (scope PORT-C)

`portable/tests/` plus `portable/cmake/tests.cmake` build `legoland_tests`, one
executable with a subcommand per test, linked the way `legoland_linkcheck` is
(whole-archive `legoland_core` + the generated closure). Each test drives real
recovered entry points over the real `gamedata/` and compares against a
`tools/oracle_*.py` built on the clean-room Python decoders. Nothing derived
from the game's assets is committed: the oracles generate their expectations
into `<build>/gen-tests/` at build time.

```bash
ninja -C portable/build legoland_tests
ctest --test-dir portable/build --output-on-failure       # the ILP32-safe tests
ninja -C portable/build-wasm legoland_tests               # needs the wasm32 closure
ctest --test-dir portable/build-wasm --output-on-failure  # run under node
```

| test | what it drives | oracle | where it runs |
| --- | --- | --- | --- |
| `save_framing` | `BeginMeasuredBlock` / `EndMeasuredBlock` / `SaveGameWrite` / `SaveGameRead` / `FindeIneList` | `tools/oracle_savechunks.py` | native + wasm32 |
| `tile_geometry` | `GetTileDimensions` / `GetTileCentre` / `GetTileBounds` / `OverNewTile` / `CrossTileCentre` | `tools/oracle_tilegeom.py` over `tools/tilemap.py` | native + wasm32 |
| `res_archive` | `RES_OpenVolume` / `RES_LoadDirectory` / `RES_OpenFileFromVolume` / `RES_ReadFile` / `RES_SetFilePointer` | `tools/oracle_res.py` over `tools/resfile.py` | wasm32 only |
| `llidb_icm` | `LLIDB_LoadICM` / `LLIDB_GetElement` / `LLIDB_FindElement` / `ElemID` | `tools/oracle_icm.py` over `tools/tilemap.py` | wasm32 only |
| `loadpos` | `LoadPos` / `BuildYRotationMatrix` / `MatrixMultiply` / `CopyMatrix` | `tools/oracle_geom.py` over `tools/geom.py` | wasm32 only |

A test is wasm32-only when its **serialised** layout is ILP32, and each test's
header comment says exactly which field makes it so. The two sharpest cases are
worth repeating here, because they are the concrete content of "ILP32 is the
real target" above:

- `RES_LoadDirectory` (0x004895a0) rewrites the directory image's links in
  place with `node->sub += (int)base`, where `base` is a malloc'd pointer. On
  wasm32 that is exactly the format; on a 64-bit host the pointer is truncated
  and the walk segfaults.
- `LLIDB_LoadICM` (0x0047aff0) bulk-reads records with
  `_read(fd, page, n * sizeof(LLElem))`, and `LLElem` is 20 bytes on ILP32 and
  40 on LP64.

Findings, the full result table and the census delta are in
`docs/lanes/scope-port-c.md`. Two of them matter to the other lanes: the host
shim must answer `GetVolumeInformationA` with a CDFS volume named `"LEGOLAND"`
or `RES_OpenFile` never returns, and `RES_LowSeek` / `RES_LowRead` are
`SetFilePointer` / `ReadFile` under local names rather than missing work.

## The browser host shim (scope PORT-B)

`src/hostwin/{ddraw,user32,gdi32,dinput,winmm,dsound}.c` is a host shim for the
six DLLs the *graphics and input* half of the game imports; `src/browser/` is
the Emscripten page around it and `cmake/browser.cmake` its targets. Full notes,
including the vtable slot table with the source line that pins each slot, are in
[`docs/lanes/scope-port-b.md`](../docs/lanes/scope-port-b.md).

**The main loop is `-sASYNCIFY`.** The recovered game has no one-frame function:
every frame is produced inside synchronous spin loops (`FlipPrimary`'s 28 ms
`while (timeGetTime() - g_flip_time < 0x1c);`, `PresentFlip`'s `GetFlipStatus`
spin, the `PeekMessageA` drain, the `WaitMessage` focus idle, `RunGame`'s
`PeekMessageA; Sleep(100)` music wait). ASYNCIFY runs that control flow
unchanged, with the yields inside the shim: `PeekMessageA` when the queue goes
empty (once per frame), `WaitMessage`, `Sleep`, `GetFlipStatus`, the present
path, and `timeGetTime` whenever 4 ms has passed — the last is what bounds
*every* wall-clock spin. Two rules follow for other lanes: **`Sleep` must yield,
never be a no-op**, and **nothing may call into wasm while a yield is unwound**
(canvas events only enqueue; `timeSetEvent` callbacks are dispatched from the
pump, not a JS timer).

| target | toolchain | state |
| --- | --- | --- |
| `legoland_hostwin` | all (in the default build) | the shim; 70 of `gen_link.py`'s 149 host traps are now real code, and 0 remain in DDRAW/USER32/GDI32/DINPUT/WINMM/DSOUND |
| `legoland_shimtest` | wasm32 | drives the shim through the startup spine's exact call sequence and paints a frame. **Runs**: 640x480 RGB565 on a canvas at ~87 fps with the page responsive, keyboard and mouse read back through DirectInput |
| `legoland_browser` | wasm32 | the game itself. Wired and complete; **blocked** on `gen_link.py`, which does not compile on wasm32 (the 12 `globals.c` redeclarations, plus `aliases.c`'s `__attribute__((alias))` on an incomplete array type off Apple) |

```bash
emcmake cmake -S portable -B portable/build-wasm -G Ninja -DLL_ILP32=ON \
    -DPython3_EXECUTABLE=$HOME/.venvs/legoland/bin/python
ninja -C portable/build-wasm legoland_shimtest
cd portable/build-wasm && python3 -m http.server 8791   # /shimtest.html
```

Assets are `--preload-file`d: `gamedata/main` at `/gamedata`, and (option
`LL_PRELOAD_RES`, default ON) the three RES volumes at `/gamedata/volumes/`,
because `RES_OpenVolume` opens `.\volumes\<stem>.res` first and `InitSession`
mounts them *before* DirectDraw exists. That is a 170 MB `.data` file — fine over
localhost, and the reason a WASMFS fetch backend is the next step.

What is not there yet: GDI text (invisible; the layout maths still runs), the AVI
frames, sound, MIDI, printing and dialogs. Each returns a documented failure or a
successful no-op; none of them traps.

## wasm32: the link closes and the startup spine runs (scope PORT-A, 2026-09-11)

```bash
emcmake cmake -S portable -B portable/build-wasm -G Ninja -DLL_ILP32=ON \
        -DPython3_EXECUTABLE=$HOME/.venvs/legoland/bin/python
ninja -C portable/build-wasm legoland_linkcheck
node portable/build-wasm/legoland_linkcheck.js          # every symbol resolved

ninja -C portable/build-wasm legoland_headless
LL_HOST_TRACE=1 LL_DATA_DIR=$PWD/gamedata/main \
        node portable/build-wasm/legoland_headless.js   # the game's own WinMain
```

`legoland_headless` (`portable/src/headless/`, `portable/cmake/headless.cmake`)
calls `WinMain(NULL, NULL, "-nointro -nomusic WINDEBUG", 1)` with `-sNODERAWFS=1`,
so `gamedata/` is read straight from disk — `$LL_DATA_DIR` is the `chdir` the
loaders' relative paths need. It gets as far as:

```
HOST GetFileVersionInfoSizeA("Legoland.exe")   # WinMain -> ReadExeVersionString
HOST GetFileVersionInfoA
HOST VerQueryValueA("\StringFileInfo\080904B0\ProductVersion")
HOST CreateMutexA("LegolandGameMutex")         # GameMain
HOST WaitForSingleObject(1, 0)                 # the one-instance test
TRAP DDRAW.dll DirectDrawCreate from gpu.c     # CheckHostSystemGPU: PORT-B
```

Every generated body now prints `TRAP <dll> <symbol> from <callers>` and exits
70, and `LL_HOST_TRACE=1` traces the KERNEL32 side. `--resmount` runs the
resource-volume mount that sits behind the DirectDraw wall;
`LL_CD_DIR=$PWD/gamedata/disc` presents that directory as the game's CD.

What `gen_link.py` had to learn for wasm, where symbols are typed and Mach-O's
tolerance is gone: one consistent declaration per symbol (globals.c is planned
before it is written, which also re-points forward references — 243 symbols);
forwarders instead of aliases, since wasm has no cross-TU alias; and signatures
read out of the objects for every stub and forwarder, because a mismatched
signature is silently replaced by wasm-ld with a trapping stub. Details and the
before/after census: `docs/lanes/scope-port-a.md`.

`portable/src/hostwin/kernel32.c` implements all 56 KERNEL32/ADVAPI32/VERSION
imports the game references (single-threaded waits, POSIX files, one monotonic
clock, `Sleep` as a no-op with the `ll_host_yield` hook PORT-B sets), declared
in the new host ABI header `portable/hostwin/include/ll_host.h`. Host API stubs:
149 → 95.

The 12 symbols the census files as "CRT-range wrappers filed as game-fn" are
forwarded rather than trapped now: each (`MemAlloc`, `Format`, `NameCompare`, …)
is a second name for a CRT function the sources declare at the same address, so
gen_link routes them through the same forwarder machinery as a stale extern
name. `- unwritten game function stubs: 0` in `gen/manifest.md`.

`linkreport.py` also reports a new row, **prototype conflicts: 542** — places
where one source's declaration of a game function lowers to a different wasm
signature than the body another source defines (`AddBasicObject` is defined with
three parameters and called with two from 21 files). Harmless in x86 cdecl,
a runtime trap on wasm.

## The loaders run: pointer tables, install paths, the spine to the sprite loader (scope PORT-A2, 2026-09-11)

```bash
ninja -C portable/build-wasm legoland_headless
LL_HOST_TRACE=1 LL_CD_DIR=$PWD/gamedata/disc LL_DATA_DIR=$PWD/gamedata/main \
        node portable/build-wasm/legoland_headless.js --resmount   # mount + list members
LL_HOST_TRACE=1 LL_CD_DIR=$PWD/gamedata/disc LL_DATA_DIR=$PWD/gamedata/main \
        node portable/build-wasm/legoland_headless.js --stages     # InitSession, step by step
```

**Pointers INTO a rebuilt global, not only AT one.** `gen_link.py --ilp32`
re-pointed a word only when it *equalled* a named symbol's address, so
`g_volume_names`' three `const char*` kept their raw x86 values,
`RES_OpenVolume` opened `"D:\.res"` and the page died with "Failed to open
resource". PORT-A sketched the fix as gap blocks between named globals;
measured, there are none — the rebuilt globals already tile `.rdata`
(35,856/36,864) and `.data` (3,669,492/3,670,016), and the three literals sit
at offset 4/20/36 *inside* the 472-byte block for 0x004bcbf4. A pointer word
now resolves to a symbol's address, an offset into a rebuilt block, an offset
into an object the game defines, or a synthesised `ll_gap_<start>` initialised
from the exe; outside the data sections it stays raw. Which words are pointers
comes from the game's own `extern` declarations, never from the value — a
three-character string at the end of a word is an in-image integer
(`"tan\0"` = 0x006e6174), and a value-only rule re-points 1,650 words of which
1,209 are string text. 272 exact + **441 interior** + 0 gaps + 12 left raw.

**Install paths.** `ll_host_resolve_path` (kernel32.c) resolves the paths the
game wrote for a 1999 Windows install: the emulated `D:` drive, backslashes,
each component matched case-insensitively ("LEGOLAND.ICM" vs `Legoland.icm`),
and — when a directory component does not exist at all — the last component
looked for in the deepest directory that did match, which is what makes the
flat `gamedata/main` serve `".\strings\stab.str"`. `msvcrt.c` routes
`_open`/`_chdir`/`_mkdir`/`_findfirst` through it and now defines `fopen`
itself, which was the one file entry point nothing wrapped: `LoadStrings`
(narration2.c) is `if (!f) exit(1)`, so the page vanished with status 1 and no
message the moment the volumes mounted.

`legoland_headless` and `legoland_tests` link PORT-B's `legoland_hostwin` and
the filtered closure `legoland_gen_browser`; `src/headless/node_shim.c` supplies
the four `ll_canvas.js` entry points and `emscripten_sleep` under node.
`--profiling-funcs` on both keeps the wasm name section.

How far the spine runs (`--stages`, and the browser page reaches the same
point): `LoadStrings` → `GetString(0xcb) = "LEGOLAND ERROR"`;
`InitHostSystemGPU` = 1; `InitScreen` = 1 (640x480 display, window "LEGOLAND");
`InitInputSystem` = 1; `RES_OpenFile(".\graphics\erase it.lls")` = 1402 bytes;
then `LoadSprite` → `RuntimeError: unreachable` in `__BMPLoader`.

**The next blocker is not the host shim.** It is
`signature_mismatch:RES_CloseFile` — screen.c, loaders.c and eleven other files
declare `extern void RES_CloseFile(void*)` while sweep4.c defines
`int RES_CloseFile(RVol*)`, and on wasm that is a poisoned call site, not a
`TRAP`. Three of the 542 prototype conflicts are now proved live on paths the
game takes. See `docs/lanes/scope-port-a2.md` §4.

Census after this lane (`linkreport.py portable/build-wasm/CMakeFiles/legoland_core.dir`):
game-fn 12 (all CRT thunks, forwarded), game-data 2612, alias 228, host 95,
crt 43, unknown 86, duplicates 0, asm stubs 26, prototype conflicts 542.

## Next

0. **The 542 prototype conflicts** are now the frontier, ahead of everything
   below: `RES_CloseFile` (13 files) and `DBPrintf` (30+) are each one
   `#ifdef LEGOLAND_PORTABLE` declaration away, and each one unblocks a whole
   loader. `docs/lanes/scope-port-a2.md` §4 has the recipe and the evidence.
1. **Host shim on SDL3, native desktop first**: window and message pump,
   one 16-bpp surface presented as a texture (`DirectDrawCreate` and the
   `IDirectDraw*` vtables in gpu.c/surface.c), DirectInput-shaped keyboard
   and mouse state, `GetTickCount`/`timeSetEvent`, DirectSound over SDL
   audio, GDI text via a bitmap font. Stub movies, music and printing.
2. **Port the stubbed blitters** as they are reached.
3. **Emscripten job** mirroring isle-portable's CI row: `emcmake`,
   pthreads, WASMFS fetch backend streaming assets from a host URL, OPFS
   saves, COOP/COEP on the host.
4. **Fill the rest**: ADPCM in C, a soft synth for the DirectMusic
   styles/segments, Indeo 5 for the AVI intros.
