message(STATUS "Add RhoneDS build")

add_executable(RhoneDsBackTest ${SOURCE_FILES_APP} ${SOURCE_FILES_LIB} ${PROTO})
target_compile_options(RhoneDsBackTest PRIVATE -Wno-unused-parameter)
target_compile_definitions(RhoneDsBackTest PRIVATE VIPER_BACKTEST)
target_compile_definitions(RhoneTestLib PRIVATE VIPER_BACKTEST)

target_include_directories(RhoneDsBackTest PUBLIC
        ./
        {RHONE_INCLUDES}
        ${RHONEPUBLIC_DIR}/rhonelogging/src
        ${RHONEPUBLIC_DIR}/rhoneom/src/om
        )

#        add_dependencies(RhoneTestLib Rhone)
add_dependencies(RhoneDsBackTest RhoneTestLib)

target_link_libraries(RhoneDsBackTest
        ${RHONE_LIBS}
        protobuf
        stdc++fs
        -Wl,--whole-archive
        RhoneTestLib
        -Wl,--no-whole-archive
        )



#include(${RHONEPUBLIC_DIR}/neorhone/cmake/FindQuickFAST.cmake)
#
#set(RHONETEST_SOURCES
#        ${RHONEPUBLIC_DIR}/rhonetest/app/main.cpp
#        ${RHONEPUBLIC_DIR}/rhonetest/app/strat/kdb/GtkdbRhoneDecoder.cpp
#        ${RHONEPUBLIC_DIR}/rhonetest/app/strat/kdb/KdbStrat.cpp
#        )
#
#add_subdirectory(${RHONEPUBLIC_DIR}/rhonetest ${PROJECT_BINARY_DIR}/RhoneTest/test)
#
## enable auto relink
#SET_SOURCE_FILES_PROPERTIES(
#        app/MainApp.cpp PROPERTIES OBJECT_DEPENDS
#        ${PROJECT_BINARY_DIR}/Rhone/bin/libRhoneTestLib.a
#)
#
#add_executable(RhoneDsBacktest ${RHONETEST_SOURCES} ${SOURCE_FILES_LIB} ${PROTO})
#target_compile_options(RhoneDsBacktest PUBLIC -Wno-write-strings)
#
#target_include_directories(RhoneDsBacktest PUBLIC
#        ${RHONE_INCLUDES}
#        ${RHONEPUBLIC_DIR}/rhonelogging/src
#        ${RHONEPUBLIC_DIR}/rhoneom/src/om
#        ${RHONEPUBLIC_DIR}/rhonetest/app
#        )
#
#add_dependencies(RhoneDsBacktest RhoneTestLib)
#
#target_link_libraries(RhoneDsBacktest PUBLIC
#        ${RHONE_LIBS}
#        protobuf
#        stdc++fs
#        neorhone_util
#        RhoneLegacy
#        -Wl,--whole-archive
#        RhoneTestLib
#        -Wl,--no-whole-archive
#        )
