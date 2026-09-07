# LEGOLAND portable

The path from the matching decompilation in `../LEGOLAND` to a game that runs
natively and in the browser, modelled on
[isle-portable](https://github.com/isledecomp/isle-portable): the same
recovered C compiled with a modern compiler, Win32 and DirectX replaced by a
host shim (SDL3 later), and Emscripten for the web build.

This directory never touches the VC6 matching build. Every change it needs in
`../LEGOLAND` sits under `#ifdef LEGOLAND_PORTABLE` (or `#ifndef`), which the
VC6 gate does not define, so `tools/verify.py` sees the original text.

## Status: native link closed (2026-09-08)

All 236 game sources compile with clang, and a whole-archive link of the
recovered game succeeds against a generated closure. Nothing runs yet; the
generated pieces trap by name when reached. The census below is the work list.

| what the link still needs | count | where it comes from |
| --- | --- | --- |
| game functions referenced but not written | 66 | matching lanes (`tools/inventory.py` lists all 340 live) |
| externs with no address comment | 102 | rename or write; see `linkreport.md` "Unclassified" |
| stale extern names (address defined under another name) | 198 | rename pass; bridged by generated aliases for now |
| Win32 / DirectX imports | 130 | host shim (`src/hostwin/`), stubbed per DLL |
| inline-asm bodies still stubbed | 23 | port by hand, see below |
| extern-only globals | 2,492 | rebuilt from `original/legoland.exe` by `gen_link.py` |

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

Inline x86 assembly (82 sites in 29 files) is invisible to a non-x86
compiler, so each site has a C fallback:

- **Ported to C**: x87 `fld/fmul/fistp` conversions (round-to-nearest, as the
  game's control word sets), the 16.16 `imul/shrd` fixed-point macros in
  person3d.c and math3d.c, the rotation-matrix and vector-transform loops,
  rdtsc timing brackets, byte swaps, control-word save/restore (no-ops), the
  z-buffer span filler in schoolcar6.c, the clear/fill loops in tri3d.c and
  text.c, the 8-bit paletted and 16-bit rectangle blits, the fast
  sqrt/rsqrt table helpers (`ll_FastSqrt`, `ll_FastRSqrt`).
- **Stubbed with `LL_UNPORTED_ASM()`** (aborts when reached): the four
  triangle rasterisers in tri3d.c, the eight RLE painters in rlepaint.c and
  two in rlepaint2.c, the RLE animation blitters in softblit.c/softblit2.c,
  the z-buffer RLE walker in bigrender.c, the two blits in blitmisc.c, the
  50% blend blit in popup.c, and the ST(0)-ABI helpers `sub_458930`,
  `FastSqrt`, `FastRSqrt`. About 6,000 lines of hand-written blitter asm.
- Structured exception handling in exceptlog.c compiles to plain blocks.
- castleobj.c's `Track_Update` (0x00427b20) collides with coaster.c's
  (0x004275d0); the portable build renames the former `Track_Update_427b20`.

## Next

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
