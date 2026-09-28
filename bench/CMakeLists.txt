foreach(bench_name bench_latency bench_throughput bench_burst)
  add_executable(${bench_name} ${bench_name}.cpp)
  target_link_libraries(${bench_name} PRIVATE spsc::spsc spsc_platform spsc_warnings)
endforeach()
