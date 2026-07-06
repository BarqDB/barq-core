set(TEST_AFL_SOURCES
    afl_runner.cpp
    fuzz_engine.cpp
    fuzz_object.cpp
    fuzz_configurator.cpp
) # TEST_AFL_SOURCES_OBJECT_STORE

set(TEST_LIBFUZZER_SOURCES
    libfuzzer_runner.cpp
    fuzz_engine.cpp
    fuzz_object.cpp
    fuzz_configurator.cpp
) # TEST_LIBFUZZER_SOURCES_OBJECT_STORE

file(GLOB FUZZER_RUN_SCRIPTS
    "scripts/start_fuzz_afl.sh"
    "scripts/start_lib_fuzzer.sh")

file(COPY ${FUZZER_RUN_SCRIPTS}
    DESTINATION ${CMAKE_CURRENT_BINARY_DIR})
file(GLOB AFL_SEEDS "testcases/*")
file(MAKE_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/testcases)
file(COPY ${AFL_SEEDS}
    DESTINATION ${CMAKE_CURRENT_BINARY_DIR}/testcases)

add_executable(barq-afl++ ${TEST_AFL_SOURCES})
target_link_libraries(barq-afl++ TestUtil ObjectStore)

if(BARQ_LIBFUZZER)
    if(${CMAKE_CXX_COMPILER_ID} MATCHES "Clang")
        add_executable(barq-libfuzz ${TEST_LIBFUZZER_SOURCES})
        target_link_libraries(barq-libfuzz TestUtil ObjectStore)
    endif()
endif()
