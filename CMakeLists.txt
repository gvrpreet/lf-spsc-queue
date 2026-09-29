cmake_minimum_required(VERSION 3.25)
project(lf_spsc_queue LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

option(SPSC_BUILD_TESTS    "Build unit/stress tests"   ON)
option(SPSC_BUILD_BENCH    "Build benchmarks"          ON)
option(SPSC_BUILD_EXAMPLES "Build example pipeline"    ON)
set(SPSC_SANITIZER "" CACHE STRING "Sanitizer: '', 'address', or 'thread'")

find_package(Threads REQUIRED)

if(SPSC_SANITIZER STREQUAL "address")
  add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer -g)
  add_link_options(-fsanitize=address,undefined)
elseif(SPSC_SANITIZER STREQUAL "thread")
  # TSan does not model standalone fences (-Wtsan). The fences in SpinThenPark are
  # verified by the ping-pong stress test instead; see docs/results/mutation_testing.md.
  add_compile_options(-fsanitize=thread -g $<$<CXX_COMPILER_ID:GNU>:-Wno-tsan>)
  add_link_options(-fsanitize=thread)
endif()

# Header-only core library.
add_library(spsc INTERFACE)
add_library(spsc::spsc ALIAS spsc)
target_include_directories(spsc INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_compile_features(spsc INTERFACE cxx_std_20)
target_link_libraries(spsc INTERFACE Threads::Threads)

# Test/bench helpers (TSC, pinning); not part of the queue library.
add_library(spsc_platform INTERFACE)
target_include_directories(spsc_platform INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/common)
target_link_libraries(spsc_platform INTERFACE Threads::Threads)

add_library(spsc_warnings INTERFACE)
target_compile_options(spsc_warnings INTERFACE -Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wno-sign-conversion)

if(SPSC_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()
if(SPSC_BUILD_BENCH)
  add_subdirectory(bench)
endif()
if(SPSC_BUILD_EXAMPLES)
  add_subdirectory(examples/order_pipeline)
endif()
