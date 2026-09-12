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
  triangle rasterisers in tri3d.c, the RECOLOURING animation blitter
  SoftBlitAnim in softblit.c,
  the z-buffer RLE walker in bigrender.c, the two blits in blitmisc.c, the
  50% blend blit in popup.c, the Gouraud span fillers `Span_FillShade` /
  `Span_FillShadeZ` (coastershade2.c) and the textured `TrackShade_FillPoly`
  (coaster13.c), and the ST(0)-ABI helpers `sub_458930`, `FastSqrt`,
  `FastRSqrt`. About 6,000 lines of hand-written blitter asm; the full list
  is the "Unported inline-asm bodies" section of `linkreport.md`.
- **The ten type-3 RLE sprite painters** (scope PORT-B3): the eight in
  rlepaint.c and the two in rlepaint2.c are now C, which is what lets the
  title screen draw. Their shared decode -- the rotating 2-bit control-stream
  reader (`LLRleCtl`, `ll_rle_open`, `ll_rle_code`, `LL_RLE_HI`/`LL_RLE_LO`)
  and the run hit test `ll_rle_hit_run` -- lives in
  `portable/hostwin/include/ll_portable.h` next to `ll_blit8`/`ll_blit16`, so
  all ten spell the decode once. Five of the ten mishandle primary code 1 in
  their top-skip pass and the two recolouring ones have three hit-test
  divergences of their own; all of that is REPRODUCED, not fixed, and
  `docs/lanes/scope-port-b3.md` says which leaf does what.
- **The type-2 LLS animation painter** (scope PORT-B3): softblit2.c's
  `SoftBlitAnimPlain` too, with its own control stream (a cached word plus
  `g_zb_bits`, an 8-bit run length spliced out of the low byte for four code
  slots, 8-bit indices through `g_sp_pal16`) in `ll_anim_open`/`ll_anim_code`/
  `ll_anim_count`/`ll_anim_hit`. Its recolouring sibling `SoftBlitAnim`
  (softblit.c 0x00465240) is deliberately still a trap: porting it means
  first deciding what to do about a misplaced `row:` label in its `__asm`
  text that the byte gate cannot see. Both findings are in
  `docs/lanes/scope-port-b3.md` §3b.
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

What is not there yet: the AVI frames, sound, MIDI, printing and dialogs. Each
returns a documented failure or a successful no-op; none of them traps. (GDI text
was on this list until PORT-B2 — see below.)

## GDI text, node safety and the instrumented page (scope PORT-B2)

Full notes: [`docs/lanes/scope-port-b2.md`](../docs/lanes/scope-port-b2.md).
Everything the *first front-end frame and the first click* need, written ahead of
the loader landing.

**GDI text is real now** (`src/hostwin/ll_font.c` plus the rewritten `gdi32.c`).
The drawing half is the obvious part; the measuring half matters more. `DrawTextA`
is the game's ONLY text-extent call — there is no `GetTextExtentPoint32A` anywhere
in the tree — and eleven call sites pass `DT_CALCRECT` and lay something out
around the rect that comes back (`frontend2.c:521` centres a button caption on it,
`bighelp.c:316` and `bubblecache.c:500` size a speech bubble to hold the text).
A `DrawTextA` that returns 1 and leaves the rect alone tells the game every block
of text is one pixel tall, so one engine does both halves and they agree by
construction. The face is a 6x7 bitmap authored for this lane — no shipped font is
reproduced — scaled to the LOGFONT's `lfHeight` so its metrics land near the real
"Lego" face's: about 53 characters of the 24-pixel font per 640-pixel line, so
lines that fitted still fit and lines that wrapped still wrap.

Alongside it, in the same reading of the front-end path: `SelectObject` returns
the previously selected object *of the same class* (text.c restores four objects
of two classes in reverse), the clip region from `CreateRectRgn(g_clip_rect)` is
honoured, DC attributes are per-DC and reset by `GetDC`, `FillRect` fills with the
brush colour packed the same way the game's own `GetNearestColour` packs 565 (so
`DrawCachedTextSprite`'s colour key matches its fill), and `MoveToEx`/`LineTo`
draw.

**Three USER32 correctness fixes** found in the same sources: `MessageBoxA` now
auto-answers per button set, always with the answer that does not ask again —
`RES_EnsureMounted` *loops* on `MB_RETRYCANCEL` until IDCANCEL, so the old
always-IDOK was a hard hang of the tab whenever the CD probe failed;
`SystemParametersInfoA(SPI_GETMOUSE)` fills its buffer, which `ResetController`
reads uninitialised into the game's own mouse acceleration (reporting acceleration
*off* also closes the cursor-drift divergence PORT-B recorded, with no game edit);
and `ShowCursor`'s display count starts at 0, so `InitScreen`'s `ShowCursor(0)`
actually hides.

**The JS library is node-safe.** `legoland_hostwin` links into `legoland_tests`
and `legoland_headless`, which run under node with no DOM. Every function degrades
and nothing throws; the display becomes a counter plus a sampled checksum, so a
headless harness can prove pixels are changing. One trap worth knowing about:
emcc's `-O2` JS optimizer **constant-folds `typeof document === 'undefined'` at
build time**, so the headless probe has to be lazy and go through `globalThis`, or
the browser page runs headless and paints nothing.

```bash
node portable/build-wasm/shimtest.js --frames 300   # prints PASS, exit 0
```

**The page** (`?trace=1` still works) shows the frame count and fps, the last
`MessageBoxA` with the answer the shim gave it, any `TRAP` line in a red banner,
and the trace toggle. `LL_HOST_TRACE` now drives the DirectX half of the shim too,
not just `kernel32.c` — without that, a run that gets past the loaders and stops
produces a trace ending at the last `ReadFile`.

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

## One block per object, naming a trap, and the spine as a test (scope PORT-A3)

Full notes: [`docs/lanes/scope-port-a3.md`](../docs/lanes/scope-port-a3.md).

**`gen_link.py` emits ONE block per object.** Sizing every rebuilt global by the
gap to the next NAMED address is what lets a pointer be re-pointed into the
middle of a block (PORT-A2 above), and it also split a named object that other
names reach into at an offset. `extern unsigned char g_key_state[256]`
(input.c:53) is also `g_left_ctrl` (+0x1d), `g_left_shift` (+0x2a),
`g_right_shift` (+0x36) and `g_right_ctrl` (+0x9d) — those four offsets are the
DIK codes themselves — so `ScanKeyboard`'s one
`GetDeviceState(kbd, 256, g_key_state)` wrote 256 bytes into a 29-byte object and
`IsLShiftDown`/`IsRShiftDown` read a byte of the wrong key. Worse and unnoticed:
`extern char g_gpu_state[0x3d8]` (util.c:79) is the host GPU block gpu.c:10 maps
field by field, and `CheckHostSystemGPU` clears the whole of it with one
`memset` — which reached only the DirectDraw pointer, leaving both surfaces, the
clipper, the palette, the fonts and the `DDSURFACEDESC` the renderer locks
through stale; and the fragments were close enough to OVERLAP, so `g_ddsd_bits`
(`lpSurface`) landed at +32 instead of +0x350.

An object whose DECLARED extent — the game's own array bounds times an element
size that is a language fact — contains other named addresses is now emitted once
with those names as offset aliases. **15 objects, 100 interior names, every case
in the image**; the list is a section of `gen/manifest.md`. A declaration whose
element type has no known size hosts nothing and keeps the tiling. The merged
block covers exactly what its tiled pieces did, so the pointer resolution and the
census are unchanged (272 exact + 441 interior + 0 gaps + 12 raw; 2145 globals
instead of 2232, same 3.7 MB).

There is no offset form of `__attribute__((alias))` and none in C at all, so an
interior alias is `.set name, host+off` module-level asm. The note above that
module-level asm does not survive the wasm backend is true of a `.text`
*function* alias; a wasm DATA symbol is a (segment, offset, size) triple, so an
interior label is exactly representable. One trap: a tentative definition is a
COMMON symbol under `-fcommon` and the assembler cannot resolve
`.set alias, common+off` at all — silently leaving the alias undefined — so a
zero-filled host block gets an explicit `= {0}`.

```bash
ninja -C portable/build legoland_tests && ctest --test-dir portable/build -R keystate
```

`portable/tests/test_keystate.c` (29 checks, both toolchains) is the proof: the
four DIK offsets, a 256-byte write that must not reach the next object, the
game's own `IsLShiftDown`/`IsRShiftDown`, eight offsets of the GPU block, and
`CheckHostSystemGPU`'s memset really clearing all of it. It is the one test with
no oracle — its expectation is the original's own layout.

**Naming a trap is one command now.**

```bash
python3 portable/tools/name_trap.py              # the whole WinMain spine
python3 portable/tools/name_trap.py -- --stages   # InitSession step by step
```

It builds `legoland_headless_debug`, runs it under node, and prints the host-call
trace tail, the named stack innermost first, and — when the innermost frame is a
`signature_mismatch:` stub — the two signatures, the file on each side, and every
`extern` declaration of that name in `LEGOLAND/*.c` with its line number. That is
the whole work item for a matching lane.

`legoland_headless_debug` is `legoland_headless` with `-O0 -g2` at LINK time and
nothing else changed, because link time is where this happens: `wasm-ld` creates
the `signature_mismatch:<callee>` stub whose entire body is `unreachable`, and
emcc's link-time `-O2` runs `wasm-opt`, which inlines a one-instruction body into
every caller — so the name is in the name section and no frame mentions it. The
game objects are shared with the optimised harness, so naming a trap costs one
link. (The browser target cannot do this: unoptimised ASYNCIFY of
`RunAppraisalScreen` exceeds wasm's per-function local limit. The headless
harness has no ASYNCIFY.)

**The node harness and the browser page now stop at the same instruction**, so CI
can run the spine without a browser. Both reach
`signature_mismatch:InitHostSystemGPU <- InitSession`: gpu.c:417 defines
`int InitHostSystemGPU(void)` and startup.c:38 declares it `void`. The live
prototype conflicts measured on the path the game takes, in the order it hits
them, are in the notes; `wasm-ld` warns about 133 candidates out of the census's
542.

```bash
ninja -C portable/build-wasm legoland_headless legoland_pathtest
ctest --test-dir portable/build-wasm -R 'headless_spine|install_paths'
```

* **`headless_spine`** runs `legoland_headless --stages loadsprite,rungame` and
  requires `--- done`: mount, 595 + 905 + 574 volume members,
  `GetString(0xcb) = "LEGOLAND ERROR"`, `InitHostSystemGPU` = 1, `InitScreen` = 1
  (640x480 RGB565), `InitInputSystem` = 1, `RES_OpenFile(".\graphics\erase
  it.lls")` = 1402 bytes, `LLIDB_LoadICM`, five menu registrations. The two skips
  are the two live prototype conflicts on that path; **delete each from that line
  in `cmake/headless.cmake` as a matching lane closes it**, which makes the test a
  ratchet.
* **`install_paths`** is `ll_host_resolve_path` over a PRELOADED MEMFS — the
  browser's filesystem, and the one the resolver had never run on. Every other
  node target uses `-sNODERAWFS=1`, which on this Mac is APFS, which is
  case-insensitive, so `stat("LEGOLAND.ICM")` succeeds first try and the
  case-folding code never executes (`ls gamedata/main/LEGOLAND.ICM` lists
  `Legoland.icm`). MEMFS is case-SENSITIVE whatever machine packaged it, and so is
  Linux CI on ext4. 20 checks over a four-file fixture cmake generates from
  nothing, so it needs no `gamedata/`: `LEGOLAND.ICM` -> `Legoland.icm`,
  `".\GRAPHICS\ERASE IT.LLS"` -> `Graphics/Erase It.lls`,
  `".\STRINGS\STAB.STR"` -> `stab.str` through the flattened-install fallback,
  `"d:\legoland.res"` -> the emulated CD, three paths that must NOT be invented,
  and `create=1` normalising only.

One thing to know about MEMFS: **`getenv` sees the host environment only under
NODERAWFS.** A page cannot be told `$LL_CD_DIR` through the environment, which is
why `src/browser/main.c` calls `setenv` itself.

Tests: native ctest 2/2 -> **3/3**, wasm32 ctest 4/5 -> **7/8** (`loadpos` is the
`RES_CloseFile` conflict, unchanged).

## CRT prototypes, indirect-call traps, CI and the title-screen gate (scope PORT-A4)

Four things, all of them about *telling you what went wrong*. Notes:
`docs/lanes/scope-port-a4.md`.

### 1. wasm-ld is silent: a CRT name is typed from libc, not from its alias

PORT-M2 left exactly one `function signature mismatch` in the tree and it was
the generator's. `DebugPrint` is a second name for 0x0049e5c5 = `printf`, and
`gen_link.py` declared the real symbol with the **alias's** signature, producing
`extern void printf(unsigned int)` against libc's `(i32, i32) -> i32`; wasm-ld
then sent every call through that declaration — the game's own `printf` calls
included — to a trapping stub.

A CRT name no object in the tree *defines* now takes its prototype from
`CRT_PROTOS`, a table of the libc signatures the game reaches. Three things make
it work:

* `crt_wasm_sig` adds **one extra i32 for a variadic callee**, because that is
  how clang lowers `...` on wasm32 (the variable arguments go in a buffer and
  its address is the last parameter). That is the whole reason libc's `printf`
  is `(i32, i32) -> i32`.
* `defined_here()` asks the *definition* map only. The older `def_sig_of` falls
  back to the signature the **references** voted for, so it answers for `malloc`
  with the game's declaration even though nothing in the tree defines `malloc`.
* `crt_alias()` bridges the two signatures with per-argument C casts and **never
  with a cast of the function pointer**: a cast call lowers to `call_indirect`,
  and binaryen's `directize` turns a constant-index one into a direct call whose
  types then disagree, failing validation far from the defect
  (`docs/lanes/scope-port-m2.md` §4).

Both spellings of a variadic alias are handled. `Format` → `sprintf` is spelled
variadic, so its last argument already *is* the varargs pointer and is forwarded
straight through (the callee declared flattened, under an `__asm__`-labelled
identifier so both spellings can coexist in one file). `DebugPrint` → `printf`
is not, so the call goes through the real variadic prototype and clang builds
the empty buffer.

Result: wasm-ld warnings **0** across all seven wasm targets, `wasm-opt
--all-features` validator errors **0** on all seven modules, 11 forwarders typed
from libc, 0 CRT targets without a table row.

### 2. Naming a `call_indirect` type mismatch

This is the next class the front end will hit, and it survives a completely
clean link: the type is an **immediate on the instruction**, not a property of a
symbol, so there is no declaration for wasm-ld to compare. node says

```
RuntimeError: function signature mismatch
```

and nothing else — no index, no caller, no types.

`name_trap.py` now reconstructs all of it. The key is that V8's
`wasm-function[N]:0xOFF` offset is the instruction's **byte offset in the module
file**, which is exactly the number `llvm-objdump -d` prints, so it names one
`call_indirect` whose type immediate is the type the slot was supposed to hold.
The workflow:

```bash
# 1. the trap, named automatically (the -O0 link is what keeps the frame)
python3 portable/tools/name_trap.py -- --stages

# 2. or explain one call site by hand, from a frame you already have
#    (a browser trap, someone else's report, a page that died in a tab)
python3 portable/tools/name_trap.py --at 0x266ea

# 3. what the table can hold at all: every signature and how many slots
python3 portable/tools/name_trap.py --table
```

and what it prints:

```
== INDIRECT CALL TYPE MISMATCH
   caller      TrackCurve_EvaluatePosition   (module offset 0x266ea)
   call site   call_indirect type 8 = (i32, i32, i32) -> void
== the call site, in order
    0x266de  i32.load	76
    0x266e3  i32.const	3
    0x266e5  i32.shl
    0x266e7  i32.load	0
    0x266ea  call_indirect	 8        <-- trapped here
== candidate targets: ... nearest type first
== by callback table: the global holding the wrong-typed bodies is what to re-declare
   g_track_desc_flat   (i32) -> void x3, (i32, f32) -> void x1, (i32) -> i32 x1
```

Three details worth knowing when you use it:

* **The candidate list is narrowed by `gen/globals.c`.** gen_link re-points the
  data words that hold a function address, so the set of functions a *static*
  game callback table can reach is exactly the set of `&Name` in the generated
  globals — 216 of the 1012 table slots' functions in the current build. A
  function nothing stores in `.data` cannot be in a slot the game loaded from
  `.data`, which takes the list from "the whole table" to something readable.
  When nothing in that set has the wrong type, the slot was written at runtime
  (`LLIDB_RegisterNewElement`, the `screen.c` tables) and the report says so and
  falls back to the whole table.
* **Candidates are ranked by type distance, not alphabetically.** Same arity and
  return with one parameter retyped first, because that is the shape of the real
  defect: a float passed as its bit pattern, or a struct flattened into ints.
* **"by callback table" is the actual work item.** One global holds a whole
  family of wrongly-typed bodies and re-declaring that table fixes every slot at
  once. The name shown is the *block's* head symbol (gen_link emits one block per
  object and the rest as offset aliases), so `gen/manifest.md`'s interior-alias
  table says which declared name the slot belongs to.

The same defect can also arrive as plain `unreachable`, because binaryen's
`directize` may rewrite a constant-index `call_indirect` into a trap before the
runtime sees the indirect call. `--indirect` forces the report in that case.
`-sASSERTIONS=2` and `-sSAFE_HEAP` are worth adding when the trap is *not* a type
mismatch (SAFE_HEAP catches the out-of-bounds load that produced a garbage table
index in the first place); they do not improve the signature-mismatch message
itself, which is why the static half exists.

`gen/manifest.md` grew the static counterpart: a **Cast forwarders** table of
every alias whose callers emitted a signature the real body does not have — 80 of
them, each a latent runtime trap of exactly this kind, with both signatures per
row.

### 3. The first present as a regression gate

`headless_spine` used to assert "the spine got this far". With PORT-B3's painters
in, it asserts **pixels**. A new `title` stage runs RunGame's own prefix down to
`ShowTitleScreen` — RunGame itself cannot be used, because four statements later
it enters `while (g_music_disabled == 0) { PeekMessageA; Sleep(100); }` and
nothing clears that flag while `DirectSoundCreate` reports no driver — then
checks the first present:

```
legoland_headless: first present 640x480, 301157/307200 non-black (98%),
                   frame checksum 0x4a092b01, 1 present call(s)
```

The non-black floor is 40%, deliberately far below the real 98%: the failure this
catches is "the painters drew nothing", never "40% of it". The checksum is an
FNV-1a over the frame's 16-bpp pixels, row by row so the pitch padding cannot
change it, and it is **pinned** in `cmake/headless.cmake` as
`LL_TITLE_FRAME_SUM`. A change that alters the title screen fails here by
design; if the change was intended, run

```bash
node portable/build-wasm/legoland_headless.js --stages rungame
```

and put the checksum it prints in `LL_TITLE_FRAME_SUM` in the same commit.
Setting it to `0` keeps the non-black assertion and only prints the checksum.

The skip list lost `loadsprite` (PORT-M2 closed the `RES_CloseFile` conflict that
poisoned `__BMPLoader`), so only `rungame` is left — the ratchet the comment in
that file describes.

### 4. CI builds the wasm32 tree

`.github/workflows/progress.yml` had one portable job, native clang — a
compile-and-link census on a target that cannot run the game. It now has a
second, `portable-wasm`, on the tree that can: `mymindstorm/setup-emsdk@v14`
pinned to 6.0.9 (the version this tree is developed against — keep it in step
with the Build section above, because a different emsdk can change the libc
signatures `CRT_PROTOS` is matched against), `emcmake cmake -DLL_ILP32=ON
-DCMAKE_BUILD_TYPE=Release`, all five targets linked, `node
legoland_linkcheck.js`, and `ctest`.

Two gates beyond "it builds":

* **No wasm-ld signature mismatch.** A mismatch is a *warning*, so the link
  succeeds and only the log says a call site is poisoned. The job tees the link
  output and greps it, which is what turns §1 into a ratchet.
* **The browser target must link.** `legoland_browser` is the one link that runs
  ASYNCIFY and binaryen's `directize`, so it is the one that catches an invalid
  module — the failure mode PORT-M2 documented, where a clean wasm-ld run still
  produces a module the validator rejects.

**Two things CI does not have**, and what follows from each:

*No `original/legoland.exe`*, so `gen_link.py` zero-fills every rebuilt global
instead of initialising it from the exe's `.data`. The link still closes and
nothing that depends on a shipped value is asserted.

*No `gamedata/`*, which needed two fixes:

1. `cmake/tests.cmake` used to `return()` at the top when `gamedata/` was
   missing, registering **nothing**. It now registers the asset-free subset and
   says in its configure output what it registered and why:

   | test | needs gamedata? | in CI |
   | --- | --- | --- |
   | `save_framing` | no — the oracle emits a synthetic script of framing operations | yes |
   | `keystate` | no — pure layout, checks gen_link's interior aliases | yes |
   | `rle_paint` | nine synthetic cases do not; the tenth decodes a shipped sprite | yes, 38 of 43 checks |
   | `anim_paint` | no — its own encoder, a synthetic type-2 frame | yes |
   | `install_paths` | no — the fixture is empty files generated at configure time | yes |
   | `tile_geometry` | the oracle samples a real level (`GLONE.MAP`) for its grid | no |
   | `res_archive`, `llidb_icm`, `loadpos` | yes, all three read the volumes | no |
   | `headless_spine` | yes — the mount, the string table and the title artwork | no |

   The four asset-reading tests are not merely unregistered but **not compiled**:
   their oracles cannot produce a header at all without the volumes.
   `LL_TESTS_NO_GAMEDATA` is what leaves them out of `ll_tests.c`'s subcommand
   table, so the driver's usage listing is an honest statement of what the build
   can run. `test_rle_paint.c` is the one hybrid: `tests.cmake` writes a stub
   oracle header defining `LL_RLE_NO_ASSETS`, and its `real_sprite()` case calls
   the new `ll_skip()` — a third outcome next to pass and fail, because a skipped
   check must not look like coverage.

2. `legoland_browser` bakes its assets in with `--preload-file
   $LL_GAMEDATA/main@/gamedata`, **unconditionally**, and emcc's `file_packager`
   fails outright on a directory that does not exist — so the browser target
   could not be linked in CI at all. The job creates an empty tree (one
   zero-byte `main/stab.str`) and configures `-DLL_GAMEDATA=<that>
   -DLL_PRELOAD_RES=OFF`, which links the real target against an empty `.data`
   file. The page cannot *run* from it and is not asked to. This is a workaround
   in the workflow rather than in `cmake/browser.cmake`, which is PORT-B's file;
   when PORT-B's WASMFS fetch backend (`scope-port-b.md` §6) replaces
   `--preload-file`, the step goes away.

Verified by replaying the job against a copy of the tree with no `gamedata/` and
no `original/`: 271 sources compile, all five targets link, 0 signature
mismatches, `legoland_linkcheck` resolves every symbol, ctest **5/5**
(`rle_paint` 38 checks with the tenth case skipped by name). The native job is
unchanged.

## The front end comes up: sound, the frame loop, and where input stops (scope PORT-B4)

Full notes: [`docs/lanes/scope-port-b4.md`](../docs/lanes/scope-port-b4.md).

**The game reaches its first front-end menu.** `PLAYER DETAILS`, the eight-slot
profile screen, renders complete — backdrop, artwork, the eight `EMPTY` captions
in the shim's own bitmap font, the pointer — and the game's own loop runs it at
**33.5 fps sustained**.

**`DirectSoundCreate` succeeds now**, and that is what unblocked the title
screen. The old failure was defended on the grounds that every *sample* entry
point is guarded by `g_samples_ready` — true, but `InitSoundSystem`
(lifecycle.c 0x004964f0) is not one: `if (!ok) return ok;` skips
`InitMusicSystem`, so nothing ever writes `g_music_disabled` and `RunGame`
(gamemain.c:370) spins on it for ever. **A failing sample system makes `-nomusic`
unreachable.** So `dsound.c` is a silent `IDirectSound` with the full
`IDirectSoundBuffer` vtable over a malloc'd PCM block. The part that could not be
stubbed is the **play cursor**: `KLIBAUDIO_LockAVISoundBuffer` (input2.c
0x004963f0) spins while the cursor is inside the window it wants to lock, and
`PumpNarration` (narration2.c 0x00498b40 — the first call in every `GameFrame`)
latches on the cursor reaching the next fill block while decrementing a counter
tested with `== 0`, so a frozen cursor drives it negative and never ends. Every
buffer therefore advances by wall clock times its byte rate, wrapping if looped
and stopping at the end if not. ole32's two DirectMusic imports live in the same
file; `CoCreateInstance` reports `REGDB_E_CLASSNOTREG`, which is the game's own
designed no-DirectMusic path.

**The loop is at its ceiling.** 33.5 fps is the 28 ms floor `FlipPrimary` and
`PresentFlip` spin on (35.7 Hz), so nothing in the shim stalls it and PORT-B's
ASYNCIFY design needed no change under the real game.

**The page takes a command line.** `?args=-nointro+-nomusic+WINDEBUG` becomes
`Module.arguments` → `lpCmdLine`; the parameter being *absent* is what selects
`main.c`'s default, so an empty `?args=` means "no switches". `?trace=1` also
shows fps now, rolling and average, over *presented* frames.

**Input is delivered and ignored.** `dinput.c` traces the mouse state reads that
carry something: holding a button for 30 presented frames gives exactly 30 state
reads, and a (+199,+200) move arrives whole. The front end acts on none of it —
the frame is byte-identical over 3,700 frames. Ruled out: the `g_screen`
clamp (the image really does hold 640x480), `g_game->in_game`, the `Controller`
layout, a second `GetDeviceState` caller, an unported asm body, a stale present.
What is left is the chain `UpdateControllerFromMouseData` → `ReadGameButtons` →
`g_input.point`, and the shape to suspect is a live global resolving to two
objects — the defect class PORT-A3 found with `g_key_state` and `g_gpu_state`.
Notes §5.

**Blockers handed on**: `CreateThread` refusing means the music-*on* command line
still hangs (PORT-A, kernel32.c — one caller in the tree, and running the start
routine inline is measured to work); `_findclose(-1)` traps the module (PORT-A,
msvcrt.c:204 — `if (handle == -1 || !f) return -1;`); 41 unwritten GAME functions,
of which `ODFError`/`ObjDefFinalize` (llidb_odf.c), `WindowProc` (screen.c) and
eight in `screens3.c` are the front end's (matching lanes); and `gen_link.py`'s
trap helper has no log-and-continue mode, which is what hides nine blockers
behind the first one.

## Input reaches the game, music-ON runs, traps can continue (scope PORT-A5)

PORT-B4's three blockers and its open question, closed. Notes
`docs/lanes/scope-port-a5.md`.

### 1. THE INPUT QUESTION: a 164-byte record was nine objects

The front end ignored every key and click over 3,700 frames while the shim
delivered state every frame. It was PORT-A3's class of defect and the biggest
instance of it in the image.

`gen_link.py` sizes a global by the gap to the next NAMED address unless the
game's own declaration gives a computable extent (array bounds times a primitive
element size). `extern GameInput g_input;` gives none -- a struct type has no size
until somebody writes the struct out -- and for a record whose fields other files
declare individually, the next named address is **its own second field**. So
`GameInput` at 0x00813a40 came out as nine separately 16-byte-aligned blocks and
the two halves of the game addressed different memory:

```
bighelp.c ReadGameButtons   g_input.point.x = g_controller->x   -> g_input + 4
screens3.c (the front end)  g_gfx_point (+0x04), g_mouse_buttons (+0x84)
                                                                -> two other objects
```

The cursor point alone is read under four names in 22 files against six that
write it as `g_input.point`.

`STRUCT_EXTENTS` in `gen_link.py` is the extent for records whose type is a
struct. A row is only allowed when the sources pin it **twice** -- the struct's
last field offset and an independent `extern` at that field's own address whose
comment gives the same number -- and both citations are in the row. The sweep for
candidates is every extern whose address comment attributes it to a record:

```
grep -hoE "/\* 0x00[0-9a-f]{6} +[a-zA-Z_]\w*\.[\w.]+" LEGOLAND/*.c | sort -u
```

Three records, all split, all on the front-end path: `GameInput` 0x00813a40
(4 -> 176 bytes, 12 interior names), `PopUpUI` 0x007fdea4 (4 -> 380, 72 -- every
popup icon, `g_popup_x`/`g_popup_y`, `g_info_active`), `Profile` 0x007cad60
(30 -> 288, 6 -- the PLAYER DETAILS scratch profile). All three are all-zero in
the image, so globals.c's BYTES are unchanged: 2149 -> 2092 definitions,
3705508 bytes both times, merged objects 15 -> 18 (100 -> 190 offset aliases).

**The sweep is not exhaustive**: it finds records a source documented with a
`g_owner.field` comment. The general fix is for the extent to come from `sizeof`
of the struct definition, which is a small C parser nobody has written yet. Until
then a split record is the first thing to suspect for any "the game ignores X".
And the optimizer hazard A3 documented grows with every merge -- `gameframe.c`
declares both `g_input` and `g_gfx_point`, `popup.c` both `g_popup` and
`g_input`; nothing measured misbehaves, and `volatile` on the lvalue is the fix
if it ever does.

### 2. `legoland_headless --probe-input`, and the ctest

One synthetic gesture through the shim's own injection points straight into the
game's chain, printing what every link holds -- and the record's layout first,
which is pure address arithmetic and is what found the bug:

```
LL_CD_DIR=$PWD/gamedata/disc LL_DATA_DIR=$PWD/gamedata/main \
  node portable/build-wasm/legoland_headless.js --probe-input

  g_gfx_point +0x4  expected +0x4  OK    ... the record is ONE object
  after UpdateController   controller x=440 y=300 dx=120 dy=60 buttons=0x201
  after ReadGameButtons    g_input.point=(440,300) btn0.state=0x5
                           | READERS: g_gfx_point=(440,300) g_mouse_buttons=0x05
  INPUT OK
```

Against the pre-fix closure the same probe reports `g_gfx_point` at +0x10,
`g_fp_w` 958,336 bytes from its field, and READERS reading (5,4) where
ReadGameButtons wrote (440,300). Registered as the ctest `probe_input` (wasm
ctest 10 -> 11; it needs `gamedata/`, like `headless_spine`).

### 3. `CreateThread` runs `MusicThread` inline: music-ON no longer hangs

`g_music_disabled` is not "music is off" -- MusicThread sets it on every failure
exit AND on its success path (musicthread.c:678), meaning "the thread has
finished starting up". `RunGame` waits for it, and a refused `CreateThread` meant
nothing ever wrote it. The start routine now runs **inline, to completion, on the
main stack**, with a `setjmp`/`longjmp` escape from a wait nothing can satisfy.
With ole32 failing by design the routine takes its first `shutdown:` rung after
273 of its 3,161 instructions and returns, so `-nointro WINDEBUG` with no
`-nomusic` now reaches the front end. A per-frame pump cannot host this routine:
it is one function with an infinite message loop, not a step function, so it needs
an ASYNCIFY fiber the day DirectMusic becomes real -- and on that day the escape
fires and says so. Details and the full reasoning: kernel32.c's threads section.

An event wait is now a POLL: `WaitForSingleObject(event, 0)` answers
`WAIT_TIMEOUT` when nothing posted it, because MusicThread's message loop is
built out of two of them and answering "posted" made it act on commands nobody
sent. Mutex and thread handles are unchanged (always free), which is what
startup.c's one-instance check wants.

### 4. `LL_TRAP_CONTINUE=1`: every blocker in one run

A generated trap is usually a logging call on ordinary data (`ODFError`) or a
subsystem the port does not have (AVIFIL32), so exiting at the first hides every
other. `LL_TRAP_CONTINUE=1` (read once, de-duplicated per symbol, default
unchanged) makes the helper print and RETURN; `name_trap.py --continue` sets it
and lists every trap in the order the game hit them. One run now enumerates nine:
`ODFError`, `ObjDefFinalize`, `lrintf` (PORT-M4's) and six AVIFIL32 entry points
(PORT-B's stubs). A returning trap hands its caller a zero it never computed, so
the mode makes a LIST OF WORK and never the claim that something works -- say
which mode produced a result.

`name_trap.py` also makes `--cd-dir`/`--data-dir` absolute and checks them: the
harness chdir()s to `LL_DATA_DIR` before `WinMain`, so a relative path failed with
a bare message and `LL_CD_DIR` then resolved against the new cwd and silently
named nothing, which looks like a missing CD.

### 5. `_findclose(-1)`, and the CRT's two failure conventions

profiles.c's `Goto_ProfileDir` closes a failed find handle unconditionally, as
shipped; `-1` cast to a pointer is not NULL, so `if (!f)` missed it and the module
died with `memory access out of bounds` on the first front-end frame. Fixed in all
three `_find*` entry points through one `ll_bad_find` test. The rest of msvcrt.c
is audited in a comment there: the fd family carries -1 into POSIX, which answers
EBADF, so it was already right; `_msize(NULL)` and `_strupr(NULL)` were unguarded
dereferences where MSVC answers and are guarded now; `_stat`/`_access`/`_unlink`
are not defined and no game source calls them.


## A trap-free page, and the front end answering the mouse (scope PORT-B6)

**Zero generated host traps, and a click that changes the screen.** The page no
longer needs `?trapcontinue=1`: every Win32 import the program names now has a
real body, and the six AVIFIL32 traps PORT-A5 left are gone.

`portable/src/hostwin/avifil32.c` — all sixteen AVIFile entry points.
`AVIFileOpenA` reports `AVIERR_FILEOPEN`, which is the path movie.c and
advisor.c are *written* for: `OpenMovie` returns 0 and `PlayMovie` retries the
second prefix and returns without entering the player at all, and the advisor
degrades to no animation because `RenderAdvisorIcon`'s only draw is behind
`if (g_vidanim)` and that stays null. The rejected alternative (a synthetic
zero-length stream) is argued in the file's header. Every entry point is
pointer-blind, because `StartAdvisorClip(NULL)` reaches two of them with a word
read out of a null struct.

`portable/src/hostwin/msacm32.c` — the ACM, and **not** a stub: `data2.c`'s
`CreateSampleFromWAV` runs every sample in the archives through
`ConvertWAVToPCM` and drops the ones that fail, so a shim that refused
everything would break the sample loader rather than silence it. PCM to 16-bit
PCM is a real conversion (including the 8-bit unsigned to 16-bit signed
widening); ADPCM is refused with `ACMERR_NOTPOSSIBLE`, which every caller
handles.

`gdi32.c` gained WINSPOOL's `EnumPrintersA`, the last generated host trap.

### Input, end to end, measured in a tab

Three host defects, each found by driving the real page and each proved with a
before/after canvas hash:

* **the press latch** (`dinput.c`). `GetDeviceState` is a poll; the browser is
  an event source; `ll_host_drain_events` empties the whole queue from inside
  it. A click whose down and up landed in one drain collapsed to "up" and the
  game read `rgbButtons[0] == 0` every time — `buttons=000` on every trace line
  for a click that demonstrably reached the canvas. A press is now held until a
  poll has reported it and that poll's frame has been presented, which is what
  `ReadGameButtons` needs to see an edge; a 250 ms wall-clock escape covers the
  loops that poll without presenting. A human click is unaffected. Keys get the
  same latch.
* **the live-surface registry** (`ddraw.c`). `ll_host_surface_pixels` answered
  "is this HDC mine" by dereferencing it, which is a hard out-of-bounds trap for
  the gdi32 cookie `CreateCompatibleDC(0)` hands `HTBubbleHelp`. Surfaces are
  now registered and the lookup follows nothing it does not own.
* **`LL.dikOf`** (`ll_canvas.js`). `KeyboardEvent.code` is optional in practice
  — a virtual keyboard, an IME, a script `dispatchEvent` and the DevTools
  protocol all deliver `{code: "", key: "A"}` — and `LL.dik[""]` is undefined,
  so every such keystroke was dropped in silence. `key` and `keyCode` are now
  fallbacks.

### Page tooling

`window.llFrameHash()` (FNV-1a over the whole canvas — the sampled headless
checksum calls two different front-end screens equal), `llNonBlack()`,
`llSnapshot()` as a data: URL (a download button cannot work; the sandbox makes
`<a download>` and script-driven saves inert), `llTrace(n)`, `llStats()`, a
collapsible panel holding only the last 300 `HOST`/`[hostwin]` lines, and the
frame hash on the status bar every two seconds.

### Screens reached

PLAYER DETAILS (`0x9d9b7c50`, 33.5 fps, zero traps), its bubble help under the
cursor, and the NEW PROFILE popup (`0x5ab7ca10`) from one real left click.

**Two more split records block the rest**, both proved by patching the generated
closure and relinking, both PORT-A's (`gen_link.py` `STRUCT_EXTENTS`, one
twice-cited row each in `docs/lanes/scope-port-b6.md` §6): the 12-byte mouse-hit
record at `0x004bdd00` is three objects, so no front-end icon can ever be
focussed; and the 270-byte `CurProfile` at `0x0080ffa0` is eight, so the
new-profile name editor is never called.
## Object extents computed from the sources (scope PORT-A6)

The extent of a global is `sizeof` of the type the game's own source declares
it with, and `portable/tools/cdecl.py` computes it: a C declaration parser over
`LEGOLAND/*.c` and `*.h` that reads the `typedef struct` definitions (nested,
anonymous, unions, arrays of them, function pointers) and lays them out as MSVC
does on x86 — `#pragma pack` honoured at each member's own line, ILP32 sizes,
`long` and every pointer 4 bytes.

This closes the class that had bitten five times. `gen_link.py` sizes every
global by the gap to the next NAMED address, and for a record whose fields other
files declare individually that gap is *its own second field*: the record came
out as one block per field and the half of the game that writes it addressed
different memory from the half that reads it. Each instance was found by a
symptom and then fixed by a hand-written row in `STRUCT_EXTENTS` —
`g_key_state`, `g_gpu_state`, `GameInput` (no input at all, A5), `PopUpUI`,
`Profile`, the 12-byte hit record (nothing clickable, B6) and `CurProfile` (no
name editor, B6). **That table is now the REGRESSION FIXTURE for the parser**,
not the mechanism: all five rows are reproduced from the sources, they are kept
as a floor so a parser regression cannot silently re-split them, and the ctest
`cdecl_extents` (asset-free, both toolchains, runs in CI) fails if one stops
being reproduced.

`gen/extents.md`, new next to `gen/manifest.md`, is the whole class in one
table: every object whose computed extent exceeds the gap-tiled size — i.e.
every object that would have been split — with the file:line of the declaration
and of the struct definition, what the interior-alias pass did with it, every
interior alias with the FIELD its offset lands on (`g_mouse_buttons+0x84
(btn0.state)`), the addresses two translation units size differently, and the
residue the parser still cannot size.

Measured, both toolchains, clean dirs: objects merged into one block **18 ->
44** (27 newly merged), interior offset aliases **202 -> 307**, globals defined
**2125 -> 2042**, data aliases **338 -> 316**, and **3705868 bytes of image both
times**. Every initialised byte and every pointer TARGET ADDRESS in the
generated `globals.c` is identical before and after. Two of the new merges are
front-end state nobody had reported: `FrontEndState g_front` at `0x0080ff80`
(`SaveFrontEndState` copies twelve bytes out of what was a four-byte object,
while `g_cur_screen` and `g_screen_mode` live at +4 and +8) and `EditMode` at
`0x008119b0` (`g_game_mode` at +4).

One fix had to come with it: which words of a block are POINTERS was a leading
COUNT, so a pointer field reached through an INTERIOR name would have kept the
original binary's address. It is now the set of word indices the declarations
give, gathered from the host's names and from every interior alias at its own
offset; the re-pointing counters are unchanged (299 at a symbol, 441 into a
block, 12 raw).

`linkreport.py`'s extern scan is statement-oriented at last (PORT-M4 section 6):
it reads to the `;` that ends the declaration, brace-aware, takes the last
identifier outside any parameter list or array bound, handles `extern int
(*g_present)(void);` and `extern char g_key_prev[KEYMAP_COUNT];`, scans
`LEGOLAND/*.h`, and ignores the word `extern` inside comments. Externs found
5301 -> 5354 with **zero** names changing address or class, and the census's
unclassified count stays at 4 — all four the toolchain's own.

`python3 portable/tools/cdecl.py --overlaps` is PORT-A3's optimiser-hazard
sweep as a command: the pairs one translation unit declares whose storage
overlaps, with the functions that touch both and the direction of each access.
372 pairs, 167 of them written through by a single function, 61 in a front-end
TU; nothing measured misbehaves at `-O2` and the recipe for a matching lane is
in `docs/lanes/scope-port-a6.md` §7.

Notes: `docs/lanes/scope-port-a6.md`.



## Typing a player name, and the one call that kills the front end (scope PORT-B7)

**The name editor works.** With PORT-B6's two split records merged, a click on
slot 1 of PLAYER DETAILS opens the NEW PROFILE popup, `EnterNewProfile`
(screens2.c 0x00491bd0) runs every frame, and real key events become characters:
`GetInputChar` decodes the DirectInput key array through the 59-entry DIK map at
0x004bad58, the letters land in `g_temp_profile.name` — read back live out of
the game's memory — and are drawn in the blue field with the blinking cursor
after them. Backspace removes one. **Nothing in the shim had to change**: the
DIK table, the press latch and the keydown/keyup pairing were already right, and
the game wants scan codes rather than `WM_CHAR` (the one `WM_CHAR` it reads is
backspace, which `user32.c` already synthesises).

### Driving the page

The page now carries the driver, in GAME pixels, because the browser-automation
pointer and key verbs do not reliably reach this canvas (measured: a batch of
correctly-mapped clicks produced zero canvas events):

```js
await llMove(320, 240);   // REQUIRED FIRST -- the pointer is relative and the
                          // game's cursor starts at the canvas centre
await llClick(260, 188);  // open the NEW PROFILE popup on slot 1
await llType('adam');     // one held KeyboardEvent per character
await llClick(505, 345);  // the Accept icon
```

plus `llAlive()` (frames presented in half a second — 0 means the game is not
running, which is a different fact from "the screen did not change"),
`llPeek()` (seven of the game's own globals, through `main.c`'s `ll_dbg_addr`
export) and `llStats().dead`. **An unhandled `RuntimeError` in the
ASYNCIFY-resumed loop now reaches the TRAP banner**: it used to be swallowed, so
a dead module looked exactly like a live one that ignored input — and that
misreading has now cost three lanes an hour each.

`legoland_browser_named` (`ninja -C portable/build-wasm legoland_browser_named`)
is the same optimised link plus `-g2`, so the module carries a name section and
`name_trap.py` can name the CALLERS of an indirect-call type mismatch.
`legoland_headless_debug` cannot stand in for it: these are only reachable by a
click.

### The blocker: one vtable slot declared two ways

About 0.4 s after that click the module dies with `function signature mismatch`
in `UnreferenceSprite` ← `FreeCachedTextEntry` ← `ExpireCachedText` ←
`RenderingComplete` ← `GameFrame`. `spritemisc.c:23-26` declares the sprite
owner's vtable slot +0x08 as `void __stdcall Destroy(Owner*)`; the owner is an
`IDirectDrawSurface` and that slot is its COM `Release`, which `sprite2.c:50`,
`printlist.c:100` and `gpu.c:118` all declare `long`. x86 ignores the result;
wasm checks the type at the call. **A matching lane's one-line fix**, with the
recipe and both citations in `docs/lanes/scope-port-b7.md` §4.

With that slot corrected in a throwaway build, the walk goes four screens
further: the front-end **main menu** (the LEGOLAND gates, seven bubbles), the
**"Select tutorial level"** notepad, and the **park-advert screen** (CALIFORNIA
/ WINDSOR / BILLUND, whose movies do nothing because the AVI shim refuses them,
exactly as designed). Every one of them runs at the 33 fps flip floor. The park
itself was not reached.

## The wedge named: 1211 raw pointers in the closure (scope PORT-B8)

PORT-B7 left one blocker it could not open: two clicks past the main menu the
page's main thread stops responding, and every tool the port has for asking it a
question -- `llFrameHash`, `llStats`, `llPeek`, `javascript_tool` -- runs on
that thread and times out with it.

**The heartbeat** (`?beat=<ms>`, `ll_host_beat` in user32.c) is the channel that
survives: `stderr` reaches the devtools console over CDP without the page's main
thread doing anything, so one line written from C inside the loop still arrives.
It beats every host entry point a spin loop could sit on, prints a timed line
with the call counts, and -- the part that matters -- dumps a **32-entry ring of
the DISTINCT entry points on every wrap**, not on a timer, so the last line a
wedged tab prints always ends within 32 host calls of the loop. The front end
makes ~360,000 host calls a second; a timed line could never do this.

The answer: **the loop makes no host call at all.** It is

```c
/* screens3.c:1320  KillLowMarkerSprites */
while (KillSprite(g_low_markers[i].lit) == 0)
    ;
```

with `.lit` NULL, because the sprite's NAME pointer was never re-pointed --
`gen-browser/globals.c` emits `g_low_markers` with the raw x86 address
`0x004bed40` where `"Appraisal_Yes.lls"` should be, and that string is *inside
the same 600-byte object*, at `+0xa0`, with no symbol of its own for the
re-pointing pass to aim at (PORT-A6's extent widening swallowed it). So
`LoadSprite` fails, the marker is null, `UnreferenceSprite` answers 0 for ever,
and the page dies. `g_level_markers` has the same 18 words wrong. Those two
tables are **both doors into the park**.

A census of the whole closure (`docs/lanes/scope-port-b8.md` §9, the script) says
the class is **1211 words in 177 objects** -- `g_fp_table` (135), `kThemeSame`
(102), `g_level_db_sections` (91), `g_power_table` (63), `g_lowlevel_ai` (59)
among them, all tables the park needs. **Owner: PORT-A, `gen_link.py`**; the
rule is that a pointer word whose target has no symbol but lies inside an
emitted object's extent becomes `(unsigned)((char*)<obj> + <offset>)`.

Also settled: the profile really is written (`/gamedata/profiles/Profile1.txt`,
272 bytes, `adam` at offset 0) and read back within the session, so the
`"cannot open output file"` lines are just `ScanForProfiles` probing the seven
absent slots in READ mode; and PORT-B7's second wedge (the advert screen's Go
Back) does not reproduce on an honest build. MEMFS still loses the profile on a
reload -- IDBFS is the next PORT-B job.

## A pointer FIELD is a pointer word: the park's doors open (scope PORT-A7)

PORT-B8 handed this one over measured: 1211 words of the generated closure still
hold RAW x86 addresses, thirty of them the progress screens' sprite names, and
the game hangs on both of them.

**The rule that was missing.** `gen_link.py` decides which words are pointers
from the game's own declarations -- never from the value, because a
three-character string at the end of a word is an in-image number and a
value-only rule gets 1,209 of 1,650 wrong. But it only ever read the TOP-LEVEL
declaration's pointer depth, so

```c
extern LevelMarker g_low_markers[5];      /* 0x004beca0 */
```

contributed no pointer slots at all, and the ten `const char*` MEMBERS of those
five records kept the original binary's addresses. PORT-A2 knew ("pointer
members of struct arrays are still raw") and PORT-A6 built the machine that
answers it -- `cdecl.py` lays these structs out -- but was only ever asked for
their `sizeof`.

`cdecl.pointer_offsets(ty)` walks the laid-out type, through nested aggregates
and every element of every array, and the rule becomes: **a word is a pointer
slot when some declaration's type puts a pointer FIELD at that ADDRESS.** By
address, not by an offset into whichever block is emitted -- `g_level_markers`
is declared at 0x004beb80 by `screens3.c` and, eight bytes in with its fields
rotated to match, at 0x004beb88 by `bigscreens.c`; the block lands on the second,
so its pointers sit at +0x14/+0x18 of each record and record 0's two names live
in the tail of `g_mode_wplus`'s block.

The value keeps one vote, and only ever a veto: a declaration that claims a
pointer where the image holds something that cannot be an address
(`void* g_music_sys` holding 1) is rejected for that array ELEMENT and reported.

```
interior re-points  441 -> 693        (+252: g_fp_table 130, g_build_followups 46,
                                       g_level_markers 18, g_low_markers 10, ...)
every other cell    0 of 21,368 differ, 2042 objects and 3,705,868 bytes unchanged
B8's value census   1211 -> 959       (the residue is string TEXT, by construction)
```

**`gen/pointers.md`** is the new artifact and **`raw pointer words` is the new
gate**: a declared pointer word, inside the image, that nothing re-pointed and
nothing can explain. It must be 0, and the ctest **`pointer_words`** (asset-free,
both toolchains) fails the build if it is not. Words left raw on purpose are
listed with a reason -- a pointer into `.text` is a function-table index on wasm,
and a word whose value is not an address is a declaration to fix (there are 12,
all a `void*` over a count or a flag; the list is in
`docs/lanes/scope-port-a7.md` §6).

**In a tab:** the tutorial screen now draws the tick beside Lesson 1 (that
sprite IS `Appraisal_Yes.lls`), and the click that wedged the page for two lanes
-- `Accept_On_Report` at (577, 419) -- returns in **827 ms** into the in-game
screen, money bar and park toolbar drawn, 34 fps, no trap. The other door
(577, 300) answers in 824 ms. What does NOT happen yet is the park RENDERING:
the map area keeps the previous screen's backdrop and the frame makes two blits
where the front end made twenty-five. That is the next blocker and it is not in
the closure.

## The park was never loaded: a 93-pair table declared as one pointer (scope PORT-B9)

A7 left the in-game screen drawing its HUD and not its map, and called it a
rendering blocker for a PORT-B lane. **It is not a rendering defect.** The
DirectDraw shim, `RenderView`, `RenderGroundLayer` and `PaintTileLayer` all run
correctly, on every frame, over a map whose **36,864 cells are entirely zero** —
and `render4.c:430` paints nothing for a cell whose tile is 0, so the terrain
pass writes no pixel and the map area keeps the previous screen's backdrop.

**Why `?beat=` could not see it.** The map renderer draws in SOFTWARE between
one `Lock` and one `Unlock`. A frame that paints ten thousand tiles and a frame
that paints none make *exactly the same host calls*. A7's "the host ring goes
byte-identical-quiet after the level loads" was true and was never evidence
about drawing. The witness has to be the game's own memory, so this lane grew
`main.c`'s `ll_dbg_addr` table from 7 globals to 60 — everything
`renderview.c:150`'s "HOST / BROWSER-RENDERER CONTRACT" block lists as
`RenderView`'s input, the globals that say whether its ground pass RAN, and the
level loader's state — behind `window.llRender()`.

**The root cause is one declaration.**

```c
/* movie.c:239 */
extern const void* g_level_db_sections;   /* 0x004bb6f8 93 {keyword, handler} pairs */
```

A 744-byte table of 93 pairs, declared as a single `const void*`. PORT-A7's rule
— a word is a pointer slot when some declaration's type puts a pointer FIELD at
that address — is therefore never asked about words 1..188 of that object. The
handler halves survive (an exact function address goes through the symbol path);
**all 92 keyword-string halves keep their raw x86 VAs**. `ParseKeywordSections`
then does `strcmp(words[0], table[i].keyword)` against a wild address, never
matches, and every level script parses to nothing: `MAP "one"` never fires,
`LoadBaseMap` never runs, the map is never filled.

This is the class A7 closed, in the shape its census could not see. A7 §5's
residue test asked "is this word past the DECLARED EXTENT of its object" and
answered yes for all 188 — correctly, the declaration is four bytes long — and
concluded "so nothing describes that storage, so it is not a pointer". Right
premise, wrong conclusion for a pointer table. A7's own residue list names it:
`g_level_db_sections 91`.

**Proved twice, live in a tab.** `window.llFixKeywordTable()` translates the 91
raw VAs through the object containing each and writes the linear address back —
the arithmetic `gen_link.py`'s interior-of-block rule would have done at build
time. All 91 then read as real keywords, and the very next park load runs
`LevelKw_MAP` → `LLIDB_FindElement("one")` → `LoadBaseMap`, which reads
`ONE.MAP`'s header (**the map header changes from the `.data` default 192×192 to
84×84**), the tile-set mapping (`g_default_tile` 1 → 32) and the terrain
element. And from the other side, writing `g_default_tile` into every cell's
tile field makes the **2:1 isometric terrain grid fill the whole 640×340 map
area on the next frame**, with no shim change at all — the software renderer,
the locked surface, the `Blt` and the canvas all work.

**The first thing behind it was PORT-M7's**, found here independently and from
the other end: `llidb_odf.c:294`'s `obj->fa4(obj->elem)` is reached for *every*
OC_USEDLL class, because no `.dll` ships anywhere in `gamedata` and
`LoadLibraryExA` is refused — and the slot held a `() -> void` cast forwarder.
M7 merged mid-lane and took cast forwarders 72 → 0; merged in here and the trap
is gone.

**What is in front of the park now is a THIRD class, and it is new.**
`sweep3.c:162`'s `SetStandardCallbacks` writes five **hard-coded function
addresses as integer literals**:

```c
p->f3 = (void*)0x45efe0;   /* AddBasicObject -- PutObjOnMap's cb_add */
```

These are literals in the CODE, not pointer words in `.data`, so `gen_link.py`
can never see them: the original is `mov dword ptr [ecx+0x98], 0x45efe0`, an
immediate that *is* the function address, and the recovery spelled the immediate
rather than the symbol. On wasm a function pointer is a table index and
4,517,856 is not one, so the first perimeter object of every level dies with
`table index is out of bounds` in `PutObjOnMap`. **29 sites in three files**:
`sweep3.c:164-168` (5, every ODF class), `loaders.c:161-191` (21, the BOATING
SCHOOL family's built-in GetInterfaces — the file's own comment says "referenced
by address only"), `coaster10.c:380/475/568` (3, `.data` addresses rather than
`.text`). The fix is the usual guard, taking `&Fn` instead of the literal.

**A7-2 is localised and still open after M7** (M7 fixed `mapscreen.c`'s
IconHandler, a different slot): it is the **MAP** button specifically — the
other five toolbar buttons all work — and it is `RenderFullMap` declared
`(void)` (`mapscreen.c:115`/`:117`, `renderview.c:2935`) stored in
`sprite2.c:199`'s `void (*)(SpriteRec*)` slot and called at `sprite2.c:253`.

**A7-3, measured and dismissed.** The one-byte reads are the game's own
`ReadLine` (`levelkw.c:865`) through the completely unbuffered `RES_ReadFile`
(`res.c:39`) — not msvcrt, not ddraw. One park load: 1194 host `ReadFile` calls,
**1153 of them one byte** (96.6% of the calls, 0.55% of the bytes). Priced
against MEMFS at **0.07 µs per read**, that is **0.08 ms of an 822 ms load**.
Recommendation: do nothing.

**B5 is closed.** PORT-A7 §8's IDBFS patch applied (`-lidbfs.js` on both browser
targets, the mount-and-`syncfs` block replacing `main()`'s bare `mkdir`, a 5 s
flush timer, plus the `<emscripten/eventloop.h>` include A7's patch did not
mention). `Profile1.txt`, 272 bytes, and **`adam` in slot 1 after a page
reload**.

**In the park**: 33.4 fps over 77 s, 0 traps, `g_sim_frame` advancing exactly
one tick per presented frame, arrow-key scrolling working in all four directions
(and `g_ground_last_x/y` following it, so the terrain pass really does re-run for
each new scroll position), a map-area click answering. Nothing animates, because
there is nothing in the park. Full notes and the blocker table with owners:
`docs/lanes/scope-port-b9.md`.

## Declarators, attributed manifests, and two sweeps promoted (scope PORT-A8)

The generator-and-tools lane. Nothing here is game code; the theme is that a
tool which is *almost* right is worse than one that is obviously wrong, because
its output gets believed.

### 1. `cdecl.py` reads declarators, not a flat shape

A C declarator is recursive and `cdecl.py`'s was not: it returned
`(pointer depth, name, array count)`, which cannot express `int (*pairs)[2]` at
all. It read that as `int *pairs[2]` — an array of two pointers instead of one
pointer to an array of two ints — so `MeshDesc` came out **0x28 instead of
0x1c**, every field past `pairs` moved, and three pointer words were invented
out of the floats past the end of the record (PORT-M8's M8-3).

The parser now builds a declarator tree and evaluates it by the language's own
outside-in rule (`* D` → pointer to T, `D[N]` → array N of T, `D(...)` →
function, `(D)` → D), so the identifier's type is whatever is left when the walk
reaches it. That single distinction — which derivation is applied *last* — is
the whole bug: in `int (*p)[2]` it is the pointer, in `int *p[2]` it is the
array. On ILP32 "pointer to T" collapses to four bytes, so the tree only has to
get the order right, never the target type.

`--selftest` covers all of it, sizes **and pointer offsets**: `T (*p)[N]`,
`T (*p)(args)`, `T (*p[N])(args)`, `T *p[N]`, `T (*p)[N][M]`, `T (**p)[N]`,
`T (*p[N])[M]`, `T *(*p)[N]`, each as a struct member and as an object's own
declarator, plus `T *f(void)` and `T (*f(int))(void)`, which are FUNCTIONS and
must not be mistaken for objects. Pointer offsets are tested as well as sizes
because an invented pointer word is the worse half: `gen_link.py` would re-point
a word the image holds a float in.

It changed exactly one thing. Over all 4,816 object declarations in the tree,
one line differs (`g_track_mesh` 0x28 → 0x1c, six pointer offsets → three);
`gen/extents.md` changes in that row and its counters, `gen/aliases.c` is
byte-identical, and `globals.c` — compared as {absolute address → initialiser}
rather than as text, because re-splitting a block moves words without changing
them — is **identical on both builds**, 22,498 elements on wasm and 86,596 on
native. The 496-byte `g_4b5f60` becomes 32 bytes and
`g_support_shadow_templates` becomes its own 464-byte object at 0x004b5f80
instead of an interior alias at +0x20, which is correct: +0x20 was never inside
`MeshDesc`.

It also recovered three real pointers. Claims are corroborated per array
element, so the two invented words holding `0x3f800000` condemned the whole
group and the three genuine pointers at +0x10/+0x14/+0x18 went with them.
`declared pointer words` therefore goes **up** (7538 → 7541) and
`pointer claims the image rejects` falls from 7 words in 2 declarations to 1 in 1.

### 2. The manifest attributes its own findings

Two lists used to be counts and bare names, which told a reader that something
was wrong and nothing about what:

* **The 11 conflicting wasm signatures** are now a table: the body's signature,
  the file that defines it, the disagreeing spelling with the file that emitted
  it, and the declaring `file:line`. Every one is DEFINED in the objects, so the
  body settles it, and every one is a game-side declaration — two clusters, not
  eleven problems. `screen.c` declares seven ObjDef callbacks with an empty
  parameter list where the body takes two or three arguments (:588, :598, :662,
  :663, :865, :877, :884), and four icon-input callbacks are spelled
  two-argument where the body is four (`bigscreens.c:904`,
  `bighelp.c:525/526/528` — PORT-M8's M8-4 class, at four sites M8-4 did not
  name). All eleven are address-taken only, so nothing traps today.
  `linkreport.wasm_sig_conflict_detail` reports the vote and
  `linkreport.extern_decl_sites` finds every declaration of a name with its line
  (the opposite of `scan_sources`, which keeps one address per name).
* **Words left raw** get a verdict column from `PTR_VERDICTS` in `gen_link.py`:
  an address, the lane that read the code, and what it concluded. The one
  surviving row is `g_vwin32`, which PORT-M8 §4g proved is not a defect — a
  Win32 HANDLE holding `INVALID_HANDLE_VALUE`. The manifest line now reads
  `rows still waiting for a verdict: 0 raw + 0 rejected`, and a future row with
  no verdict says `OPEN` in its own table instead of hiding inside a count.

### 3. `portable/tools/extern_sweep.py` — a class no byte gate can see

PORT-M6 §1f. One `extern` statement, several declarators, several addresses in
its one trailing comment:

```c
extern void *g_route_open, *g_route_closed;   /* 0x00668fc0, 0x00668fc4 */
```

Every scanner in the tree takes the FIRST address and gives it to every name in
the statement, so both objects were emitted at 0x00668fc0 and in the portable
build **`g_route_open` WAS `g_route_closed`**. The C compiles to identical bytes
either way — on x86 the declaration only has to say a pointer lives somewhere
and the linker supplies the address — so `audit.py`, `relocs.py` and
`verify.py` are all silent while two live globals alias each other.

PORT-M6 and PORT-M8 each re-ran this from a scratch script. It is now a tool
with a `--selftest` (four positive shapes, five negative — one address with two
names is the ordinary case, two addresses with one name is prose, and a function
declaration may legitimately cite a sibling's address) and **a ctest**, because
the only way to keep the class closed is to sweep for the shape:

```
python3 portable/tools/extern_sweep.py            # 0 statements -- closed
python3 portable/tools/extern_sweep.py --selftest
```

Exit status is the gate: 0 closed, 1 on any hit. Sources only — no gamedata, no
image, no build products — so it runs in CI beside `cdecl_extents`. Currently
**0 tree-wide**.

### 4. `portable/tools/slot_sweep.py` — who CALLS a vtable slot

PORT-M8's two scratch sweeps, promoted and merged into one pass. A slot's real
type is whatever its call site pushes, and the call site may be in a file that
never declares the callee; reading declarations cannot settle it, and reading
the image can. **Two forms are needed**, because VC6 emits an indirect slot call
two ways depending on register pressure:

```
call dword ptr [ecx + 0xb0]              <- direct
mov  eax, [edi + 0xa0]  ...  call eax    <- load-then-call
```

Each form is invisible to a sweep for the other, which is how a slot that is
called can look uncalled. Two things were needed to make the load form work at
all on the real image, and both are in `--selftest` as regressions:

* **Resynchronise the disassembly.** One `md.disasm` over `.text` dies at the
  first jump table or COMDAT padding — on this image at 0x00437ee3, 80,686 of
  roughly 200,000 instructions, short of every site PORT-M8 found. The sweep
  restarts a byte along and drops its tracked registers at each resync.
* **A tracked register lives until it is called or redefined, not until the next
  branch.** Every load-then-call site in the image is written
  `mov eax,[edi+0xa0]; test eax,eax; je skip; push…; call eax`, so ending the
  register's life at the `je` loses the site — and at the `test` too, since
  `test`/`cmp` set flags and redefine nothing. Getting this wrong returned
  "no call site" for +0xa0, +0xac, +0xb8 and +0xbc, which is precisely the false
  negative that makes a slot look untypable.

Validated against PORT-M8's whole known set, reproducing its addresses:

| slot | sites found | where |
| --- | --- | --- |
| `+0xa0` | 2, load | `RenderFullMap` call 0x004571a3, `RenderView` call 0x0045b95a (2 pushes each) |
| `+0xac` | 1, load | `LLIDB_UnLoadLLSData` call 0x0047c6c2 |
| `+0xb0` | 2, direct | `DrawAndClearPrintList` 0x00485ac3 and 0x00485b70 (6 pushes each) |
| `+0xb8` | 1, load | `LoadGame` call 0x0047f517 |
| `+0xbc` | 1, load | `SaveGame` call 0x0047e63d |
| `+0x8c` | **0** | no caller in the shipped binary — M8-5 confirmed independently |

```
python3 portable/tools/slot_sweep.py 0xb0            # both forms
python3 portable/tools/slot_sweep.py 0xa0 --form load
python3 portable/tools/slot_sweep.py --selftest      # no image needed
```

Hits are grouped under the `// FUNCTION:` marker that owns them, with
`file:line`. The push count is a **hint**, not proof — it counts pushes since
the previous call, so spills inflate it; confirm the arity in the C at the
marker. The sweep needs `original/legoland.exe` and capstone and is therefore
not in the asset-free set, but its matcher is tested on hand-assembled bytes and
`--selftest` skips visibly if capstone is missing.

### 5. `name_trap.py` knows two more trap kinds

`RuntimeError: table index is out of bounds` and `memory access out of bounds`
are first-class kinds now; before this both fell into the "a real body faulted,
good luck" arm. For the table case the wasm is decoded at the trapping offset to
find the `call_indirect`, its type, the table size, and **where its index operand
came from** — a constant, a memory load, or a parameter, each reported
differently. When the index is a raw x86 VA the tool says so and prints the
`// FUNCTION:` marker that owns the address.

That is PORT-B9's **B9-4** class: function addresses written as integer literals
in the recovered C, because the original is `mov dword ptr [ecx+0x98], 0x45efe0`
and the recovery spelled the immediate instead of the symbol. It matched the
original bytes perfectly and cannot work on wasm, where a function "pointer" is a
table index. The literal is usually stored by a *different* function from the one
that traps, so the index at the call site is an `i32.load`; `--va-literals`
therefore sweeps the whole module for the class and names every one:

```
python3 portable/tools/name_trap.py --va-literals
python3 portable/tools/name_trap.py --at 0x929af --kind table
```

Two things make that sweep sharp rather than useless, and both cost a wrong
answer first:

* **`.text` only, not the whole image.** Linear memory overlaps the image's DATA
  range, so a perfectly ordinary pointer to a rebuilt global is an `i32.const` in
  0x004ab000..0x00836000 — **2,585** of them in this module. Narrowed to
  0x00401000..0x004ab000 the same sweep returns **9**, and the verdict is
  exactness: a value that is *exactly* a `// FUNCTION:` marker address is a
  recovered function address and nothing else. Five are; the other four are
  ordinary integers (a mask, an `open` flag) and are listed separately rather
  than dressed up as findings.
* **Both halves of an `i64.const`.** The optimiser merges two adjacent 4-byte
  stores of constants into one 8-byte store, so `sweep3.c`'s five literals reach
  the module as one `i32.const` and two `i64.const`s. An i32-only sweep finds
  **one of the five**.

On this tree it names all five of `SetStandardCallbacks`' literals, including the
two PORT-B9 left unnamed — `0x00480b70` is `SetEditObjectFromElem`
(pathobj2.c:243) and `0x0045f220` is `StandardRemoveObject` (objmap2.c:982).
`loaders.c`'s 21 sites are absent from both linked modules: `GetInterface` is
dead-code-eliminated, so they are latent rather than live. `coaster10.c`'s three
are `.data` addresses and out of this window by design — the `.data` half of the
class is `gen/pointers.md`'s gate.

Full notes: `docs/lanes/scope-port-a8.md`.

## The tutorial is playable: a ride bought, paths dragged, the park saved and loaded (scope PORT-B10)

**The game plays.** From a cold page load: the Duty Manager's briefing read and
both pages turned, the LEGOLAND menu opened, a **Space Tower Ride bought for 40
bricks**, paths laid, the park **saved through the game's own Save Game screen**,
the page **reloaded**, and the saved park **loaded back** with the ride and the
hand-laid paths where they were. **34.7 fps, `llStats().dead` null, `traps: []`
for roughly 25 000 presented frames — no trap of any kind appeared all session,
so `name_trap.py` had nothing to name.** A7-2 (the MAP button) is gone.

```js
// the whole walk, in GAME pixels, on ?args=-nointro+WINDEBUG&beat=1000
// (from a virgin IDBFS -- once a save exists the front end routes via the
//  title screen instead; see the lane notes)
await llMove(320,240); await llClick(260,188); await llType('adam');
await llClick(505,345); await llClick(252,362); await llClick(577,419);
await llClick(455,445); await llClick(577,419);   // -> THE PARK      money 1000
await llClick(558,243); await llClick( 53,394);   // -> the LEGOLAND menu
await llClick(558,243); await llClick( 27,156);
await llClick(330,200);                           // -> "Your First Ride!"  990
await llClick(577,419); await llClick( 40,443);   // -> the park, PATH armed
await llDrag(292,296, 266,232, 10);               // a RUN of path
```

Per-screen hashes, and what they are and are not reproducible against, are in
`docs/lanes/scope-port-b10.md` §1.

**The money is the proof it is playing and not just drawing**: `llPark().money`
reads `g_bricks`, the same value `RenderMoneyBar` prints — 1000 on arrival, 1030
as the level accrues, **990 the instant the ride is placed**, and the side panel
says the Space Tower costs 40. The visitor cap moves with the park too, 3 -> 4
once the ride is standing.

**Two shim defects, one root cause: our metrics were not the game's.**

* **The briefing was clipped mid-word** — *"Sorry your firs"*. movie.c's
  `LoadHelpTextFor` reads `Intervals\<key>` as LINES and uimisc2.c's
  `PrintReportLine` prints each DT_SINGLELINE into a box that is always
  0x1cc = 460 pixels wide. The text is **pre-wrapped in the shipped data** and
  its longest line is 63 characters; PORT-B2's monospaced 9-pixel advance made
  that 567. The face is now proportional — each glyph advances by its own ink
  extent, drawn AT the pen — and the same line measures 433.
* **The money bar printed `1A3A` for 1030.** money.c hands the count to the
  cached-text blitter in a box exactly the coin sprite's height (~20 px) while
  asking for the lfHeight 24 font, so a 5/6-of-the-cell ink box lost the curve
  that closes every '0'; with the slashed zero art what was left read as an A.
  The ink box is now 2/3 of the cell with the leading biased above it, and the
  zero is a plain oval. The park reads 1000.

**Performance: the port is not the limit.** 28.85 ms per frame against the
game's own 28 ms floor (`FlipPrimary`'s `while (timeGetTime() - g_flip_time <
0x1c)`), i.e. **97.1% of the game's own 35.71 fps ceiling**. The software
terrain pass, every sprite, the GDI text, the present blit and the entire host
shim all fit inside the time the game spends waiting. There is nothing a
profile would find worth moving.

**The page grew one verb it could not do without**: `llDrag(x0,y0,x1,y1)`.
A click places one path square; a run of path is a drag, and the tutorial's
second objective is a run of eight or ten — so `llClick` alone cannot exercise
the path tool at all. Also `llPark()` (money, people, visitors, sim frame, out
of the game's globals) and `llAscii(x,y,w,h,thr,light)`, which reads a
rectangle of the canvas back as text: the game draws all of its own numbers, and
twenty rows of llAscii is how `1A3A` was caught.

**Open, game-side** (full table with evidence and owners in
`docs/lanes/scope-port-b10.md` §5): the tutorial's `LINK "SPACE TOWER RIDE"`
objective never satisfies however much path is laid, so `g_num_visitors` stays
0 and **no visitors ever arrive** — the quiet-failure shape, no trap, owner
PORT-M/game-side; a bubble drawn over the bottom panel is never erased; the
Space Tower renders as a squat block rather than a tower. And one opportunity:
**`gamedata/Lego.TTF` ships with the game and is already mounted** — a
TrueType rasteriser in the shim would give the game its actual metrics and close
the whole class both defects above belong to.

## The game's own type, and the game's own voice (scope PORT-B11)

**The port reads the game's typeface and the Duty Manager speaks.**

### PARK-4: `gamedata/main/Lego.TTF` is shipped, so read it

PORT-B2 and PORT-B10 drew a 6x7 bitmap face by hand because "Lego" could not be
rasterised — and both of PORT-B10's text defects (the briefing clipped mid-word
at 460 px, the money bar's zeroes reading as a capital A) were the same defect
twice: *our metrics are not the game's metrics*. But the font is right there,
77,012 bytes of real sfnt, and gpu.c's `InitHostSystemGPU` hands it to
`AddFontResourceA` before anything draws.

`portable/src/hostwin/ll_ttf.c` reads it — sfnt directory, `head`, `hhea`/`hmtx`,
`maxp`, `loca`, `glyf` simple and composite, `cmap` formats 4 and 0, `OS/2`
v0..v5 — flattens the outlines and fills them non-zero-winding at 5 sub-rows per
pixel with exact horizontal coverage. Written here, not vendored: no
third-party source and no licence file enters the tree. No hinting (a bytecode
interpreter is a lane of its own) and no kerning (correctly: GDI does not apply
a font's kern table unless the application asks, and no call site does).

The height mapping is GDI's, and it is the whole point: `lfHeight` is a CELL
height, so pixels-per-font-unit is `lfHeight / (usWinAscent + usWinDescent)` —
2110 + 595 over a 2048 em here, which makes an `lfHeight` of 18 only 13.6 pixels
per em with a 14-pixel ascent, and puts the baseline at `y + tmAscent`.

**The evidence that the mapping is right** is the shipped data. The tutorial
letter is pre-wrapped and `PrintReportLine` draws each line DT_SINGLELINE into a
box exactly 0x1cc = 460 px wide. Through the real face, its longest line (63
characters) measures **383 px at lfHeight 18**, and 487 / 571 / 597 at the other
three heights. Exactly one of the four fits, and it is the one text.c's
addresses say the report screen asks for. A font file we were handed and a set
of line lengths shipped as data agree — and nothing was tuned to make them.

Synthetic bold falls out of the file too: Lego.TTF is `usWeightClass` 400
"Regular", GDI emboldens only a request of FW_BOLD or more, so the 700 fonts get
a pixel of overhang per character and the 600 report body does not — which is
what PORT-B10 had to arrange by hand to stop the briefing clipping.

The bitmap face stays as the fallback, because `legoland_tests` and
`legoland_headless` have no mounted gamedata. `llFont()` on the page reports the
face and every LOGFONT the game asked for with what it got.

### Sound: MS ADPCM, and Web Audio behind PORT-B4's silent device

Two halves, and the samples had to decode first. Every sound the game loads runs
through `ConvertWAVToPCM` unconditionally and is DROPPED if it fails, so a
refused codec is not a silent sample — it is a sample that never exists.
`msacm32.c` now has a real MS ADPCM decoder. The census over the three shipped
archives: 155 WAVE members, of which **21 are MS ADPCM**, and **all 1,266 files
in `gamedata/disc/Speech` are**. The shim converted 134 of 155 and no speech at
all; it now converts **155 of 155 and 1,266 of 1,266**, byte-identical to an
independent reference on every one of the 1,266.

`portable/src/hostwin/ll_audio.c` is the Web Audio back end. `dsound.c` keeps
every DirectSound semantic PORT-B4 established — the wall-clock cursor, the
refcount `Release` returns, the status a finished one-shot reports, the volume
`GetVolume` round-trips — and gains a voice per buffer. Volume stays in
hundredths of a dB (`10^(v/2000)`), `SetFrequency` becomes a playbackRate, pan
becomes a StereoPanner position. EM_JS rather than a `--js-library` entry, so it
links into the node targets too (where it is a set of counters) without touching
`node_shim.c` or `tests.cmake`.

**Two playback modes, decided by the game's own call pattern.** A sound effect is
written once in full before it is played (one `DSBLOCK_ENTIREBUFFER` Lock at load
time), so it becomes one AudioBuffer and one source node. The narration buffer is
40 KB of ten blocks played LOOPING for ever while `PumpNarration` rewrites the
block ahead of the cursor — a snapshot would loop the first 1.9 seconds of every
line — so a buffer that is ever Locked WITHOUT `DSBLOCK_ENTIREBUFFER` is marked
streamed and fed as small AudioBuffers scheduled back to back, **each copied out
only after the cursor `dsound.c` reports has passed over it**. The cursor is what
the game fills against, so "behind the cursor" is exactly "already written".

**Measured**, on a build with `-DLL_PRELOAD_SPEECH=ON`, walking to the tutorial
briefing with the AudioContext's `createBuffer` wrapped:

```
llAudio() -> { state: "running", voices_created: 4,
               stream_chunks: 510, stream_bytes: 1305600, underruns: 2 }
440 AudioBuffers, 1280 frames each @ 22050 Hz;  426 carry real waveform,
peaks to 0.986;  gain 0.3162 == -1000 cB, the game's speech slider
= 29.6 seconds of speech, at 33.9 fps with traps [] and dead null
```

That is MS ADPCM off the disk, through the ACM, through the game's ring, through
`PumpNarration`, through the shim's cursor, to the speaker. **The narration
speaks.**

The browser's autoplay policy is handled: the AudioContext is made eagerly at
`DirectSoundCreate` (so its one-shot gesture listeners are installed before the
front end is drawn), blocked Plays are counted rather than dropped, and the
page's **sound** cell turns amber and is click-to-enable. `?sound=test` runs one
generated tone through the game's own load-and-play sequence, for when the
question is "is the audio path wired up" rather than "did the game get there".

### Two open items, both named, neither this lane's

**Sound EFFECTS do not play, and it is the raw-pointer class again.** Over a
whole walk to the park, `CreateSoundBuffer` is called **once**. `Load_FXList`
builds `".\sfx\<name>"` and the RES layer resolves it correctly — but
`extern FXEntry g_game_fx[];` (mapinit.c:18) has **no bound**, so `cdecl.py`
cannot compute an extent, `gen_link.py`'s pointer scan skips the object, and
`gen-browser/globals.c` emits `g_game_fx[0].name` as the bare literal
`0x004b9b94` — the original image's address of `"Flowers.wav"`. All 23 names are
raw. (`Space Tower01.wav` is the one that works, because mechrides.c:673 happens
to declare `extern void* g_spacetower_fx;` — bounded — so its word 0 is
re-pointed. One declaration is the whole explanation for "exactly one buffer".)

`pointers.md`'s **"raw pointer words: 0"** is blind to this, because it counts
only words the pointer scan visited. A scan that does not depend on the
declaration — any word in an emitted `.data` block whose value lands in the
original image's data range and points at a printable string — finds **122 raw
words in 18 objects**, including 63 in `g_power_table` ("Small Power Station",
"Dino Big", "T-Rex"), which is very likely a second live defect nobody has
looked for. The fix is a bound on each `extern`, in files this lane does not own;
the recipe and the full list are in `docs/lanes/scope-port-b11.md` §3.

**PARK-2 is settled, and it is not the surface model.** The original never
creates a flip chain: `screen.c:1988-2004` makes one DDSCAPS_OFFSCREENPLAIN
surface and `FlipPrimary` **Blts** it to the primary, and `ddraw.c` models
exactly that. There is also no erase path to run — the bubble drawers are
stateless and the HUD is repainted before them every frame. The residue is a
10-pixel bubble tail left in the **112x96 advisor window** at (522, 378), which
`InterfaceBG.lls` leaves transparent on purpose and which the original fills
every frame from `RenderAdvisorIcon` — inside `if (g_vidanim)`, which is dead in
this port because `avifil32.c` refuses the advisor AVI. The same stub is why the
bubble was raised from a panel pixel at all (icon ownership is established by the
blit, and `BltAdvisor` never runs). So PARK-2 re-files as a PORT-B item, a
sibling of PARK-4, with the cost written up in `docs/lanes/scope-port-b11.md` §4.

## Gates for the two classes no gate could see (scope PORT-A9)

Both of the previous two lanes closed with the same complaint: **a gate that
reports zero is only as good as the set it walks.** PORT-B11 §3 found 23 sound
effects missing under `raw pointer words: 0`; PORT-M10 §1f found a whole ABI
class that `wasm-ld`, `linkreport.py` and `test_callback_types.c` are all blind
to, and left its sweep in `tools/`, outside every gate. This lane makes both
measurable from a clean build, and measuring them found more of each.

### `gen/rawwords.md` — the census that does not ask the declarations

`pointers.md` counts the words the game's own declarations call pointers, and
`raw pointer words: 0` is true of exactly those. `extern FXEntry g_game_fx[];`
(mapinit.c:18) has no bound, so `cdecl.py` can compute no extent, so
`gen_link.py` never visits the object, so its three raw x86 string addresses were
never raw *pointer* words at all.

The new census asks the image instead: every 4-byte word of every emitted
object, declaration or no declaration, whose value lands in the original
`.rdata`/`.data`. Nothing is re-pointed on that evidence — PORT-A2 measured what
value-chosen re-pointing does (1,650 words moved, 1,209 of them string TEXT, the
loader broken) — but it is reported, because in the rebuilt layout nothing is at
0x004b9b94. Two vetoes, both A2's measurement read backwards:

* three printable bytes and a zero high byte is a string literal (`"tan\0"` =
  0x006e6174);
* `'w' 0 'm' 0` is UTF-16 — `kThemeSame`'s 98 words are a wide string table.

Overridden by exactly one thing: a value pointing at the **first** byte of a C
string. Pointing *into* one is no evidence at all in a section that is mostly
string pool — `"ACK\0"`, the tail of "LOG FLUME TRACK", reads as a pointer five
bytes into "mcop_b2s.lls" — while pointing at a string's start is the claim every
defect of this class has made, `g_entrance_fx`'s one word ("turnstyles.wav")
included.

| | wasm32 closure |
| --- | --- |
| words in the image range, text vetoes applied | 989 |
| visited by the declaration scan | 882 |
| **objects with words it never visited** | **27 (107 words)** |
| of those, pointing at a C string | 55 |
| at a symbol's own address | 3 |
| at unidentified data | 49 |

23 of the 107 are sound-effect sample names, 30 are the CRT's own `_matherr`
table swallowed by an unbounded `NearOffset[]`, and 6 are false positives with a
reason (a BGR cursor colour that happens to equal `g_front`'s address; UTF-16 at
an odd byte phase inside the interface-name pool). Every row names the
declaration that would fix it. The ctest `raw_words` gates the list against
`portable/tests/rawwords_baseline.txt`: a new row or a row that grew fails, a row
that shrinks prints `SHRUNK` and passes, so a bound landing in the game sources
tightens the baseline instead of fighting it.

### An unbounded array of a struct with pointer fields is no longer a silent skip

The generator supplies the count the declaration is missing, from the gap-tiled
block: the object is at least the **complete** elements that fit in it. The bytes
past the last whole element are never scanned, and a guessed extent pays a price
a declared one does not — any element whose claimed pointer word is inline text
is dropped. Without that rule, the first run re-pointed **55 words of the credits
roll** ("ton\0" of "Anton") into `g_zbuf_pixels`: A2's catastrophe arriving by a
different door.

Proved with PORT-A8's address-keyed bytes proof over `globals.c`: 21,397 elements
both sides, 0 addresses added or removed, **72 values changed**, every one a raw
image literal becoming a re-pointed interior and every one read by eye —
`g_power_table`'s 63 element names (B11's suspected second live defect),
`g_money_fx` 2, `g_joust_fx` 1, two of `g_game_fx`'s three, `g_spacetower_fx` 2,
`g_support_model_a` 2. The native 64-bit closure is untouched (85,495 elements, 0
changes). It is not a substitute for a bound: `g_game_fx`'s third name lives in
the 8 bytes past the second element of a 32-byte tile, and only a real bound
reaches it.

### `--probe-audio`: the sound-effect blocker as a number

`legoland_headless --probe-audio [N]` runs the game's own `InitSoundSampleSystem`,
wraps the resulting `IDirectSound`'s vtable with a counting copy (so neither the
shim nor the game has to change), then makes the FX calls the park makes —
`Load_FXList(g_game_fx, 0x17)` and `LoadMoneySFX()` — and prints each entry's
`.name`, whether it is a readable string at all, and whether the sample loaded:

```
g_game_fx[ 0] name=0x19f54 Flowers.wav        sample=loaded
g_game_fx[ 2] name=0x4b9b6c <not a string -- a RAW image address>  sample=NULL
g_game_fx: 2/23 loaded, 21 name(s) still raw
g_money_fx: 2/2 loaded, 0 name(s) still raw
AUDIO 4 sound buffer(s) created, 122290 byte(s) of PCM
```

Four, where B11 measured one `CreateSoundBuffer` call in a whole session — all
four from words the extent rule above recovered. `probe_audio` is a local
(asset-needing) ctest with 4 as a **floor**: the other 21 names are bounds the
game sources owe, and when they land the probe asks for the floor to be raised.

### `portable/tools/bvstruct_sweep.py`: M10's sweep as a gate, and 13 more sites

Promoted with `--selftest` and two ctests. Two changes to the sweep itself, both
of which change its answers:

* it reads the sources the way the **portable build** does, skipping the arms of
  `#ifndef LEGOLAND_PORTABLE` that only VC6 compiles. M10's fix shape is a
  portable arm next to the VC6 one, so the old sweep reported all three of its
  own repairs as hits;
* it sizes a typedef **tree-wide**. The old sweep looked a type up only in the
  file that mentioned it, so `union BPosW { unsigned short w; BPos b; }` — a
  2-byte 2-member union, the very shape M10 §1b measured as indirect — came out
  "size unknown" and was filed under the heading that says wasm-ld already warns.
  It does not, and that mis-filing hid **thirteen** more live sites: the driving
  school's two manoeuvre routines, four boating-school/mermaid removal handlers
  registered through the `+0x9c` slot as integers, the restaurant's start/stop
  sound pair (newly audible since B11), the jungle cruise's route rebuild, the
  media shop's removal and the mechanics hut's eviction.

`--selftest` is 23 classifier checks plus the ABI claim itself, compiled by the
real emcc and clang:

```
struct{u8,u8}    native slot=0x233f   wasm32 slot=0x0aec
union{u16;BPos}  native slot=0x233f   wasm32 slot=0x0aec
one member       identical on both -- passed direct, safe
```

so a toolchain that stopped disagreeing fails the test instead of turning every
row into a phantom. All 15 ruled-on addresses are in
`portable/tests/bvstruct_accepted.txt` with their direction, citations and M10's
recipe; any other silent site fails the build. Notes:
`docs/lanes/scope-port-a9.md`.


## One name, two addresses — and a hidden tab that stalls (scope PORT-A10)

### `portable/tools/addr_sweep.py` — the sibling of `extern_sweep`

`extern int g_view_left;  /* 0x004b95f4 */` in `scrolltick.c` and
`extern int g_view_left;  /* 0x008299ac */` in `coaster3d.c` were two different
objects on x86: each `.obj` carried its own address and the real linker gave
each spelling the memory it meant. The portable build has no `.obj` —
`gen_link.py` plans **one storage object per name** — so the two collapse and
every TU that spelled the loser is sheared, silently. PORT-P3 measured what that
costs in the running tab: the map scroll clamp reads the coaster's *zeroed* 3D
clip rect instead of its own 243200-unit slack, so 475 px of every map is
unreachable and the Mechanic's Hut lesson 4 tells you to click cannot be brought
on screen; and `g_popup` is sheared by 0x1c, so **no gardener and no mechanic can
be hired anywhere in the game**.

The C compiles to identical bytes either way, so `audit.py`, `relocs.py`,
`match.py` and `verify.py` are blind to this state *and to the fixed one* — the
same blindness `extern_sweep.py` exists for. That one catches one `extern`
STATEMENT with several declarators and several addresses; this one catches one
NAME with several addresses across FILES, which no single statement reveals.

```bash
python3 portable/tools/addr_sweep.py                    # sweep LEGOLAND/
python3 portable/tools/addr_sweep.py --selftest         # shapes, no sources
python3 portable/tools/addr_sweep.py --names-only       # the cheap half
```

Two checks: **NAME** (one name, several addresses — data declarations, function
declarations and the `// FUNCTION:` markers, read a statement at a time) and
**ADDR** (one address, several names whose `cdecl.py`-computed sizes disagree,
with the project's `<= 4`-byte head-name convention vetoed — it is 35 rows
without that veto and 5 with it).

It finds **18** names, not PORT-P3's 17. The extra one is a *function*:
`Track_Update` is defined twice, at `0x004275d0` (coaster.c) and `0x00427b20`
(castleobj.c) — and it is not a live defect, because PORT-M7 already gave the
second its own symbol with `#define Track_Update Track_Update_427b20`. The row
is kept so the gate notices if that mitigation is ever dropped.

`portable/tests/addr_collisions.txt` holds all 23 rows with a reason each, and
the ctest `addr_sweep` gates them: a row not in the file, or a row that grew an
address or changed a size, fails; a row that is no longer reported prints `FIXED`
and passes. That last rule is what lets the baseline hold PORT-P3's open
findings while PORT-M15 renames them in parallel, in either merge order.

`gen_link.py` also stops choosing silently. `linkreport.scan_sources` keeps one
address per name with a `setdefault` — first file wins, nothing downstream told.
The manifest now carries `- names with conflicting addresses: 18 (...)` and a
section attributing every citation to `file:line`, there is a stderr warning, and
the generated `globals.c` opens with a block comment marking with `->` the
address it actually emitted for each name. `-DLL_FAIL_ON_ADDR_COLLISION` turns
that notice into a real `#error`, for after the renames land.

### The page: `llFind`, and why a hidden tab used to stall

PORT-P3 filed two findings against the shell page rather than the game.

**P3-4 was two bugs.** Every `ObjDef` carries two names — the LLIDB *element*
name (`+0xc4 -> +0x00`, "ENTRANCE 1") and the *display* name (`+0x78`, "Park
Entrance") — and `llLink`/`llPad` compared only the first, so every name a
player or `llCellAt` itself would give answered `found: false`. Separately,
`ObjDef +0x04` is **not** a complete placed-instance list: measured in tutorial
lesson 1, "Park Entrance" has an EMPTY chain and 120 cells on the map, anchored
at (81,40). So the old `{found: true, insts: []}` was right about the chain and
wrong about the park, and it reads as a game defect that is not there.

Both are fixed. The class lookup tries the element name, the display name, then
either case-insensitively, then either normalised (which is what makes
`MECHANICS HUT` match `Mechanic's Hut`), and reports `matchedBy`. Instances come
from the chain when it has any and from a census of the MAP when it does not —
both sources report the same anchor, so `EventTick_Link`'s arithmetic is
untouched — and every result says `instSource`. New probes: **`llFind(name)`**
(every placed instance with its class, both names, anchor, footprint, cell count,
bbox and flags; substring match, so `llFind('hut')` finds the hut; no argument =
the whole park census), `llClasses()` and `llMapObjects()`.

**P3-7 is sharper than "throttled to 1 Hz".** The port yields through
`emscripten_sleep`, which the loader implements as `new Promise(r =>
setTimeout(r, ms))` — so the game's whole main loop is ONE `setTimeout` chain,
and Chrome's background budget applies to chains. Measured per 10-second window
in a hidden background tab with nothing polling it: **33.5 fps for the first
thirty seconds, then exactly five wake-ups per ten seconds — 0.50 fps — flat,
for as long as you leave it.** The `?beat=1000` heartbeat agrees from inside the
wasm: one BEAT at `t=59493`, the next at `t=102493`, **seven host calls in 43
seconds**, with the yield's own `since_yield` still reading 0 ms.

Two traps for anyone re-measuring it: the cliff is thirty seconds away, so a
twenty-second measurement sees nothing; and driving the tab from a debugger
**un-throttles it**, so the numbers have to come from an in-page sampler that is
started, left alone, and read once.

Three candidates were measured in that same tab. `requestAnimationFrame` is
**suspended** (three frames did not arrive in six seconds) so it cannot be a
fallback at all. A Web Worker timer works (0.3 ms) but costs a Worker, a Blob URL
and a `postMessage` round trip per yield, and the game yields ~1400 times in five
seconds. A **`MessageChannel` hop** costs 0.003 ms and is not a timer, so no
timer budget applies to it. PORT-P3's audio oscillator works too, but needs an
AudioContext the autoplay policy will not start without a user gesture — which a
test runner may never have — and leaves the tab permanently audible.

So while `document.hidden` is true the page delivers 0-or-1 ms timers as
MessageChannel messages; everything longer is a real wait (the 5 s profile flush,
`MessageBoxA`'s pause) and is left alone. A hopped call still gets a handle and
`clearTimeout` still cancels it. Same protocol, same tab, `?awake=1`: **35.7 fps
flat through the thirty-second cliff and past it** — 0.50 fps -> 35.8, a factor
of 71 — which is the game's own 28 ms `FlipPrimary` ceiling, with no permission
and no worker. The
cost is that a hidden tab really does keep running and burns a core: right for a
testing page, wrong for a shipping one, so `?awake=0` turns it off and
`llAwake()` reports and pins it. Notes: `docs/lanes/scope-port-a10.md`.

## The cheat ring, and one memcpy that overlaps itself (scope PORT-B12)

PORT-P1 reported that **roughly every seventh typed character is doubled in the
cheat ring**, filed it against the press latch, and could not exercise
`RunAppraisalScreen` because of it. Neither half of that is where the defect
is, and the screen is now reachable.

### It is not the keyboard, at any of the three layers

Measured in a running free-play park while `llType('ABCDEFGHIJKLMNOP')` runs
(`portable/src/browser/replays/b12-01-*.js`):

| layer | measurement | result |
| --- | --- | --- |
| the DOM | `LL_DEBUG.push` wrapped, every accepted key event counted | **16 keydowns, 16 keyups**, one per character, DIKs correct |
| the shim | the game's own `g_key_state[256]` sampled at **1 ms** | **16 rises, 16 falls, strictly alternating**; never a second rise inside a press |
| the game | `g_typed_key_prev` while a key is held | exactly one entry set, to `0x80`, at the right key-map index |

PORT-B6's press latch is correct; `latch_press` is the only writer that raises
`down`, and it runs only from the sixteen `EV_KEYDOWN` records the first row
counts. Nothing in `dinput.c` needed to change.

### `memcpy(g_type_buf, g_type_buf + 1, 19)`

input.c:368 shifts the twenty-byte ring with a `memcpy` whose **source and
destination overlap**, which is undefined behaviour. VC6's x86 `memcpy` copies
forward and happens to produce the intended shift. LLVM sees a constant length,
inlines the copy as wide load/stores that are allowed to assume no overlap, and
**duplicates three of the nineteen bytes**.

Drive the shift with no keyboard at all — write twenty distinct bytes over the
ring, type one character, read back:

```
before   0123456789abcdefghij
after    02345678aabcdefhiijP      three bytes duplicated, three lost
expected 123456789abcdefghijZ
```

and the same thing in eleven lines outside the game entirely:

```
$ emcc -O2 -o t.js t.c && node t.js        # memcpy(b, b+1, 19)
got      BCDEFGHJJKLMNOPQRST7
expected BCDEFGHIJKLMNOPQRST7
```

Three wrong bytes at n = 19 — the count the live game shows. It is size- and
alignment-dependent (n = 20 comes out clean, n = 63 loses eight), which is why
it looks arbitrary.

**Why it looked periodic.** It is not periodic in time or in keystrokes; it is
periodic in the ring. A fixed set of ring offsets is corrupted on every shift,
and a ring that moves one byte per character walks each character past one of
them every seven characters — hence a doubled pair always seven apart, with the
phase differing per run. Each cheat is matched with a `strnicmp` against the
ring's tail **at a fixed offset** (`&g_type_buf[11]` for `:PRAISEME`), so one
extra or missing character makes the comparison impossible: **every cheat in
the game is dead.**

**The fix is one line, in `LEGOLAND/input.c`** — `memmove` under a
`LEGOLAND_PORTABLE` guard, so the `#else` arm's bytes cannot move. There is no
shim-side fix: the miscompiled copy is *inlined*, so an overlap-safe `memcpy`
in `msvcrt.c` would never be called. `docs/lanes/scope-port-b12.md` §2.5 has
the recipe; §2.6 sweeps the class tree-wide (three sites: this one,
`narration2.c:429` latent, `ridecb5.c:767` a harmless no-op).

### The appraisal screen, reached

On a build with that one line, in a free-play park: `llKey('ShiftLeft')` then
`llType('PRAISEME')` leaves the ring reading `FGHIJKLMNOP:PRAISEME` — the word
at exactly bytes `[11..19]` — the cheat fires, `AppraisalDueTick` pauses the
sim (`llPark().simFrame` freezes) and **`RunAppraisalScreen` (appraisalscreen.c:381,
the 8,085-instruction WIP body nobody had seen run) draws**: the REPORT notepad
with its spiral binding, "Congratulations you have built a thriving Park!", and
the inspector minifigure with his pencil. Its icons answer, the greyed page
buttons are correct, and the GoBack icon at game (525, 370) closes it and hands
the park back with the sim resuming. Zero traps.

### P1-6: the visitors reach the print list and stop

Confirmed, and narrowed to one function (`b12-02-*.js`). Hiding the whole bloke
chain (`flags62 |= 0x80`, which makes `RenderPeople` skip it) changes the frame
by fewer pixels than the park's own animation noise — so no bloke is drawn
anywhere. But 23 type-`0x2000` print-list nodes with real depth keys sit in the
arena, and `Render3DPerson` **is entered for 27 of 30 persons every frame**
(its first three statements write `scale = 1.0f` before any early-out, which
makes a poked sentinel a perfect probe). The records are healthy: a proper
16.16 rotation matrix, a real frame index, a non-null face table. Everything
shim-side on the path checks out — `user32.c`'s `IntersectRect` is alias-safe,
which rin.c:549 depends on, and the locked surface is the one every visible
sprite is blitted into.

And `Draw3DPersonModel` never reaches its vertex loops. It copies every vertex
of every person it draws into two plain .bss scratch arrays (`g_xverts`,
`g_vert_key`), so diff the whole static-data region between two frames: **739
changed words in 0..6 MB**, and the only contiguous run is the bloke AI's own
8.8 walking positions — which vanishes when the chain is hidden. No array
anywhere in static data receives per-vertex data.

So the bail is at one of `Render3DPerson`'s two early-outs (rin.c:546-554), and
the first is cleared. The candidate is `GetVideoSurface`, which
**returns 0 whenever `g_video_locked == 0`** — the sources call it "the can I
draw? test". Test first that the video surface is not locked while
`DrawAndClearPrintList` walks the list: every 3D person would silently return
while every sprite still drew, because sprites go through `PrintSprite` ->
`RenderSprite`, which pushes and pops its own lock around each blit. If it *is*
locked, the second candidate is the 63.3% WIP body's own portable arms
(`FMUL`/`FMULA`/`FMULP`/`TOFIX`/`SHADE`, person3d.c:529-596), where a 16.16
scale that comes out zero rasterises to nothing, silently. Owner: a matching /
render lane. Notes: `docs/lanes/scope-port-b12.md`.

## A leaked GDI handle turns text boxes black (scope PORT-B13)

PORT-P4 filed three text boxes — the money readout, the objective help bubble
and the info pop-up's body — that **fill solid black and stay that way**, with
the text printing legibly on top of the black, and suspected an ignored
`SetBkMode(TRANSPARENT)`. It is not `SetBkMode` (`llGdi().setBkMode` is
`{transparent: 4344, opaque: 0}` over a whole lesson — every call honoured) and
it is not `ll_font.c`. **It is a leaked handle in `gdi32.c`.**

The three boxes are one thing: cached-text sprites, painted by
`DrawCachedTextSprite` (bubblecache.c:419). That routine fills the whole cell
with `GetNearestColor(ink)`, prints the text in `paper` on top, and then
**makes that same ink the sprite's COLOUR KEY** (line 459) — so the fill is
meant to VANISH when `RenderSprite` blits the cell with `DDBLT_KEYSRC`. A black
box under legible text is therefore not "an opaque background": it is the fill
and the key being two different colours, and only two numbers tell you which of
them moved.

They moved because a memory DC is two things in this shim — an entry in the
DC-attribute table and a slot in the OBJECT table that `obj_new` gave
`CreateCompatibleDC` — and `DeleteDC` released only the first. Every *balanced*
`CreateCompatibleDC`/`DeleteDC` pair the game makes still leaked a slot, and the
game makes one per tooltip (`fpui2.c:1009/1016`, `bighelp.c:321/328`,
`bubblecache.c:505/512`). With all 256 gone, `obj_new` returned a handle with no
slot in it rather than failing, `obj_get` rejected it, `ll_host_brush_colour`
fell back to BLACK, and the fill went down as `0x0000` while the key stayed
`0x001f`. The key matched nothing; the cell blitted opaque.

Measured in the lesson-5 park, one build, identical script either side (the
"before" column is the same tree with one `#ifdef` backing out the slot
release). Each hover is one tooltip measure:

| after 298 HUD hovers | before | after |
| --- | --- | --- |
| `llGdi().objs.byClass.dc` | **255** (table full) | **0** |
| `objs.exhausted` / `fill.noBrush` | 1 / 1, rising | **0 / 0** |
| money readout, x 220..430 y 8..30, PURE black | **51.7%** | **0.0%** |
| help bubble, x 420..630 y 318..398, PURE black | **86.6%** | **2.2%** (its ink) |
| panel caption box, x 3..208 y 357..383, PURE black | **70.8%** | **4.4%** (its ink) |
| `fillLast.colorref` / `ck.lastLow` | `0x0` / `0x1f` | `0xff0000` / `0x1f` |

The last row is the finding in two numbers: the fill and the colour key have to
be the same colour.

`DeleteDC` now frees the slot, and the object table **grows** — 256, doubling,
capped at 4095 because the handle format (`0x4c47 | class << 12 | slot`) has a
12-bit slot field — with a trace if it is ever genuinely exhausted. The growth
is not cover for the bug above; it is cover for the game's OWN leak, which the
recovery already records at misc3.c:1054: `MeasurePopUpTitle` and
`MeasurePopUpBody` never `DeleteDC` theirs, so "every pop-up resize leaks a DC".
Windows absorbs that with ~10,000 handles per process; 4095 slots is ~2,000
pop-up opens, which no session reaches. **Nothing in `LEGOLAND/*.c` changed.**

New page hook, reads only: **`llGdi()`** — the object census by class with its
high-water mark and exhaustion count, `SetBkMode` by mode, the last `FillRect`
(brush handle, COLORREF, the 565 pixel written, rect, surface) and the colour
key and keyed-blit counters beside it. It exists because a black box on the
canvas is two different bugs that look identical; `fillLast.c565` against
`ck.lastLow` is what separates them.

**The page stops lying to the driver, too.** `max-width: 100%` let the flex line
squash the canvas, and `MouseEvent.clientX` is a `long` — so `llPoint`'s
fractions were truncated and several game columns arrived as one. Measured in a
312 px pane (scale 0.272): game x 320, 321, 322 and 323 all reached the game as
**320**, and at PORT-P4's collapsed rect (width 2) all 640 columns do. The
canvas now keeps its intrinsic size (`flex: none`, `min-width: min-content`, so
it follows a mode change instead of hard-coding 640x480) and wears an OUTLINE
rather than a border, so `getBoundingClientRect()` is exactly 640x480 and
`llPoint` is the identity at every pane size; a narrow pane scrolls. `llPoint`
throws on a canvas below half size rather than clicking one spot silently.

And `llSleep` — exported now — yields through the **keep-awake MessageChannel
hop** while that hop is engaged, instead of setting a real timer a hidden tab
clamps to ~1 Hz. `?awake=1` was only ever waking the GAME. With the clamp
installed explicitly, one `llClick` goes **3024 ms -> 818 ms** and one `llType`
of 20 characters **48101 ms -> 5645 ms**, with the game at 35.7 fps throughout;
unclamped it costs nothing (5605 vs 5641 ms). Replay
`portable/src/browser/replays/b13-01-the-gdi-object-table-leaks-memory-dcs.js`,
notes `docs/lanes/scope-port-b13.md`.

## Next

0. **The prototype conflicts** are the frontier, ahead of everything below, and
   `python3 portable/tools/name_trap.py` now names them one at a time in the
   order the game hits them. Three are measured live: `InitHostSystemGPU`
   (one word in `LEGOLAND/startup.c:38`, and the only thing between this port
   and the front end), `RES_CloseFile` (13 files) and `InitSoundSystem`;
   `RES_CloseVolume` and `DBPrintf` sit behind them.
   `docs/lanes/scope-port-a3.md` §3 is the list with the file and line for each,
   and §2 the recipe; `docs/lanes/scope-port-a2.md` §4 has the original
   evidence.
1. **Host shim on SDL3, native desktop first**: window and message pump,
   one 16-bpp surface presented as a texture (`DirectDrawCreate` and the
   `IDirectDraw*` vtables in gpu.c/surface.c), DirectInput-shaped keyboard
   and mouse state, `GetTickCount`/`timeSetEvent`, DirectSound over SDL
   audio, GDI text via a bitmap font. Stub movies, music and printing.
2. **Port the stubbed blitters** as they are reached. The ten RLE sprite
   painters and the plain type-2 animation painter are done (PORT-B3); the
   next ones the front end will want are softblit.c's `SoftBlitAnim` (read
   `docs/lanes/scope-port-b3.md` §3b first — its `row:` label needs moving)
   and bigrender.c's z-buffer RLE walker.
3. **Emscripten job** mirroring isle-portable's CI row: `emcmake`,
   pthreads, WASMFS fetch backend streaming assets from a host URL, OPFS
   saves, COOP/COEP on the host.
4. **Fill the rest**: ADPCM in C, a soft synth for the DirectMusic
   styles/segments, Indeo 5 for the AVI intros.
