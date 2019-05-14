cmake_minimum_required(VERSION 3.0)
project(RhoneDsTest CXX)

set(RHONEDS_TEST_DIR ${CMAKE_CURRENT_SOURCE_DIR})
set(RHONELIB_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../../rhonepublic/bin)

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_CURRENT_LIST_DIR}/bin)
include("${RHONELIB_DIR}/cmake/RhoneClient.cmake")

message(STATUS "CMAKE_BUILD_TYPE=${CMAKE_BUILD_TYPE}")
message(STATUS "Using RhoneLib from ${RHONELIB_DIR}")

RHONE_SET_COMMON_COMPILER_OPTIONS()

file(GLOB_RECURSE SOURCE_FILES_TEST "${RHONEDS_TEST_DIR}/Fixtures/*.cpp" "${RHONEDS_TEST_DIR}/Mocks/*.cpp")

file(GLOB SOURCE_FILES_BASE "${RHONEDS_TEST_DIR}/../src/*.cpp")
file(GLOB SOURCE_FILES_MATH "${RHONEDS_TEST_DIR}/../src/math/*.cpp")
file(GLOB SOURCE_FILES_MESSAGES "${RHONEDS_TEST_DIR}/../src/messages/*.cpp")

file(GLOB_RECURSE SOURCE_FILES_APP "${RHONEDS_TEST_DIR}/../app/*.cpp")
file(GLOB_RECURSE PROTO "${RHONEDS_TEST_DIR}/../src/messages/*.cc")
file(GLOB SOURCE_FILES_GTEST "${RHONEDS_TEST_DIR}/gtest-1.8.0-fused/gmock-gtest-all.cc")
file(GLOB SOURCE_FILES_MAIN "${RHONEDS_TEST_DIR}/test_main.cpp")

add_executable(RhoneDsTest ${SOURCE_FILES_GTEST} ${SOURCE_FILES_TEST} ${SOURCE_FILES_BASE} ${SOURCE_FILES_MATH} ${SOURCE_FILES_MESSAGES} ${SOURCE_FILES_MAIN} ${PROTO})

target_compile_definitions(RhoneDsTest PUBLIC)
ConfigureRhoneDependencies(RhoneDsTest RHONE_LIVE)

target_include_directories(RhoneDsTest PRIVATE "${RHONELIB_DIR}/include/")
target_include_directories(RhoneDsTest PRIVATE "${RHONEDS_TEST_DIR}/../src/")
target_include_directories(RhoneDsTest PRIVATE "${RHONEDS_TEST_DIR}/../src/messages")
target_include_directories(RhoneDsTest PRIVATE "${RHONEDS_TEST_DIR}/Mocks")
target_include_directories(RhoneDsTest PRIVATE "${RHONEDS_TEST_DIR}/gtest-1.8.0-fused")

if (CMAKE_BUILD_TYPE MATCHES Debug)
    SET(RELEASE binDebug)
else()
    SET(RELEASE binRelease)
endif()


#target_link_directories(RhoneDsTest PRIVATE "${RHONELIB_DIR}/../rhonesimulator/simulator/quickfix/.libs") #quickfix
#target_link_libraries(RhoneDsTest "-Wl,--whole-archive \"${RHONELIB_DIR}/${RELEASE}/libgtRhoneLib.a\" -Wl,--no-whole-archive")
target_link_libraries(RhoneDsTest onload_ext z rt pthread log4cxx numa config++ boost_filesystem boost_system ciul1 dl protobuf curl sybdb "/usr/lib64/libtdsodbc.so.0" lz4 cryptopp stdc++fs)
set_target_properties(RhoneDsTest PROPERTIES LINK_FLAGS "-L/usr/local/lib -Wl,-rpath,/usr/local/lib")

