add_executable(order_pipeline main.cpp)
target_include_directories(order_pipeline PRIVATE ${CMAKE_CURRENT_SOURCE_DIR} ${PROJECT_SOURCE_DIR}/bench)
target_link_libraries(order_pipeline PRIVATE spsc::spsc spsc_platform spsc_warnings)
