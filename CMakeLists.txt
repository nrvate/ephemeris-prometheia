cmake_minimum_required(VERSION 3.20)
project(ephemeris-prometheia VERSION 0.4.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

include(GNUInstallDirs)
find_package(PkgConfig REQUIRED)
pkg_check_modules(ZSTD REQUIRED IMPORTED_TARGET libzstd)

set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

add_library(prometheia
  src/c_api.cpp
  src/catalog.cpp
  src/cbor.cpp
  src/de.cpp
  src/elements.cpp
  src/engine.cpp
  src/hypotheticals.cpp
  src/segments.cpp
  src/spk.cpp
  src/stars.cpp
  src/frames.cpp
  src/kepler.cpp
  src/time.cpp)
target_include_directories(prometheia PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
  $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)
target_link_libraries(prometheia PUBLIC PkgConfig::ZSTD)

# The shipped hypothetical-body element set (docs/HYPOTHETICALS.md) is compiled
# in from data/hypotheticals.jsonl, read at configure time and re-read whenever
# the file changes; the engine parses it with the same reader an operator's
# element file goes through.
file(READ ${CMAKE_CURRENT_SOURCE_DIR}/data/hypotheticals.jsonl PROMETHEIA_HYPOTHETICALS_JSONL)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
  ${CMAKE_CURRENT_SOURCE_DIR}/data/hypotheticals.jsonl)
configure_file(src/hypotheticals_shipped.inc.in
  ${CMAKE_CURRENT_BINARY_DIR}/generated/hypotheticals_shipped.inc @ONLY)
target_include_directories(prometheia PRIVATE ${CMAKE_CURRENT_BINARY_DIR}/generated)
target_compile_options(prometheia PRIVATE -Wall -Wextra -Werror)

option(PROMETHEIA_SANITIZE "Build with ASan+UBSan" OFF)
if(PROMETHEIA_SANITIZE)
  add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer -g)
  add_link_options(-fsanitize=address,undefined)
endif()

enable_testing()
# Real-data tests look for JPL files under ephe/ (gitignored) by default.
add_compile_definitions(PROMETHEIA_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}")

# Tests use doctest (third_party/doctest, MIT); one shared runner main.
add_library(prometheia_doctest_main STATIC tests/doctest_main.cpp)
target_include_directories(prometheia_doctest_main PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/third_party)

# prometheia_test(<ctest name> <sources...>): executable test_<name>.
function(prometheia_test name)
  add_executable(test_${name} ${ARGN})
  target_link_libraries(test_${name} PRIVATE prometheia prometheia_doctest_main)
  add_test(NAME ${name} COMMAND test_${name})
endfunction()

prometheia_test(cbor tests/test_cbor.cpp)
prometheia_test(catalog tests/test_catalog.cpp)
prometheia_test(kepler tests/test_kepler.cpp)
prometheia_test(elements tests/test_elements.cpp)
prometheia_test(hypotheticals tests/test_hypotheticals.cpp)
prometheia_test(integrator tests/test_integrator.cpp)
prometheia_test(mechanics tests/test_mechanics.cpp)
prometheia_test(radau tests/test_radau.cpp)
prometheia_test(de tests/test_de.cpp)
prometheia_test(spk tests/test_spk.cpp)
prometheia_test(engine tests/test_engine.cpp)
prometheia_test(engine_catalog tests/test_engine_catalog.cpp)

# The C interface: C++ checks against the engine, plus a translation unit
# compiled as strict C99 to prove the header and calling pattern are C.
prometheia_test(c_api tests/test_c_api.cpp tests/c_api_smoke.c)
target_link_libraries(test_c_api PRIVATE m)
set_source_files_properties(tests/c_api_smoke.c PROPERTIES
  COMPILE_OPTIONS "-Wall;-Wextra;-pedantic;-Werror")

prometheia_test(time tests/test_time.cpp)
prometheia_test(frames tests/test_frames.cpp)
prometheia_test(stars tests/test_stars.cpp)
prometheia_test(segments tests/test_segments.cpp)

# JPL Horizons referee corpus (M5); needs the DE440 binary, SKIPs without it.
prometheia_test(horizons tests/test_horizons.cpp)

# prometheiad's protocol core (docs/SERVER.md): Astrolog's ephemeris protocol
# (third_party/ephproto, pinned) over the engine, with no sockets in it.
add_library(prometheia_server STATIC server/session.cpp server/dataset.cpp server/limits.cpp
  server/log.cpp server/metrics.cpp server/objects.cpp server/segcache.cpp)
target_include_directories(prometheia_server PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/server)
target_include_directories(prometheia_server SYSTEM PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/third_party/ephproto/v4)
target_link_libraries(prometheia_server PUBLIC prometheia)
target_compile_options(prometheia_server PRIVATE -Wall -Wextra -Werror)

# libFuzzer targets (docs/SERVER.md, "Fuzzing"): clang only, built by
# tools/fuzz.sh in build-fuzz/, never by the gate.
option(PROMETHEIA_FUZZ "Build the libFuzzer targets (clang)" OFF)
if(PROMETHEIA_FUZZ)
  foreach(target fuzz_session fuzz_frame)
    add_executable(${target} fuzz/${target}.cpp)
    target_link_libraries(${target} PRIVATE prometheia_server)
    target_include_directories(${target} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/tests)
    target_link_options(${target} PRIVATE -fsanitize=fuzzer)
  endforeach()
endif()

prometheia_test(server tests/test_server.cpp)
target_link_libraries(test_server PRIVATE prometheia_server)
prometheia_test(segcache tests/test_segcache.cpp)
target_link_libraries(test_segcache PRIVATE prometheia_server)

# The protocol v4 conformance pin: Astrolog's fixture set through the
# vendored codec (tests/test_ephproto4.cpp; env-gated, SKIPs without the
# fixtures). The v4 header is a separate include dir from the still-live v3.
prometheia_test(ephproto4 tests/test_ephproto4.cpp)
target_include_directories(test_ephproto4 SYSTEM PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/third_party/ephproto/v4)

# The WebSocket head: uWebSockets + uSockets (third_party, Apache-2.0),
# without zlib (compression is off). With OpenSSL found, uSockets is built
# with TLS and prometheiad can serve wss:// (PROMETHEIA_TLS).
find_package(OpenSSL)
option(PROMETHEIA_WITH_TLS "Build prometheiad with TLS (needs OpenSSL)" ${OPENSSL_FOUND})
file(GLOB USOCKETS_SOURCES third_party/uSockets/src/*.c third_party/uSockets/src/eventing/*.c)
if(PROMETHEIA_WITH_TLS)
  list(APPEND USOCKETS_SOURCES third_party/uSockets/src/crypto/openssl.c
    third_party/uSockets/src/crypto/sni_tree.cpp)
endif()
add_library(usockets STATIC ${USOCKETS_SOURCES})
target_include_directories(usockets SYSTEM PUBLIC third_party/uSockets/src)
set_target_properties(usockets PROPERTIES C_STANDARD 11 C_EXTENSIONS ON)
target_compile_options(usockets PRIVATE -w)
if(PROMETHEIA_WITH_TLS)
  target_compile_definitions(usockets PUBLIC LIBUS_USE_OPENSSL)
  target_link_libraries(usockets PUBLIC OpenSSL::SSL OpenSSL::Crypto)
else()
  target_compile_definitions(usockets PUBLIC LIBUS_NO_SSL)
endif()

find_package(Threads REQUIRED)
add_library(prometheia_ws STATIC server/ws_server.cpp server/ws_client.cpp server/tls.cpp)
target_include_directories(prometheia_ws SYSTEM PRIVATE third_party/uWebSockets/src)
target_compile_definitions(prometheia_ws PRIVATE UWS_NO_ZLIB)
if(PROMETHEIA_WITH_TLS)
  target_compile_definitions(prometheia_ws PUBLIC PROMETHEIA_TLS)
endif()
target_link_libraries(prometheia_ws PUBLIC prometheia_server usockets Threads::Threads)
target_compile_options(prometheia_ws PRIVATE -Wall -Wextra -Werror)

add_executable(prometheiad server/prometheiad_main.cpp)
target_link_libraries(prometheiad PRIVATE prometheia_ws)
target_compile_options(prometheiad PRIVATE -Wall -Wextra -Werror)

add_executable(prometheia-wire-client server/wire_client_main.cpp)
target_link_libraries(prometheia-wire-client PRIVATE prometheia_ws)
target_compile_options(prometheia-wire-client PRIVATE -Wall -Wextra -Werror)

# Many connections for a while (docs/SERVER.md, "Load and soak"); by hand,
# never in the gate.
add_executable(prometheia-load server/load_main.cpp)
target_link_libraries(prometheia-load PRIVATE prometheia_ws)
target_compile_options(prometheia-load PRIVATE -Wall -Wextra -Werror)

prometheia_test(prometheiad tests/test_prometheiad.cpp)
target_link_libraries(test_prometheiad PRIVATE prometheia_ws)

add_executable(prometheia-bench tools/bench/bench_main.cpp)
target_link_libraries(prometheia-bench PRIVATE prometheia)

add_executable(prometheia-catalog-bench tools/bench/catalog_bench_main.cpp)
target_link_libraries(prometheia-catalog-bench PRIVATE prometheia Threads::Threads)

add_executable(prometheia-convert tools/convert/convert_main.cpp)
target_link_libraries(prometheia-convert PRIVATE prometheia)

add_executable(prometheia-info tools/info/info_main.cpp)
target_link_libraries(prometheia-info PRIVATE prometheia)

# Regenerates src/mean_elements.inc from a DE file (docs/ENGINE.md).
add_executable(prometheia-gen-mean-elements tools/gen/gen_mean_elements.cpp)
target_link_libraries(prometheia-gen-mean-elements PRIVATE prometheia)

# Delta T from a published sidereal time, for tools/check/crosstest.py.
add_executable(prometheia-ut1 tools/check/ut1_main.cpp)
target_link_libraries(prometheia-ut1 PRIVATE prometheia)

add_executable(prometheia-spk-trim tools/spktrim/spk_trim_main.cpp)
target_link_libraries(prometheia-spk-trim PRIVATE prometheia)

# ephem: the command-line consumer of the C interface, in strict C99.
add_executable(ephem tools/ephem/ephem.c)
target_link_libraries(ephem PRIVATE prometheia m)
target_compile_options(ephem PRIVATE -Wall -Wextra -pedantic -Werror)

prometheia_test(ephem tests/test_ephem.cpp)
target_compile_definitions(test_ephem PRIVATE EPHEM_BIN="$<TARGET_FILE:ephem>")
add_dependencies(test_ephem ephem)

# Install: the library, the public headers (C++ and C), the tools and a
# pkg-config file. `cmake --install build --prefix /usr/local`.
install(TARGETS prometheia ephem prometheia-info prometheia-convert prometheia-spk-trim
  prometheiad prometheia-wire-client
  ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
  LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
  RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
install(DIRECTORY include/prometheia DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
# Two pkg-config files: the build-tree copy points at the real uninstalled
# artifacts (PKG_CONFIG_PATH=build works, as the Astrolog plugin's build
# does), and the installed copy stays relocatable around its install prefix.
configure_file(tools/prometheia.pc.build.in prometheia.pc @ONLY)
configure_file(tools/prometheia.pc.in prometheia.pc.install @ONLY)
install(FILES ${CMAKE_CURRENT_BINARY_DIR}/prometheia.pc.install
  DESTINATION ${CMAKE_INSTALL_LIBDIR}/pkgconfig RENAME prometheia.pc)
