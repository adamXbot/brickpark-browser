# cmake/browser.cmake -- owned by scope PORT-B (docs/SCOPE_PORT_WAVE.md).
# Included from CMakeLists.txt; add this lane's targets here, not there.
#
# Three things live here:
#
#   legoland_hostwin   the host shim itself (DDRAW / USER32 / GDI32 / DINPUT /
#                      WINMM / DSOUND). Part of the default build on EVERY
#                      toolchain, including the native 64-bit one, so a syntax
#                      or signature error shows up in `ninja -C build`
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
  # PORT-B11: the game's OWN typeface. gamedata/main/Lego.TTF is shipped and
  # gpu.c hands it to AddFontResourceA, so ll_ttf.c reads it and ll_font.c
  # measures and draws with the real metrics; without the file (the native and
  # node builds have no mounted gamedata) it falls back to the bitmap face.
  "${CMAKE_CURRENT_SOURCE_DIR}/src/hostwin/ll_ttf.c"
  "${CMAKE_CURRENT_SOURCE_DIR}/src/hostwin/dinput.c"
  "${CMAKE_CURRENT_SOURCE_DIR}/src/hostwin/winmm.c"
  "${CMAKE_CURRENT_SOURCE_DIR}/src/hostwin/dsound.c"
  # PORT-B11: the Web Audio back end behind PORT-B4's silent IDirectSound. EM_JS
  # rather than a --js-library entry, so it links into the node targets too
  # (where it is a set of counters); see ll_audio.c's header.
  "${CMAKE_CURRENT_SOURCE_DIR}/src/hostwin/ll_audio.c"
  # PORT-B6: the last two DLLs the import table names that were still generated
  # traps. avifil32.c reports "this file will not open" through all sixteen
  # AVIFile entry points, which is the path movie.c/advisor.c are written for;
  # msacm32.c is a real PCM converter, because data2.c runs every sample in the
  # archives through it and drops the ones it cannot convert. With these two the
  # page runs with ZERO traps.
  "${CMAKE_CURRENT_SOURCE_DIR}/src/hostwin/avifil32.c"
  "${CMAKE_CURRENT_SOURCE_DIR}/src/hostwin/msacm32.c"
  # The DirectMusic lane: the game's music. ll_dls.c is a DLS Level 1 synth,
  # ll_dmfile.c reads imusic\'s segments, styles and band, ll_dmperf.c is the
  # performance and style engine, and ll_dmusic.c is the COM MusicThread calls
  # plus the pump that feeds ll_audio.c. Without Web Audio (the native and
  # node builds) CoCreateInstance still fails exactly as it always did.
  "${CMAKE_CURRENT_SOURCE_DIR}/src/hostwin/ll_dls.c"
  "${CMAKE_CURRENT_SOURCE_DIR}/src/hostwin/ll_dmfile.c"
  "${CMAKE_CURRENT_SOURCE_DIR}/src/hostwin/ll_dmperf.c"
  "${CMAKE_CURRENT_SOURCE_DIR}/src/hostwin/ll_dmusic.c")

add_library(legoland_hostwin STATIC ${LL_HOSTWIN_SOURCES})
target_include_directories(legoland_hostwin PUBLIC
  "${CMAKE_CURRENT_SOURCE_DIR}/hostwin/include")
# No -include ll_portable.h and no LEGOLAND_PORTABLE here: the shim is ordinary
# C that the game calls into, not game code.
target_compile_options(legoland_hostwin PRIVATE -fno-strict-aliasing -w)

# ---- the DirectMusic lane: which DLS collection plays the music -------------
#
# The game ships no instruments of its own. Its bands name General MIDI / GS
# programs, which DirectMusic plays from the collection the registry's
# GMFilePath names -- and the LEGOLAND CD brings that collection along: its
# DirectX 7 redistributable, directx.cab, holds the Roland GS Sound Set as
# gm16.dls, which its directx.inf installs as GMFilePath on Windows 95. So the
# instruments come from the disc like everything else: tools/mscab.py extracts
# gm16.dls from LL_DIRECTX_CAB (gamedata/disc/directx.cab unless set -- a
# mounted CD's directx.cab works as well) into the build tree as dls/gm.dls.
# LL_MUSIC_DLS plays the music with another GM/GS DLS Level 1 collection
# instead. Either is read from the user's own files, exactly like gamedata/, and
# never committed. No collection: the page runs as before, silently. Decided
# here, above the native return, because tests.cmake's dmusic_selftest uses it
# too.
set(LL_DIRECTX_CAB "${LL_ROOT}/gamedata/disc/directx.cab" CACHE FILEPATH
    "The LEGOLAND CD's directx.cab: its gm16.dls plays the game's music")
set(LL_MUSIC_DLS "" CACHE FILEPATH
    "A GM/GS DLS collection to play the music with instead of the CD's gm16.dls")
set(LL_MUSIC_DLS_FILE "")
set(LL_MUSIC_DLS_TARGET "")
if(LL_MUSIC_DLS)
  set(LL_MUSIC_DLS_FILE "${LL_MUSIC_DLS}")
elseif(EXISTS "${LL_DIRECTX_CAB}")
  set(LL_MUSIC_DLS_FILE "${CMAKE_BINARY_DIR}/dls/gm.dls")
  add_custom_command(
    OUTPUT "${LL_MUSIC_DLS_FILE}"
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tools/mscab.py"
            --member gm16.dls --out "${LL_MUSIC_DLS_FILE}" "${LL_DIRECTX_CAB}"
    DEPENDS "${LL_DIRECTX_CAB}" "${CMAKE_CURRENT_SOURCE_DIR}/tools/mscab.py"
    COMMENT "dls/gm.dls: the instruments, gm16.dls from the disc's directx.cab"
    VERBATIM)
  add_custom_target(legoland_music_dls DEPENDS "${LL_MUSIC_DLS_FILE}")
  set(LL_MUSIC_DLS_TARGET legoland_music_dls)
endif()

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
set(LL_WASM_BASE_LINK
  -O2
  -sASYNCIFY=1
  -sASYNCIFY_STACK_SIZE=131072
  -sALLOW_MEMORY_GROWTH=1
  -sINITIAL_MEMORY=268435456
  -sSTACK_SIZE=8388608
  -sEXIT_RUNTIME=0
  -sASSERTIONS=1
  --js-library "${CMAKE_CURRENT_SOURCE_DIR}/src/browser/ll_canvas.js")
# The developer pages are emcc's --shell-file output; the player page
# (legoland_web, below) is a static page that loads the module itself.
set(LL_WASM_COMMON_LINK ${LL_WASM_BASE_LINK}
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
  # afternoon once (PORT-A2). CMakeLists.txt's own `gen` command has
  # the same gap; nobody but the integrator may edit that file.
  DEPENDS legoland_core legoland_hostwin
          "${CMAKE_CURRENT_SOURCE_DIR}/tools/gen_link.py"
          "${CMAKE_CURRENT_SOURCE_DIR}/tools/linkreport.py"
          "${CMAKE_CURRENT_SOURCE_DIR}/tools/cdecl.py"
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

# PARK-2: the advisor's six clips, decoded at BUILD time into the frames
# avifil32.c serves. InterfaceBG.lls leaves the in-game advisor window at game
# (522, 378) transparent and only an advisor frame ever repaints it, so with no
# frames the cursor and every bubble drawn over that window stay on screen
# (docs/lanes/scope-port-b11.md §4). The clips are Indeo 5, which no browser
# decodes; ffmpeg's indeo5 decoder is the only new dependency and
# tools/advisor_frames.py has the file format. Without ffmpeg or
# gamedata/ the page builds as before and the window is a hole again.
# The six names are advisor.c's kAdBlink..kAdWobble, the only advisor clips the
# exe names; gamedata/main spells some in lower case, so the match is by
# lower-cased stem.
set(LL_ADVISOR_CLIPS AD_Blink AD_LR AD_Phone AD_PhoneGesture AD_PhoneDown AD_Wobble)
set(LL_ADVISOR_DIR "${CMAKE_BINARY_DIR}/advisor")
set(LL_ADVISOR_FRAMES)
find_program(LL_FFMPEG ffmpeg)
if(LL_FFMPEG AND EXISTS "${LL_GAMEDATA}/main")
  file(GLOB _ll_avis "${LL_GAMEDATA}/main/*.avi" "${LL_GAMEDATA}/main/*.AVI")
  set(_ll_advisor_avis)
  foreach(_clip ${LL_ADVISOR_CLIPS})
    string(TOLOWER "${_clip}" _want)
    set(_found)
    foreach(_avi ${_ll_avis})
      get_filename_component(_stem "${_avi}" NAME_WE)
      string(TOLOWER "${_stem}" _stem)
      if(_stem STREQUAL _want)
        set(_found "${_avi}")
      endif()
    endforeach()
    if(_found)
      list(APPEND _ll_advisor_avis "${_found}")
      list(APPEND LL_ADVISOR_FRAMES "${LL_ADVISOR_DIR}/${_want}.llv")
    else()
      message(STATUS "advisor frames: ${_clip}.avi is not in ${LL_GAMEDATA}/main")
    endif()
  endforeach()
endif()
if(LL_ADVISOR_FRAMES)
  add_custom_command(
    OUTPUT ${LL_ADVISOR_FRAMES}
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tools/advisor_frames.py"
            --ffmpeg "${LL_FFMPEG}" --out "${LL_ADVISOR_DIR}" ${_ll_advisor_avis}
    DEPENDS ${_ll_advisor_avis} "${CMAKE_CURRENT_SOURCE_DIR}/tools/advisor_frames.py"
    COMMENT "advisor_frames.py: the advisor's Indeo 5 clips as 16-bpp frames (PARK-2)"
    VERBATIM)
  add_custom_target(legoland_advisor_frames DEPENDS ${LL_ADVISOR_FRAMES})
  list(APPEND LL_PRELOAD "SHELL:--preload-file ${LL_ADVISOR_DIR}@/gamedata/advisor")
else()
  message(STATUS "advisor frames: not built (ffmpeg: ${LL_FFMPEG}); the in-game "
                 "advisor window will not repaint (PARK-2)")
endif()
# The DirectMusic lane: imusic\ and the instruments.
#
# MusicThread opens "imusic\segtheme1.sgt" and its loader scans imusic\ for
# *.sgt and *.sty, but gamedata/main is flat (the retail install had IMusic\).
# The 243 music files are copied into the build tree and preloaded once at
# /gamedata/imusic, and the instruments chosen above land at
# /gamedata/dls/gm.dls, where ll_dmusic.c looks. Both or neither: segments with
# no instruments would play silence.
set(LL_MUSIC_DIR "${CMAKE_BINARY_DIR}/imusic")
set(LL_MUSIC_STAMP)
set(LL_MUSIC_LINK_DEPENDS)
file(GLOB _ll_music_files "${LL_GAMEDATA}/main/*.sgt" "${LL_GAMEDATA}/main/*.sty"
                          "${LL_GAMEDATA}/main/*.bnd")
if(_ll_music_files AND LL_MUSIC_DLS_FILE)
  list(REMOVE_DUPLICATES _ll_music_files)
  list(LENGTH _ll_music_files _ll_music_count)
  set(LL_MUSIC_STAMP "${LL_MUSIC_DIR}/.staged")
  set(LL_MUSIC_LINK_DEPENDS "${LL_MUSIC_STAMP}" "${LL_MUSIC_DLS_FILE}")
  add_custom_command(
    OUTPUT "${LL_MUSIC_STAMP}"
    COMMAND "${CMAKE_COMMAND}" -E make_directory "${LL_MUSIC_DIR}"
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different ${_ll_music_files} "${LL_MUSIC_DIR}"
    COMMAND "${CMAKE_COMMAND}" -E touch "${LL_MUSIC_STAMP}"
    DEPENDS ${_ll_music_files}
    COMMENT "imusic: the game's ${_ll_music_count} DirectMusic files"
    VERBATIM)
  add_custom_target(legoland_music_data DEPENDS "${LL_MUSIC_STAMP}")
  if(LL_MUSIC_DLS_TARGET)
    add_dependencies(legoland_music_data ${LL_MUSIC_DLS_TARGET})
    set(_ll_music_dls_from "the disc's ${LL_DIRECTX_CAB}")
  else()
    set(_ll_music_dls_from "${LL_MUSIC_DLS_FILE}")
  endif()
  list(APPEND LL_PRELOAD
       "SHELL:--preload-file ${LL_MUSIC_DIR}@/gamedata/imusic"
       "SHELL:--preload-file ${LL_MUSIC_DLS_FILE}@/gamedata/dls/gm.dls")
  message(STATUS "music: ${_ll_music_count} imusic files, instruments from "
                 "${_ll_music_dls_from}")
else()
  message(STATUS "music: not packaged (music files: ${_ll_music_count}, instruments: "
                 "'${LL_MUSIC_DLS_FILE}'); put the CD's directx.cab in gamedata/disc "
                 "(or set LL_DIRECTX_CAB) to hear the music")
endif()

# file_packager packs the frames and the music into legoland.data AT LINK: they
# are link inputs.
set(LL_BROWSER_LINK_DEPENDS ${LL_WASM_LINK_DEPENDS} ${LL_ADVISOR_FRAMES} ${LL_MUSIC_LINK_DEPENDS})

# The page's "version" cell: the commits this build was made from, linked to
# their repositories. build_info.py runs on every build and rewrites the file
# only when a commit changes, so an unchanged tree does not relink.
set(LL_BUILD_INFO_JS "${CMAKE_BINARY_DIR}/build_info.js")
add_custom_target(legoland_build_info
  COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tools/build_info.py"
          --root "${CMAKE_CURRENT_SOURCE_DIR}" --decomp "${LL_DECOMP}"
          --out "${LL_BUILD_INFO_JS}"
  BYPRODUCTS "${LL_BUILD_INFO_JS}"
  COMMENT "build_info.py: the commits for the page's version cell"
  VERBATIM)
list(APPEND LL_BROWSER_LINK_DEPENDS "${LL_BUILD_INFO_JS}")

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
  "SHELL:--pre-js ${LL_BUILD_INFO_JS}"
  # The closure closes, so an undefined symbol is a build failure again
  # (PORT-A2; it was 0 while PORT-A's --ilp32 work was in flight). Turning it
  # on changes nothing else: the link is clean, and the page still runs the
  # spine to exactly the same point. Keeping it at 0 would hide the next
  # missing name behind a bare `RuntimeError: unreachable`, which is precisely
  # the failure mode that made this lane's prototype conflicts so expensive to
  # find. NOTE: it does NOT catch those conflicts -- a signature mismatch is a
  # wasm-ld WARNING and a poisoned call site, not an undefined symbol.
  -sERROR_ON_UNDEFINED_SYMBOLS=1
  # PORT-B9/B5: IDBFS, so a profile written this session is still there on the
  # next page load (main.c mounts it over /gamedata/profiles). The JS library
  # only -- no ASYNCIFY change is needed, this target already has it.
  -lidbfs.js)
set_target_properties(legoland_browser PROPERTIES
  SUFFIX ".html" OUTPUT_NAME "legoland"
  LINK_DEPENDS "${LL_BROWSER_LINK_DEPENDS}")
add_dependencies(legoland_browser legoland_build_info)
if(TARGET legoland_advisor_frames)
  add_dependencies(legoland_browser legoland_advisor_frames)
endif()
if(TARGET legoland_music_data)
  add_dependencies(legoland_browser legoland_music_data)
endif()

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
#   ninja -C build-wasm legoland_browser_named
#   # drive legoland_dbg.html, then:
#   python3 tools/name_trap.py --wasm build-wasm/legoland_dbg.wasm \
#           --at 0x<offset from the innermost frame>
add_executable(legoland_browser_named EXCLUDE_FROM_ALL
  "${CMAKE_CURRENT_SOURCE_DIR}/src/browser/main.c")
target_link_libraries(legoland_browser_named PRIVATE
  legoland_hostwin
  "$<LINK_LIBRARY:WHOLE_ARCHIVE,legoland_core>"
  "$<LINK_LIBRARY:WHOLE_ARCHIVE,legoland_gen_browser>")
target_compile_options(legoland_browser_named PRIVATE -w)
target_link_options(legoland_browser_named PRIVATE
  ${LL_WASM_COMMON_LINK} ${LL_PRELOAD} "SHELL:--pre-js ${LL_BUILD_INFO_JS}"
  -g2 -sERROR_ON_UNDEFINED_SYMBOLS=1
  -lidbfs.js)
set_target_properties(legoland_browser_named PROPERTIES
  SUFFIX ".html" OUTPUT_NAME "legoland_dbg"
  LINK_DEPENDS "${LL_BROWSER_LINK_DEPENDS}")
add_dependencies(legoland_browser_named legoland_build_info)
if(TARGET legoland_advisor_frames)
  add_dependencies(legoland_browser_named legoland_advisor_frames)
endif()
if(TARGET legoland_music_data)
  add_dependencies(legoland_browser_named legoland_music_data)
endif()

# ---- legoland_web: the player page ------------------------------------------
#
# What a PLAYER opens, as opposed to the instrumented page above: a launcher that
# installs the game's files into the browser -- downloaded from the site's data
# pack, or read out of the player's own disc image or install folder -- manages
# the saves, runs the same game module, and has somewhere to go when the game
# exits. README.md, "The player page", has the whole story.
#
# It is one static directory that any web server can host, build-wasm/web/:
#
#   index.html, launcher.css, js/*.js   copied from src/web
#   legoland.js, legoland.wasm          this link: no --preload-file (the page
#                                       fills /gamedata from IndexedDB) and no
#                                       auto-run (the page calls callMain once
#                                       the files are in)
#   version.json                        the build id the page cache-busts the
#                                       module with
#   data/                               the data pack (manifest.json, core.tar,
#                                       speech.tar, volumes/), only when
#                                       gamedata/ is here and LL_WEB_DATA is ON.
#                                       It is the game's own files: host it only
#                                       where you have the right to.
#
#   ninja -C build-wasm legoland_web
#   python3 -m http.server -d build-wasm/web 8080
option(LL_WEB_DATA "Build the player page's downloadable data pack from gamedata/" ON)
set(LL_WEB_SRC "${CMAKE_CURRENT_SOURCE_DIR}/src/web")
set(LL_WEB_OUT "${CMAKE_BINARY_DIR}/web")
set(LL_WEB_TOOL "${CMAKE_CURRENT_SOURCE_DIR}/tools/web_datapack.py")
file(GLOB LL_WEB_FILES CONFIGURE_DEPENDS
     "${LL_WEB_SRC}/*.html" "${LL_WEB_SRC}/*.css" "${LL_WEB_SRC}/*.svg" "${LL_WEB_SRC}/js/*.js")
add_custom_command(
  OUTPUT "${LL_WEB_OUT}/.static.stamp"
  COMMAND "${Python3_EXECUTABLE}" "${LL_WEB_TOOL}" static
          --src "${LL_WEB_SRC}" --out "${LL_WEB_OUT}"
  DEPENDS ${LL_WEB_FILES} "${LL_WEB_TOOL}"
  COMMENT "web_datapack.py static: the player page's HTML, CSS and scripts"
  VERBATIM)
add_custom_target(legoland_web_static DEPENDS "${LL_WEB_OUT}/.static.stamp")

if(LL_WEB_DATA AND EXISTS "${LL_GAMEDATA}/main/stab.str" AND EXISTS "${LL_GAMEDATA}/disc/Legoland.res")
  file(GLOB LL_WEB_GAMEDATA CONFIGURE_DEPENDS
       "${LL_GAMEDATA}/main/*" "${LL_GAMEDATA}/disc/*.res")
  add_custom_command(
    OUTPUT "${LL_WEB_OUT}/data/manifest.json"
    COMMAND "${Python3_EXECUTABLE}" "${LL_WEB_TOOL}" data
            --gamedata "${LL_GAMEDATA}" --advisor "${LL_ADVISOR_DIR}"
            --out "${LL_WEB_OUT}/data"
    DEPENDS ${LL_WEB_GAMEDATA} ${LL_ADVISOR_FRAMES} "${LL_WEB_TOOL}"
    COMMENT "web_datapack.py data: the player page's data pack from gamedata/"
    VERBATIM)
  add_custom_target(legoland_web_data DEPENDS "${LL_WEB_OUT}/data/manifest.json")
  if(TARGET legoland_advisor_frames)
    add_dependencies(legoland_web_data legoland_advisor_frames)
  endif()
else()
  message(STATUS "player page: no data pack (LL_WEB_DATA=${LL_WEB_DATA}, gamedata "
                 "at ${LL_GAMEDATA}); the page will offer the disc import only")
endif()

add_executable(legoland_web EXCLUDE_FROM_ALL
  "${CMAKE_CURRENT_SOURCE_DIR}/src/browser/main.c")
target_link_libraries(legoland_web PRIVATE
  legoland_hostwin
  "$<LINK_LIBRARY:WHOLE_ARCHIVE,legoland_core>"
  "$<LINK_LIBRARY:WHOLE_ARCHIVE,legoland_gen_browser>")
target_compile_options(legoland_web PRIVATE -w)
target_link_options(legoland_web PRIVATE
  ${LL_WASM_BASE_LINK}
  -sINVOKE_RUN=0
  -sEXPORTED_RUNTIME_METHODS=FS,callMain
  "SHELL:--pre-js ${LL_BUILD_INFO_JS}"
  -sERROR_ON_UNDEFINED_SYMBOLS=1
  -lidbfs.js)
set_target_properties(legoland_web PROPERTIES
  SUFFIX ".js" OUTPUT_NAME "legoland"
  RUNTIME_OUTPUT_DIRECTORY "${LL_WEB_OUT}"
  LINK_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/src/browser/ll_canvas.js;${LL_BUILD_INFO_JS}")
add_dependencies(legoland_web legoland_build_info)
add_custom_command(TARGET legoland_web POST_BUILD
  COMMAND "${Python3_EXECUTABLE}" "${LL_WEB_TOOL}" stamp --out "${LL_WEB_OUT}"
  VERBATIM)
add_dependencies(legoland_web legoland_web_static)
if(TARGET legoland_web_data)
  add_dependencies(legoland_web legoland_web_data)
endif()
