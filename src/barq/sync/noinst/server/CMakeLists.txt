set(SERVER_SOURCES
    access_control.cpp
    access_token.cpp
    server.cpp
    server_dir.cpp
    server_file_access_cache.cpp
    server_history.cpp
)

set(SERVER_HEADERS
    access_control.hpp
    access_token.hpp
    clock.hpp
    crypto_server.hpp
    permissions.hpp
    server.hpp
    server_dir.hpp
    server_file_access_cache.hpp
    server_history.hpp
    server_impl_base.hpp
)

option(BARQ_BUILD_SERVER "Build the Barq sync server executable." OFF)

if(BARQ_BUILD_SERVER)
    set(SYNC_SERVER_EXCLUDE_FROM_ALL)
else()
    set(SYNC_SERVER_EXCLUDE_FROM_ALL EXCLUDE_FROM_ALL)
endif()

add_library(SyncServer STATIC ${SYNC_SERVER_EXCLUDE_FROM_ALL} ${SERVER_SOURCES} ${SERVER_HEADERS})
add_library(Barq::SyncServer ALIAS SyncServer)

set_target_properties(SyncServer PROPERTIES
    OUTPUT_NAME "barq-server"
)
target_link_libraries(SyncServer PUBLIC Sync QueryParser)

if(APPLE AND NOT BARQ_FORCE_OPENSSL)
    target_sources(SyncServer PRIVATE crypto_server_apple.mm)
elseif(BARQ_HAVE_OPENSSL)
    target_sources(SyncServer PRIVATE crypto_server_openssl.cpp)
else()
    target_sources(SyncServer PRIVATE crypto_server_stub.cpp)
endif()

if(BARQ_BUILD_SERVER)
    add_executable(BarqSyncServer server_main.cpp)
    set_target_properties(BarqSyncServer PROPERTIES
        OUTPUT_NAME "barq-server"
        DEBUG_POSTFIX "${CMAKE_DEBUG_POSTFIX}"
    )
    target_link_libraries(BarqSyncServer PRIVATE SyncServer)

    install(TARGETS BarqSyncServer
        COMPONENT runtime
        DESTINATION bin
    )
endif()
