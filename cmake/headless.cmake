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
  else()
    message(STATUS "PORT-A headless: gamedata/ not present, "
                   "headless_spine not registered (it needs the title artwork "
                   "out of Graphics1.res)")
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
