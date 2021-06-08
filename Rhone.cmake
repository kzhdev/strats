message(STATUS "Add Rhone build")

set(BUILD_RHONESIMULATOR ON CACHE BOOL "" FORCE)
add_subdirectory(${RHONEPUBLIC_DIR}/neorhone ${PROJECT_BINARY_DIR}/Rhone)

#add_subdirectory(${RHONEPUBLIC_DIR}/rhonetest ${PROJECT_BINARY_DIR}/RhoneTest)
#target_include_directories(RhoneTestLib PRIVATE /usr/include/QuickFAST)
#target_link_libraries(RhoneTest
#        -Wl,--whole-archive
#        RhoneTestLib
#        -Wl,--no-whole-archive
#        )