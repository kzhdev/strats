SET(RHONEDS_DIR ${CMAKE_CURRENT_LIST_DIR})

function (CreateRhoneDsLibTarget)
    message(STATUS "CreateRhoneDsLibTarget()")

    file(GLOB_RECURSE SOURCE_FILES_LIB "${RHONEDS_DIR}/src/*.cpp")
    file(GLOB_RECURSE PROTO "${RHONEDS_DIR}/src/messages/*.cc")

    add_library(RhoneDsLib STATIC ${SOURCE_FILES_LIB} ${PROTO})

    target_compile_options(RhoneDsLib PUBLIC -Wno-reorder)

    RhoneGetRhoneHfIncludeDirs(RHONEHF_INCLUDE_DIRS)
    target_include_directories(RhoneDsLib PRIVATE ${RHONEHF_INCLUDE_DIRS})

    SET(RHONECORE_DIR "${RHONEDS_DIR}/../rhonepublic/rhonecore")
    include(${RHONECORE_DIR}/RhoneCore.cmake)

    #Setting last param to TRUE builds an "OMS" version of the rhonecore lib whose target is "gtCoreLibOMS":
    CreateRhoneCoreTargets(FALSE FALSE)
endfunction()

function (SetRhoneDsLinkLibraries TARGET)
    RhoneLinkLibraryWithWholeArchive(${TARGET} RhoneDsLib)
    RhoneLinkLibraryWithWholeArchive(${TARGET} gtRhoneLib)
    RhoneCommonTargetLinkLibraries(${TARGET} RHONE_LIVE)
    set_target_properties(${TARGET} PROPERTIES LINK_FLAGS "-L/usr/local/lib -Wl,-rpath,/usr/local/lib")
    target_link_libraries(${TARGET} "/usr/lib64/libtdsodbc.so.0")
    target_link_libraries(${TARGET} z)
    target_link_libraries(${TARGET} curl)
endfunction()
