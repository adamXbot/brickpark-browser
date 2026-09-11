# portable/cmake/browser.cmake -- owned by scope PORT-B (docs/SCOPE_PORT_WAVE.md).
# Included from portable/CMakeLists.txt; add this lane's targets here, not there.
#
# Three things live here:
#
#   legoland_hostwin   the host shim itself (DDRAW / USER32 / GDI32 / DINPUT /
#                      WINMM / DSOUND). Part of the default build on EVERY
#                      toolchain, including the native 64-bit one, so a syntax
#                      or signature error shows up in `ninja -C portable/build`
#                      rather than only in the wasm build.
#   legoland_shimtest  wasm32 only. A page that drives the shim through the
#                      exact sequence the game's startup spine drives it
#                      through, with the game's own vtable-offset structs, and
#                      paints a test frame. This is what is testable TODAY,
#                      because the generated closure does not compile on wasm32
#                      yet (PORT-A deliverable 1).
#   legoland_browser   wasm32 only. The real thing: the whole game archive plus
#                      the shim plus the generated closure. Blocked on PORT-A's
#                      `gen_link.py --ilp32` fix; wired up and ready.
#
# Main-loop strategy: -sASYNCIFY. Every blocking construct the game already has
# yields to the browser from inside the shim. The reasoning, the yield points
# and the consequences are in docs/lanes/scope-port-b.md §1.

set(LL_HOSTWIN_SOURCES
  "${CMAKE_CURRENT_SOURCE_DIR}/src/hostwin/ddraw.c"
  "${CMAKE_CURRENT_SOURCE_DIR}/src/hostwin/user32.c"
  "${CMAKE_CURRENT_SOURCE_DIR}/src/hostwin/gdi32.c"
  # PORT-B2: the bitmap face and the DrawText layout engine that gdi32.c's
  # TextOutA and user32.c's DrawTextA draw and MEASURE with. Not optional --
  # eleven DT_CALCRECT call sites lay the front end out around its answers.
  "${CMAKE_CURRENT_SOURCE_DIR}/src/hostwin/ll_font.c"
  "${CMAKE_CURRENT_SOURCE_DIR}/src/hostwin/dinput.c"
  "${CMAKE_CURRENT_SOURCE_DIR}/src/hostwin/winmm.c"
  "${CMAKE_CURRENT_SOURCE_DIR}/src/hostwin/dsound.c"
  # PORT-B6: the last two DLLs the import table names that were still generated
  # traps. avifil32.c reports "this file will not open" through all sixteen
  # AVIFile entry points, which is the path movie.c/advisor.c are written for;
  # msacm32.c is a real PCM converter, because data2.c runs every sample in the
  # archives through it and drops the ones it cannot convert. With these two the
  # page runs with ZERO traps.
  "${CMAKE_CURRENT_SOURCE_DIR}/src/hostwin/avifil32.c"
  "${CMAKE_CURRENT_SOURCE_DIR}/src/hostwin/msacm32.c")

add_library(legoland_hostwin STATIC ${LL_HOSTWIN_SOURCES})
target_include_directories(legoland_hostwin PUBLIC
  "${CMAKE_CURRENT_SOURCE_DIR}/hostwin/include")
# No -include ll_portable.h and no LEGOLAND_PORTABLE here: the shim is ordinary
# C that the game calls into, not game code.
target_compile_options(legoland_hostwin PRIVATE -fno-strict-aliasing -w)

if(NOT EMSCRIPTEN)
  # Everything below needs emcc. The native build stops here, having still
  # compiled the shim.
  return()
endif()

# ---- shared Emscripten link flags -----------------------------------------
#
# ASYNCIFY_STACK_SIZE: the side buffer the unwound C stack is copied into. The
# default 4096 bytes is far too small -- a yield can happen inside timeGetTime
# at any depth of the render path. 128 KiB with a clear overflow message.
# ASYNCIFY_IMPORTS is NOT set: every yield goes through emscripten_sleep, which
# is already in Emscripten's default import list (scope-port-b.md §1).
# -O2 at link as well as compile: unoptimised ASYNCIFY instrumentation of the
# largest bodies (RunAppraisalScreen, 8,085 instructions) exceeds the wasm
# per-function local limit ("local count too large" at instantiate).
set(LL_WASM_COMMON_LINK
  -O2
  -sASYNCIFY=1
  -sASYNCIFY_STACK_SIZE=131072
  -sALLOW_MEMORY_GROWTH=1
  -sINITIAL_MEMORY=268435456
  -sSTACK_SIZE=8388608
  -sEXIT_RUNTIME=0
  -sASSERTIONS=1
  --js-library "${CMAKE_CURRENT_SOURCE_DIR}/src/browser/ll_canvas.js"
  --shell-file "${CMAKE_CURRENT_SOURCE_DIR}/src/browser/index.html")

# The JS library and the shell page are INPUTS to the link, but they arrive as
# link options, which CMake does not scan for dependencies -- so editing either
# one left ninja saying "no work to do" and the browser silently running the
# previous build. That wasted a debugging round in PORT-B2 (a change to
# ll_canvas.js appeared not to take effect, twice). LINK_DEPENDS makes them
# real dependencies of every target that uses LL_WASM_COMMON_LINK.
set(LL_WASM_LINK_DEPENDS
  "${CMAKE_CURRENT_SOURCE_DIR}/src/browser/ll_canvas.js"
  "${CMAKE_CURRENT_SOURCE_DIR}/src/browser/index.html")

# ---- legoland_shimtest: the shim on its own --------------------------------
add_executable(legoland_shimtest EXCLUDE_FROM_ALL
  "${CMAKE_CURRENT_SOURCE_DIR}/src/browser/shimtest.c")
target_link_libraries(legoland_shimtest PRIVATE legoland_hostwin)
target_compile_options(legoland_shimtest PRIVATE -w)
target_link_options(legoland_shimtest PRIVATE ${LL_WASM_COMMON_LINK})
set_target_properties(legoland_shimtest PROPERTIES
  SUFFIX ".html" OUTPUT_NAME "shimtest"
  LINK_DEPENDS "${LL_WASM_LINK_DEPENDS}")

# ---- legoland_browser: the game ---------------------------------------------
#
# Its own closure. gen_link.py scans ONE object directory, so it cannot see the
# shim's definitions and would emit a trapping stub for each of them; the filter
# step comments those out of the generated C (it never hand-writes a stub --
# docs/SCOPE_PORT_WAVE.md rule 2). A separate gen directory from the shared
# `gen/` keeps this out of legoland_linkcheck's way.
set(LL_BROWSER_GEN_DIR "${CMAKE_BINARY_DIR}/gen-browser")
set(LL_BROWSER_GEN_SOURCES
  "${LL_BROWSER_GEN_DIR}/globals.c" "${LL_BROWSER_GEN_DIR}/aliases.c"
  "${LL_BROWSER_GEN_DIR}/stubs.c"   "${LL_BROWSER_GEN_DIR}/host_stubs.c")
add_custom_command(
  OUTPUT ${LL_BROWSER_GEN_SOURCES}
         "${LL_BROWSER_GEN_DIR}/manifest.md" "${LL_BROWSER_GEN_DIR}/ll_gen.h"
  COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tools/gen_link.py"
          "${CMAKE_BINARY_DIR}/CMakeFiles/legoland_core.dir"
          --out "${LL_BROWSER_GEN_DIR}" --exe "${LL_EXE}" ${LL_GEN_FLAGS}
  COMMAND "${Python3_EXECUTABLE}"
          "${CMAKE_CURRENT_SOURCE_DIR}/src/browser/closure_filter.py"
          --gen "${LL_BROWSER_GEN_DIR}"
          --objs "${CMAKE_BINARY_DIR}/CMakeFiles/legoland_hostwin.dir"
  # linkreport.py is not a bystander: gen_link.py imports it and gets its
  # object scanner, its source scanner, its classifier and its wasm signature
  # reader from it, so a change there changes the generated closure. Without
  # this line an edit to linkreport.py leaves gen-browser stale, which cost an
  # afternoon once (PORT-A2). portable/CMakeLists.txt's own `gen` command has
  # the same gap; nobody but the integrator may edit that file.
  DEPENDS legoland_core legoland_hostwin
          "${CMAKE_CURRENT_SOURCE_DIR}/tools/gen_link.py"
          "${CMAKE_CURRENT_SOURCE_DIR}/tools/linkreport.py"
          "${CMAKE_CURRENT_SOURCE_DIR}/src/browser/closure_filter.py"
  COMMENT "gen_link.py + closure_filter.py: the browser target's link closure"
  VERBATIM)
add_library(legoland_gen_browser STATIC EXCLUDE_FROM_ALL ${LL_BROWSER_GEN_SOURCES})
target_include_directories(legoland_gen_browser PRIVATE "${LL_BROWSER_GEN_DIR}")
target_compile_options(legoland_gen_browser PRIVATE -w)

# Assets. `--preload-file src@dst` bakes the tree into a .data file the loader
# fetches before main() runs.
#   LL_PRELOAD_MAIN  gamedata/main (14 MB): the loose assets the game opens by
#                    name from its working directory.
#   LL_PRELOAD_RES   the three RES volumes (157 MB), placed at /gamedata/volumes
#                    because RES_OpenVolume (data2.c 0x00489750) _splitpath's the
#                    name and opens `.\volumes\<stem>.res` first. WITHOUT THEM
#                    InitSession stops at its first RES_OpenVolume, which is
#                    before InitHostSystemGPU -- so no frame is ever drawn.
#                    Default ON for that reason; turn it off for a quick
#                    load-and-trap run.
#   LL_PRELOAD_SPEECH the 58 MB speech tree. Off: nothing plays it yet.
# A 170 MB .data file is fine over localhost and not fine over the internet; the
# replacement is a WASMFS fetch backend, noted in scope-port-b.md §6.
option(LL_PRELOAD_RES "Preload the three RES volumes (157 MB)" ON)
option(LL_PRELOAD_SPEECH "Preload the speech tree (58 MB)" OFF)
set(LL_GAMEDATA "${LL_ROOT}/gamedata" CACHE PATH "Asset tree to preload")
# Each pair is one SHELL: group: CMake de-duplicates repeated link-option
# tokens, so a bare `--preload-file a --preload-file b` reaches emcc as
# `--preload-file a b` and emcc then treats b as an input file.
set(LL_PRELOAD "SHELL:--preload-file ${LL_GAMEDATA}/main@/gamedata")
# stab.str a SECOND time, at .\strings\stab.str (PORT-B2).
#
# LoadStrings (narration2.c 0x00498d00) is `f = fopen(".\\strings\\stab.str",
# "r"); if (!f) exit(1);` -- a bare exit(1), no message, no MessageBoxA. It runs
# in InitSession immediately after the three RES volumes open and BEFORE
# InitHostSystemGPU, so with the file missing the page loads the volumes and then
# vanishes with status 1 and no explanation. That is exactly what it did once
# PORT-A2's loader fix let it get that far.
#
# The file exists -- gamedata/main/stab.str, 11 KB, the 303-entry front-end
# string table ("Player details", "Empty slot", "Cancel") that GetString(id)
# serves and every front-end label draws -- but gamedata/main is FLAT: the
# extraction has no subdirectories at all, while the shipped install had
# strings\. Mapping the one file to both places costs 11 KB and needs no change
# to the game, the loader or the extraction.
#
# It is the only such case. Sweeping every `.\<dir>\` literal in LEGOLAND/*.c
# finds .\3ddata\, .\compsprite\, .\dlls\, .\graphics\ and .\volumes\ besides,
# but all except .\volumes\ are paths INSIDE the mounted RES archives (they go
# through RES_OpenFile, not the CRT) and .\volumes\ is already mapped below.
list(APPEND LL_PRELOAD
     "SHELL:--preload-file ${LL_GAMEDATA}/main/stab.str@/gamedata/strings/stab.str")
if(LL_PRELOAD_RES)
  foreach(vol Legoland Graphics1 Graphics2)
    list(APPEND LL_PRELOAD
         "SHELL:--preload-file ${LL_GAMEDATA}/disc/${vol}.res@/gamedata/volumes/${vol}.res")
  endforeach()
endif()
if(LL_PRELOAD_SPEECH)
  list(APPEND LL_PRELOAD "SHELL:--preload-file ${LL_GAMEDATA}/disc/Speech@/gamedata/speech")
endif()

add_executable(legoland_browser EXCLUDE_FROM_ALL
  "${CMAKE_CURRENT_SOURCE_DIR}/src/browser/main.c")
target_link_libraries(legoland_browser PRIVATE
  legoland_hostwin
  "$<LINK_LIBRARY:WHOLE_ARCHIVE,legoland_core>"
  "$<LINK_LIBRARY:WHOLE_ARCHIVE,legoland_gen_browser>")
target_compile_options(legoland_browser PRIVATE -w)
target_link_options(legoland_browser PRIVATE
  ${LL_WASM_COMMON_LINK}
  ${LL_PRELOAD}
  # The closure closes, so an undefined symbol is a build failure again
  # (PORT-A2; it was 0 while PORT-A's --ilp32 work was in flight). Turning it
  # on changes nothing else: the link is clean, and the page still runs the
  # spine to exactly the same point. Keeping it at 0 would hide the next
  # missing name behind a bare `RuntimeError: unreachable`, which is precisely
  # the failure mode that made this lane's prototype conflicts so expensive to
  # find. NOTE: it does NOT catch those conflicts -- a signature mismatch is a
  # wasm-ld WARNING and a poisoned call site, not an undefined symbol.
  -sERROR_ON_UNDEFINED_SYMBOLS=1)
set_target_properties(legoland_browser PROPERTIES
  SUFFIX ".html" OUTPUT_NAME "legoland"
  LINK_DEPENDS "${LL_WASM_LINK_DEPENDS}")

# ---- legoland_browser_named: the same page, with a name section -------------
#
# PORT-B7. A `call_indirect` whose target's type is not the call site's reaches
# the page as `RuntimeError: function signature mismatch` and nothing else, and
# `name_trap.py` turns the innermost `wasm-function[N]:0xOFF` into a call site
# and a candidate list -- but the CALLERS stay numbers unless the module carries
# a name section, and the first front-end click produces a five-frame stack of
# them. `legoland_headless_debug` cannot help: the mismatch is reached by a
# CLICK, so it only happens in a tab.
#
# -O0 is not available here (headless.cmake:97 -- unoptimised ASYNCIFY of
# RunAppraisalScreen exceeds wasm's per-function local limit), so this is the
# ordinary optimised link plus `-g2`, which keeps the name section through
# wasm-opt. Same code, same behaviour, ~4 MB larger; served as legoland_dbg.html
# beside the real page, and only when someone asks for it (EXCLUDE_FROM_ALL).
#
#   ninja -C portable/build-wasm legoland_browser_named
#   # drive legoland_dbg.html, then:
#   python3 portable/tools/name_trap.py --wasm portable/build-wasm/legoland_dbg.wasm \
#           --at 0x<offset from the innermost frame>
add_executable(legoland_browser_named EXCLUDE_FROM_ALL
  "${CMAKE_CURRENT_SOURCE_DIR}/src/browser/main.c")
target_link_libraries(legoland_browser_named PRIVATE
  legoland_hostwin
  "$<LINK_LIBRARY:WHOLE_ARCHIVE,legoland_core>"
  "$<LINK_LIBRARY:WHOLE_ARCHIVE,legoland_gen_browser>")
target_compile_options(legoland_browser_named PRIVATE -w)
target_link_options(legoland_browser_named PRIVATE
  ${LL_WASM_COMMON_LINK} ${LL_PRELOAD} -g2 -sERROR_ON_UNDEFINED_SYMBOLS=1)
set_target_properties(legoland_browser_named PROPERTIES
  SUFFIX ".html" OUTPUT_NAME "legoland_dbg"
  LINK_DEPENDS "${LL_WASM_LINK_DEPENDS}")
