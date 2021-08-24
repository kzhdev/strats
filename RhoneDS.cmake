message(STATUS "Add RhoneDS build")

# enable auto relink
SET_SOURCE_FILES_PROPERTIES(
        app/MainApp.cpp PROPERTIES OBJECT_DEPENDS
        ${PROJECT_BINARY_DIR}/Rhone/bin/libRhone.a
)

add_executable(${BINARY_NAME} ${SOURCE_FILES_APP} ${SOURCE_FILES_LIB} ${PROTO})
target_compile_options(${BINARY_NAME} PUBLIC -Wno-write-strings)

if (${ENABLE_PERF})
    target_compile_definitions(${BINARY_NAME} PRIVATE -DENABLE_PERF)
endif()

if ($(ENABLE_LATENCY_TRACK))
    target_compile_definitions(${BINARY_NAME} PRIVATE -DENABLE_LATENCY_TRACK)
endif()

target_include_directories(${BINARY_NAME} PUBLIC
        ${RHONE_INCLUDES}
        ${RHONEPUBLIC_DIR}/rhonelogging/src
        ${RHONEPUBLIC_DIR}/rhoneom/src/om
)

add_dependencies(${BINARY_NAME} Rhone)

target_link_libraries(${BINARY_NAME} PUBLIC
        ${RHONE_LIBS}
        protobuf
        stdc++fs
        -Wl,--whole-archive
        Rhone
        -Wl,--no-whole-archive
)

