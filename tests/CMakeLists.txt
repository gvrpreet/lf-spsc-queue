include(FetchContent)
FetchContent_Declare(
  googletest
  URL https://github.com/google/googletest/archive/refs/tags/v1.15.2.tar.gz
  DOWNLOAD_EXTRACT_TIMESTAMP TRUE
  SYSTEM
)
set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
set(BUILD_GMOCK OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(googletest)
include(GoogleTest)

foreach(test_name test_spsc_basic test_wraparound_stress test_full_queue)
  add_executable(${test_name} ${test_name}.cpp)
  target_link_libraries(${test_name} PRIVATE spsc::spsc spsc_platform spsc_warnings GTest::gtest_main)
  if(SPSC_SANITIZER STREQUAL "thread")
    # Kernels with vm.mmap_rnd_bits=32 (e.g. WSL2 6.x) break GCC 13's TSan
    # ("unexpected memory mapping"). Running with ASLR disabled avoids it
    # without needing root; ctest uses this for discovery and execution.
    set_target_properties(${test_name} PROPERTIES CROSSCOMPILING_EMULATOR "setarch;-R")
  endif()
  gtest_discover_tests(${test_name} DISCOVERY_TIMEOUT 30 PROPERTIES TIMEOUT 600)
endforeach()
