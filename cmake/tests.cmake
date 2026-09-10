# portable/cmake/tests.cmake -- owned by scope PORT-C (docs/SCOPE_PORT_WAVE.md).
# Included from portable/CMakeLists.txt; add this lane's targets here, not there.
#
# Headless tests of the recovered game code's BEHAVIOUR against the real
# gamedata/, with the expected values produced by the clean-room Python
# decoders in ../tools (tools/oracle_*.py). Notes and the result table:
# docs/lanes/scope-port-c.md.
#
# One executable with subcommands (`legoland_tests <name>`), linked against the
# whole of legoland_core plus the generated closure, exactly as
# legoland_linkcheck is: a subcommand that reaches a piece nobody has ported
# yet dies in gen_link's trap printing the symbol, which is the useful result.
#
# The run target is wasm32 under node (-sNODERAWFS=1, cwd inside gamedata/).
# Tests whose data structures carry no `long` or pointer field in a SERIALISED
# layout also run on the native 64-bit build and are registered there too; each
# test's header comment says which it is and why.

if(NOT EXISTS "${LL_ROOT}/gamedata/disc/Legoland.res")
  message(STATUS "PORT-C tests: gamedata/ not present, tests not registered")
  return()
endif()

enable_testing()

set(LL_TESTS_DIR "${CMAKE_CURRENT_SOURCE_DIR}/tests")
set(LL_ORACLE_DIR "${CMAKE_BINARY_DIR}/gen-tests")
file(MAKE_DIRECTORY "${LL_ORACLE_DIR}")

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
endfunction()

ll_oracle(savechunks)
ll_oracle(tilegeom)

set(LL_ORACLE_HEADERS
  "${LL_ORACLE_DIR}/oracle_savechunks.h"
  "${LL_ORACLE_DIR}/oracle_tilegeom.h")

# ---- the driver ------------------------------------------------------------
set(LL_TEST_SOURCES
  "${LL_TESTS_DIR}/ll_tests.c"
  "${LL_TESTS_DIR}/test_save_framing.c"
  "${LL_TESTS_DIR}/test_tile_geometry.c")

add_executable(legoland_tests EXCLUDE_FROM_ALL
  ${LL_TEST_SOURCES} ${LL_ORACLE_HEADERS})
target_include_directories(legoland_tests PRIVATE
  "${LL_TESTS_DIR}" "${LL_ORACLE_DIR}")
target_compile_options(legoland_tests PRIVATE -Wall -Wno-unused-function)
target_link_libraries(legoland_tests PRIVATE
  "$<LINK_LIBRARY:WHOLE_ARCHIVE,legoland_core>"
  "$<LINK_LIBRARY:WHOLE_ARCHIVE,legoland_gen>")

if(EMSCRIPTEN)
  # NODERAWFS: the game's own relative paths read straight off the disk, so a
  # test's working directory is all that has to be right.
  target_link_options(legoland_tests PRIVATE -sNODERAWFS=1 -sALLOW_MEMORY_GROWTH=1)
endif()

# ---- ctest -----------------------------------------------------------------
# WORKING_DIRECTORY is gamedata/main: that is where LEGOLAND.ICM and the
# .\volumes\*.res / %s%s.res lookups resolve from (docs/runtime/assets.md).
# Tests that only need a scratch file run in the build directory.
function(ll_add_test name cwd)
  add_test(NAME ${name}
           COMMAND legoland_tests ${name}
           WORKING_DIRECTORY "${cwd}")
  set_tests_properties(${name} PROPERTIES
    FAIL_REGULAR_EXPRESSION "FAIL|TRAP|unwritten|unhosted")
endfunction()

ll_add_test(save_framing "${CMAKE_BINARY_DIR}")
ll_add_test(tile_geometry "${CMAKE_BINARY_DIR}")
