SET(RHONEDS_DIR ${CMAKE_CURRENT_LIST_DIR})

function (CreateRhoneDsExeTarget BINARY_NAME)
    message(STATUS "CreateRhoneDsExeTarget()")

    set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${RHONEDS_DIR}/bin)
    add_executable(${BINARY_NAME} ${SOURCE_FILES_APP} ${SOURCE_FILES_LIB} ${PROTO})
    target_compile_options(${BINARY_NAME} PUBLIC -Wno-write-strings)

    RhoneGetRhoneHfIncludeDirs(RHONEHF_INCLUDE_DIRS)
    target_include_directories(${BINARY_NAME} PRIVATE ${RHONEHF_INCLUDE_DIRS})

    add_dependencies(${BINARY_NAME} Rhone)
    SetRhoneDsLinkLibraries(${BINARY_NAME})
endfunction()

function (SetRhoneDsLinkLibraries TARGET)
    RhoneLinkLibraryWithWholeArchive(${TARGET} RhoneDsLib)
    RhoneCommonTargetLinkLibraries(${TARGET} RHONE_LIVE)
    target_link_libraries(${TARGET} "-Wl,--whole-archive \"${PROJECT_BINARY_DIR}/Rhone/libRhone.a\" -Wl,--no-whole-archive")
    set_target_properties(${TARGET} PROPERTIES LINK_FLAGS "-L/usr/local/lib -Wl,-rpath,/usr/local/lib")
    target_link_libraries(${TARGET} "/usr/lib64/libtdsodbc.so.0")
    target_link_libraries(${TARGET} z)
    target_link_libraries(${TARGET} curl)
    target_link_libraries(${TARGET} stdc++fs)
    target_link_libraries(${TARGET} "-Wl,--whole-archive \"$<TARGET_FILE:${LIB}>\" -Wl,--no-whole-archive")
    set_target_properties(${TARGET} PROPERTIES
            LINK_FLAGS "-Wl,--whole-archive"
            )
    target_link_libraries(${TARGET} Rhone)
endfunction()


