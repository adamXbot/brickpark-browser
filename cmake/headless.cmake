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
endif()
