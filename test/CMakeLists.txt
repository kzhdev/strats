cmake_minimum_required(VERSION 3.0)
project(RhoneDsTest CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(RHONEDS_TEST_DIR ${CMAKE_CURRENT_SOURCE_DIR})
set(RHONELIB_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../../rhonepublic/bin)
set(RHONECMAKE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../../rhonepublic/cmake)

ADD_DEFINITIONS(-DBOOST_DISABLE_ASSERTS=1)

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_CURRENT_LIST_DIR}/bin)
include("${RHONECMAKE_DIR}/RhoneClient.cmake")

message(STATUS "CMAKE_BUILD_TYPE=${CMAKE_BUILD_TYPE}")
message(STATUS "Using RhoneLib from ${RHONELIB_DIR}")

RHONE_SET_COMMON_COMPILER_OPTIONS()

file(GLOB_RECURSE SOURCE_FILES_TEST "${RHONEDS_TEST_DIR}/Fixtures/*.cpp" "${RHONEDS_TEST_DIR}/Mocks/*.cpp")

file(GLOB SOURCE_FILES_BASE "${RHONEDS_TEST_DIR}/../src/*.cpp")
file(GLOB SOURCE_FILES_MATH "${RHONEDS_TEST_DIR}/../src/math/*.cpp")
file(GLOB SOURCE_FILES_MESSAGES "${RHONEDS_TEST_DIR}/../src/messages/*.cpp")
file(GLOB SOURCE_FILES_LOGGING "${RHONEDS_TEST_DIR}/../src/logging/*.cpp")

file(GLOB_RECURSE REMOVE_FILE "${RHONEDS_TEST_DIR}/../src/logging/LogFileGenerator.cpp" "${RHONEDS_TEST_DIR}/../src/logging/LogStatistics.cpp")
list(REMOVE_ITEM SOURCE_FILES_LOGGING ${REMOVE_FILE})

file(GLOB_RECURSE SOURCE_FILES_APP "${RHONEDS_TEST_DIR}/../app/*.cpp")
file(GLOB_RECURSE PROTO "${RHONEDS_TEST_DIR}/../src/messages/*.cc")
file(GLOB SOURCE_FILES_MAIN "${RHONEDS_TEST_DIR}/test_main.cpp")

#list(REMOVE_ITEM SOURCE_FILES_TEST "${RHONEDS_TEST_DIR}/Fixtures/FormatterFixture.cpp")
#list(REMOVE_ITEM SOURCE_FILES_TEST "${RHONEDS_TEST_DIR}/Fixtures/ImpliedFixture.cpp")
#list(REMOVE_ITEM SOURCE_FILES_TEST "${RHONEDS_TEST_DIR}/Fixtures/InternalMatchingEngineFixture.cpp")
#list(REMOVE_ITEM SOURCE_FILES_TEST "${RHONEDS_TEST_DIR}/Fixtures/MathStructuresTests.cpp")
#list(REMOVE_ITEM SOURCE_FILES_TEST "${RHONEDS_TEST_DIR}/Fixtures/PnlCalculatorFixture.cpp")
#list(REMOVE_ITEM SOURCE_FILES_TEST "${RHONEDS_TEST_DIR}/Fixtures/PnlCalculatorTests.cpp")
#list(REMOVE_ITEM SOURCE_FILES_TEST "${RHONEDS_TEST_DIR}/Fixtures/SimpleTimeBufferFixture.cpp")
#list(REMOVE_ITEM SOURCE_FILES_TEST "${RHONEDS_TEST_DIR}/Fixtures/TestSymbolsResource.cpp")
#list(REMOVE_ITEM SOURCE_FILES_TEST "${RHONEDS_TEST_DIR}/Fixtures/StackerFailedDecrementBlockNewTests.cpp")
#list(REMOVE_ITEM SOURCE_FILES_TEST "${RHONEDS_TEST_DIR}/Fixtures/StackerFixture.cpp")
#list(REMOVE_ITEM SOURCE_FILES_TEST "${RHONEDS_TEST_DIR}/Fixtures/StackerICECorrectLevelTests.cpp")
#list(REMOVE_ITEM SOURCE_FILES_TEST "${RHONEDS_TEST_DIR}/Fixtures/StackerICEQuoteTests.cpp")
#list(REMOVE_ITEM SOURCE_FILES_TEST "${RHONEDS_TEST_DIR}/Fixtures/StackerModifyTests.cpp")
#list(REMOVE_ITEM SOURCE_FILES_TEST "${RHONEDS_TEST_DIR}/Fixtures/StackerNonAtomicOverfillTests.cpp")
#list(REMOVE_ITEM SOURCE_FILES_TEST "${RHONEDS_TEST_DIR}/Fixtures/StackerTests.cpp")
#list(REMOVE_ITEM SOURCE_FILES_TEST "${RHONEDS_TEST_DIR}/Fixtures/StackerWithHedgeTests.cpp")
list(REMOVE_ITEM SOURCE_FILES_TEST "${RHONEDS_TEST_DIR}/Fixtures/IntegrationTest.cpp")
list(REMOVE_ITEM SOURCE_FILES_TEST "${RHONEDS_TEST_DIR}/Fixtures/InternalMatchingEngineTests.cpp")
list(REMOVE_ITEM SOURCE_FILES_TEST "${RHONEDS_TEST_DIR}/Fixtures/MarketMakerFixture.cpp")
list(REMOVE_ITEM SOURCE_FILES_TEST "${RHONEDS_TEST_DIR}/Fixtures/MarketMakerTests.cpp")
list(REMOVE_ITEM SOURCE_FILES_TEST "${RHONEDS_TEST_DIR}/Fixtures/RelativeValueTests.cpp")
list(REMOVE_ITEM SOURCE_FILES_TEST "${RHONEDS_TEST_DIR}/Fixtures/ViperConfigTest.cpp")

add_executable(RhoneDsTest ${SOURCE_FILES_GTEST} ${SOURCE_FILES_TEST} ${SOURCE_FILES_BASE} ${SOURCE_FILES_MATH} ${SOURCE_FILES_MESSAGES} ${SOURCE_FILES_LOGGING} ${SOURCE_FILES_MAIN} ${PROTO})

target_compile_definitions(RhoneDsTest PUBLIC)
ConfigureRhoneDependencies(RhoneDsTest RHONE_LIVE)

target_compile_definitions(RhoneDsTest PRIVATE VIPER_UNIT_TEST=1)

include(${RHONECMAKE_DIR}/RhoneCoreBuildSettings.cmake)

RhoneGetRhoneHfIncludeDirs(RHONEHF_INCLUDE_DIRS)
target_include_directories(RhoneDsTest PRIVATE ${RHONEHF_INCLUDE_DIRS})
target_include_directories(RhoneDsTest PRIVATE "${RHONEDS_TEST_DIR}/../../rhonepublic/rhonelogging/src")
target_include_directories(RhoneDsTest PRIVATE "${RHONEDS_TEST_DIR}/../../rhonepublic/rhoneom/src/om")
target_include_directories(RhoneDsTest PRIVATE "${RHONEDS_TEST_DIR}/../../rhonepublic/3rdparty/kdb/include")
target_include_directories(RhoneDsTest PRIVATE "${RHONEDS_TEST_DIR}/../src/")
target_include_directories(RhoneDsTest PRIVATE "${RHONEDS_TEST_DIR}/../src/messages")
target_include_directories(RhoneDsTest PRIVATE "${RHONEDS_TEST_DIR}/../src/logging")
target_include_directories(RhoneDsTest PRIVATE "${RHONEDS_TEST_DIR}/Mocks")

if (CMAKE_BUILD_TYPE MATCHES Debug)
    SET(RELEASE binDebug)
else()
    SET(RELEASE binRelease)
endif()

#SET(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -DBOOST_DISABLE_ASSERTS=1 -march=skylake -mtune=skylake -mno-avx2")
#SET(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -DBOOST_DISABLE_ASSERTS=1 -mno-avx2")

SET(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -fuse-ld=gold -L/usr/lib64")

#target_link_directories(RhoneDsTest PRIVATE "${RHONELIB_DIR}/../rhonesimulator/simulator/quickfix/.libs") #quickfix
#target_link_libraries(RhoneDsTest "-Wl,--whole-archive \"${RHONELIB_DIR}/${RELEASE}/libgtRhoneLib.a\" -Wl,--no-whole-archive")
target_link_libraries(RhoneDsTest /usr/lib64/libcryptopp.so onload_ext z rt pthread log4cxx numa config++ boost_filesystem boost_system ciul1 dl protobuf curl sybdb cryptopp "/usr/lib64/libtdsodbc.so.0" lz4 stdc++fs gtest)
set_target_properties(RhoneDsTest PROPERTIES LINK_FLAGS "-L/usr/local/lib -Wl,-rpath,/usr/local/lib")
