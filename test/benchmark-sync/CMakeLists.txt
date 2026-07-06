if(BARQ_ENABLE_SYNC)
    add_executable(barq-benchmark-sync bench_transform.cpp ../test_all.cpp)
    add_dependencies(benchmarks barq-benchmark-sync)
    # Sync lib is included with SyncServer
    target_link_libraries(barq-benchmark-sync TestUtil SyncServer)
endif()