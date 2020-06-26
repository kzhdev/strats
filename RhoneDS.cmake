function (BuildRhoneDS BINARY_NAME)
        message(STATUS "Using RhonePublic sources for compile")

#       set(BUILD_WITH_OMS ON CACHE BOOL "" FORCE)
        set(BUILD_RHONETEST OFF CACHE BOOL "" FORCE)
        set(BUILD_RHONESIMULATOR ON CACHE BOOL "" FORCE)
        add_subdirectory(${RHONEPUBLIC_DIR} ${PROJECT_BINARY_DIR}/Rhone)

        include(${RHONEPUBLIC_DIR}/cmake/RhoneCoreBuildSettings.cmake)
        RHONE_SET_COMMON_COMPILER_OPTIONS()

        CreateRhoneDsExeTarget(${BINARY_NAME})

        if (CMAKE_BUILD_TYPE MATCHES Debug)
                SET(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKGER_FLAGS} -fuse-ld=gold")
                target_link_libraries(${BINARY_NAME} /usr/lib64/libcryptopp.a)
        endif()

        if(EXISTS ${RHONELIB_DIR}/rhone.q)
                ADD_CUSTOM_COMMAND(TARGET ${BINARY_NAME} POST_BUILD
                        COMMAND cp ${RHONELIB_DIR}/rhone.q ${CMAKE_RUNTIME_OUTPUT_DIRECTORY})
        endif()
endfunction()

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
        RhoneCommonTargetLinkLibraries(${TARGET} RHONE_LIVE)
        target_link_libraries(${BINARY_NAME} "-Wl,--whole-archive \"${PROJECT_BINARY_DIR}/Rhone/libRhone.a\" -Wl,--no-whole-archive")
        set_target_properties(${TARGET} PROPERTIES LINK_FLAGS "-L/usr/local/lib -Wl,-rpath,/usr/local/lib")
        target_link_libraries(${TARGET} "/usr/lib64/libtdsodbc.so.0" /usr/lib64/liboapi.a)
        target_link_libraries(${TARGET} z)
        target_link_libraries(${TARGET} stdc++fs)
endfunction()

