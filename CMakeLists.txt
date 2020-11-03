cmake_minimum_required(VERSION 3.13)
project(RhoneDS CXX)

set(CMAKE_CXX_STANDARD 17)
#set(CMAKE_VERBOSE_MAKEFILE ON)

set(RHONEDS_DIR ${CMAKE_CURRENT_LIST_DIR})
set(RHONEPUBLIC_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../rhonepublic)

if (CMAKE_CXX_COMPILER_ID STREQUAL "GNU" AND CMAKE_CXX_COMPILER_VERSION VERSION_GREATER 7.0)
    SET(HIGH_GCC_VERSION TRUE)
    include(CheckIPOSupported)
    check_ipo_supported()

    add_compile_options(-faligned-new)
endif()

message(STATUS "VIPER_VERSION: ${VIPER_VERSION}")
message(STATUS "VIPER_GITHASH: ${VIPER_GITHASH}")
message(STATUS "RHONEPUBLIC_GITHASH: ${RHONEPUBLIC_GITHASH}")
message(STATUS "CMAKE_BUILD_TYPE=${CMAKE_BUILD_TYPE}")

ADD_DEFINITIONS(-DVIPER_GITHASH=${VIPER_GITHASH})
ADD_DEFINITIONS(-DVIPER_VERSION=${VIPER_VERSION})
ADD_DEFINITIONS(-DRHONEPUBLIC_GITHASH=${RHONEPUBLIC_GITHASH})

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

SET(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Werror=implicit-fallthrough -Wno-class-memaccess")

if (CMAKE_BUILD_TYPE MATCHES Debug)
    SET(RELEASE binDebug)
else()
    SET(RELEASE binRelease)
endif()

SET(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${RHONEDS_DIR}/bin/${RELEASE}")

if (CMAKE_BUILD_TYPE MATCHES Debug)
    add_compile_definitions(IS_DEV)
    SET(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -fuse-ld=gold -L/usr/lib64")
endif()

message(STATUS "Building RhoneDS binary")
include(${RHONEDS_DIR}/RhoneDS.cmake)
set(BINARY_NAME RhoneDs)
if(VIPER_VERSION)
    SET(BINARY_NAME RhoneDs_${VIPER_VERSION})
endif()
message(STATUS "Creating binary ${BINARY_NAME}")

#For some reason the simulator cannot run on corlsd006 unless asan is statically linked
#set(CMAKE_CXX_FLAGS_RELEASE "${CMAKE_CXX_FLAGS_RELEASE} -fno-omit-frame-pointer -fsanitize=address -static-libasan")
#set(CMAKE_LINKER_FLAGS_RELEASE "${CMAKE_LINKER_FLAGS_RELEASE} -fno-omit-frame-pointer -fsanitize=address -fsanitize-recover=address -static-libasan")

#Enable libcpp debug
#SET(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -D_GLIBCXX_DEBUG")

BuildRhoneDS(${BINARY_NAME})

add_subdirectory(src/logging EXCLUDE_FOM_ALL)
#add_subdirectory(test EXCLUDE_FROM_ALL)
