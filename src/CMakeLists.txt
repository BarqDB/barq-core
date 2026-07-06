add_subdirectory(barq)
add_subdirectory(external/IntelRDFPMathLib20U2)

if (BARQ_ENABLE_GEOSPATIAL)
    add_subdirectory(external/s2)
endif()

if(CMAKE_SYSTEM_NAME MATCHES "^Windows")
  add_subdirectory(win32)
endif()

install(FILES barq.hpp
        DESTINATION include
        COMPONENT devel)
