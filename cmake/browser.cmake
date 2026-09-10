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
  "${CMAKE_CURRENT_SOURCE_DIR}/src/hostwin/dinput.c"
  "${CMAKE_CURRENT_SOURCE_DIR}/src/hostwin/winmm.c"
  "${CMAKE_CURRENT_SOURCE_DIR}/src/hostwin/dsound.c")

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
set(LL_WASM_COMMON_LINK
  -sASYNCIFY=1
  -sASYNCIFY_STACK_SIZE=131072
  -sALLOW_MEMORY_GROWTH=1
  -sINITIAL_MEMORY=268435456
  -sSTACK_SIZE=8388608
  -sEXIT_RUNTIME=0
  -sASSERTIONS=1
  --js-library "${CMAKE_CURRENT_SOURCE_DIR}/src/browser/ll_canvas.js"
  --shell-file "${CMAKE_CURRENT_SOURCE_DIR}/src/browser/index.html")

# ---- legoland_shimtest: the shim on its own --------------------------------
add_executable(legoland_shimtest EXCLUDE_FROM_ALL
  "${CMAKE_CURRENT_SOURCE_DIR}/src/browser/shimtest.c")
target_link_libraries(legoland_shimtest PRIVATE legoland_hostwin)
target_compile_options(legoland_shimtest PRIVATE -w)
target_link_options(legoland_shimtest PRIVATE ${LL_WASM_COMMON_LINK})
set_target_properties(legoland_shimtest PROPERTIES
  SUFFIX ".html" OUTPUT_NAME "shimtest")

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
  DEPENDS legoland_core legoland_hostwin
          "${CMAKE_CURRENT_SOURCE_DIR}/tools/gen_link.py"
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
set(LL_PRELOAD --preload-file "${LL_GAMEDATA}/main@/gamedata")
if(LL_PRELOAD_RES)
  foreach(vol Legoland Graphics1 Graphics2)
    list(APPEND LL_PRELOAD --preload-file
         "${LL_GAMEDATA}/disc/${vol}.res@/gamedata/volumes/${vol}.res")
  endforeach()
endif()
if(LL_PRELOAD_SPEECH)
  list(APPEND LL_PRELOAD --preload-file "${LL_GAMEDATA}/disc/Speech@/gamedata/speech")
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
  # Until PORT-A's closure is complete, an undefined symbol must be a runtime
  # trap rather than a link failure, so the page still loads and says what it
  # reached. Drop this once the closure is closed.
  -sERROR_ON_UNDEFINED_SYMBOLS=0)
set_target_properties(legoland_browser PROPERTIES
  SUFFIX ".html" OUTPUT_NAME "legoland")
