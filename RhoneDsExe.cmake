SET(RHONEDS_DIR ${CMAKE_CURRENT_LIST_DIR})

function (CreateRhoneDsExeTarget BINARY_NAME)
    message(STATUS "CreateRhoneDsExeTarget()")

    set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${RHONEDS_DIR}/bin)
    add_executable(${BINARY_NAME} ${SOURCE_FILES_APP})
    target_compile_options(${BINARY_NAME} PUBLIC -Wno-write-strings)

    RhoneGetRhoneHfIncludeDirs(RHONEHF_INCLUDE_DIRS)
    target_include_directories(${BINARY_NAME} PRIVATE ${RHONEHF_INCLUDE_DIRS})

    SetRhoneDsLinkLibraries(${BINARY_NAME})
endfunction()
