cmake_minimum_required(VERSION 3.0)
project(RhoneDS CXX)

set(RHONEDS_DIR ${CMAKE_CURRENT_LIST_DIR})
set(RHONELIB_DIR /home/pkusch/ClionProjects/rhonepublic/bin)
set(RHONEPUBLIC_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../rhonepublic)

message(STATUS "CMAKE_BUILD_TYPE=${CMAKE_BUILD_TYPE}")

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${RHONEDS_DIR}/bin)
file(GLOB_RECURSE SOURCE_FILES_APP "${RHONEDS_DIR}/app/*.cpp")
file(GLOB_RECURSE SOURCE_FILES_LIB "${RHONEDS_DIR}/oms/*.cpp")
file(GLOB_RECURSE SOURCE_FILES_LIB "${RHONEDS_DIR}/src/*.cpp")
file(GLOB_RECURSE PROTO "${RHONEDS_DIR}/oms/messages/*.cc")



if (CMAKE_BUILD_TYPE MATCHES Debug)
    SET(RELEASE binDebug)
else()
    SET(RELEASE binRelease)
endif()

SET(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${RHONEDS_DIR}/bin/${RELEASE}")

message(STATUS "Building RhoneDS binary")
include(${RHONEDS_DIR}/RhoneDS.cmake)
BuildRhoneDS(RhoneDS "RhoneDS")
