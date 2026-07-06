add_executable(barq-benchmark-common-tasks main.cpp)
target_link_libraries(barq-benchmark-common-tasks TestUtil QueryParser)
add_dependencies(benchmarks barq-benchmark-common-tasks)

