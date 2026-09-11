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
await llMove(320,240); await llClick(260,188); await llClick(505,345);
await llClick(252,362); await llClick(577,419);   // -> the briefing
await llClick(455,445); await llClick(577,419);   // -> THE PARK
await llClick(558,243); await llClick( 53,394);   // -> the LEGOLAND menu
await llClick(558,243); await llClick( 27,156);
await llClick(330,200);                           // -> "Your First Ride!"
await llClick(577,419); await llClick( 40,443);   // -> the park, PATH armed
await llDrag(292,296, 266,232, 10);               // a RUN of path
```

Per-screen hashes are in `docs/lanes/scope-port-b10.md` §1.

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
