add_executable(BarqImporter importer_tool.cpp importer.cpp importer.hpp)
set_target_properties(BarqImporter PROPERTIES
    OUTPUT_NAME "barq-importer"
    DEBUG_POSTFIX ${CMAKE_DEBUG_POSTFIX}
)
target_link_libraries(BarqImporter Storage)

if(NOT APPLE AND NOT ANDROID AND NOT CMAKE_SYSTEM_NAME MATCHES "^Windows")
    add_executable(BarqDaemon barqd.cpp)
    set_target_properties(BarqDaemon PROPERTIES
        OUTPUT_NAME "barqd"
        DEBUG_POSTFIX ${CMAKE_DEBUG_POSTFIX})
    target_link_libraries(BarqDaemon Storage)
    list(APPEND ExecTargetsToInstall BarqDaemon)
endif()

add_executable(BarqTrawler barq_trawler.cpp)
set_target_properties(BarqTrawler PROPERTIES
    OUTPUT_NAME "barq-trawler"
    DEBUG_POSTFIX ${CMAKE_DEBUG_POSTFIX}
)
target_link_libraries(BarqTrawler Storage)
if (EMSCRIPTEN)
    set_target_properties(BarqTrawler PROPERTIES EXCLUDE_FROM_ALL TRUE)
endif()

add_executable(BarqEnumerate barq_enumerate.cpp)
set_target_properties(BarqEnumerate PROPERTIES
    OUTPUT_NAME "barq-enumerate"
    DEBUG_POSTFIX ${CMAKE_DEBUG_POSTFIX}
)
target_link_libraries(BarqEnumerate ObjectStore)
# FIXME can be fixed for others, but requires link and install fixes for libuv target
if (NOT APPLE)
    set_target_properties(BarqEnumerate PROPERTIES EXCLUDE_FROM_ALL TRUE)
endif()

add_executable(BarqDecrypt barq_decrypt.cpp)
set_target_properties(BarqDecrypt PROPERTIES
    OUTPUT_NAME "barq-decrypt"
    DEBUG_POSTFIX ${CMAKE_DEBUG_POSTFIX}
)
target_link_libraries(BarqDecrypt Storage)
if (NOT BARQ_ENABLE_ENCRYPTION)
    set_target_properties(BarqDecrypt PROPERTIES EXCLUDE_FROM_ALL TRUE)
endif()

add_executable(BarqEncrypt barq_encrypt.cpp)
set_target_properties(BarqEncrypt PROPERTIES
    OUTPUT_NAME "barq-encrypt"
    DEBUG_POSTFIX ${CMAKE_DEBUG_POSTFIX}
)
target_link_libraries(BarqEncrypt Storage)

add_executable(BarqBrowser barq_browser.cpp)
set_target_properties(BarqBrowser PROPERTIES
    OUTPUT_NAME "barq-browser-10"
    DEBUG_POSTFIX ${CMAKE_DEBUG_POSTFIX}
)
target_link_libraries(BarqBrowser Storage)

if(BARQ_ENABLE_SYNC)
add_executable(Barq2JSON barq2json.cpp )
set_target_properties(Barq2JSON PROPERTIES
    OUTPUT_NAME "barq2json"
    DEBUG_POSTFIX ${CMAKE_DEBUG_POSTFIX}
)
target_link_libraries(Barq2JSON Sync QueryParser)
list(APPEND ExecTargetsToInstall Barq2JSON)
endif()

add_executable(BarqDump barq_dump.c)
set_target_properties(BarqDump PROPERTIES
    OUTPUT_NAME "barq-dump"
    DEBUG_POSTFIX ${CMAKE_DEBUG_POSTFIX}
)

install(TARGETS ${ExecTargetsToInstall}
        COMPONENT runtime
        DESTINATION bin)
