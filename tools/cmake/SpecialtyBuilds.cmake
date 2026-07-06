if (CMAKE_BUILD_TYPE MATCHES "RelAssert")
    set(BARQ_ENABLE_ASSERTIONS ON CACHE BOOL "Build with assertions")
endif()

if (CMAKE_BUILD_TYPE MATCHES "RelASAN")
    set(BARQ_ASAN ON CACHE BOOL "Build with address sanitizer")
endif()

if (CMAKE_BUILD_TYPE MATCHES "RelTSAN")
    set(BARQ_TSAN ON CACHE BOOL "Build with address sanitizer")
endif()

if (CMAKE_BUILD_TYPE MATCHES "RelMSAN")
    set(BARQ_MSAN ON CACHE BOOL "Build with memory sanitizer")
endif()

if (CMAKE_BUILD_TYPE MATCHES "RelUSAN")
    set(BARQ_USAN ON CACHE BOOL "Build with undefined sanitizer")
endif()

# -------------
# Coverage
# -------------
option(BARQ_COVERAGE "Compile with coverage support." OFF)
if(BARQ_COVERAGE)
    if(MSVC)
        message(FATAL_ERROR
                "Code coverage is not yet supported on Visual Studio builds")
    else()
        set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -g --coverage -fprofile-arcs -ftest-coverage -fno-elide-constructors")
        if("${CMAKE_CXX_COMPILER_ID}" STREQUAL "GNU")
            set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -fno-inline -fno-inline-small-functions -fno-default-inline")
        endif()
    endif()
endif()

option(BARQ_LLVM_COVERAGE "Compile with llvm's code coverage support." OFF)
if (BARQ_LLVM_COVERAGE)
    if(${CMAKE_CXX_COMPILER_ID} MATCHES "Clang")
        set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -fprofile-instr-generate -fcoverage-mapping")
        set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_LINK_FLAGS} -fprofile-instr-generate -fcoverage-mapping")
    else()
        message(FATAL_ERROR "Code coverage is only supported with clang")
    endif()
endif()


# -------------
# AFL
# -------------
option(BARQ_AFL "Compile for fuzz testing." OFF)
if(BARQ_AFL)
    if(${CMAKE_CXX_COMPILER_ID} MATCHES "Clang")
        set(FUZZ_COMPILER_NAME "afl-clang++")
    elseif(${CMAKE_CXX_COMPILER_ID} MATCHES "GNU")
        set(FUZZ_COMPILER_NAME "afl-g++")
    else()
        message(FATAL_ERROR
                "Running AFL with your compiler (${CMAKE_CXX_COMPILER_ID}) is not supported")
    endif()
    find_program(AFL ${FUZZ_COMPILER_NAME})
    if(NOT AFL)
        message(FATAL_ERROR "AFL not found!")
    endif()
    set(CMAKE_CXX_COMPILER "${AFL}")
endif()

# -------------
# libfuzzer
# -------------
option(BARQ_LIBFUZZER "Compile with llvm libfuzzer" OFF)
if(BARQ_LIBFUZZER)
    if(${CMAKE_CXX_COMPILER_ID} MATCHES "Clang")
        # todo: add the undefined sanitizer here after blacklisting false positives
        set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -g -fsanitize=fuzzer,address")
    else()
        message(FATAL_ERROR
                "Compiling for libfuzzer is only supported with clang")
    endif()
endif()

# -------------
# Address Sanitizer
# -------------
option(BARQ_ASAN "Compile with address sanitizer support" OFF)
if(BARQ_ASAN)
    if (MSVC)
        set(BARQ_SANITIZER_FLAGS /fsanitize=address)
        set(BARQ_SANITIZER_LINK_FLAGS /INCREMENTAL:NO)

        if ("${CMAKE_BUILD_TYPE}" STREQUAL "RelASAN")
            list(APPEND BARQ_SANITIZER_FLAGS /Ox /Zi)
            list(APPEND BARQ_SANITIZER_LINK_FLAGS /DEBUG)
        elseif (NOT "${CMAKE_BUILD_TYPE}" MATCHES ".*Deb.*")
            # disable warning for better stacktrace for asan
            # pdbs can be activated with RelWithDebInfo or RelASAN
            list(APPEND BARQ_SANITIZER_FLAGS /wd5072)
        endif()
    else()
        set(BARQ_SANITIZER_FLAGS -fsanitize=address -fno-sanitize-recover=all -fsanitize-address-use-after-scope -fno-omit-frame-pointer)
    endif()
endif()

# -------------
# Thread Sanitizer
# -------------
option(BARQ_TSAN "Compile with thread sanitizer support" OFF)
if(BARQ_TSAN)
    if(MSVC)
        message(FATAL_ERROR
                "The Thread Sanitizer is not yet supported on Visual Studio builds")
    else()
        set(BARQ_SANITIZER_FLAGS -fsanitize=thread -fno-sanitize-recover=all -fno-omit-frame-pointer)
    endif()
endif()

# -------------
# Memory Sanitizer
# -------------
option(BARQ_MSAN "Compile with memory sanitizer support" OFF)
if(BARQ_MSAN)
    if(MSVC)
        message(FATAL_ERROR
                "The Memory Sanitizer is not yet supported on Visual Studio builds")
    else()
        set(BARQ_SANITIZER_FLAGS -fsanitize=memory -fsanitize-memory-track-origins -fno-omit-frame-pointer)
    endif()
endif()

# -------------
# Undefined Sanitizer
# -------------
option(BARQ_USAN "Compile with undefined sanitizer support" OFF)
if(BARQ_USAN)
    if(MSVC)
        message(FATAL_ERROR
                "The Undefined Sanitizer is not yet supported on Visual Studio builds")
    else()
        set(BARQ_SANITIZER_FLAGS -fsanitize=undefined -fno-sanitize-recover=all -fno-omit-frame-pointer)
    endif()
endif()

if (BARQ_SANITIZER_FLAGS)
    if (NOT MSVC)
        if ("${CMAKE_BUILD_TYPE}" MATCHES "Rel[ATMU]SAN")
            list(APPEND BARQ_SANITIZER_FLAGS -O1 -g)
        endif()

        set(BARQ_SANITIZER_LINK_FLAGS ${BARQ_SANITIZER_FLAGS})

        if (CMAKE_COMPILER_IS_GNUXX) # activated for clang automatically according to docs
            list(APPEND BARQ_SANITIZER_LINK_FLAGS -pie)
        endif()
    endif()

    add_compile_options(${BARQ_SANITIZER_FLAGS})
    if (MSVC)
        add_compile_definitions(_DISABLE_STRING_ANNOTATION _DISABLE_VECTOR_ANNOTATION)
    endif()

    if (BARQ_SANITIZER_LINK_FLAGS)
        add_link_options(${BARQ_SANITIZER_LINK_FLAGS})
    endif()
endif()
