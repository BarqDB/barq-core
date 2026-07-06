add_executable(barq-benchmark-crud main.cpp)
add_dependencies(benchmarks barq-benchmark-crud)
target_link_libraries(barq-benchmark-crud TestUtil)
