cmake_minimum_required(VERSION 3.20)
project(ephemeris-prometheia VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

find_package(PkgConfig REQUIRED)
pkg_check_modules(ZSTD REQUIRED IMPORTED_TARGET libzstd)

add_library(prometheia
  src/catalog.cpp
  src/cbor.cpp
  src/de.cpp
  src/kepler.cpp
  src/time.cpp)
target_include_directories(prometheia PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(prometheia PUBLIC PkgConfig::ZSTD)
target_compile_options(prometheia PRIVATE -Wall -Wextra -Werror)

option(PROMETHEIA_SANITIZE "Build with ASan+UBSan" OFF)
if(PROMETHEIA_SANITIZE)
  add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer -g)
  add_link_options(-fsanitize=address,undefined)
endif()

enable_testing()
add_executable(test_cbor tests/test_cbor.cpp)
target_link_libraries(test_cbor PRIVATE prometheia)
add_test(NAME cbor COMMAND test_cbor)

add_executable(test_catalog tests/test_catalog.cpp)
target_link_libraries(test_catalog PRIVATE prometheia)
add_test(NAME catalog COMMAND test_catalog)

add_executable(test_kepler tests/test_kepler.cpp)
target_link_libraries(test_kepler PRIVATE prometheia)
add_test(NAME kepler COMMAND test_kepler)

add_executable(test_integrator tests/test_integrator.cpp)
target_link_libraries(test_integrator PRIVATE prometheia)
add_test(NAME integrator COMMAND test_integrator)

add_executable(test_mechanics tests/test_mechanics.cpp)
target_link_libraries(test_mechanics PRIVATE prometheia)
add_test(NAME mechanics COMMAND test_mechanics)

add_executable(test_radau tests/test_radau.cpp)
target_link_libraries(test_radau PRIVATE prometheia)
add_test(NAME radau COMMAND test_radau)

add_executable(test_de tests/test_de.cpp)
target_link_libraries(test_de PRIVATE prometheia)
add_test(NAME de COMMAND test_de)

add_executable(test_time tests/test_time.cpp)
target_link_libraries(test_time PRIVATE prometheia)
add_test(NAME time COMMAND test_time)

add_executable(prometheia-bench tools/bench/bench_main.cpp)
target_link_libraries(prometheia-bench PRIVATE prometheia)

add_executable(prometheia-convert tools/convert/convert_main.cpp)
target_link_libraries(prometheia-convert PRIVATE prometheia)

add_executable(prometheia-info tools/info/info_main.cpp)
target_link_libraries(prometheia-info PRIVATE prometheia)
