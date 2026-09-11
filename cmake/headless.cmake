# portable/cmake/headless.cmake -- owned by scope PORT-A (docs/SCOPE_PORT_WAVE.md).
# Included from portable/CMakeLists.txt; add this lane's targets here, not there.
#
# Two things live here:
#
# 1. The KERNEL32/ADVAPI32/VERSION shim joins legoland_core. It has to be in
#    THAT target, not a library of its own: gen_link.py reads the objects of
#    legoland_core to decide what is still missing, and only stops generating
#    a trapping stub for a symbol it can see defined there. (PORT-B's shims
#    want the same treatment from browser.cmake.)
#
# 2. legoland_headless: the game's own WinMain under node, with no graphics.
#    wasm32 only -- a 64-bit build of the game links but cannot run (`long`
#    and pointer fields change every struct offset), so there is nothing to
#    run headless there.

target_sources(legoland_core PRIVATE
  "${CMAKE_CURRENT_SOURCE_DIR}/src/hostwin/kernel32.c")

# legoland_core is compiled with -Wno-everything for the game's sources; the
# shim is new code and wants its warnings back (source options come after the
# target's on the command line).
set_source_files_properties("${CMAKE_CURRENT_SOURCE_DIR}/src/hostwin/kernel32.c"
  PROPERTIES COMPILE_OPTIONS "-Wall;-Wextra;-Wno-unused-parameter")

# ---- the extent parser's own gate (PORT-A6) ---------------------------------
# `cdecl.py` computes every object's extent from the game's own struct
# definitions; `STRUCT_EXTENTS` in gen_link.py is now the REGRESSION FIXTURE for
# it -- five records that each broke something visible before somebody worked
# out its size by hand. The selftest checks the MSVC layout rules against
# hand-computed types and then checks that all five of those rows are still
# reproduced from the real sources. It reads only LEGOLAND/*.c, so unlike
# headless_spine and probe_input it needs no gamedata/ and runs in CI, on both
# toolchains.
enable_testing()
add_test(NAME cdecl_extents
         COMMAND "${Python3_EXECUTABLE}"
                 "${CMAKE_CURRENT_SOURCE_DIR}/tools/cdecl.py" --selftest)
set_tests_properties(cdecl_extents PROPERTIES
  PASS_REGULAR_EXPRESSION "cdecl selftest: 0 failure"
  FAIL_REGULAR_EXPRESSION "FAIL"
  TIMEOUT 300)

# ---- the multi-address extern gate (PORT-M6 §1f, promoted by PORT-A8) -------
# One `extern` statement, several declarators, several addresses in its one
# trailing comment:
#
#     extern void *g_route_open, *g_route_closed;   /* 0x00668fc0, 0x00668fc4 */
#
# Every scanner in the tree takes the FIRST address and gives it to every name
# in the statement, so both objects are emitted at 0x00668fc0 and, in the
# portable build, `g_route_open` IS `g_route_closed`. PORT-M6 found three and
# split them.
#
# This is a class NO BYTE GATE CAN SEE. The C compiles to identical bytes either
# way -- on x86 the declaration only has to say "a pointer lives somewhere" and
# the linker supplies the address -- so audit.py, relocs.py and verify.py are
# all silent, and two live globals alias each other. PORT-M6 and PORT-M8 each
# re-ran it from a scratch script; a test holds the line without anyone
# remembering to look. Exit status is the gate: 0 closed, 1 on any hit.
#
# Sources only -- no gamedata/, no image, no build products -- so it runs in CI
# on both toolchains, like cdecl_extents.
add_test(NAME extern_sweep
         COMMAND "${Python3_EXECUTABLE}"
                 "${CMAKE_CURRENT_SOURCE_DIR}/tools/extern_sweep.py")
set_tests_properties(extern_sweep PROPERTIES
  PASS_REGULAR_EXPRESSION "0 multi-address extern statement"
  FAIL_REGULAR_EXPRESSION "FIX:"
  TIMEOUT 300)

# The sweep's own shapes, positive and negative, so a refactor of the statement
# reader cannot quietly turn the gate above into a test that always passes.
add_test(NAME extern_sweep_selftest
         COMMAND "${Python3_EXECUTABLE}"
                 "${CMAKE_CURRENT_SOURCE_DIR}/tools/extern_sweep.py" --selftest)
set_tests_properties(extern_sweep_selftest PROPERTIES
  PASS_REGULAR_EXPRESSION "extern_sweep selftest: 0 failure"
  FAIL_REGULAR_EXPRESSION "FAIL"
  TIMEOUT 300)

# ---- slot_sweep's pattern matcher (PORT-M8's two sweeps, promoted by A8) ----
# `slot_sweep.py` answers "who CALLS this vtable slot" off the shipped image, in
# both forms VC6 emits (`call [reg+off]` and `mov reg,[base+off] ... call reg`).
# The sweep itself needs original/legoland.exe and capstone and so is NOT part
# of the asset-free set, but its matcher is testable on hand-assembled bytes --
# including the null-check shape (`test`/`je` between the load and the call)
# that every load-then-call site in the image uses and whose mishandling makes a
# called slot look uncalled.
add_test(NAME slot_sweep_selftest
         COMMAND "${Python3_EXECUTABLE}"
                 "${CMAKE_CURRENT_SOURCE_DIR}/tools/slot_sweep.py" --selftest)
set_tests_properties(slot_sweep_selftest PROPERTIES
  PASS_REGULAR_EXPRESSION "slot_sweep selftest: 0 failure"
  FAIL_REGULAR_EXPRESSION "FAIL"
  TIMEOUT 300)

# ---- the by-value-struct ABI gate (PORT-M10's sweep, promoted by PORT-A9) ----
# A parameter one TU spells as a by-value aggregate of 4 bytes or fewer and
# another spells as a scalar is one dword on x86 cdecl and is NOT the same thing
# on wasm32: the aggregate is passed INDIRECTLY, so the callee reads a
# shadow-stack pointer. Both lower to one i32, so wasm-ld warns about nothing,
# linkreport.py's conflict vote sees no conflict and test_callback_types.c
# type-checks slots rather than parameters. Nothing in the tree could see this
# class until PORT-M10 found it by hand: AddObjectToBuildList filed the Space
# Tower's map tile as (114, 10), so LINK never satisfied and the tower drew as a
# squat block (PARK-1 and PARK-3 in one defect).
#
# The gate is the accepted list: `portable/tests/bvstruct_accepted.txt` holds the
# 15 addresses a lane has ruled on, each with its reason, and any OTHER silent
# site fails. Sources only -- no gamedata/, no image, no build products -- so it
# runs in CI on both toolchains, like extern_sweep.
add_test(NAME bvstruct_sweep
         COMMAND "${Python3_EXECUTABLE}"
                 "${CMAKE_CURRENT_SOURCE_DIR}/tools/bvstruct_sweep.py"
                 "${LL_ROOT}/LEGOLAND"
                 --baseline "${CMAKE_CURRENT_SOURCE_DIR}/tests/bvstruct_accepted.txt")
set_tests_properties(bvstruct_sweep PROPERTIES
  PASS_REGULAR_EXPRESSION "0 unaccepted silent site"
  FAIL_REGULAR_EXPRESSION "FIX "
  TIMEOUT 300)

# The sweep's own shapes AND the ABI claim itself. The classifier half is the
# usual positive/negative control (a 2-byte BPos is reported, a single-element
# struct is not, an 8-byte Pos is noisy not silent, and a FIXED site -- the
# scalar in the LEGOLAND_PORTABLE arm -- is not reported, which is what makes a
# gate possible at all). The other half compiles PORT-M10 §1b's three-file repro
# with the real emcc and the real clang and checks that they still disagree:
# native packs (63,35) as 0x233f, wasm32 hands over a shadow-stack pointer. If a
# future emsdk changed that, this sweep would be reporting phantoms, and the
# control says so instead of passing quietly. It SKIPS that half (with a printed
# line) when emcc or node is not on PATH, so the native build still runs the
# classifier checks.
add_test(NAME bvstruct_sweep_selftest
         COMMAND "${Python3_EXECUTABLE}"
                 "${CMAKE_CURRENT_SOURCE_DIR}/tools/bvstruct_sweep.py" --selftest)
set_tests_properties(bvstruct_sweep_selftest PROPERTIES
  PASS_REGULAR_EXPRESSION "bvstruct_sweep selftest: 0 failure"
  FAIL_REGULAR_EXPRESSION "FAIL"
  TIMEOUT 600)

# ---- PORT-A9: the census that does NOT ask the declarations ------------------
# `pointer_words` (tests.cmake) is honest about the words the DECLARATION scan
# visited, and that is the hole PORT-B11 §3 fell into: `extern FXEntry
# g_game_fx[];` has no bound, so cdecl.py computes no extent, so gen_link never
# visits the object, so its three raw x86 string addresses are not raw POINTER
# words -- 23 sound effects lost under a gate reporting zero.
#
# `gen/rawwords.md` asks the image instead: every 4-byte word of every emitted
# object whose value lands in the original .rdata/.data, with the inline-text
# vetoes A2 measured. The residue is not zero yet (most of it is bounds PORT-M11
# owes), so the gate is a BASELINE -- `portable/tests/rawwords_baseline.txt`
# lists the accepted rows with a reason each. A NEW row or a row that GREW
# fails; a row that shrinks prints SHRUNK and passes, so a game-side fix tightens
# the file instead of fighting it.
#
# Asset-free and toolchain-independent: it reads the censuses this build wrote,
# and in the 64-bit build the census is vacuous by construction (re-pointing is
# off), exactly like the pointer gate.
add_test(NAME raw_words
         COMMAND "${Python3_EXECUTABLE}"
                 "${CMAKE_CURRENT_SOURCE_DIR}/tools/gen_link.py"
                 "${CMAKE_BINARY_DIR}" --check-rawwords
                 --baseline "${CMAKE_CURRENT_SOURCE_DIR}/tests/rawwords_baseline.txt")
set_tests_properties(raw_words PROPERTIES
  PASS_REGULAR_EXPRESSION "raw-word gate: 0 failure"
  FAIL_REGULAR_EXPRESSION "FAIL"
  TIMEOUT 300)

if(EMSCRIPTEN)
  # Two harnesses from the same sources: `legoland_headless` is the one to run,
  # and `legoland_headless_debug` is the one that NAMES a trap. See
  # ll_headless_target below for what the second one changes and why.
  function(ll_headless_target name)
    add_executable(${name} EXCLUDE_FROM_ALL
      src/headless/main.c src/headless/node_shim.c)
    target_include_directories(${name} PRIVATE
      "${CMAKE_CURRENT_SOURCE_DIR}/hostwin/include")
    # PORT-B's shim, and with it the FILTERED closure: legoland_gen has a
    # trapping stub for every DDRAW/USER32/GDI32/DINPUT/WINMM name, which would
    # be a duplicate definition of the shim's real one. legoland_gen_browser is
    # the same closure with those stubs commented out (browser.cmake's
    # closure_filter.py). Linking a target that browser.cmake defines further
    # down is fine: CMake resolves target names at generate time.
    #
    # Before this the harness stopped at `TRAP DDRAW.dll DirectDrawCreate`, which
    # was the right answer while PORT-B's shim did not exist and is the wrong one
    # now: everything interesting in the game is behind that call.
    # src/headless/node_shim.c supplies the five ll_canvas.js entry points and
    # emscripten_sleep, which the browser gets from --js-library and ASYNCIFY.
    target_link_libraries(${name} PRIVATE
      legoland_hostwin
      "$<LINK_LIBRARY:WHOLE_ARCHIVE,legoland_core>"
      "$<LINK_LIBRARY:WHOLE_ARCHIVE,legoland_gen_browser>")
    target_link_options(${name} PRIVATE
      # The real filesystem, so gamedata/ is read in place from the cwd node
      # was started in -- no --preload-file, no asset packaging.
      -sNODERAWFS=1
      # The rebuilt globals alone are 3.7 MB of .data, and the game allocates
      # the map and the sprite caches on top.
      -sALLOW_MEMORY_GROWTH=1
      -sINITIAL_MEMORY=134217728
      # The game recurses through its object lists; 64 KB of stack is not enough.
      -sSTACK_SIZE=8388608
      # exit(70) from a trap has to reach node as an exit code.
      -sEXIT_RUNTIME=1
      -sASSERTIONS=1
      # Keep the wasm name section. This harness exists to say WHERE the game
      # stopped, and without it a prototype conflict arrives as
      # `RuntimeError: unreachable at wasm-function[26]` -- with it, the frame
      # reads `signature_mismatch:LLIDB_FindElement`, which names the defect.
      --profiling-funcs)
  endfunction()

  ll_headless_target(legoland_headless)

  # ---- naming a trap ------------------------------------------------------
  # A prototype conflict is not a TRAP and not an undefined symbol: wasm-ld
  # replaces the mismatched call with a stub it calls
  # `signature_mismatch:<callee>`, whose whole body is `unreachable`. The name
  # section keeps that stub's name (--profiling-funcs above), so in principle
  # the stack frame says which function is poisoned -- except that emcc's
  # link-time -O2 runs wasm-opt, which INLINES a one-instruction body into
  # every caller. The frame then reads `main` (or whatever the call was folded
  # into) and the callee's name is gone.
  #
  # So the only thing this target changes is link-time optimisation: -O0 leaves
  # wasm-opt out of the link, the stub survives as a real function, and the
  # stack reads `signature_mismatch:RES_CloseFile <- LoadPos <- main`.
  # -g2 keeps the full name section rather than just the function names.
  #
  # It is a separate target, not a build-type switch, because it has to be
  # buildable in the SAME build directory as the target it explains: the whole
  # point is to name a trap the optimised harness just hit, without
  # reconfiguring and without rebuilding legoland_core (the game objects are
  # shared, and it is the LINK that creates the stub).
  #
  #   ninja -C portable/build-wasm legoland_headless_debug
  #   python3 portable/tools/name_trap.py            # builds it if need be
  #
  # ASYNCIFY is what makes this cheap: the browser target cannot be linked at
  # -O0 (unoptimised ASYNCIFY of RunAppraisalScreen exceeds wasm's per-function
  # local limit, see portable/README.md), and the headless harness has no
  # ASYNCIFY at all.
  ll_headless_target(legoland_headless_debug)
  target_link_options(legoland_headless_debug PRIVATE -O0 -g2)

  # ---- the spine as a CI gate ---------------------------------------------
  # The browser page and this harness stop at the same place for the same reason
  # (docs/lanes/scope-port-a3.md §3), so the startup spine is a test instead of
  # something a human looks at in a tab. `--stages` walks InitSession's own order
  # with the prototypes the DEFINING files use.
  #
  # THE SKIP LIST IS A RATCHET. It started as `loadsprite,rungame`: `loadsprite`
  # because __BMPLoader's `RES_CloseFile` call was poisoned by a prototype
  # conflict, `rungame` because the front end does not return. PORT-M2 closed the
  # first, so it is gone from this line -- the eight cursor sprites load. Only
  # `rungame` is left, and it is not a defect: four statements past
  # ShowTitleScreen the front end enters `while (g_music_disabled == 0)
  # { PeekMessageA; Sleep(100); }` and nothing clears that flag while
  # DirectSoundCreate reports no driver (PORT-B's dsound.c,
  # docs/lanes/scope-port-b3.md). The `title` stage runs RunGame's prefix up to
  # ShowTitleScreen instead, so the picture is still gated.
  #
  # ---- what the two numbers mean ------------------------------------------
  # `title` asserts the first present is at least 40% non-black and compares the
  # frame's FNV-1a checksum against the value below. 0x4a092b01 is the title
  # artwork as PORT-B3's painters produce it (640x480, 301157/307200 = 98%
  # non-black). A change that alters the title screen fails here by design:
  #
  #   * if the change was intended, run
  #     `node legoland_headless.js --stages rungame` and put the checksum it
  #     prints in LL_TITLE_FRAME_SUM below, in the same commit as the change;
  #   * if it was not, the frame is the evidence -- something in the sprite
  #     pipeline (rlepaint.c, softblit*.c, the palette, the clip rect) moved.
  #
  # Unpinning it (setting LL_TITLE_FRAME_SUM to 0) keeps the non-black assertion
  # and only prints the checksum, which is the right thing to do if the frame
  # ever turns out to depend on something outside the tree.
  set(LL_TITLE_FRAME_SUM "0x4a092b01")
  #
  # ctest runs it only when gamedata/ is there: the mount, the directories, the
  # string table and the title artwork are the substance of the test, and CI has
  # no game assets (.github/workflows/progress.yml).
  if(EXISTS "${LL_ROOT}/gamedata/disc/Legoland.res")
    enable_testing()
    add_test(NAME headless_spine
             COMMAND node "$<TARGET_FILE_DIR:legoland_headless>/legoland_headless.js"
                     --frame-sum "${LL_TITLE_FRAME_SUM}" --stages rungame)
    set_tests_properties(headless_spine PROPERTIES
      ENVIRONMENT "LL_CD_DIR=${LL_ROOT}/gamedata/disc;LL_DATA_DIR=${LL_ROOT}/gamedata/main"
      # `SPINE OK, first present checksum ...` is printed only after every stage
      # ran AND the frame passed both assertions, which is the only single
      # pattern that says both (PASS_REGULAR_EXPRESSION is an OR over a list).
      PASS_REGULAR_EXPRESSION "SPINE OK, first present checksum"
      FAIL_REGULAR_EXPRESSION "FAIL|TRAP|unreachable|RuntimeError|cannot chdir"
      TIMEOUT 300)
    # ---- input reaches the game's own globals (PORT-A5) -------------------
    # One synthetic gesture through the shim's injection points, straight into
    # ScanMouse / ScanKeyboard / UpdateControllerFromMouseData /
    # ReadGameButtons, with the layout of the 0x00813a40 record checked first.
    # The record is `GameInput` to the files that write it and a dozen loose
    # globals to the files that read it, and when the closure split it into one
    # block per field the two halves addressed different memory -- the front end
    # then ignored every key and click, silently, for 3,700 frames (PORT-B4 §5).
    # This is the gate that makes that regression impossible to re-introduce
    # without a red test. It needs the volumes mounted (InitInputSystem opens
    # the display first), hence gamedata/.
    add_test(NAME probe_input
             COMMAND node "$<TARGET_FILE_DIR:legoland_headless>/legoland_headless.js"
                     --probe-input)
    set_tests_properties(probe_input PROPERTIES
      ENVIRONMENT "LL_CD_DIR=${LL_ROOT}/gamedata/disc;LL_DATA_DIR=${LL_ROOT}/gamedata/main"
      PASS_REGULAR_EXPRESSION "INPUT OK"
      FAIL_REGULAR_EXPRESSION "FAIL|TRAP|unreachable|RuntimeError|cannot chdir"
      TIMEOUT 300)
  else()
    message(STATUS "PORT-A headless: gamedata/ not present, "
                   "headless_spine and probe_input not registered (they need "
                   "the title artwork and the volumes out of Graphics1.res)")
  endif()

  # ---- install-path resolution over a PRELOADED MEMFS ----------------------
  # `ll_host_resolve_path` is what makes the paths the game wrote for a 1999
  # install resolve on a modern host, and on this Mac it could not have had a
  # useful test: every other node target uses -sNODERAWFS=1, which goes straight
  # to APFS, and APFS is case-insensitive by default, so the case-folding code
  # never runs (`ls gamedata/main/LEGOLAND.ICM` lists `Legoland.icm`). The
  # browser's preloaded MEMFS is the opposite and the one that matters: it is
  # case-SENSITIVE whatever machine packaged it, and so is Linux CI on ext4.
  #
  # So this target is the browser's filesystem under node: no NODERAWFS, a
  # --preload-file tree, and nothing else. The fixture below is generated from
  # nothing -- empty files whose NAMES mirror the real install's mixed case -- so
  # no game asset is involved and the test runs with or without gamedata/.
  set(LL_PATHFIX "${CMAKE_BINARY_DIR}/gen-pathfix")
  file(WRITE "${LL_PATHFIX}/main/Legoland.icm" "")              # asked for as LEGOLAND.ICM
  file(WRITE "${LL_PATHFIX}/main/stab.str" "")                  # asked for as .\strings\Stab.str
  file(WRITE "${LL_PATHFIX}/main/Graphics/Erase It.lls" "")     # real subdir, mixed case, a space
  file(WRITE "${LL_PATHFIX}/cd/Legoland.res" "")                # the emulated D: drive
  add_executable(legoland_pathtest EXCLUDE_FROM_ALL
    src/headless/pathtest.c src/hostwin/kernel32.c)
  target_include_directories(legoland_pathtest PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/hostwin/include")
  target_compile_options(legoland_pathtest PRIVATE
    -Wall -Wextra -Wno-unused-parameter)
  target_link_options(legoland_pathtest PRIVATE
    -sEXIT_RUNTIME=1 -sALLOW_MEMORY_GROWTH=1
    "SHELL:--preload-file ${LL_PATHFIX}/main@/gamedata"
    "SHELL:--preload-file ${LL_PATHFIX}/cd@/cd")
  enable_testing()
  add_test(NAME install_paths
           COMMAND node "$<TARGET_FILE_DIR:legoland_pathtest>/legoland_pathtest.js")
  # No ENVIRONMENT here on purpose: emscripten seeds its environment from the
  # host's process.env only under NODERAWFS, so a MEMFS build cannot be told
  # $LL_CD_DIR that way and the test sets it itself -- which is exactly what
  # PORT-B's src/browser/main.c has to do for the page.
  set_tests_properties(install_paths PROPERTIES
    FAIL_REGULAR_EXPRESSION "FAIL"
    TIMEOUT 120)
endif()
