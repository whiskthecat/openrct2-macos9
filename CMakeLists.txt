# Builds the real OpenRCT2 engine (port/upstream, branch classic-mac) with a
# minimal dependency set for three targets: the host (little-endian reference),
# big-endian PowerPC Linux (byte-order test loop under qemu-user) and
# Mac OS 9 via Retro68. Networking, scripting, TrueType and HTTP are off.
cmake_minimum_required(VERSION 3.22)
project(OpenRCT2Port C CXX)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()
if(CMAKE_SYSTEM_NAME MATCHES "Retro")
  # Every object is post-processed for the toolchain's weak-common bug.
  set(CMAKE_C_COMPILER_LAUNCHER "${CMAKE_CURRENT_LIST_DIR}/container/weakfix-launcher.sh")
  set(CMAKE_CXX_COMPILER_LAUNCHER "${CMAKE_CURRENT_LIST_DIR}/container/weakfix-launcher.sh")
endif()
find_program(CCACHE ccache)
if(CCACHE AND NOT CMAKE_SYSTEM_NAME MATCHES "Retro")
  set(CMAKE_C_COMPILER_LAUNCHER "${CCACHE}")
  set(CMAKE_CXX_COMPILER_LAUNCHER "${CCACHE}")
endif()

set(UP "${CMAKE_CURRENT_LIST_DIR}/upstream")
set(TP "${CMAKE_CURRENT_LIST_DIR}/third_party")

# Generated content identity is linked into the actual executable, not just a
# sidecar. The build shims prepare before the build system reads timestamps.
find_package(Python3 REQUIRED COMPONENTS Interpreter)
set(IDENTITY "${CMAKE_CURRENT_LIST_DIR}/tools/build_identity.py")
add_custom_target(rct_provenance
  COMMAND ${Python3_EXECUTABLE} ${IDENTITY} prepare "${CMAKE_BINARY_DIR}"
  BYPRODUCTS "${CMAKE_BINARY_DIR}/rct-build.cpp")
add_library(rctbuild STATIC "${CMAKE_BINARY_DIR}/rct-build.cpp")
add_dependencies(rctbuild rct_provenance)
target_include_directories(rctbuild PUBLIC "${CMAKE_CURRENT_LIST_DIR}/compat")
if(CMAKE_SYSTEM_NAME MATCHES "Retro")
  target_compile_options(rctbuild PRIVATE -mcpu=750 -mno-altivec)
endif()

function(rct_receipt target)
  add_custom_command(TARGET ${target} POST_BUILD
    COMMAND ${Python3_EXECUTABLE} ${IDENTITY} stamp "${CMAKE_BINARY_DIR}" ${ARGN})
endfunction()

# --- portable C dependencies, compiled from source -------------------------
file(GLOB ZLIB_SOURCES CONFIGURE_DEPENDS "${TP}/zlib/*.c")
add_library(rct_zlib STATIC ${ZLIB_SOURCES})
target_include_directories(rct_zlib PUBLIC "${TP}/zlib")
target_compile_options(rct_zlib PRIVATE -w)
target_compile_definitions(rct_zlib PRIVATE HAVE_UNISTD_H)

file(GLOB PNG_SOURCES CONFIGURE_DEPENDS "${TP}/libpng/png*.c")
list(FILTER PNG_SOURCES EXCLUDE REGEX "pngtest\\.c$")
add_library(rct_png STATIC ${PNG_SOURCES})
target_include_directories(rct_png PUBLIC "${TP}/libpng")
target_compile_options(rct_png PRIVATE -w)
target_compile_definitions(rct_png PRIVATE PNG_ARM_NEON_OPT=0 PNG_POWERPC_VSX_OPT=0 PNG_INTEL_SSE_OPT=0 PNG_MIPS_MSA_OPT=0)
target_link_libraries(rct_png PUBLIC rct_zlib)

file(GLOB ZSTD_SOURCES CONFIGURE_DEPENDS "${TP}/zstd/lib/common/*.c" "${TP}/zstd/lib/compress/*.c" "${TP}/zstd/lib/decompress/*.c")
add_library(rct_zstd STATIC ${ZSTD_SOURCES})
target_include_directories(rct_zstd PUBLIC "${TP}/zstd/lib")
target_compile_options(rct_zstd PRIVATE -w)
target_compile_definitions(rct_zstd PRIVATE ZSTD_DISABLE_ASM=1 ZSTD_LEGACY_SUPPORT=0)

# --- the engine --------------------------------------------------------------
file(GLOB_RECURSE ENGINE_SOURCES CONFIGURE_DEPENDS "${UP}/src/openrct2/*.cpp")
list(FILTER ENGINE_SOURCES EXCLUDE REGEX "/core/(Zip|ZipAndroid|RTL\\.ICU|RTL\\.FriBidi)\\.cpp$")
file(GLOB COMPAT_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_LIST_DIR}/compat/*.cpp")
add_library(openrct2engine STATIC ${ENGINE_SOURCES} ${COMPAT_SOURCES})
target_include_directories(openrct2engine PUBLIC "${UP}/src")
target_include_directories(openrct2engine SYSTEM PUBLIC
  "${CMAKE_CURRENT_LIST_DIR}/compat/icu" "${UP}/src/thirdparty" "${TP}/nlohmann")
target_compile_definitions(openrct2engine PUBLIC
  OPENRCT2_ORIGINAL_LIMITS=1  # map, ride and entity tables sized for the original game (see README, "Memory")
  DISABLE_NETWORK DISABLE_HTTP DISABLE_TTF DISABLE_VERSION_CHECKER OPENRCT2_NO_PROFILING DEBUG=0
  DOCDIR="/usr/share/doc/openrct2" _FILE_OFFSET_BITS=64)
target_compile_options(openrct2engine PUBLIC -fstrict-aliasing -fno-char8_t -Wall -Wno-unknown-pragmas
  -Wno-missing-braces -Wno-comment -Wno-unused-parameter -Wno-attributes -Wno-deprecated-declarations)
target_link_libraries(openrct2engine PUBLIC rct_png rct_zlib rct_zstd rctbuild)

# Field offsets are an ABI choice, so layout A/B uses two builds rather than a live switch.
option(GUEST_HOT_LAYOUT "Keep ordinary guest tick fields together within unchanged entity slots" ON)
if(NOT GUEST_HOT_LAYOUT)
  target_compile_definitions(openrct2engine PUBLIC OPENRCT2_LEGACY_GUEST_LAYOUT=1)
endif()

option(SCROLL_TRACE "Measurement-only scroll cache instruction scopes and lifetime events" OFF)
if(SCROLL_TRACE)
  target_compile_definitions(openrct2engine PUBLIC OPENRCT2_SCROLL_TRACE=1)
endif()

option(PERF_METRICS "Measurement-only guest, damage and sprite counters" OFF)
if(PERF_METRICS)
  target_compile_definitions(openrct2engine PUBLIC OPENRCT2_PERF_METRICS=1)
endif()

option(OG_INSTRUCTION_MARKERS "Emit optional no-op stage markers for instruction profiling" OFF)
option(RIDEFPS_FIXTURES "Linux-only constructed animated ride/scenery test scenes" OFF)
option(RIDEFPS_LEGACY_MAGNIFY "Linux-only original magnifier for paired work measurements" OFF)
option(RIDEFPS_LEGACY_BLOCKS "Linux-only magnifier before train block/span improvements" OFF)
if(RIDEFPS_LEGACY_BLOCKS AND NOT CMAKE_SYSTEM_NAME MATCHES "Retro")
  foreach(sprite_source Drawing.Sprite.cpp Drawing.Sprite.BMP.cpp Drawing.Sprite.RLE.cpp)
    set_property(SOURCE "${UP}/src/openrct2/drawing/${sprite_source}" APPEND
      PROPERTY COMPILE_DEFINITIONS OPENRCT2_LEGACY_MAGNIFY_BLOCKS=1)
  endforeach()
endif()
option(RIDEFPS_LEGACY_RING_INVALIDATION "Linux-only original frozen Space Rings for baseline measurements" OFF)
if(RIDEFPS_LEGACY_MAGNIFY AND NOT CMAKE_SYSTEM_NAME MATCHES "Retro")
  set_property(SOURCE "${UP}/src/openrct2/drawing/Drawing.Sprite.RLE.cpp" APPEND
    PROPERTY COMPILE_DEFINITIONS OPENRCT2_LEGACY_MAGNIFY=1)
endif()
if(RIDEFPS_LEGACY_RING_INVALIDATION AND NOT CMAKE_SYSTEM_NAME MATCHES "Retro")
  set_source_files_properties("${UP}/src/openrct2/entity/EntityBase.cpp"
    PROPERTIES COMPILE_DEFINITIONS OPENRCT2_LEGACY_RING_INVALIDATION=1)
endif()
if(SCROLL_TRACE AND OG_INSTRUCTION_MARKERS)
  message(FATAL_ERROR "SCROLL_TRACE and OG_INSTRUCTION_MARKERS use different stage IDs; enable only one")
endif()
if(OG_INSTRUCTION_MARKERS)
  target_compile_definitions(openrct2engine PUBLIC OPENRCT2_OG_TRACE=1)
endif()

option(FORMATTER_AUDIT "Assert legacy formatter argument widths and record UI coverage" OFF)
if(FORMATTER_AUDIT)
  target_compile_definitions(openrct2engine PUBLIC OPENRCT2_FORMATTER_AUDIT=1)
endif()

option(PAK_VFS "Use the pack-file virtual file system on this host (tests the Mac OS 9 file layer)" OFF)
if(PAK_VFS AND NOT CMAKE_SYSTEM_NAME MATCHES "Retro")
  get_target_property(ENGINE_ALL_SOURCES openrct2engine SOURCES)
  # Also run everything on one thread, as the Mac build does.
  list(FILTER ENGINE_ALL_SOURCES EXCLUDE REGEX "/platform/Platform\\.(Linux|Posix)\\.cpp$|/core/JobPool\\.cpp$")
  set_target_properties(openrct2engine PROPERTIES SOURCES
    "${ENGINE_ALL_SOURCES};${CMAKE_CURRENT_LIST_DIR}/compat/classic/ClassicVfs.cpp;${CMAKE_CURRENT_LIST_DIR}/compat/classic/ClassicLoadProgress.cpp;${CMAKE_CURRENT_LIST_DIR}/compat/classic/ClassicDiskIo.cpp;${CMAKE_CURRENT_LIST_DIR}/compat/classic/ClassicAutosave.cpp;${CMAKE_CURRENT_LIST_DIR}/compat/classic/PlatformClassic.cpp;${CMAKE_CURRENT_LIST_DIR}/compat/classic/ClassicHostPosix.cpp;${CMAKE_CURRENT_LIST_DIR}/compat/classic/JobPoolSync.cpp;${CMAKE_CURRENT_LIST_DIR}/compat/classic/ClassicAudio.cpp;${CMAKE_CURRENT_LIST_DIR}/compat/classic/ClassicSoftwareMixer.cpp;${CMAKE_CURRENT_LIST_DIR}/compat/classic/ClassicMixerStream.cpp")
  target_compile_definitions(openrct2engine PUBLIC OPENRCT2_PAK_VFS=1 OPENRCT2_SINGLE_THREAD=1)
  target_include_directories(openrct2engine PUBLIC "${CMAKE_CURRENT_LIST_DIR}/compat/classic")
endif()
if(NOT CMAKE_SYSTEM_NAME MATCHES "Retro")
  find_package(Threads REQUIRED)
  target_link_libraries(openrct2engine PUBLIC Threads::Threads)
  add_executable(openrct2-cli "${UP}/src/openrct2-cli/Cli.cpp")
  target_link_libraries(openrct2-cli PRIVATE openrct2engine)
  # The CLI does not yet report its identity at startup; receipts still verify
  # its linked identity, retained by explicitly including that translation unit.
  target_sources(openrct2-cli PRIVATE "${CMAKE_BINARY_DIR}/rct-build.cpp")
  add_dependencies(openrct2-cli rct_provenance)
  rct_receipt(openrct2-cli openrct2-cli)
  add_executable(rctsim "${CMAKE_CURRENT_LIST_DIR}/tools/rctsim.cpp")
  target_link_libraries(rctsim PRIVATE openrct2engine)
  option(SIMCOST_TEST_UTIL_RAND "Fix the non-simulation RNG in the Linux headless state oracle only" OFF)
  if(SIMCOST_TEST_UTIL_RAND)
    target_sources(rctsim PRIVATE tools/FixedUtilRand.cpp)
    target_link_options(rctsim PRIVATE "-Wl,--wrap=_Z8UtilRandv")
  endif()
  rct_receipt(rctsim rctsim)
  # Action parity driver: the same source also links to the pristine pinned engine.
  add_executable(rctparity tools/parity/driver.cpp tools/FixedUtilRand.cpp)
  target_link_libraries(rctparity PRIVATE openrct2engine)
  target_link_options(rctparity PRIVATE "-Wl,--wrap=_Z8UtilRandv")
  rct_receipt(rctparity rctparity)
  if(PAK_VFS)
    add_executable(autosave-snapshot-tests tools/autosave_snapshot_tests.cpp)
    target_link_libraries(autosave-snapshot-tests PRIVATE openrct2engine)
    rct_receipt(autosave-snapshot-tests autosave-snapshot-tests)
    add_executable(diskio-autosave-tests "${CMAKE_CURRENT_LIST_DIR}/tools/diskio_autosave_tests.cpp")
    target_link_libraries(diskio-autosave-tests PRIVATE openrct2engine)
    rct_receipt(diskio-autosave-tests diskio-autosave-tests)
    add_executable(diskio-memory-tests "${CMAKE_CURRENT_LIST_DIR}/tools/diskio_memory_tests.cpp")
    target_link_libraries(diskio-memory-tests PRIVATE openrct2engine)
    rct_receipt(diskio-memory-tests diskio-memory-tests)
  endif()
  add_executable(sv4-layout "${CMAKE_CURRENT_LIST_DIR}/tools/sv4_layout.cpp")
  target_link_libraries(sv4-layout PRIVATE openrct2engine)
  rct_receipt(sv4-layout sv4-layout)
endif()

if(CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64|AMD64")
  set_source_files_properties("${UP}/src/openrct2/drawing/AVX2Drawing.cpp" PROPERTIES COMPILE_FLAGS "-mavx2")
  set_source_files_properties("${UP}/src/openrct2/drawing/SSE41Drawing.cpp" PROPERTIES COMPILE_FLAGS "-msse4.1")
endif()

if(CMAKE_SYSTEM_NAME STREQUAL "Linux" AND CMAKE_SYSTEM_PROCESSOR STREQUAL "powerpc")
  target_link_libraries(openrct2engine PUBLIC atomic)  # 64-bit atomics on 32-bit PowerPC
endif()

if(CMAKE_SYSTEM_NAME MATCHES "Retro")
  # Mac OS 9: single cooperative thread, Toolbox file system.
  get_target_property(ENGINE_ALL_SOURCES openrct2engine SOURCES)
  list(FILTER ENGINE_ALL_SOURCES EXCLUDE REGEX "/core/(JobPool|FileWatcher)\\.cpp$|/interface/StdInOutConsole\\.cpp$")
  file(GLOB CLASSIC_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_LIST_DIR}/compat/classic/*.cpp")
  list(FILTER CLASSIC_SOURCES EXCLUDE REGEX "/ClassicDesktop\\.cpp$")
  set_target_properties(openrct2engine PROPERTIES SOURCES "${ENGINE_ALL_SOURCES};${CLASSIC_SOURCES}")
  target_compile_options(openrct2engine PUBLIC -include "${CMAKE_CURRENT_LIST_DIR}/compat/classic/prefix.h")
  target_compile_definitions(openrct2engine PUBLIC OPENRCT2_CLASSIC_MAC=1 OPENRCT2_PAK_VFS=1)
  target_include_directories(openrct2engine PUBLIC "${CMAKE_CURRENT_LIST_DIR}/compat/classic")
  # Use the upstream build-info hook without changing its pinned release version.
  set_source_files_properties("${UP}/src/openrct2/Version.cpp" PROPERTIES
    COMPILE_DEFINITIONS "OPENRCT2_BUILD_INFO_HEADER=\"ClassicVersion.h\"")
  target_compile_options(openrct2engine PUBLIC -mcpu=750 -mno-altivec)
  # This toolchain makes int32_t a long. The engine mixes int and int32_t in
  # template arguments everywhere, so make the 32-bit typedefs plain int
  # (same size and representation on this target).
  target_compile_options(openrct2engine PUBLIC
    -U__INT32_TYPE__ "-D__INT32_TYPE__=int" -U__UINT32_TYPE__ "-D__UINT32_TYPE__=unsigned int"
    -U__INT_LEAST32_TYPE__ "-D__INT_LEAST32_TYPE__=int" -U__UINT_LEAST32_TYPE__ "-D__UINT_LEAST32_TYPE__=unsigned int"
    -U__INT_FAST32_TYPE__ "-D__INT_FAST32_TYPE__=int" -U__UINT_FAST32_TYPE__ "-D__UINT_FAST32_TYPE__=unsigned int")
endif()

if(CMAKE_SYSTEM_NAME MATCHES "Retro")
  add_application(RCTSim CREATOR "ORCS" "${CMAKE_CURRENT_LIST_DIR}/tools/rctsim.cpp" "${CMAKE_CURRENT_LIST_DIR}/tools/RCTSim.r")
  target_link_libraries(RCTSim openrct2engine)
  rct_receipt(RCTSim_APPL RCTSim.bin RCTSim.APPL RCTSim.pef)
  # No --gc-sections: with it the C++ stream library crashed at first use on Mac OS 9.
endif()

# --- the game's user interface (windows, toolbars, input), without SDL -------
option(BUILD_UI "Build the UI library against the SDL header shim" ON)
if(BUILD_UI)
  # Our indexed display/scroll layer presents game pixels at their original size.
  target_compile_definitions(openrct2engine PUBLIC OPENRCT2_SDL_SHIM=1)
  file(GLOB_RECURSE UI_SOURCES CONFIGURE_DEPENDS "${UP}/src/openrct2-ui/*.cpp")
  list(FILTER UI_SOURCES EXCLUDE REGEX "/openrct2-ui/(audio|scripting|drawing/engines)/|/UiContext\\.(Android|Win32|Linux)\\.cpp$|/openrct2-ui/Ui\\.cpp$|openrct2ui_pch")
  file(GLOB SDL_SHIM_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_LIST_DIR}/compat/sdl/*.cpp")
  add_library(openrct2ui STATIC ${UI_SOURCES} ${SDL_SHIM_SOURCES})
  target_include_directories(openrct2ui PUBLIC "${UP}/src" "${UP}/src/openrct2-ui")
  # <SDL.h> and <SDL2/SDL.h> both resolve to the bundled headers.
  target_include_directories(openrct2ui SYSTEM PUBLIC "${CMAKE_CURRENT_LIST_DIR}/compat/sdl/include" "${TP}/sdl2/include")
  target_link_libraries(openrct2ui PUBLIC openrct2engine)
  target_compile_definitions(openrct2ui PUBLIC DISABLE_OPENGL)
  if(RIDEFPS_FIXTURES AND NOT CMAKE_SYSTEM_NAME MATCHES "Retro")
    target_sources(openrct2ui PRIVATE tools/ridefps_fixture.cpp)
    add_executable(magnify-cases tools/magnify_cases.cpp)
    target_link_libraries(magnify-cases PRIVATE openrct2engine)
    target_compile_definitions(openrct2ui PRIVATE OPENRCT2_RIDEFPS_FIXTURES=1)
  endif()
  if(NOT CMAKE_SYSTEM_NAME MATCHES "Retro")
    add_executable(rctgame "${CMAKE_CURRENT_LIST_DIR}/tools/rctgame.cpp" "${CMAKE_CURRENT_LIST_DIR}/tools/interpolation_audit.cpp")
    if(FORMATTER_AUDIT)
      target_sources(rctgame PRIVATE "${CMAKE_CURRENT_LIST_DIR}/tools/formatter_ui_audit.cpp")
    endif()
    target_link_libraries(rctgame PRIVATE openrct2ui)
    rct_receipt(rctgame rctgame)
    if(PAK_VFS)
      add_executable(formatter-cases "${CMAKE_CURRENT_LIST_DIR}/tools/formatter_cases.cpp")
      target_link_libraries(formatter-cases PRIVATE openrct2ui)
      target_sources(formatter-cases PRIVATE "${CMAKE_BINARY_DIR}/rct-build.cpp")
      add_dependencies(formatter-cases rct_provenance)
      rct_receipt(formatter-cases formatter-cases)
      target_compile_options(formatter-cases PRIVATE -UNDEBUG)
    endif()
  else()
    add_application(RCT CREATOR "ORCG" "${CMAKE_CURRENT_LIST_DIR}/tools/rctgame.cpp"
      "${CMAKE_CURRENT_LIST_DIR}/compat/classic/ClassicDesktop.cpp" "${CMAKE_CURRENT_LIST_DIR}/tools/RCT.r")
    # Retro68's Rez command otherwise watches only RCT.r, ignoring its includes.
    add_custom_command(OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/RCT.r.rsrc.bin" APPEND
      DEPENDS "${CMAKE_CURRENT_LIST_DIR}/compat/classic/ClassicVersion.h"
              "${CMAKE_CURRENT_LIST_DIR}/tools/RCTIcon.r")
    target_link_libraries(RCT openrct2ui QuickTimeLib NameRegistryLib DisplayLib IconServicesLib)
    rct_receipt(RCT_APPL RCT.bin RCT.APPL RCT.pef)
  endif()
endif()
