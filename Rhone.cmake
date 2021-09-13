message(STATUS "Add Rhone build")

set(BUILD_RHONETEST OFF CACHE BOOL "" FORCE)
set(BUILD_RHONESIMULATOR ON CACHE BOOL "" FORCE)
add_subdirectory(${RHONEPUBLIC_DIR}/neorhone ${PROJECT_BINARY_DIR}/Rhone)

if (CMAKE_BUILD_TYPE MATCHES Debug)
    target_compile_definitions(RhoneLegacy PRIVATE -DHFDEBUG_DETAIL)
endif()