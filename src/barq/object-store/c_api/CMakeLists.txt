set(BARQ_FFI_SOURCES
    ../../../barq.h

    config.cpp
    dictionary.cpp
    error.cpp
    logging.cpp
    notifications.cpp
    object.cpp
    list.cpp
    query.cpp
    barq.cpp
    scheduler.cpp
    schema.cpp
    set.cpp
    util.cpp
    vector.cpp

    conversion.hpp
    types.hpp
    util.hpp
    # barq/object-store/c_api/barq.c
)

if(BARQ_ENABLE_SYNC)
    list(APPEND BARQ_FFI_SOURCES
        app.cpp
        http.cpp
        sync.cpp
        socket_provider.cpp
    )
endif()

if(NOT EMSCRIPTEN)
    add_library(BarqFFI SHARED ${BARQ_FFI_SOURCES})
else()
    add_executable(BarqFFI ${BARQ_FFI_SOURCES})
endif()
add_library(BarqFFIStatic STATIC ${BARQ_FFI_SOURCES})

target_compile_definitions(BarqFFI PRIVATE -DBarq_EXPORTS)
target_compile_definitions(BarqFFIStatic PUBLIC -DBARQ_NO_DLLIMPORT)

target_link_libraries(BarqFFI PRIVATE Storage ObjectStore QueryParser)
target_link_libraries(BarqFFIStatic PRIVATE Storage ObjectStore QueryParser)

if (${BARQ_ENABLE_SYNC})
    target_link_libraries(BarqFFI PRIVATE Sync)
    target_link_libraries(BarqFFIStatic PRIVATE Sync)
endif()

if (ANDROID)
    target_link_libraries(BarqFFI PUBLIC android log)
endif()

target_include_directories(BarqFFIStatic INTERFACE
    $<BUILD_INTERFACE:${BarqCore_SOURCE_DIR}/src>
    $<INSTALL_INTERFACE:include>
)

target_include_directories(BarqFFI INTERFACE
    $<BUILD_INTERFACE:${BarqCore_SOURCE_DIR}/src>
    $<INSTALL_INTERFACE:include>
)

set_target_properties(BarqFFI PROPERTIES
    OUTPUT_NAME "barq-ffi"
    CXX_VISIBILITY_PRESET hidden
    VISIBILITY_INLINES_HIDDEN ON

    # FIXME: Not building by default because of the link-time dependency on
    # libuv. This should be fixed by refactoring Scheduler to be a dynamic
    # interface with an implementation selected at runtime.
    EXCLUDE_FROM_ALL ON
)

set_target_properties(BarqFFIStatic PROPERTIES
    OUTPUT_NAME "barq-ffi-static"
    CXX_VISIBILITY_PRESET hidden
    VISIBILITY_INLINES_HIDDEN ON
)

# install(TARGETS BarqFFI DESTINATION lib COMPONENT devel)

install(TARGETS BarqFFIStatic DESTINATION lib COMPONENT devel)
install(FILES ../../../barq.h DESTINATION include COMPONENT devel)
