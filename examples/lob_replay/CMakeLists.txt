# Named spsc_lob_replay: lob-engine already defines a `lob_replay` library target.
add_executable(spsc_lob_replay main.cpp)
target_include_directories(spsc_lob_replay PRIVATE ${PROJECT_SOURCE_DIR}/bench)
target_link_libraries(spsc_lob_replay PRIVATE spsc::spsc spsc_platform lob::replay)

if(SPSC_SANITIZER STREQUAL "thread")
  # Same ASLR workaround as tests/CMakeLists.txt.
  set_target_properties(spsc_lob_replay PROPERTIES CROSSCOMPILING_EMULATOR "setarch;-R")
endif()

# Correctness check on lob-engine's committed golden logs: the two-thread pipeline must
# reproduce the single-threaded book exactly (the program exits non-zero otherwise).
if(DEFINED lob_engine_SOURCE_DIR)
  set(_lob_src ${lob_engine_SOURCE_DIR})
else()
  set(_lob_src ${SPSC_LOB_ENGINE_DIR})
endif()
if(SPSC_BUILD_TESTS)
  foreach(golden golden_2000_seed7 golden_v2_2000_seed7)
    if(EXISTS ${_lob_src}/tests/data/${golden}.bin)
      add_test(NAME lob_replay_${golden}
               COMMAND spsc_lob_replay ${_lob_src}/tests/data/${golden}.bin --reps=1
                       --producer-cpu=-1 --consumer-cpu=-1)
    endif()
  endforeach()
endif()
