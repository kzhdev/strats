SET(RHONEDS_TEST_DIR ${CMAKE_CURRENT_LIST_DIR})

function (CreateRhoneDsTestTarget)
    message(STATUS "CreateRhoneDsTestTarget()")

    set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_CURRENT_LIST_DIR}/bin)
    file(GLOB_RECURSE SOURCE_FILES_TEST "${RHONEDS_TEST_DIR}/*.cpp")
    add_executable(RhoneDsTest ${SOURCE_FILES_TEST})

    target_compile_definitions(RhoneDsTest PUBLIC -DOMS)

    RhoneGetRhoneHfIncludeDirs(RHONEHF_INCLUDE_DIRS)
    target_include_directories(RhoneDsTest PRIVATE "${RHONEHF_INCLUDE_DIRS}")
    target_include_directories(RhoneDsTest PRIVATE "${RHONEDS_TEST_DIR}/../oms")
    target_include_directories(RhoneDsTest PRIVATE "${RHONEDS_TEST_DIR}/../oms/messages")

    SetRhoneOmsLinkLibraries(RhoneDsTest)
    target_link_libraries(RhoneDsTest gtest gtest_main gmock)

    include(CTest)
    enable_testing()
    add_test(OmsTest RhoneDsTest)
endfunction()
