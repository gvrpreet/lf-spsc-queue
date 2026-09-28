add_executable(order_pipeline main.cpp)
target_link_libraries(order_pipeline PRIVATE spsc::spsc spsc_platform spsc_warnings)
