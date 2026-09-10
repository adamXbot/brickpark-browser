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
  add_executable(legoland_headless EXCLUDE_FROM_ALL src/headless/main.c)
  target_include_directories(legoland_headless PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/hostwin/include")
  target_link_libraries(legoland_headless PRIVATE
    "$<LINK_LIBRARY:WHOLE_ARCHIVE,legoland_core>"
    "$<LINK_LIBRARY:WHOLE_ARCHIVE,legoland_gen>")
  target_link_options(legoland_headless PRIVATE
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
    -sASSERTIONS=1)
endif()
