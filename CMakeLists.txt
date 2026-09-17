cmake_minimum_required(VERSION 3.20)
project(ephemeris-prometheia VERSION 0.1.0 LANGUAGES C CXX)

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
  src/engine.cpp
  src/spk.cpp
  src/frames.cpp
  src/kepler.cpp
  src/time.cpp)
target_include_directories(prometheia PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
  $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)
target_link_libraries(prometheia PUBLIC PkgConfig::ZSTD)
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

# JPL Horizons referee corpus (M5); needs the DE440 binary, SKIPs without it.
prometheia_test(horizons tests/test_horizons.cpp)

add_executable(prometheia-bench tools/bench/bench_main.cpp)
target_link_libraries(prometheia-bench PRIVATE prometheia)

add_executable(prometheia-convert tools/convert/convert_main.cpp)
target_link_libraries(prometheia-convert PRIVATE prometheia)

add_executable(prometheia-info tools/info/info_main.cpp)
target_link_libraries(prometheia-info PRIVATE prometheia)

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
  ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
  LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
  RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
install(DIRECTORY include/prometheia DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
configure_file(tools/prometheia.pc.in prometheia.pc @ONLY)
install(FILES ${CMAKE_CURRENT_BINARY_DIR}/prometheia.pc
  DESTINATION ${CMAKE_INSTALL_LIBDIR}/pkgconfig)
