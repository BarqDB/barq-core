find_package(SQLite3 REQUIRED)

add_executable(barq-benchmark-vector-search main.cpp)
add_dependencies(benchmarks barq-benchmark-vector-search)
target_link_libraries(barq-benchmark-vector-search TestUtil SQLite::SQLite3)
