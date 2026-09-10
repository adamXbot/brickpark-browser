# portable/cmake/tests.cmake -- owned by scope PORT-C (docs/SCOPE_PORT_WAVE.md).
# Included from portable/CMakeLists.txt; add this lane's targets here, not there.
#
# Headless tests of the recovered game code's BEHAVIOUR against the real
# gamedata/, with the expected values produced by the clean-room Python
# decoders in ../tools (tools/oracle_*.py). Notes, the result table and every
# divergence found: docs/lanes/scope-port-c.md.
#
# One executable with subcommands (`legoland_tests <name>`), linked against the
# whole of legoland_core plus the generated closure, exactly as
# legoland_linkcheck is: a subcommand that reaches a piece nobody has ported
# yet dies in gen_link's trap printing the symbol, which is the useful result.
#
# The real run target is wasm32 under node (-sNODERAWFS=1, cwd inside
# gamedata/). Tests whose data structures carry no `long` or pointer field in a
# SERIALISED layout also run on the native 64-bit build and are registered
# there too; each test's header comment says which it is and why.
#
#   cmake -S portable -B portable/build -G Ninja -DPython3_EXECUTABLE=$PY
#   ninja -C portable/build legoland_tests && ctest --test-dir portable/build
#
#   emcmake cmake -S portable -B portable/build-wasm -G Ninja -DLL_ILP32=ON \
#       -DPython3_EXECUTABLE=$PY
#   ninja -C portable/build-wasm legoland_tests      # waits on PORT-A's closure
#   ctest --test-dir portable/build-wasm

if(NOT EXISTS "${LL_ROOT}/gamedata/disc/Legoland.res")
  message(STATUS "PORT-C tests: gamedata/ not present, tests not registered")
  return()
endif()

enable_testing()

set(LL_TESTS_DIR "${CMAKE_CURRENT_SOURCE_DIR}/tests")
set(LL_ORACLE_DIR "${CMAKE_BINARY_DIR}/gen-tests")
file(MAKE_DIRECTORY "${LL_ORACLE_DIR}")

# ---- the stopgap host slice ------------------------------------------------
# The asset loaders need a handful of KERNEL32 calls and the CRT-range wrappers
# portable/README.md's census files as "game-fn". Those are PORT-A's to own
# (brief deliverable 3, src/hostwin/kernel32.c). Until that file exists, this
# lane compiles its own minimal version INTO legoland_core -- which is what
# stops gen_link.py emitting a trap for the same names (it reads the defined
# set from every object under legoland_core.dir, and the referenced set only
# from the ../LEGOLAND/*.c ones). The guard makes it disappear by itself the
# moment PORT-A lands; see the header of the file for the full reasoning.
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/src/hostwin/kernel32.c")
  message(STATUS "PORT-C tests: PORT-A's kernel32 shim is present, "
                 "tests/support/ll_test_host.c not compiled")
else()
  message(STATUS "PORT-C tests: compiling the stopgap "
                 "tests/support/ll_test_host.c into legoland_core "
                 "(PORT-A's src/hostwin/kernel32.c does not exist yet)")
  target_sources(legoland_core PRIVATE
    "${LL_TESTS_DIR}/support/ll_test_host.c")
endif()

# ---- oracles ---------------------------------------------------------------
# Every expectation is generated at build time from gamedata/ by a Python
# oracle, so nothing derived from the game's assets is ever committed.
function(ll_oracle name)
  add_custom_command(
    OUTPUT "${LL_ORACLE_DIR}/oracle_${name}.h"
    COMMAND "${Python3_EXECUTABLE}" "${LL_ROOT}/tools/oracle_${name}.py"
            --header "${LL_ORACLE_DIR}/oracle_${name}.h" ${ARGN}
    DEPENDS "${LL_ROOT}/tools/oracle_${name}.py"
    COMMENT "oracle_${name}.py: expected values from the Python decoders"
    VERBATIM)
  set(LL_ORACLE_HEADERS ${LL_ORACLE_HEADERS}
      "${LL_ORACLE_DIR}/oracle_${name}.h" PARENT_SCOPE)
endfunction()

ll_oracle(savechunks)
ll_oracle(tilegeom)
ll_oracle(res)
ll_oracle(icm)
ll_oracle(geom)
# PORT-B3: one real 16-bpp COMP frame decoded by tools/comp.py.
ll_oracle(rlepaint)

# ---- the driver ------------------------------------------------------------
add_executable(legoland_tests EXCLUDE_FROM_ALL
  "${LL_TESTS_DIR}/ll_tests.c"
  "${LL_TESTS_DIR}/test_save_framing.c"
  "${LL_TESTS_DIR}/test_tile_geometry.c"
  "${LL_TESTS_DIR}/test_res_archive.c"
  "${LL_TESTS_DIR}/test_llidb_icm.c"
  "${LL_TESTS_DIR}/test_loadpos.c"
  # PORT-A3 (docs/lanes/scope-port-a3.md): the generator's interior aliases.
  "${LL_TESTS_DIR}/test_keystate.c"
  # PORT-B3 (docs/lanes/scope-port-b3.md): the ten type-3 RLE painters and
  # the type-2 LLS animation painter.
  "${LL_TESTS_DIR}/test_rle_paint.c"
  "${LL_TESTS_DIR}/test_anim_paint.c"
  ${LL_ORACLE_HEADERS})
target_include_directories(legoland_tests PRIVATE
  "${LL_TESTS_DIR}" "${LL_ORACLE_DIR}")
target_compile_options(legoland_tests PRIVATE -Wall -Wno-unused-function)

# PORT-C is closed; PORT-A2 owns this block (docs/lanes/scope-port-a2.md §3).
#
# `loadpos` traps in USER32's ShowWindow because the closure the tests link,
# legoland_gen, has a TRAPPING STUB for every DDRAW/USER32/GDI32/DINPUT/WINMM
# name -- PORT-B's real shim lives in legoland_hostwin, which nothing here
# linked. Linking it means also swapping the closure for legoland_gen_browser,
# the same closure with those stubs filtered out (browser.cmake's
# closure_filter.py); linking both would be a duplicate definition of every
# one of them.
#
# The shim needs five browser-only symbols under node: the four ll_canvas.js
# entry points (--js-library, not linked here, and its ll_js_display_open
# touches `document`) and emscripten_sleep (ASYNCIFY, which the tests do not
# want). src/headless/node_shim.c stubs exactly those five, as the brief
# requires -- PORT-B's user32.c and ddraw.c are NOT edited. They tolerate node
# otherwise: the shim only reaches JS through those four calls.
if(EMSCRIPTEN)
  target_sources(legoland_tests PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/src/headless/node_shim.c")
  target_link_libraries(legoland_tests PRIVATE
    legoland_hostwin
    "$<LINK_LIBRARY:WHOLE_ARCHIVE,legoland_core>"
    "$<LINK_LIBRARY:WHOLE_ARCHIVE,legoland_gen_browser>")
else()
  target_link_libraries(legoland_tests PRIVATE
    "$<LINK_LIBRARY:WHOLE_ARCHIVE,legoland_core>"
    "$<LINK_LIBRARY:WHOLE_ARCHIVE,legoland_gen>")
endif()

if(EMSCRIPTEN)
  # NODERAWFS: the game's own relative paths read straight off the disk, so a
  # test's working directory is all that has to be right. The closure's globals
  # are a few megabytes of .data, hence the memory settings.
  target_link_options(legoland_tests PRIVATE
    -sNODERAWFS=1 -sALLOW_MEMORY_GROWTH=1 -sINITIAL_MEMORY=134217728
    -sSTACK_SIZE=4194304 -sEXIT_RUNTIME=1
    # Keep the wasm name section: a prototype conflict is not a TRAP, it is a
    # poisoned call site, and without this it reads `wasm-function[112]`.
    --profiling-funcs)
endif()

# ---- ctest -----------------------------------------------------------------
# ll_add_test(<name> <working dir> <ILP32-only?>)
#   The working directory is the one the game's own relative paths are written
#   against: gamedata/disc for the .res volumes, gamedata/main for
#   LEGOLAND.ICM (docs/runtime/assets.md). Tests needing only a scratch file
#   run in the build directory.
#   An ILP32-only test is registered for the Emscripten build only: on a 64-bit
#   host its serialised layouts are wrong by construction (the per-test header
#   comment says exactly how), so running it there would report noise. It still
#   COMPILES and LINKS in the native build, which is what keeps it honest while
#   PORT-A's wasm32 closure is in flight.
function(ll_add_test name cwd ilp32_only)
  if(ilp32_only AND NOT EMSCRIPTEN)
    message(STATUS "PORT-C tests: ${name} is ILP32-only, "
                   "built but not registered in this build")
    return()
  endif()
  add_test(NAME ${name}
           COMMAND legoland_tests ${name}
           WORKING_DIRECTORY "${cwd}")
  # LL_CD_DIR: with PORT-B's shim linked (above), a test that reaches
  # RES_EnsureMounted no longer trips over a generated trap -- it runs
  # sysmisc.c's real missing-CD loop, `while (!RES_FindVolumeOnAnyDrive(...))
  # MessageBoxA("Please insert the LEGOLAND CD-ROM")`, which has NO other exit
  # and spun `loadpos` until ctest's timeout. Pointing the emulated CD drive at
  # the volumes is the answer for a test; the answer for the page is PORT-B's
  # MessageBoxA returning IDCANCEL.
  set_tests_properties(${name} PROPERTIES
    FAIL_REGULAR_EXPRESSION "FAIL|TRAP|unwritten|unhosted"
    ENVIRONMENT "LL_CD_DIR=${LL_ROOT}/gamedata/disc"
    TIMEOUT 300)
endfunction()

ll_add_test(save_framing  "${CMAKE_BINARY_DIR}"       FALSE)
ll_add_test(tile_geometry "${CMAKE_BINARY_DIR}"       FALSE)
ll_add_test(res_archive   "${LL_ROOT}/gamedata/disc"  TRUE)
ll_add_test(llidb_icm     "${LL_ROOT}/gamedata/main"  TRUE)
ll_add_test(loadpos       "${LL_ROOT}/gamedata/disc"  TRUE)
# PORT-A3: pure layout, no assets and no host shim -- runs on both toolchains.
ll_add_test(keystate      "${CMAKE_BINARY_DIR}"       FALSE)
# PORT-B3: a synthetic RLE frame into a local surface. No assets, no host shim,
# no serialised layout, so it runs on both toolchains too.
ll_add_test(rle_paint     "${CMAKE_BINARY_DIR}"       FALSE)
ll_add_test(anim_paint    "${CMAKE_BINARY_DIR}"       FALSE)
