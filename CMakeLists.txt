cmake_minimum_required(VERSION 3.13)
project(RhoneDS CXX)

set(CMAKE_CXX_STANDARD 17)
#set(CMAKE_VERBOSE_MAKEFILE ON)

find_package(Boost COMPONENTS filesystem system)

set(RHONEDS_DIR ${CMAKE_CURRENT_LIST_DIR})
set(RHONEPUBLIC_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../rhonepublic)

if (CMAKE_CXX_COMPILER_ID STREQUAL "GNU" AND CMAKE_CXX_COMPILER_VERSION VERSION_GREATER 7.0)
    SET(HIGH_GCC_VERSION TRUE)
    include(CheckIPOSupported)
    check_ipo_supported()
    add_compile_options(-faligned-new -Wno-missing-field-initializers -Wno-ignored-qualifiers -Wno-shadow)
endif()

message(STATUS "VIPER_VERSION: ${VIPER_VERSION}")
message(STATUS "VIPER_GITHASH: ${VIPER_GITHASH}")
message(STATUS "RHONEPUBLIC_GITHASH: ${RHONEPUBLIC_GITHASH}")
message(STATUS "CMAKE_BUILD_TYPE=${CMAKE_BUILD_TYPE}")
message(STATUS "ENABLE_PERF: ${ENABLE_PERF}")

ADD_DEFINITIONS(-DVIPER_GITHASH=${VIPER_GITHASH})
ADD_DEFINITIONS(-DVIPER_VERSION=${VIPER_VERSION})
ADD_DEFINITIONS(-DRHONEPUBLIC_GITHASH=${RHONEPUBLIC_GITHASH})
ADD_DEFINITIONS(-DIS_VIPER_BUILD=1)

#This is TEMPORARY!  If you see this outside of the v3 branch, remove is
ADD_DEFINITIONS(-DBOOST_DISABLE_ASSERTS=1)

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${RHONEDS_DIR}/bin)
file(GLOB_RECURSE SOURCE_FILES_APP "app/*.cpp")
file(GLOB_RECURSE SOURCE_FILES_LIB "src/*.cpp")
file(GLOB_RECURSE PROTO "src/messages/*.cc")

# This is why you don't glob sources in CMake:
# Remove LogFileGenerator.cpp from the sources.  This was originally
# solved by naming it .c !!
file(GLOB_RECURSE REMOVE_FILE "src/logging/LogFileGenerator.cpp" "src/logging/LogStatistics.cpp")
list(REMOVE_ITEM SOURCE_FILES_LIB ${REMOVE_FILE})

SET(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${RHONEDS_DIR}/bin/${RELEASE}")

# Common compile options
add_compile_options(
        -std=c++17
        -Wall
        -march=native
        -mno-avx
        -mno-avx2
        -Wno-strict-aliasing
        -Wno-multichar
        -Werror=implicit-fallthrough
        -Wno-class-memaccess
        -Wno-deprecated-declarations
        -Wno-address-of-packed-member
        -Wno-stringop-truncation
        -Wno-unused-variable
)

add_definitions(-DBOOST_NO_MT)

if (CMAKE_BUILD_TYPE MATCHES Debug)
    SET(RELEASE binDebug)
    add_compile_definitions(IS_DEV HFDEBUG _GLIBCXX_DEBUG)
    add_compile_options(-g3 -O0)
    SET(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -fuse-ld=gold -L/usr/lib64")
else()
    SET(RELEASE binRelease)
    add_compile_options(-g -O3)
endif()

if(VIPER_VERSION)
    SET(BINARY_NAME RhoneDs_${VIPER_VERSION})
else ()
    set(BINARY_NAME RhoneDs)
endif()

#For some reason the simulator cannot run on corlsd006 unless asan is statically linked
#set(CMAKE_CXX_FLAGS_RELEASE "${CMAKE_CXX_FLAGS_RELEASE} -fno-omit-frame-pointer -fsanitize=address -static-libasan")
#set(CMAKE_LINKER_FLAGS_RELEASE "${CMAKE_LINKER_FLAGS_RELEASE} -fno-omit-frame-pointer -fsanitize=address -fsanitize-recover=address -static-libasan")

include(Rhone.cmake)
include(RhoneDS.cmake)
#include(RhoneDS_BackTest.cmake)

if ((DEFINED HIGH_GCC_VERSION) AND NOT (CMAKE_BUILD_TYPE MATCHES Debug))
    set_property(TARGET ${BINARY_NAME} PROPERTY INTERPROCEDURAL_OPTIMIZATION TRUE)
    set_property(TARGET Rhone PROPERTY INTERPROCEDURAL_OPTIMIZATION TRUE)
    target_compile_options(Rhone PUBLIC -march=skylake -mtune=skylake)
    target_compile_options(${BINARY_NAME} PUBLIC -march=skylake -mtune=skylake)
endif()

add_subdirectory(src/logging cc)
add_subdirectory(test EXCLUDE_FROM_ALL)

include(RhoneDS_BackTest.cmake)
