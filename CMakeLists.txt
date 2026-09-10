# LEGOLAND portable build.
#
# Compiles the matching decompilation in ../LEGOLAND with a modern compiler
# (clang, gcc or emcc) into one static library. Nothing links into a program
# yet: run `python3 portable/tools/linkreport.py <build dir>` on the objects
# to get the authoritative list of what is still missing (unwritten game
# functions, Win32/DirectX host API, unported asm bodies).
#
# The sources are ILP32 code (int == long == pointer == 4 bytes). A 64-bit
# host compile links and is a correct symbol census, but it is not runnable:
# struct layouts written with `long` and pointer fields change size. The run
# targets are wasm32 (Emscripten) and 32-bit native.
cmake_minimum_required(VERSION 3.20)
project(legoland_portable C)

set(LL_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/..")
file(GLOB LL_CORE_SOURCES CONFIGURE_DEPENDS "${LL_ROOT}/LEGOLAND/*.c")

add_library(legoland_core STATIC
  ${LL_CORE_SOURCES}
  src/hostwin/portable.c
  src/hostwin/msvcrt.c
)
target_include_directories(legoland_core PUBLIC
  "${CMAKE_CURRENT_SOURCE_DIR}/hostwin/include"
  "${LL_ROOT}/LEGOLAND"
)
target_compile_definitions(legoland_core PUBLIC LEGOLAND_PORTABLE=1)

# -fms-extensions: __declspec, __int64, __fastcall/__stdcall (ignored off x86).
# -fcommon: the sources rely on tentative definitions merging across files.
# -fno-strict-aliasing / -fwrapv: the code reinterprets float/int storage and
#   relies on two's-complement wraparound throughout.
# -include ll_portable.h: the asm fallbacks and CRT names, without touching
#   the include lines the VC6 build sees.
target_compile_options(legoland_core PRIVATE
  -fms-extensions -fdeclspec -fcommon -fno-strict-aliasing -fwrapv
  -Wno-everything
  -include "${CMAKE_CURRENT_SOURCE_DIR}/hostwin/include/ll_portable.h"
)
if(CMAKE_C_COMPILER_ID STREQUAL "GNU")
  # gcc has no -fdeclspec / -Wno-everything.
  set_property(TARGET legoland_core PROPERTY COMPILE_OPTIONS
    -fms-extensions -fcommon -fno-strict-aliasing -fwrapv -w
    -include "${CMAKE_CURRENT_SOURCE_DIR}/hostwin/include/ll_portable.h")
endif()

# ---- link closure ---------------------------------------------------------
# gen_link.py reads the compiled objects and writes what is still missing as
# C (globals from the exe, aliases, trapping stubs), then legoland_linkcheck
# links the WHOLE archive against it. `ninja legoland_linkcheck` is the
# milestone check; `python3 tools/linkreport.py build` is the census.
find_package(Python3 COMPONENTS Interpreter REQUIRED)
set(LL_EXE "${LL_ROOT}/original/legoland.exe" CACHE FILEPATH "Original legoland.exe (initialised data source)")
option(LL_ILP32 "Re-point 32-bit pointer tables at rebuilt symbols (wasm32/i386 only)" OFF)
set(LL_GEN_FLAGS "")
if(LL_ILP32)
  set(LL_GEN_FLAGS --ilp32)
endif()
set(LL_GEN_DIR "${CMAKE_BINARY_DIR}/gen")
set(LL_GEN_SOURCES
  "${LL_GEN_DIR}/globals.c" "${LL_GEN_DIR}/aliases.c"
  "${LL_GEN_DIR}/stubs.c"   "${LL_GEN_DIR}/host_stubs.c")
add_custom_command(
  OUTPUT ${LL_GEN_SOURCES} "${LL_GEN_DIR}/manifest.md" "${LL_GEN_DIR}/ll_gen.h"
  COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tools/gen_link.py"
          "${CMAKE_BINARY_DIR}/CMakeFiles/legoland_core.dir"
          --out "${LL_GEN_DIR}" --exe "${LL_EXE}" ${LL_GEN_FLAGS}
  DEPENDS legoland_core
          "${CMAKE_CURRENT_SOURCE_DIR}/tools/gen_link.py"
          "${CMAKE_CURRENT_SOURCE_DIR}/tools/linkreport.py"
          "${CMAKE_CURRENT_SOURCE_DIR}/tools/win32_imports.txt"
  COMMENT "gen_link.py: globals, aliases and stubs for the link closure"
  VERBATIM)
add_library(legoland_gen STATIC EXCLUDE_FROM_ALL ${LL_GEN_SOURCES})
target_include_directories(legoland_gen PRIVATE "${LL_GEN_DIR}")
target_compile_options(legoland_gen PRIVATE -w)

add_executable(legoland_linkcheck EXCLUDE_FROM_ALL src/linkcheck.c)
target_link_libraries(legoland_linkcheck PRIVATE
  "$<LINK_LIBRARY:WHOLE_ARCHIVE,legoland_core>"
  "$<LINK_LIBRARY:WHOLE_ARCHIVE,legoland_gen>")

# ---- port wave lanes (docs/SCOPE_PORT_WAVE.md) ------------------------------
# Each lane owns one of these files so three agents never edit this one.
include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/headless.cmake")   # PORT-A: wasm32 link closure + node startup harness
include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/browser.cmake")    # PORT-B: Emscripten canvas host shim
include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/tests.cmake")      # PORT-C: headless subsystem tests against gamedata/
