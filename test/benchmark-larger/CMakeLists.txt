add_executable(barq-benchmark-larger EXCLUDE_FROM_ALL main.cpp)
add_dependencies(benchmarks barq-benchmark-larger)
target_link_libraries(barq-benchmark-larger TestUtil)
